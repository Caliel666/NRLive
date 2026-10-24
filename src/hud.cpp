#include "hud.h"
#include "sysmon.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <objidl.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

namespace {

constexpr int kRing = 2000;          // frametimes kept for lows and the graph
constexpr DWORD kGraphFrameMs = 33;  // graph redraw cadence (~30 Hz)
constexpr UINT WM_HUD_KICK = WM_APP + 41;
const wchar_t* kHudClass = L"NRLiveMangoHud";

// OpenNR palette
constexpr COLORREF kPanel = RGB(14, 14, 16);
constexpr COLORREF kBorder = RGB(58, 22, 26);
constexpr COLORREF kTitleA = RGB(122, 14, 20);
constexpr COLORREF kTitleB = RGB(70, 10, 14);
constexpr COLORREF kMuted = RGB(150, 150, 158);
constexpr COLORREF kPillOff = RGB(40, 40, 44);

struct Shared {
  OverlayHudConfig cfg;
  bool cfgDirty = true;
  bool enabled = true;
  bool presentation = true;
  bool exclude = false, excludeDirty = false;
  bool topmost = false;
  double sumMs = 0; int frames = 0;
  double latSum = 0; int latN = 0;
  double gpuSum[4]{}; int gpuN[4]{};
  bool fsr = false, nr = false, fg = false;
  ::Size cap{}, out{};
  float ring[kRing]{};
  int ringIdx = 0, ringCount = 0;
};

std::mutex g_m;
Shared g_s;
std::thread g_thread;
std::atomic<HWND> g_hud{ nullptr };
HWND g_output = nullptr;
HINSTANCE g_inst = nullptr;
std::atomic<bool> g_quit{ false };
SysMon g_sys;

struct View {
  float outFps = 0, srcFps = 0, genFps = 0, ftMs = 0, low1 = 0, low01 = 0;
  float lat = -1, gpu[4]{ -1, -1, -1, -1 };
  bool fsr = false, nr = false, fg = false;
  ::Size cap{}, out{};
  SysSnapshot sys;
  float drawMs = 0;
};

enum class St { Label, Inline, Value, Unit, Big, PillOn, PillOff, Title, Muted };
struct Seg { std::wstring text; St st; float w = 0; };
struct Row { enum Kind { Text, Graph, Title } kind = Text; std::vector<Seg> segs; float h = 0; };

Color argb(COLORREF c, BYTE a = 255) { return Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

std::wstring fmt(const wchar_t* f, double v) { wchar_t b[32]; swprintf_s(b, f, v); return b; }

// ── Drawing resources (HUD thread only) ──────────────────────────────────
struct Res {
  int scale = 0;
  float s = 1;
  std::unique_ptr<FontFamily> ui, semi;
  std::unique_ptr<Font> label, value, unit, big, title;
  std::unique_ptr<StringFormat> sf;
  std::unique_ptr<Bitmap> measureBmp;
  std::unique_ptr<Gdiplus::Graphics> measure;
  // canvas
  HDC dc = nullptr;
  HBITMAP bmp = nullptr, oldBmp = nullptr;
  void* bits = nullptr;
  int capW = 0, capH = 0;

  void fonts(int pct) {
    if (pct == scale && label) return;
    scale = pct; s = pct / 100.0f;
    ui = std::make_unique<FontFamily>(L"Segoe UI");
    semi = std::make_unique<FontFamily>(L"Segoe UI Semibold");
    if (!semi->IsAvailable()) semi = std::make_unique<FontFamily>(L"Segoe UI");
    label = std::make_unique<Font>(ui.get(), 10.0f * s, FontStyleBold, UnitPixel);
    value = std::make_unique<Font>(semi.get(), 13.0f * s, FontStyleRegular, UnitPixel);
    unit = std::make_unique<Font>(ui.get(), 10.0f * s, FontStyleRegular, UnitPixel);
    big = std::make_unique<Font>(ui.get(), 24.0f * s, FontStyleBold, UnitPixel);
    title = std::make_unique<Font>(ui.get(), 10.0f * s, FontStyleBold, UnitPixel);
    if (!sf) {
      sf.reset(StringFormat::GenericTypographic()->Clone());
      sf->SetFormatFlags(sf->GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);
      measureBmp = std::make_unique<Bitmap>(1, 1, PixelFormat32bppPARGB);
      measure = std::make_unique<Gdiplus::Graphics>(measureBmp.get());
      measure->SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    }
  }
  Font* font(St st) const {
    switch (st) {
      case St::Label: case St::Inline: case St::PillOn: case St::PillOff: return label.get();
      case St::Big: return big.get();
      case St::Unit: case St::Muted: return unit.get();
      case St::Title: return title.get();
      default: return value.get();
    }
  }
  float width(const std::wstring& t, St st) const {
    RectF r;
    measure->MeasureString(t.c_str(), (INT)t.size(), font(st), PointF(0, 0), sf.get(), &r);
    return r.Width + ((st == St::PillOn || st == St::PillOff) ? 10.0f * s : 0.0f);
  }
  bool canvas(int w, int h) {
    if (w <= capW && h <= capH && dc) return true;
    release();
    capW = std::max(w, capW); capH = std::max(h, capH);
    HDC screen = GetDC(nullptr);
    dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = capW;
    bi.bmiHeader.biHeight = -capH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!dc || !bmp) { release(); return false; }
    oldBmp = (HBITMAP)SelectObject(dc, bmp);
    return true;
  }
  void release() {
    if (dc && oldBmp) SelectObject(dc, oldBmp);
    if (bmp) DeleteObject(bmp);
    if (dc) DeleteDC(dc);
    dc = nullptr; bmp = oldBmp = nullptr; bits = nullptr; capW = capH = 0;
  }
  void reset() {
    release();
    measure.reset(); measureBmp.reset(); sf.reset();
    label.reset(); value.reset(); unit.reset(); big.reset(); title.reset();
    ui.reset(); semi.reset(); scale = 0;
  }
};

void addPath(GraphicsPath& p, RectF r, float rad) {
  float d = rad * 2;
  if (d <= 0.5f) { p.AddRectangle(r); return; }
  p.AddArc(r.X, r.Y, d, d, 180, 90);
  p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
  p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
  p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
  p.CloseFigure();
}

std::vector<Row> buildRows(const OverlayHudConfig& c, const View& v, bool showCost) {
  std::vector<Row> rows;
  auto row = [&](std::initializer_list<Seg> segs) { rows.push_back({ Row::Text, segs }); };
  auto num0 = [](float x) { return fmt(L"%.0f", x); };
  auto num1 = [](float x) { return fmt(L"%.1f", x); };
  const bool horizontal = c.layout == 1;
  if (c.title) rows.push_back({ Row::Title, { { L"NRLIVE", St::Title } } });
  if (c.fps) {
    Row r{ Row::Text, { { L"FPS", St::Label }, { num0(v.outFps), St::Big } } };
    if (c.frametime) { r.segs.push_back({ num1(v.ftMs), St::Value }); r.segs.push_back({ L"ms", St::Unit }); }
    rows.push_back(r);
  } else if (c.frametime) {
    row({ { L"FT", St::Label }, { num1(v.ftMs), St::Value }, { L"ms", St::Unit } });
  }
  if (c.fps_detail) {
    Row r{ Row::Text, { { L"SRC", St::Label }, { num0(v.srcFps), St::Value } } };
    if (v.fg) { r.segs.push_back({ L"GEN", St::Inline }); r.segs.push_back({ num0(v.genFps), St::Value }); }
    r.segs.push_back({ L"fps", St::Unit });
    rows.push_back(r);
  }
  if (c.lows)
    row({ { L"LOW", St::Label }, { num0(v.low1), St::Value }, { L"1%", St::Unit },
          { num0(v.low01), St::Value }, { L"0.1%", St::Unit } });
  if (c.frame_timing) rows.push_back({ Row::Graph, {} });
  if (c.gpu_load || c.gpu_clock || c.gpu_temp) {
    Row r{ Row::Text, { { L"GPU", St::Label } } };
    if (c.gpu_load && v.sys.gpuLoad >= 0) { r.segs.push_back({ num0(v.sys.gpuLoad), St::Value }); r.segs.push_back({ L"%", St::Unit }); }
    if (c.gpu_clock && v.sys.gpuClockMhz > 0) { r.segs.push_back({ num0(v.sys.gpuClockMhz), St::Value }); r.segs.push_back({ L"MHz", St::Unit }); }
    if (c.gpu_temp && v.sys.gpuTempC > 0) { r.segs.push_back({ num0(v.sys.gpuTempC), St::Value }); r.segs.push_back({ L"°C", St::Unit }); }
    if (r.segs.size() == 1) r.segs.push_back({ L"n/a", St::Muted });
    rows.push_back(r);
  }
  if (c.vram) {
    if (v.sys.vramUsedGb >= 0)
      row({ { L"VRAM", St::Label }, { num1(v.sys.vramUsedGb), St::Value },
            { v.sys.vramTotalGb > 0 ? L"/ " + num1(v.sys.vramTotalGb) + L" GB" : L"GB", St::Unit } });
    else row({ { L"VRAM", St::Label }, { L"n/a", St::Muted } });
  }
  if (c.cpu_load)
    row({ { L"CPU", St::Label }, { v.sys.cpuLoad >= 0 ? num0(v.sys.cpuLoad) : L"-", St::Value }, { L"%", St::Unit } });
  if (c.ram && v.sys.ramUsedGb >= 0)
    row({ { L"RAM", St::Label }, { num1(v.sys.ramUsedGb), St::Value }, { L"/ " + num1(v.sys.ramTotalGb) + L" GB", St::Unit } });
  if (c.pipeline)
    row({ { L"FSR", v.fsr ? St::PillOn : St::PillOff }, { L"NR", v.nr ? St::PillOn : St::PillOff },
          { L"FG", v.fg ? St::PillOn : St::PillOff } });
  if (c.gpu_times) {
    Row r{ Row::Text, { { L"PASS", St::Label } } };
    const wchar_t* names[3] = { L"MV", L"NR", L"FSR" };
    for (int i = 0; i < 3; ++i)
      if (v.gpu[i] >= 0) { r.segs.push_back({ names[i], St::Inline }); r.segs.push_back({ fmt(L"%.2f", v.gpu[i]), St::Value }); }
    if (v.gpu[3] >= 0) { r.segs.push_back({ L"ALL", St::Inline }); r.segs.push_back({ fmt(L"%.2f", v.gpu[3]), St::Value }); r.segs.push_back({ L"ms", St::Unit }); }
    else r.segs.push_back({ L"-", St::Muted });
    rows.push_back(r);
  }
  if (c.latency)
    row({ { L"LAT", St::Label }, { v.lat >= 0 ? num1(v.lat) : L"-", St::Value }, { L"ms", St::Unit } });
  if (c.resolution) {
    wchar_t a[32], b[32];
    swprintf_s(a, L"%ux%u", v.cap.w, v.cap.h);
    swprintf_s(b, L"%ux%u", v.out.w, v.out.h);
    row({ { L"RES", St::Label }, { a, St::Value }, { L"→", St::Unit }, { b, St::Value } });
  }
  if (c.clock) {
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t b[16]; swprintf_s(b, L"%02d:%02d", t.wHour, t.wMinute);
    // In the vertical layout the clock sits in the title strip when there is one.
    if (horizontal || !c.title) row({ { L"TIME", St::Label }, { b, St::Value } });
    else rows.front().segs.push_back({ b, St::Title });
  }
  if (showCost) row({ { L"HUD", St::Label }, { fmt(L"%.2f", v.drawMs), St::Value }, { L"ms/draw", St::Unit } });
  return rows;
}

// ── Draw one HUD frame into the canvas, return its size ─────────────────────
SIZE draw(Res& R, const OverlayHudConfig& c, const View& v, const float* graph, int graphN) {
  const float s = R.s;
  const bool horizontal = c.layout == 1;
  const float pad = 8 * s, gap = 5 * s, lineH = 18 * s, bigH = 30 * s, titleH = 20 * s, graphH = 40 * s;
  std::vector<Row> rows = buildRows(c, v, GetEnvironmentVariableW(L"NRLIVE_HUD_COST", nullptr, 0) > 0);

  float labelW = 0;
  for (auto& r : rows)
    for (size_t i = 0; i < r.segs.size(); ++i) {
      r.segs[i].w = R.width(r.segs[i].text, r.segs[i].st);
      if (i == 0 && r.segs[i].st == St::Label) labelW = std::max(labelW, r.segs[i].w);
    }
  labelW += 8 * s;

  auto segAdvance = [&](const Seg& g, const Seg* next) {
    float adv = g.w;
    if (next) adv += (next->st == St::Unit) ? 2 * s : gap;
    return adv;
  };

  float W = 0, H = 0;
  const float sparkW = 90 * s, divW = 14 * s;
  if (horizontal) {
    float x = pad;
    for (size_t ri = 0; ri < rows.size(); ++ri) {
      auto& r = rows[ri];
      if (r.kind == Row::Graph) x += sparkW;
      else for (size_t i = 0; i < r.segs.size(); ++i) x += segAdvance(r.segs[i], i + 1 < r.segs.size() ? &r.segs[i + 1] : nullptr);
      if (ri + 1 < rows.size()) x += divW;
    }
    W = x + pad;
    H = (c.fps ? bigH : lineH) + pad;
  } else {
    float maxW = rows.size() > 1 ? 150 * s : 0; // a lone FPS row stays compact
    for (auto& r : rows) {
      if (r.kind == Row::Title) { r.h = titleH; continue; }
      if (r.kind == Row::Graph) { r.h = graphH + 4 * s; continue; }
      bool hasBig = false;
      float w = 0;
      for (size_t i = 0; i < r.segs.size(); ++i) {
        const Seg& g = r.segs[i];
        hasBig |= g.st == St::Big;
        if (i == 0 && g.st == St::Label) { w += labelW; continue; }
        w += segAdvance(g, i + 1 < r.segs.size() ? &r.segs[i + 1] : nullptr);
      }
      maxW = std::max(maxW, w);
      r.h = hasBig ? bigH : lineH;
    }
    W = maxW + pad * 2;
    H = pad;
    for (auto& r : rows) H += r.h;
    H += pad * 0.75f;
    if (!rows.empty() && rows.front().kind == Row::Title) H -= pad * 0.5f; // title strip starts at the edge
  }
  const int iw = (int)std::ceil(W), ih = (int)std::ceil(H);
  if (!R.canvas(iw, ih)) return { 0, 0 };

  // Clear the used area of the DIB (top-down, stride = capW).
  for (int y = 0; y < ih; ++y) memset((BYTE*)R.bits + (size_t)y * R.capW * 4, 0, (size_t)iw * 4);
  Bitmap bmp(R.capW, R.capH, R.capW * 4, PixelFormat32bppPARGB, (BYTE*)R.bits);
  Gdiplus::Graphics g(&bmp);
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
  g.SetPixelOffsetMode(PixelOffsetModeHalf);

  const float radius = 7 * s;
  RectF panel(0.5f, 0.5f, W - 1, H - 1);
  GraphicsPath shape;
  addPath(shape, panel, radius);
  if (c.background) {
    SolidBrush bg(argb(kPanel, (BYTE)std::lround(c.background_alpha * 255)));
    g.FillPath(&bg, &shape);
    Pen border(argb(kBorder, (BYTE)std::lround(std::min(1.0f, c.background_alpha + 0.2f) * 255)), 1.0f);
    g.DrawPath(&border, &shape);
  }

  SolidBrush shadow(Color(170, 0, 0, 0));
  auto text = [&](const std::wstring& t, St st, float x, float y, COLORREF col) {
    Font* f = R.font(st);
    if (c.text_outline) {
      const float o = std::max(1.0f, s);
      g.DrawString(t.c_str(), (INT)t.size(), f, PointF(x + o, y + o), R.sf.get(), &shadow);
    }
    SolidBrush b(argb(col));
    g.DrawString(t.c_str(), (INT)t.size(), f, PointF(x, y), R.sf.get(), &b);
  };
  auto fontH = [&](St st) { return R.font(st)->GetHeight(&g); };
  auto colorOf = [&](St st) {
    switch (st) {
      case St::Label: case St::Inline: return c.accent_color;
      case St::Unit: case St::Muted: return kMuted;
      default: return c.text_color;
    }
  };
  auto drawSeg = [&](const Seg& sg, float x, float rowY, float rowH) {
    if (sg.st == St::PillOn || sg.st == St::PillOff) {
      const float ph = 15 * s, py = rowY + (rowH - ph) / 2;
      GraphicsPath p; addPath(p, RectF(x, py, sg.w, ph), 4 * s);
      SolidBrush fill(sg.st == St::PillOn ? argb(c.accent_color) : argb(kPillOff, 230));
      g.FillPath(&fill, &p);
      const float ty = py + (ph - fontH(sg.st)) / 2;
      SolidBrush tb(sg.st == St::PillOn ? Color(255, 255, 255, 255) : argb(kMuted));
      g.DrawString(sg.text.c_str(), (INT)sg.text.size(), R.font(sg.st), PointF(x + 5 * s, ty), R.sf.get(), &tb);
      return;
    }
    // Baseline-ish alignment: smaller fonts sit low next to the big number.
    const float fh = fontH(sg.st);
    float y = rowY + (rowH - fh) / 2;
    if (rowH > lineH + 1 && sg.st != St::Big) y = rowY + rowH - fh - 4 * s;
    text(sg.text, sg.st, x, y, colorOf(sg.st));
  };
  auto drawGraph = [&](float x, float y, float w, float h, bool labels) {
    SolidBrush well(Color(90, 0, 0, 0));
    g.FillRectangle(&well, x, y, w, h);
    if (graphN < 2) return;
    float mx = 0, mn = 1e9f;
    for (int i = 0; i < graphN; ++i) { mx = std::max(mx, graph[i]); mn = std::min(mn, graph[i]); }
    // Keep the typical frame near the middle, like MangoHud, and spikes visible.
    float avg = 0;
    for (int i = 0; i < graphN; ++i) avg += graph[i];
    avg /= graphN;
    const float top = std::max({ 8.0f, mx * 1.1f, avg * 2.0f });
    std::vector<PointF> pts;
    pts.reserve(graphN + 2);
    for (int i = 0; i < graphN; ++i)
      pts.emplace_back(x + w * i / (float)(graphN - 1), y + h - std::min(h, h * graph[i] / top));
    std::vector<PointF> poly = pts;
    poly.emplace_back(x + w, y + h);
    poly.emplace_back(x, y + h);
    SolidBrush fill(argb(c.graph_color, 55));
    g.FillPolygon(&fill, poly.data(), (INT)poly.size());
    Pen line(argb(c.graph_color), std::max(1.0f, 1.4f * s));
    line.SetLineJoin(LineJoinRound);
    g.DrawLines(&line, pts.data(), (INT)pts.size());
    if (labels) {
      text(fmt(L"%.1f ms", mx), St::Muted, x + 3 * s, y + 1 * s, kMuted);
      const std::wstring m = fmt(L"min %.1f", mn);
      text(m, St::Muted, x + 3 * s, y + h - fontH(St::Muted) - 1 * s, kMuted);
    }
  };

  if (horizontal) {
    const float rowY = pad / 2, rowH = H - pad;
    float x = pad;
    for (size_t ri = 0; ri < rows.size(); ++ri) {
      auto& r = rows[ri];
      if (r.kind == Row::Graph) {
        drawGraph(x, rowY + 3 * s, sparkW, rowH - 6 * s, false);
        x += sparkW;
      } else {
        for (size_t i = 0; i < r.segs.size(); ++i) {
          Seg sg = r.segs[i];
          if (sg.st == St::Title) sg.st = St::Label;
          drawSeg(sg, x, rowY, rowH);
          x += segAdvance(r.segs[i], i + 1 < r.segs.size() ? &r.segs[i + 1] : nullptr);
        }
      }
      if (ri + 1 < rows.size()) {
        Pen div(argb(kMuted, 90), 1.0f);
        g.DrawLine(&div, x + divW / 2, rowY + 5 * s, x + divW / 2, rowY + rowH - 5 * s);
        x += divW;
      }
    }
  } else {
    float y = pad;
    for (auto& r : rows) {
      if (r.kind == Row::Title) {
        y = 0;
        g.SetClip(&shape);
        LinearGradientBrush tb(PointF(0, 0), PointF(W, 0), argb(kTitleA, 245), argb(kTitleB, 245));
        g.FillRectangle(&tb, 0.0f, 0.0f, W, r.h);
        g.ResetClip();
        SolidBrush stripe(argb(c.accent_color));
        g.FillRectangle(&stripe, 0.0f, r.h - std::max(1.0f, s), W, std::max(1.0f, s));
        const float ty = (r.h - fontH(St::Title)) / 2;
        text(r.segs[0].text, St::Title, pad, ty, RGB(255, 255, 255));
        if (r.segs.size() > 1) {
          const auto& t = r.segs[1];
          text(t.text, St::Title, W - pad - t.w, ty, RGB(255, 225, 225));
        }
        y = r.h + pad * 0.5f;
        continue;
      }
      if (r.kind == Row::Graph) {
        drawGraph(pad, y + 2 * s, W - pad * 2, r.h - 4 * s, true);
        y += r.h;
        continue;
      }
      float x = pad;
      for (size_t i = 0; i < r.segs.size(); ++i) {
        const Seg& sg = r.segs[i];
        drawSeg(sg, x, y, r.h);
        if (i == 0 && sg.st == St::Label) x += labelW;
        else x += segAdvance(sg, i + 1 < r.segs.size() ? &r.segs[i + 1] : nullptr);
      }
      y += r.h;
    }
  }
  return { iw, ih };
}

// ── HUD thread ──────────────────────────────────────────────────────────
LRESULT CALLBACK hudProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_NCHITTEST) return HTTRANSPARENT;
  if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  if (m == WM_ERASEBKGND) return 1;
  return DefWindowProcW(h, m, w, l);
}

