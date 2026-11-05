#include "menu_kit.h"
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>
#include <objidl.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

using namespace Gdiplus;

namespace kit {
namespace {

// OptiScaler's ApplyThemeStyle() with the OpenNR accent (0.86, 0.09, 0.12) on its dark theme.
const Color kWindow(255, 20, 20, 20);
const Color kTitle(255, 107, 23, 27);
const Color kTabBar(255, 26, 26, 26);
const Color kFrame(255, 34, 34, 34);
const Color kFrameHover(255, 69, 34, 35);
const Color kFrameActive(255, 87, 32, 35);
const Color kAccentSoft(255, 86, 23, 26);
const Color kAccentMed(255, 131, 23, 27);
const Color kAccent(255, 219, 23, 31);
const Color kAccentReadable(255, 255, 52, 60);
const Color kText(255, 237, 237, 237);
const Color kDim(255, 148, 148, 148);
const Color kBorder(255, 64, 64, 64);
const Color kTip(255, 30, 30, 30);

void roundPath(GraphicsPath& p, RectF r, float rad)
{
  rad = std::min(rad, std::min(r.Width, r.Height) / 2);
  if (rad < 0.5f) { p.AddRectangle(r); return; }
  const float d = rad * 2;
  p.AddArc(r.X, r.Y, d, d, 180, 90);
  p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
  p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
  p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
  p.CloseFigure();
}

RectF toF(const RECT& r) { return RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top)); }
bool inRect(const RECT& r, POINT p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }

} // namespace

struct Panel::Impl {
  Graphics* g = nullptr;
  HDC dc = nullptr;
  RECT client{};
  float s = 1;
  Input in;
  int seq = 0;
  // body: the scrolling area between the tab bar and the footer
  RECT body{};
  bool inBody = false;
  float scroll = 0, contentH = 0;
  int y = 0, x0 = 0, w = 0;
  int rowN = 0, rowI = 0, rowY = 0, rowH = 0;
  RECT caption{};
  std::vector<RECT> captionHoles;
  std::wstring tip;
  RECT tipAnchor{};
  bool off = false;
  bool interactionPass = false;
  bool hoverChanged = false;
  std::vector<uint8_t> eventHover;
  std::vector<uint8_t> previousHover;

  void recordHover(bool hovered)
  {
    if (interactionPass)
      eventHover.push_back(hovered ? 1u : 0u);
  }
  float drag = 0; // an on-release slider's value while it is held
  float fontScale = 0;
  std::unique_ptr<Font> font, bold, title, note;
  std::unique_ptr<Graphics> measure, gfx;
  HDC measureDc = nullptr;

  ~Impl() { gfx.reset(); measure.reset(); if (measureDc) DeleteDC(measureDc); }

  int px(float v) const { return (int)std::lround(v * s); }

  void fonts()
  {
    if (fontScale == s) return;
    fontScale = s;
    font = std::make_unique<Font>(L"Segoe UI", 13.0f * s, FontStyleRegular, UnitPixel);
    bold = std::make_unique<Font>(L"Segoe UI Semibold", 13.0f * s, FontStyleRegular, UnitPixel);
    title = std::make_unique<Font>(L"Segoe UI Semibold", 14.0f * s, FontStyleRegular, UnitPixel);
    note = std::make_unique<Font>(L"Segoe UI", 11.5f * s, FontStyleRegular, UnitPixel);
    if (!measureDc) measureDc = CreateCompatibleDC(nullptr);
    measure = std::make_unique<Graphics>(measureDc);
  }

  // Grey: everything a disabled widget draws, at less than half its alpha.
  Color ink(const Color& c) const { return off ? Color((BYTE)(c.GetA() * 0.4f), c.GetR(), c.GetG(), c.GetB()) : c; }

  void fill(RECT r, float rad, const Color& c)
  {
    if (!g) return;
    SolidBrush b(ink(c));
    GraphicsPath p; roundPath(p, toF(r), rad * s);
    g->FillPath(&b, &p);
  }

  void label(const wchar_t* t, RECT r, Font* f, const Color& c, StringAlignment align = StringAlignmentNear)
  {
    if (!g || !t || !*t) return;
    StringFormat sf;
    sf.SetAlignment(align);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush b(ink(c));
    g->DrawString(t, -1, f, toF(r), &sf, &b);
  }

