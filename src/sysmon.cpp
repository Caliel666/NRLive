#include "sysmon.h"
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <algorithm>
#include <cwchar>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "pdh.lib")

using Microsoft::WRL::ComPtr;

// Minimal D3DKMT declarations (d3dkmthk.h drags in kernel headers). These are
// the thunks Task Manager uses for GPU temperature and engine clocks; drivers
// without WDDM 2.4 perf data simply fail the query and the value stays hidden.
namespace {
typedef LONG NTSTATUS_;
typedef UINT D3DKMT_HANDLE_;
struct KmtOpenFromLuid { LUID AdapterLuid; D3DKMT_HANDLE_ hAdapter; };
struct KmtClose { D3DKMT_HANDLE_ hAdapter; };
struct KmtQuery { D3DKMT_HANDLE_ hAdapter; UINT Type; void* pPrivateDriverData; UINT PrivateDriverDataSize; };
struct KmtNodePerf {
  UINT32 NodeOrdinal; UINT32 PhysicalAdapterIndex;
  ULONGLONG Frequency, MaxFrequency, MaxFrequencyOC;
  ULONG Voltage, VoltageMax, VoltageMaxOC;
  ULONGLONG MaxTransitionLatency;
};
struct KmtAdapterPerf {
  UINT32 PhysicalAdapterIndex;
  ULONGLONG MemoryFrequency, MaxMemoryFrequency, MaxMemoryFrequencyOC, MemoryBandwidth, PCIEBandwidth;
  ULONG FanRPM, Power, Temperature;
  UCHAR PowerStateOverride;
};
constexpr UINT KMTQAITYPE_NODEPERFDATA_ = 61;
constexpr UINT KMTQAITYPE_ADAPTERPERFDATA_ = 62;
typedef NTSTATUS_(APIENTRY* PfnOpen)(KmtOpenFromLuid*);
typedef NTSTATUS_(APIENTRY* PfnClose)(const KmtClose*);
typedef NTSTATUS_(APIENTRY* PfnQuery)(const KmtQuery*);

struct Kmt {
  PfnOpen open = nullptr; PfnClose close = nullptr; PfnQuery query = nullptr;
  D3DKMT_HANDLE_ adapter = 0;
  int clockNode = -1; // node with the highest reported clock; found once
  bool init(LUID luid) {
    HMODULE g = GetModuleHandleW(L"gdi32.dll");
    if (!g) return false;
    open = (PfnOpen)GetProcAddress(g, "D3DKMTOpenAdapterFromLuid");
    close = (PfnClose)GetProcAddress(g, "D3DKMTCloseAdapter");
    query = (PfnQuery)GetProcAddress(g, "D3DKMTQueryAdapterInfo");
    if (!open || !close || !query) return false;
    KmtOpenFromLuid o{ luid, 0 };
    if (open(&o) != 0) return false;
    adapter = o.hAdapter;
    return true;
  }
  void shutdown() { if (adapter && close) { KmtClose c{ adapter }; close(&c); } adapter = 0; }
  float temperature() {
    if (!adapter) return -1;
    KmtAdapterPerf p{};
    KmtQuery q{ adapter, KMTQAITYPE_ADAPTERPERFDATA_, &p, sizeof(p) };
    if (query(&q) != 0 || p.Temperature == 0) return -1;
    return p.Temperature / 10.0f;
  }
  float nodeClock(UINT node) {
    KmtNodePerf p{};
    p.NodeOrdinal = node;
    KmtQuery q{ adapter, KMTQAITYPE_NODEPERFDATA_, &p, sizeof(p) };
    if (query(&q) != 0) return -1;
    return p.Frequency ? (float)(p.Frequency / 1000000.0) : -1;
  }
  float clockMhz() {
    if (!adapter) return -1;
    if (clockNode < 0) {
      // The 3D engine is node 0 on every driver seen so far; probe a few in
      // case it is not, and keep the first one that reports a clock.
      for (UINT n = 0; n < 8 && clockNode < 0; ++n)
        if (nodeClock(n) > 0) clockNode = (int)n;
      if (clockNode < 0) { clockNode = 0; return -1; }
    }
    return nodeClock((UINT)clockNode);
  }
};

struct Pdh {
  PDH_HQUERY query = nullptr;
  PDH_HCOUNTER engine = nullptr, memory = nullptr;
  std::wstring luidTag;
  bool primed = false;
  void init(LUID luid, bool wantEngine, bool wantMemory) {
    if (!query && PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) { query = nullptr; return; }
    wchar_t tag[64];
    swprintf_s(tag, L"luid_0x%08X_0x%08X", (unsigned)luid.HighPart, (unsigned)luid.LowPart);
    luidTag = tag;
    // English names: counter names are localised on non-English Windows.
    if (wantEngine && !engine &&
        PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &engine) != ERROR_SUCCESS)
      engine = nullptr;
    if (wantMemory && !memory &&
        PdhAddEnglishCounterW(query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &memory) != ERROR_SUCCESS)
      memory = nullptr;
  }
  void shutdown() { if (query) PdhCloseQuery(query); query = nullptr; engine = memory = nullptr; primed = false; }
  static bool readArray(PDH_HCOUNTER c, std::vector<BYTE>& buf, PDH_FMT_COUNTERVALUE_ITEM_W*& items, DWORD& count) {
    DWORD size = 0; count = 0;
    PDH_STATUS s = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr);
    if (s != PDH_MORE_DATA || !size) return false;
    buf.resize(size);
    items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
    return PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items) == ERROR_SUCCESS;
  }
  void sample(SysSnapshot& s) {
    if (!query || PdhCollectQueryData(query) != ERROR_SUCCESS) return;
    if (!primed) { primed = true; return; } // rate counters need two samples
    std::vector<BYTE> buf;
    PDH_FMT_COUNTERVALUE_ITEM_W* items = nullptr;
    DWORD count = 0;
    if (engine && readArray(engine, buf, items, count)) {
      // Sum per engine across processes, report the busiest engine.
      std::map<std::wstring, double> perEngine;
      for (DWORD i = 0; i < count; ++i) {
        const std::wstring name = items[i].szName ? items[i].szName : L"";
        if (name.find(luidTag) == std::wstring::npos || items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
        size_t eng = name.find(L"_eng_");
        if (eng == std::wstring::npos) continue;
        perEngine[name.substr(eng)] += items[i].FmtValue.doubleValue;
      }
      double best = 0;
      for (auto& [k, v] : perEngine) best = std::max(best, v);
      if (!perEngine.empty()) s.gpuLoad = (float)std::min(100.0, best);
    }
    if (memory && readArray(memory, buf, items, count)) {
      double bytes = 0; bool found = false;
      for (DWORD i = 0; i < count; ++i) {
        const std::wstring name = items[i].szName ? items[i].szName : L"";
        if (name.find(luidTag) == std::wstring::npos || items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
        bytes += items[i].FmtValue.doubleValue; found = true;
      }
      if (found) s.vramUsedGb = (float)(bytes / (1024.0 * 1024.0 * 1024.0));
    }
  }
};

ULONGLONG ft64(const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }
} // namespace