POINT placement(const OverlayHudConfig& c, SIZE sz, float s) {
  RECT o{};
  GetWindowRect(g_output, &o);
  const int m = (int)std::lround(12 * s);
  POINT p{ o.left + m, o.top + m };
  const int right = o.right - sz.cx - m, bottom = o.bottom - sz.cy - m;
  const int centre = o.left + ((o.right - o.left) - sz.cx) / 2;
  switch (c.position) {
    case 1: p.x = right; break;
    case 2: p.y = bottom; break;
    case 3: p.x = right; p.y = bottom; break;
    case 4: p.x = centre; break;
    case 5: p.x = centre; p.y = bottom; break;
  }
  return p;
}

void threadMain(HANDLE ready) {
  ULONG_PTR gdipToken = 0;
  GdiplusStartupInput gin;
  GdiplusStartup(&gdipToken, &gin, nullptr);
  HWND hud = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
    kHudClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_inst, nullptr);
  g_hud = hud;
  SetEvent(ready);
  if (!hud) { GdiplusShutdown(gdipToken); return; }

  Res R;
  OverlayHudConfig cfg;
  View view;
  std::vector<float> graph, lowsBuf;
  bool shown = false;
  LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
  ULONGLONG lastRefresh = 0, lastDraw = 0;
  double drawMsEma = 0;

  while (!g_quit) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) g_quit = true;
      TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    if (g_quit) break;

    const ULONGLONG now = GetTickCount64();
    bool enabled, presentation, topmost, excludeDirty, exclude, cfgChanged;
    bool refresh = false;
    {
      std::lock_guard<std::mutex> lock(g_m);
      cfgChanged = g_s.cfgDirty;
      if (cfgChanged) { cfg = g_s.cfg; g_s.cfgDirty = false; }
      enabled = g_s.enabled; presentation = g_s.presentation;
      topmost = g_s.topmost; g_s.topmost = false;
      excludeDirty = g_s.excludeDirty; exclude = g_s.exclude; g_s.excludeDirty = false;
      if (now - lastRefresh >= (ULONGLONG)cfg.sample_ms || cfgChanged) {
        refresh = true;
        lastRefresh = now;
        if (g_s.frames > 0 && g_s.sumMs > 0) {
          view.ftMs = (float)(g_s.sumMs / g_s.frames);
          view.srcFps = (float)(1000.0 * g_s.frames / g_s.sumMs);
        } else if (!cfgChanged) {
          view.srcFps = 0;
        }
        view.lat = g_s.latN ? (float)(g_s.latSum / g_s.latN) : -1;
        for (int i = 0; i < 4; ++i) view.gpu[i] = g_s.gpuN[i] ? (float)(g_s.gpuSum[i] / g_s.gpuN[i]) : -1;
        view.fsr = g_s.fsr; view.nr = g_s.nr; view.fg = g_s.fg;
        view.cap = g_s.cap; view.out = g_s.out;
        g_s.sumMs = 0; g_s.frames = 0; g_s.latSum = 0; g_s.latN = 0;
        for (int i = 0; i < 4; ++i) { g_s.gpuSum[i] = 0; g_s.gpuN[i] = 0; }
        if (cfg.lows) {
          lowsBuf.resize(g_s.ringCount);
          for (int i = 0; i < g_s.ringCount; ++i) lowsBuf[i] = g_s.ring[(g_s.ringIdx - 1 - i + kRing) % kRing];
        }
      }
      if (cfg.frame_timing) {
        const int n = std::min(cfg.graph_samples, g_s.ringCount);
        graph.resize(n);
        for (int i = 0; i < n; ++i) graph[i] = g_s.ring[(g_s.ringIdx - n + i + kRing) % kRing];
      }
    }

    const bool want = enabled && presentation && IsWindowVisible(g_output);
    if (excludeDirty) SetWindowDisplayAffinity(hud, exclude ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);

    SysMonWants sw;
    if (want) {
      sw.gpuLoad = cfg.gpu_load; sw.gpuClock = cfg.gpu_clock; sw.gpuTemp = cfg.gpu_temp;
      sw.vram = cfg.vram; sw.cpu = cfg.cpu_load; sw.ram = cfg.ram;
    }
    g_sys.configure(sw, cfg.sample_ms);

    if (!want) {
      if (shown) { ShowWindow(hud, SW_HIDE); shown = false; }
    } else {
      if (refresh) {
        view.fg = view.fg && view.srcFps > 0;
        view.outFps = view.srcFps * (view.fg ? 2.0f : 1.0f);
        view.genFps = view.fg ? view.srcFps : 0;
        if (cfg.lows) {
          view.low1 = percentileLowFps(lowsBuf, 0.01f);
          view.low01 = percentileLowFps(lowsBuf, 0.001f);
        }
        view.sys = g_sys.snapshot();
      }
      if (refresh || !shown || (cfg.frame_timing && now - lastDraw >= kGraphFrameMs)) {
        lastDraw = now;
        LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
        // Scale is relative to a 96 DPI monitor, like the rest of Windows.
        const UINT dpi = GetDpiForWindow(g_output);
        R.fonts(cfg.scale * (int)(dpi ? dpi : 96) / 96);
        view.drawMs = (float)drawMsEma;
        SIZE sz = draw(R, cfg, view, graph.data(), (int)graph.size());
        if (sz.cx > 0) {
          POINT dst = placement(cfg, sz, R.s), src{ 0, 0 };
          BLENDFUNCTION blend{ AC_SRC_OVER, 0, (BYTE)std::lround(cfg.opacity * 255), AC_SRC_ALPHA };
          UpdateLayeredWindow(hud, nullptr, &dst, &sz, R.dc, &src, 0, &blend, ULW_ALPHA);
          if (!shown) { SetWindowPos(hud, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW); shown = true; }
        }
        QueryPerformanceCounter(&t1);
        const double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
        drawMsEma = drawMsEma ? drawMsEma * 0.9 + ms * 0.1 : ms;
      }
      if (topmost && shown) SetWindowPos(hud, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    DWORD wait = 250;
    if (want) {
      const ULONGLONG next = lastRefresh + cfg.sample_ms;
      wait = next > now ? (DWORD)(next - now) : 1;
      if (cfg.frame_timing) wait = std::min<DWORD>(wait, kGraphFrameMs);
    }
    MsgWaitForMultipleObjects(0, nullptr, FALSE, wait, QS_ALLINPUT);
  }
  g_sys.stop();
  R.reset();
  DestroyWindow(hud);
  g_hud = nullptr;
  GdiplusShutdown(gdipToken);
}