  int textHeight(const wchar_t* t, Font* f, int width)
  {
    RectF out;
    StringFormat sf;
    measure->MeasureString(t, -1, f, RectF(0, 0, (REAL)width, 10000), &sf, &out);
    return (int)std::ceil(out.Height);
  }

  bool hit(const RECT& r) const
  {
    if (!inRect(r, in.mouse)) return false;
    return !inBody || inRect(body, in.mouse);
  }

  // A widget's rectangle in client space; rows split the width.
  // A row the widgets did not fill ends at the next full-width widget.
  void closeRow()
  {
    if (rowN > 0 && rowI > 0) y = rowY + rowH + px(4);
    rowN = 0;
  }

  RECT alloc(int h, bool full = false)
  {
    const int gap = px(8);
    if (full) closeRow();
    const int top = inBody ? body.top + y - (int)scroll : y;
    if (rowN > 0 && !full) {
      const int cw = (w - gap * (rowN - 1)) / rowN;
      const int x = x0 + rowI * (cw + gap);
      RECT r{ x, (inBody ? body.top + rowY - (int)scroll : rowY), x + cw, 0 };
      r.bottom = r.top + h;
      rowH = std::max(rowH, h);
      if (++rowI == rowN) { y = rowY + rowH + px(4); rowN = 0; }
      return r;
    }
    rowN = 0;
    RECT r{ x0, top, x0 + w, top + h };
    y += h + px(4);
    return r;
  }

  void info(const RECT& row, const wchar_t* t)
  {
    if (!t) return;
    const int d = px(14);
    RECT r{ row.right - d, (row.top + row.bottom - d) / 2, row.right, (row.top + row.bottom + d) / 2 };
    const bool over = hit(off ? row : r);
    recordHover(over);
    if (over) { tip = t; tipAnchor = r; }
    if (!g) return;
    Pen pen(over ? kAccentReadable : kDim, 1.2f * s);
    g->DrawEllipse(&pen, toF(r));
    label(L"i", r, note.get(), over ? kAccentReadable : kDim, StringAlignmentCenter);
  }

  // Click behaviour shared by every widget.
  bool behave(int id, const RECT& r, int& active, bool& hovered, bool& held)
  {
    if (off) { hovered = held = false; recordHover(false); return false; }
    hovered = hit(r) && (active == 0 || active == id);
    recordHover(hovered);
    bool clicked = false;
    if (!g) {
      if (in.pressed && hovered) active = id;
      clicked = in.released && active == id && hit(r);
    }
    held = active == id && in.down;
    return clicked;
  }
};

Panel::Panel() : m(std::make_unique<Impl>()) {}
Panel::~Panel() = default;

void Panel::begin(HDC dc, const RECT& client, float scale, const Input& in)
{
  m->dc = dc;
  m->client = client;
  m->s = scale;
  m->in = in;
  m->seq = 0;
  m->interactionPass = (dc == nullptr);
  if (m->interactionPass) m->eventHover.clear();
  m->inBody = false;
  m->rowN = 0;
  m->off = false;
  m->tip.clear();
  m->captionHoles.clear();
  m->fonts();
  m->x0 = client.left + m->px(12);
  m->w = client.right - client.left - m->px(24);
  m->y = client.top;
  m->gfx.reset();
  if (dc) {
    m->gfx = std::make_unique<Graphics>(dc);
    m->gfx->SetSmoothingMode(SmoothingModeAntiAlias);
    m->gfx->SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    m->gfx->SetPixelOffsetMode(PixelOffsetModeHalf);
    SolidBrush bg(kWindow);
    m->gfx->FillRectangle(&bg, toF(client));
  }
  m->g = m->gfx.get();
}

