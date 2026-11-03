#pragma once
#include <windows.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>

// System sensors for the HUD, sampled on a worker thread so the render loop
// never waits on PDH or the kernel thunks. Values below zero mean unavailable.
struct SysSnapshot {
  float gpuLoad = -1;      // %, busiest engine of the adapter (Task Manager style)
  float gpuClockMhz = -1;  // 3D engine clock
  float gpuTempC = -1;
  float vramUsedGb = -1;   // dedicated memory in use by all processes
  float vramTotalGb = -1;
  float cpuLoad = -1;      // %
  float ramUsedGb = -1;
  float ramTotalGb = -1;
};

struct SysMonWants {
  bool gpuLoad = false, gpuClock = false, gpuTemp = false, vram = false, cpu = false, ram = false;
  bool any() const { return gpuLoad || gpuClock || gpuTemp || vram || cpu || ram; }
};

class SysMon {
public:
  ~SysMon() { stop(); }
  void setAdapter(LUID luid);
  // Starts, reconfigures or stops the worker. Cheap to call often.
  void configure(const SysMonWants& wants, int periodMs);
  void stop();
  SysSnapshot snapshot();

private:
  void run();
  std::thread m_thread;
  std::mutex m_mutex;
  std::condition_variable m_cv;
  bool m_quit = false;
  SysMonWants m_wants{};
  int m_periodMs = 500;
  LUID m_luid{};
  bool m_hasLuid = false;
  SysSnapshot m_snap{};
};