void SysMon::setAdapter(LUID luid) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_luid = luid; m_hasLuid = true;
}

void SysMon::configure(const SysMonWants& wants, int periodMs) {
  if (!wants.any()) { stop(); return; }
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_wants = wants;
    m_periodMs = std::clamp(periodMs, 250, 2000);
  }
  if (!m_thread.joinable()) {
    m_quit = false;
    m_thread = std::thread([this] { run(); });
  }
}

void SysMon::stop() {
  if (!m_thread.joinable()) return;
  { std::lock_guard<std::mutex> lock(m_mutex); m_quit = true; }
  m_cv.notify_all();
  m_thread.join();
  std::lock_guard<std::mutex> lock(m_mutex);
  m_snap = {};
}

SysSnapshot SysMon::snapshot() {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_snap;
}

void SysMon::run() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  LUID luid{};
  bool hasLuid = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    luid = m_luid; hasLuid = m_hasLuid;
  }
  float vramTotal = -1;
  if (hasLuid) {
    ComPtr<IDXGIFactory4> f;
    ComPtr<IDXGIAdapter1> a;
    DXGI_ADAPTER_DESC1 d{};
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->EnumAdapterByLuid(luid, IID_PPV_ARGS(&a))) && SUCCEEDED(a->GetDesc1(&d)))
      vramTotal = (float)(d.DedicatedVideoMemory / (1024.0 * 1024.0 * 1024.0));
  }
  Kmt kmt;
  bool kmtOk = hasLuid && kmt.init(luid);
  Pdh pdh;
  FILETIME idle0{}, kern0{}, user0{};
  GetSystemTimes(&idle0, &kern0, &user0);

  for (;;) {
    SysMonWants w; int period;
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      if (m_quit) break;
      w = m_wants; period = m_periodMs;
    }
    SysSnapshot s;
    if (hasLuid && (w.gpuLoad || w.vram)) {
      pdh.init(luid, w.gpuLoad, w.vram);
      pdh.sample(s);
    }
    if (w.vram) s.vramTotalGb = vramTotal;
    if (kmtOk && w.gpuTemp) s.gpuTempC = kmt.temperature();
    if (kmtOk && w.gpuClock) s.gpuClockMhz = kmt.clockMhz();
    if (w.cpu) {
      FILETIME idle, kern, user;
      if (GetSystemTimes(&idle, &kern, &user)) {
        const ULONGLONG di = ft64(idle) - ft64(idle0);
        const ULONGLONG total = (ft64(kern) - ft64(kern0)) + (ft64(user) - ft64(user0));
        if (total) s.cpuLoad = (float)(100.0 * (1.0 - (double)di / (double)total));
        idle0 = idle; kern0 = kern; user0 = user;
      }
    }
    if (w.ram) {
      MEMORYSTATUSEX m{ sizeof(m) };
      if (GlobalMemoryStatusEx(&m)) {
        s.ramTotalGb = (float)(m.ullTotalPhys / (1024.0 * 1024.0 * 1024.0));
        s.ramUsedGb = (float)((m.ullTotalPhys - m.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0));
      }
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    // Keep the previous rate values while PDH primes its first sample.
    if (s.gpuLoad < 0) s.gpuLoad = w.gpuLoad ? m_snap.gpuLoad : -1;
    if (s.vramUsedGb < 0) s.vramUsedGb = w.vram ? m_snap.vramUsedGb : -1;
    m_snap = s;
    m_cv.wait_for(lock, std::chrono::milliseconds(period), [this] { return m_quit; });
    if (m_quit) break;
  }
  pdh.shutdown();
  kmt.shutdown();
}