void Panel::end()
{
  auto& d = *m;
  d.closeRow();
  if (d.inBody) {
    d.contentH = (float)d.y + d.px(6);
    const float viewH = (float)(d.body.bottom - d.body.top);
    if (!d.g && d.in.wheel && inRect(d.body, d.in.mouse))
      d.scroll -= d.in.wheel / (float)WHEEL_DELTA * 60.0f * d.s;
    d.scroll = std::clamp(d.scroll, 0.0f, std::max(0.0f, d.contentH - viewH));
    if (d.g) {
      d.g->ResetClip();
      if (d.contentH > viewH) {
        const float trackH = viewH - d.px(8);
        const float thumbH = std::max(24.0f * d.s, trackH * viewH / d.contentH);
        const float t = d.scroll / (d.contentH - viewH);
        RECT thumb{ d.client.right - d.px(7), d.body.top + d.px(4) + (int)((trackH - thumbH) * t),
                    d.client.right - d.px(3), 0 };
        thumb.bottom = thumb.top + (int)thumbH;
        d.fill(thumb, 2, kAccentSoft);
      }
    }
  }
  if (d.g && !d.tip.empty()) {
    const int pad = d.px(8), width = d.px(260);
    const int h = d.textHeight(d.tip.c_str(), d.note.get(), width) + pad * 2;
    RECT r{ d.tipAnchor.right - width - pad * 2, d.tipAnchor.bottom + d.px(4), d.tipAnchor.right, 0 };
    r.bottom = r.top + h;
    if (r.bottom > d.client.bottom - d.px(4)) OffsetRect(&r, 0, d.tipAnchor.top - d.px(4) - r.bottom);
    if (r.left < d.client.left + d.px(4)) OffsetRect(&r, d.client.left + d.px(4) - r.left, 0);
    d.fill(r, 6, kTip);
    Pen pen(kBorder, 1.0f);
    GraphicsPath p; roundPath(p, toF(r), 6 * d.s);
    d.g->DrawPath(&pen, &p);
    StringFormat sf;
    SolidBrush b(kText);
    RECT tr = r; InflateRect(&tr, -pad, -pad);
    d.g->DrawString(d.tip.c_str(), -1, d.note.get(), toF(tr), &sf, &b);
  }
  if (d.interactionPass) {
    d.hoverChanged = d.eventHover != d.previousHover;
    d.previousHover = d.eventHover;
  } else {
    d.hoverChanged = false;
  }
  if (!d.g && d.in.released) m_active = 0;
  d.interactionPass = false;
  d.g = nullptr;
  d.gfx.reset();
}

bool Panel::titleBar(const wchar_t* title, int* tab, const wchar_t* const* tabs, int count, bool* close)
{
  auto& d = *m;
  const int th = d.px(32), tabH = d.px(30);
  RECT bar{ d.client.left, d.client.top, d.client.right, d.client.top + th };
  d.caption = bar;
  if (d.g) {
    SolidBrush b(kTitle);
    d.g->FillRectangle(&b, toF(bar));
    RECT t = bar; t.left += d.px(14);
    d.label(title, t, d.title.get(), kText);
  }
  // close
  RECT x{ bar.right - th, bar.top, bar.right, bar.bottom };
  d.captionHoles.push_back(x);
  bool hov, held;
  const int id = ++d.seq;
  if (d.behave(id, x, m_active, hov, held) && close) *close = true;
  if (d.g) {
    if (hov) { SolidBrush b(held ? kAccent : kAccentMed); d.g->FillRectangle(&b, toF(x)); }
    const float c = (x.left + x.right) / 2.0f, m2 = (x.top + x.bottom) / 2.0f, r = 5 * d.s;
    Pen pen(kText, 1.6f * d.s);
    d.g->DrawLine(&pen, c - r, m2 - r, c + r, m2 + r);
    d.g->DrawLine(&pen, c - r, m2 + r, c + r, m2 - r);
  }
  // tabs
  RECT strip{ d.client.left, bar.bottom, d.client.right, bar.bottom + tabH };
  if (d.g) { SolidBrush b(kTabBar); d.g->FillRectangle(&b, toF(strip)); }
  bool changed = false;
  int tx = d.client.left + d.px(10);
  for (int i = 0; i < count; ++i) {
    RectF out;
    d.measure->MeasureString(tabs[i], -1, d.bold.get(), PointF(0, 0), &out);
    RECT r{ tx, strip.top + d.px(5), tx + (int)out.Width + d.px(22), strip.bottom };
    tx = r.right + d.px(4);
    const int tid = ++d.seq;
    bool th2, tl;
    if (d.behave(tid, r, m_active, th2, tl) && *tab != i) { *tab = i; changed = true; }
    const bool sel = *tab == i;
    if (d.g) {
      RECT body = r; body.bottom += d.px(4); // square bottom: drawn past the strip, clipped below
      d.g->SetClip(toF(r));
      d.fill(body, 4, sel ? kAccentMed : (th2 ? kAccentSoft : kFrame));
      d.g->ResetClip();
      if (sel) { RECT o{ r.left + d.px(3), r.top, r.right - d.px(3), r.top + d.px(2) }; d.fill(o, 1, kAccentReadable); }
      d.label(tabs[i], r, d.bold.get(), sel ? kText : kDim, StringAlignmentCenter);
    }
  }
  if (d.g) {
    SolidBrush b(kAccentMed);
    d.g->FillRectangle(&b, RectF((REAL)strip.left, (REAL)strip.bottom - 1, (REAL)(strip.right - strip.left), 1));
  }
  d.y = strip.bottom;
  return changed;
}