void kick() {
  if (HWND h = g_hud.load()) PostMessageW(h, WM_HUD_KICK, 0, 0);
}

} // namespace

bool hudStart(HINSTANCE inst, HWND output) {
  if (g_thread.joinable()) return true;
  g_inst = inst; g_output = output; g_quit = false;
  WNDCLASSEXW c{ sizeof(c) };
  c.hInstance = inst; c.lpfnWndProc = hudProc; c.lpszClassName = kHudClass;
  RegisterClassExW(&c);
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_thread = std::thread(threadMain, ready);
  WaitForSingleObject(ready, 5000);
  CloseHandle(ready);
  return g_hud.load() != nullptr;
}

void hudStop() {
  if (!g_thread.joinable()) return;
  g_quit = true;
  kick();
  g_thread.join();
}

void hudSetConfig(const OverlayHudConfig& cfg) {
  { std::lock_guard<std::mutex> lock(g_m); g_s.cfg = cfg; g_s.cfgDirty = true; }
  kick();
}

void hudSetEnabled(bool enabled) {
  { std::lock_guard<std::mutex> lock(g_m); g_s.enabled = enabled; g_s.cfgDirty = true; }
  kick();
}

void hudSetPresentationVisible(bool visible) {
  { std::lock_guard<std::mutex> lock(g_m); g_s.presentation = visible; if (visible) g_s.topmost = true; }
  kick();
}

