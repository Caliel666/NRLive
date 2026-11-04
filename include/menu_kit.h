#pragma once
#include <windows.h>
#include <memory>

// A small immediate-mode widget set for the overlay's settings panel, drawn with
// GDI+ in the look of the OpenNR OptiScaler menu (Dear ImGui style). The panel
// runs the same layout twice: once per mouse event with no drawing (it updates
// values and returns true from the widget that changed), and once in WM_PAINT.
namespace kit {

struct Input {
  POINT mouse{ -1, -1 };
  bool down = false;     // left button held
  bool pressed = false;  // went down in this event
  bool released = false; // went up in this event
  int wheel = 0;         // WHEEL_DELTA units
};

class Panel {
public:
  Panel();
  ~Panel();

  // dc null: interaction pass. Draws into dc otherwise (double buffered by the caller).
  void begin(HDC dc, const RECT& client, float scale, const Input& in);
  void end();

  // Title bar: title, tabs on the right and a close button. Returns true when the tab changed.
  bool titleBar(const wchar_t* title, int* tab, const wchar_t* const* tabs, int count, bool* close);
  // Point (client) where the title bar should drag the window.
  bool isCaption(POINT p) const;
  void beginBody();

  // Collapsing header; returns whether it is open.
  bool section(const wchar_t* title, bool* open);
  // A smaller collapsing heading inside a section; indent(true) after it, indent(false) to end.
  bool tree(const wchar_t* title, bool* open);
  void indent(bool in);
  // Widgets until disabled(false) are grey and take no input. Their tip shows anywhere on the row.
  void disabled(bool on);
  bool checkbox(const wchar_t* label, bool* v, const wchar_t* tip = nullptr);
  bool slider(const wchar_t* label, float* v, float lo, float hi, const wchar_t* fmt, const wchar_t* tip = nullptr);
  bool slider(const wchar_t* label, int* v, int lo, int hi, const wchar_t* fmt, const wchar_t* tip = nullptr);
  // Changes *v only when the mouse is let go: for a value whose every step costs something.
  bool sliderOnRelease(const wchar_t* label, float* v, float lo, float hi, const wchar_t* fmt, const wchar_t* tip = nullptr);
  bool choice(const wchar_t* label, int* v, const wchar_t* const* names, int count, const wchar_t* tip = nullptr);
  bool stepper(const wchar_t* label, int* v, const wchar_t* const* names, int count, const wchar_t* tip = nullptr);
  bool button(const wchar_t* label, bool accent = false);
  bool swatch(const wchar_t* label, COLORREF c);
  void text(const wchar_t* s, bool dim = true);
  void status(const wchar_t* s); // fixed footer line
  // The next n widgets share one row.
  void row(int n);

  bool dragging() const { return m_active != 0; }
  // Client height that shows the whole body, from the last pass.
  int wantedHeight() const;

private:
  struct Impl;
  bool sliderImpl(const wchar_t* label, float* v, float lo, float hi, const wchar_t* fmt, const wchar_t* tip, bool onRelease);
  std::unique_ptr<Impl> m;
  int m_active = 0;
};

// The overlay's button bar: rounded buttons with a text label or an icon.
enum BarIcon { BarText = 0, BarCamera, BarGear };
struct BarButton { const wchar_t* label; BarIcon icon; bool active; };
void drawBar(HDC dc, const RECT& client, float scale, const BarButton* buttons, int count, int hot,
             int size, int gap, int pad);

} // namespace kit