int Panel::wantedHeight() const
{
  const auto& d = *m;
  return (d.body.top - d.client.top) + (int)d.contentH + d.px(24);
}

bool Panel::takeHoverChanged()
{
  const bool changed = m->hoverChanged;
  m->hoverChanged = false;
  return changed;
}

bool Panel::isCaption(POINT p) const
{
  if (!inRect(m->caption, p)) return false;
  for (const auto& h : m->captionHoles) if (inRect(h, p)) return false;
  return true;
}

void Panel::beginBody()
{
  auto& d = *m;
  d.body = { d.client.left, d.y + d.px(6), d.client.right, d.client.bottom - d.px(24) };
  d.inBody = true;
  d.y = d.px(4);
  if (d.g) d.g->SetClip(toF(d.body));
}

bool Panel::section(const wchar_t* title, bool* open)
{
  auto& d = *m;
  d.y += d.px(2);
  RECT r = d.alloc(d.px(24), true);
  bool hov, held;
  const bool clicked = d.behave(++d.seq, r, m_active, hov, held);
  if (clicked) *open = !*open;
  if (d.g) {
    d.fill(r, 4, held ? kAccent : (hov ? kAccentMed : kAccentSoft));
    const float cx = r.left + 13 * d.s, cy = (r.top + r.bottom) / 2.0f, a = 4 * d.s;
    PointF tri[3];
    if (*open) { tri[0] = { cx - a, cy - a / 2 }; tri[1] = { cx + a, cy - a / 2 }; tri[2] = { cx, cy + a * 0.8f }; }
    else { tri[0] = { cx - a / 2, cy - a }; tri[1] = { cx - a / 2, cy + a }; tri[2] = { cx + a * 0.8f, cy }; }
    SolidBrush b(kText);
    d.g->FillPolygon(&b, tri, 3);
    RECT t = r; t.left += d.px(26);
    d.label(title, t, d.bold.get(), kText);
  }
  return *open;
}

bool Panel::tree(const wchar_t* title, bool* open)
{
  auto& d = *m;
  RECT r = d.alloc(d.px(22), true);
  bool hov, held;
  if (d.behave(++d.seq, r, m_active, hov, held)) *open = !*open;
  if (d.g) {
    const float cx = r.left + 7 * d.s, cy = (r.top + r.bottom) / 2.0f, a = 3.5f * d.s;
    PointF tri[3];
    if (*open) { tri[0] = { cx - a, cy - a / 2 }; tri[1] = { cx + a, cy - a / 2 }; tri[2] = { cx, cy + a * 0.8f }; }
    else { tri[0] = { cx - a / 2, cy - a }; tri[1] = { cx - a / 2, cy + a }; tri[2] = { cx + a * 0.8f, cy }; }
    SolidBrush b(hov ? kAccentReadable : kText);
    d.g->FillPolygon(&b, tri, 3);
    RECT t = r; t.left += d.px(18);
    d.label(title, t, d.bold.get(), hov ? kAccentReadable : kText);
  }
  return *open;
}

void Panel::indent(bool in)
{
  auto& d = *m;
  d.closeRow();
  const int by = d.px(14) * (in ? 1 : -1);
  d.x0 += by;
  d.w -= by;
}

void Panel::disabled(bool on)
{
  m->closeRow();
  m->off = on;
}

bool Panel::checkbox(const wchar_t* label, bool* v, const wchar_t* tip)
{
  auto& d = *m;
  RECT row = d.alloc(d.px(22));
  RECT area = row;
  if (tip) area.right -= d.px(18);
  bool hov, held;
  const bool clicked = d.behave(++d.seq, area, m_active, hov, held);
  if (clicked) *v = !*v;
  const int bs = d.px(16);
  RECT box{ row.left, (row.top + row.bottom - bs) / 2, row.left + bs, (row.top + row.bottom + bs) / 2 };
  if (d.g) {
    d.fill(box, 3, held ? kFrameActive : (hov ? kFrameHover : kFrame));
    if (*v) {
      Pen pen(d.ink(kAccentReadable), 2.0f * d.s);
      pen.SetLineJoin(LineJoinRound);
      PointF pts[3] = { { box.left + 3.5f * d.s, box.top + 8.5f * d.s },
                        { box.left + 6.5f * d.s, box.top + 11.5f * d.s },
                        { box.left + 12.5f * d.s, box.top + 4.5f * d.s } };
      d.g->DrawLines(&pen, pts, 3);
    }
    RECT t = area; t.left = box.right + d.px(8);
    d.label(label, t, d.font.get(), kText);
  }
  d.info(row, tip);
  return clicked;
}