void hudSetAdapter(LUID luid) { g_sys.setAdapter(luid); }

void hudSetCaptureExcluded(bool exclude) {
  {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_s.exclude == exclude) return;
    g_s.exclude = exclude; g_s.excludeDirty = true;
  }
  kick();
}

void hudReassertTopmost() {
  { std::lock_guard<std::mutex> lock(g_m); g_s.topmost = true; }
  kick();
}

void hudPushFrame(const HudFrame& f) {
  std::lock_guard<std::mutex> lock(g_m);
  if (f.sourceMs >= 0.5f && f.sourceMs <= 1000.0f) {
    g_s.sumMs += f.sourceMs; g_s.frames++;
    g_s.ring[g_s.ringIdx] = f.sourceMs;
    g_s.ringIdx = (g_s.ringIdx + 1) % kRing;
    g_s.ringCount = std::min(g_s.ringCount + 1, kRing);
  }
  if (f.latencyMs >= 0 && f.latencyMs < 1000) { g_s.latSum += f.latencyMs; g_s.latN++; }
  const float gpu[4] = { f.gpuMotionMs, f.gpuNrMs, f.gpuFsrMs, f.gpuTotalMs };
  for (int i = 0; i < 4; ++i) if (gpu[i] >= 0) { g_s.gpuSum[i] += gpu[i]; g_s.gpuN[i]++; }
  g_s.fsr = f.fsr; g_s.nr = f.nr; g_s.fg = f.fg;
  g_s.cap = f.capture; g_s.out = f.output;
}