// Labelled widgets keep the info column so their frames line up; bare ones fill the row.
static RECT frameOf(const RECT& row, int infoW, bool hasLabel, bool hasTip)
{
  RECT f = row;
  if (hasLabel || hasTip) f.right -= infoW;
  if (hasLabel) f.right = f.left + (int)((f.right - f.left) * 0.62f);
  return f;
}

bool Panel::slider(const wchar_t* label, float* v, float lo, float hi, const wchar_t* fmt, const wchar_t* tip)
{
  return sliderImpl(label, v, lo, hi, fmt, tip, false);
}

bool Panel::sliderOnRelease(const wchar_t* label, float* v, float lo, float hi, const wchar_t* fmt, const wchar_t* tip)
{
  return sliderImpl(label, v, lo, hi, fmt, tip, true);
}

bool Panel::sliderImpl(const wchar_t* label, float* v, float lo, float hi, const wchar_t* fmt, const wchar_t* tip, bool onRelease)
{
  auto& d = *m;
  RECT row = d.alloc(d.px(22));
  RECT f = frameOf(row, d.px(18), label != nullptr, tip != nullptr);
  const int id = ++d.seq;
  bool hov, held;
  d.behave(id, f, m_active, hov, held);
  const int grab = d.px(10);
  const float old = *v;
  if (!d.g && m_active == id && d.in.down && hi > lo) {
    float t = float(d.in.mouse.x - (f.left + grab / 2)) / float(std::max(1L, f.right - f.left - grab));
    (onRelease ? d.drag : *v) = lo + std::clamp(t, 0.0f, 1.0f) * (hi - lo);
  }
  if (onRelease && !d.g && m_active == id && d.in.released) *v = d.drag;
  if (d.g) {
    const float shown = onRelease && held ? d.drag : *v;
    const float t = hi > lo ? std::clamp((shown - lo) / (hi - lo), 0.0f, 1.0f) : 0;
    d.fill(f, 4, held ? kFrameActive : (hov ? kFrameHover : kFrame));
    const int gx = f.left + (int)(t * (f.right - f.left - grab));
    RECT fillR{ f.left, f.top, gx + grab / 2, f.bottom };
    d.g->SetClip(toF(fillR), CombineModeIntersect);
    d.fill(f, 4, Color(70, 219, 23, 31));
    d.g->SetClip(toF(d.body));
    RECT gr{ gx, f.top + d.px(2), gx + grab, f.bottom - d.px(2) };
    d.fill(gr, 3, held ? kAccentReadable : kAccentMed);
    wchar_t buf[48]{};
    swprintf_s(buf, fmt, shown);
    d.label(buf, f, d.font.get(), kText, StringAlignmentCenter);
    RECT lr{ f.right + d.px(8), row.top, row.right - d.px(18), row.bottom };
    d.label(label, lr, d.font.get(), kText);
  }
  d.info(row, tip);
  return *v != old;
}

bool Panel::slider(const wchar_t* label, int* v, int lo, int hi, const wchar_t* fmt, const wchar_t* tip)
{
  float f = (float)*v;
  wchar_t intFmt[32];
  // %d formats become a float format with no decimals.
  std::wstring s = fmt;
  const size_t p = s.find(L"%d");
  if (p != std::wstring::npos) s.replace(p, 2, L"%.0f");
  wcsncpy_s(intFmt, s.c_str(), _TRUNCATE);
  slider(label, &f, (float)lo, (float)hi, intFmt, tip);
  const int n = std::clamp((int)std::lround(f), lo, hi);
  const bool changed = n != *v;
  *v = n;
  return changed;
}

bool Panel::choice(const wchar_t* label, int* v, const wchar_t* const* names, int count, const wchar_t* tip)
{
  auto& d = *m;
  RECT row = d.alloc(d.px(22));
  RECT f = frameOf(row, d.px(18), label != nullptr, tip != nullptr);
  bool changed = false;
  const int gap = d.px(2);
  const int cw = (f.right - f.left - gap * (count - 1)) / count;
  for (int i = 0; i < count; ++i) {
    RECT r{ f.left + i * (cw + gap), f.top, f.left + i * (cw + gap) + cw, f.bottom };
    if (i == count - 1) r.right = f.right;
    bool hov, held;
    if (d.behave(++d.seq, r, m_active, hov, held) && *v != i) { *v = i; changed = true; }
    const bool sel = *v == i;
    if (d.g) {
      d.fill(r, 4, sel ? (hov ? kAccent : kAccentMed) : (held ? kFrameActive : (hov ? kFrameHover : kFrame)));
      d.label(names[i], r, d.font.get(), sel ? kText : kDim, StringAlignmentCenter);
    }
  }
  if (d.g && label) {
    RECT t{ f.right + d.px(8), row.top, row.right - d.px(18), row.bottom };
    d.label(label, t, d.font.get(), kText);
  }
  d.info(row, tip);
  return changed;
}

bool Panel::stepper(const wchar_t* label, int* v, const wchar_t* const* names, int count, const wchar_t* tip)
{
  auto& d = *m;
  RECT row = d.alloc(d.px(22));
  RECT f = frameOf(row, d.px(18), label != nullptr, tip != nullptr);
  const int aw = d.px(22);
  RECT l{ f.left, f.top, f.left + aw, f.bottom }, r{ f.right - aw, f.top, f.right, f.bottom };
  bool lh, lhd, rh, rhd;
  const int old = *v;
  if (d.behave(++d.seq, l, m_active, lh, lhd)) *v = (*v + count - 1) % count;
  if (d.behave(++d.seq, r, m_active, rh, rhd)) *v = (*v + 1) % count;
  if (d.g) {
    d.fill(f, 4, kFrame);
    d.fill(l, 4, lhd ? kAccent : (lh ? kAccentMed : kAccentSoft));
    d.fill(r, 4, rhd ? kAccent : (rh ? kAccentMed : kAccentSoft));
    SolidBrush ink(d.ink(kText));
    const float a = 3.5f * d.s, ly = (l.top + l.bottom) / 2.0f;
    const float lx = (l.left + l.right) / 2.0f, rx = (r.left + r.right) / 2.0f;
    PointF lt[3] = { { lx + a * 0.6f, ly - a }, { lx + a * 0.6f, ly + a }, { lx - a * 0.8f, ly } };
    PointF rt[3] = { { rx - a * 0.6f, ly - a }, { rx - a * 0.6f, ly + a }, { rx + a * 0.8f, ly } };
    d.g->FillPolygon(&ink, lt, 3);
    d.g->FillPolygon(&ink, rt, 3);
    RECT mid{ l.right, f.top, r.left, f.bottom };
    d.label(names[std::clamp(*v, 0, count - 1)], mid, d.font.get(), kText, StringAlignmentCenter);
    if (label) {
      RECT t{ f.right + d.px(8), row.top, row.right - d.px(18), row.bottom };
      d.label(label, t, d.font.get(), kText);
    }
  }
  d.info(row, tip);
  return *v != old;
}

bool Panel::button(const wchar_t* label, bool accent)
{
  auto& d = *m;
  RECT r = d.alloc(d.px(24));
  bool hov, held;
  const bool clicked = d.behave(++d.seq, r, m_active, hov, held);
  if (d.g) {
    d.fill(r, 4, held ? kAccent : (hov ? kAccentMed : (accent ? kAccentSoft : kFrame)));
    d.label(label, r, d.font.get(), kText, StringAlignmentCenter);
  }
  return clicked;
}

bool Panel::swatch(const wchar_t* label, COLORREF c)
{
  auto& d = *m;
  RECT r = d.alloc(d.px(24));
  bool hov, held;
  const bool clicked = d.behave(++d.seq, r, m_active, hov, held);
  if (d.g) {
    d.fill(r, 4, held ? kFrameActive : (hov ? kFrameHover : kFrame));
    RECT sw{ r.left + d.px(4), r.top + d.px(4), r.left + d.px(30), r.bottom - d.px(4) };
    d.fill(sw, 3, Color(255, GetRValue(c), GetGValue(c), GetBValue(c)));
    RECT t = r; t.left = sw.right + d.px(8);
    d.label(label, t, d.font.get(), kText);
  }
  return clicked;
}

void Panel::text(const wchar_t* s, bool dim)
{
  auto& d = *m;
  if (!s || !*s) return;
  const int h = d.textHeight(s, d.note.get(), d.w) + d.px(2);
  RECT r = d.alloc(h, true);
  if (d.g) {
    StringFormat sf;
    SolidBrush b(d.ink(dim ? kDim : kText));
    d.g->DrawString(s, -1, d.note.get(), toF(r), &sf, &b);
  }
}

void Panel::status(const wchar_t* s)
{
  auto& d = *m;
  if (!d.g) return;
  d.g->ResetClip();
  RECT r{ d.client.left, d.client.bottom - d.px(24), d.client.right, d.client.bottom };
  SolidBrush b(kTabBar);
  d.g->FillRectangle(&b, toF(r));
  SolidBrush line(kBorder);
  d.g->FillRectangle(&line, RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), 1));
  r.left += d.px(12); r.right -= d.px(12);
  d.label(s, r, d.note.get(), kDim);
  if (d.inBody) d.g->SetClip(toF(d.body));
}

void Panel::row(int n)
{
  auto& d = *m;
  d.closeRow();
  d.rowN = n;
  d.rowI = 0;
  d.rowY = d.y;
  d.rowH = 0;
}

void drawBar(HDC dc, const RECT& client, float s, const BarButton* buttons, int count, int hot,
             int size, int gap, int pad)
{
  Graphics g(dc);
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
  g.SetPixelOffsetMode(PixelOffsetModeHalf);
  SolidBrush bg(kWindow);
  g.FillRectangle(&bg, toF(client));
  Font font(L"Segoe UI Semibold", 12.0f * s, FontStyleRegular, UnitPixel);
  StringFormat sf;
  sf.SetAlignment(StringAlignmentCenter);
  sf.SetLineAlignment(StringAlignmentCenter);
  for (int i = 0; i < count; ++i) {
    const auto& b = buttons[i];
    RECT r{ pad + i * (size + gap), pad, pad + i * (size + gap) + size, pad + size };
    const bool over = hot == i;
    const Color fillC = b.active ? (over ? kAccentMed : kAccentSoft) : (over ? kFrameHover : kFrame);
    SolidBrush fb(fillC);
    GraphicsPath p; roundPath(p, toF(r), 6 * s);
    g.FillPath(&fb, &p);
    const Color ink = b.active ? kAccentReadable : (over ? kText : kDim);
    if (b.active) {
      SolidBrush ab(kAccentReadable);
      GraphicsPath u; roundPath(u, RectF(r.left + 14 * s, r.bottom - 5 * s, size - 28 * s, 2.5f * s), 1.2f * s);
      g.FillPath(&ab, &u);
    }
    const float cx = (r.left + r.right) / 2.0f, cy = (r.top + r.bottom) / 2.0f;
    Pen pen(ink, 1.8f * s);
    pen.SetLineJoin(LineJoinRound);
    if (b.icon == BarCamera) {
      GraphicsPath body; roundPath(body, RectF(cx - 13 * s, cy - 7 * s, 26 * s, 17 * s), 4 * s);
      g.DrawPath(&pen, &body);
      g.DrawEllipse(&pen, RectF(cx - 5 * s, cy - 3 * s, 10 * s, 10 * s));
      g.DrawLine(&pen, cx - 5 * s, cy - 7 * s, cx - 3 * s, cy - 11 * s);
      g.DrawLine(&pen, cx - 3 * s, cy - 11 * s, cx + 3 * s, cy - 11 * s);
      g.DrawLine(&pen, cx + 3 * s, cy - 11 * s, cx + 5 * s, cy - 7 * s);
    } else if (b.icon == BarGear) {
      g.DrawEllipse(&pen, RectF(cx - 5 * s, cy - 5 * s, 10 * s, 10 * s));
      for (int k = 0; k < 8; ++k) {
        const float a = k * 3.14159265f / 4;
        g.DrawLine(&pen, cx + std::cos(a) * 8 * s, cy + std::sin(a) * 8 * s, cx + std::cos(a) * 12 * s,
                   cy + std::sin(a) * 12 * s);
      }
    } else {
      SolidBrush tb(ink);
      g.DrawString(b.label, -1, &font, toF(r), &sf, &tb);
    }
  }
}

} // namespace kit
