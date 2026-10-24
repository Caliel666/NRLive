#include "target.h"
#include <tlhelp32.h>
#include <dwmapi.h>
#include <algorithm>
#include <vector>
#include <cwctype>
#include <regex>

namespace {

std::wstring lower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return (wchar_t)::towlower(c); });
    return s;
}

bool isOwnWindow(HWND h)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

bool isCloaked(HWND h)
{
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        return cloaked != FALSE;
    return false;
}

bool isCandidate(HWND h)
{
    if (!h || !IsWindow(h) || isOwnWindow(h) || isCloaked(h)) return false;
    if (!IsWindowVisible(h)) return false;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return false;
    RECT r{};
    if (!GetWindowRect(h, &r)) return false;
    return (r.right - r.left) > 0 && (r.bottom - r.top) > 0;
}

std::wstring windowTitle(HWND h)
{
    wchar_t buf[512]{};
    GetWindowTextW(h, buf, 512);
    return buf;
}

std::wstring processName(DWORD pid)
{
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};
    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(proc, 0, path, &size)) {
        std::wstring full(path);
        name = full.substr(full.find_last_of(L'\\') + 1);
    }
    CloseHandle(proc);
    return name;
}

std::vector<HWND> topLevelWindows()
{
    std::vector<HWND> result;
    EnumWindows([](HWND h, LPARAM p) -> BOOL {
        if (isCandidate(h)) ((std::vector<HWND>*)p)->push_back(h);
        return TRUE;
    }, (LPARAM)&result);
    return result;
}

int64_t areaOf(HWND h)
{
    RECT r{};
    GetWindowRect(h, &r);
    return (int64_t)(r.right - r.left) * (r.bottom - r.top);
}

// Prefers windows that actually carry a title, then the largest one.
HWND bestForPid(DWORD pid)
{
    HWND best = nullptr;
    int64_t bestArea = -1;
    for (HWND h : topLevelWindows()) {
        DWORD wpid = 0;
        GetWindowThreadProcessId(h, &wpid);
        if (wpid != pid) continue;
        int64_t a = areaOf(h);
        bool titled = !windowTitle(h).empty();
        if (!best || (titled && a > bestArea)) { best = h; bestArea = a; }
    }
    return best;
}

bool regexSearch(const std::wstring& pattern, const std::wstring& text)
{
    try {
        const std::wregex re(pattern, std::regex_constants::ECMAScript | std::regex_constants::icase);
        return std::regex_search(text, re);
    } catch (const std::regex_error&) {
        // A malformed regex should not make the target unusable. Fall back to
        // the same case-insensitive substring behaviour used by older builds.
        return lower(text).find(lower(pattern)) != std::wstring::npos;
    }
}

HWND findByTitle(const std::wstring& pattern)
{
    HWND exact = nullptr, match = nullptr;
    for (HWND h : topLevelWindows()) {
        std::wstring t = windowTitle(h);
        if (t.empty()) continue;
        if (lower(t) == lower(pattern)) {
            if (!exact) exact = h;
        } else if (!match && regexSearch(pattern, t)) {
            match = h;
        }
    }
    return exact ? exact : match;
}

DWORD findPidByProcessName(const std::wstring& pattern)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD found = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == 0 || pe.th32ProcessID == GetCurrentProcessId()) continue;
            const std::wstring name = pe.szExeFile;
            if (regexSearch(pattern, name)) {
                found = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

bool parseHotkey(const std::wstring& text, UINT& modifiers, UINT& vk)
{
    modifiers = 0;
    vk = 0;
    std::wstring key;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'+', start);
        std::wstring part = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        part.erase(std::remove_if(part.begin(), part.end(), [](wchar_t c) { return iswspace(c) != 0; }), part.end());
        part = lower(part);
        if (part.empty()) return false;

        if (part == L"ctrl" || part == L"control") modifiers |= MOD_CONTROL;
        else if (part == L"shift") modifiers |= MOD_SHIFT;
        else if (part == L"alt") modifiers |= MOD_ALT;
        else if (part == L"win" || part == L"windows" || part == L"meta") modifiers |= MOD_WIN;
        else {
            if (!key.empty()) return false;
            key = part;
        }

        if (end == std::wstring::npos) break;
        start = end + 1;
    }

    if (key.empty()) return false;

    if (key.size() == 1) {
        wchar_t c = key[0];
        if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9')) {
            vk = (UINT)towupper(c);
            return true;
        }
    }

    static const std::pair<const wchar_t*, UINT> named[] = {
        {L"esc", VK_ESCAPE}, {L"escape", VK_ESCAPE}, {L"enter", VK_RETURN},
        {L"return", VK_RETURN}, {L"space", VK_SPACE}, {L"tab", VK_TAB},
        {L"backspace", VK_BACK}, {L"delete", VK_DELETE}, {L"del", VK_DELETE},
        {L"insert", VK_INSERT}, {L"home", VK_HOME}, {L"end", VK_END},
        {L"pageup", VK_PRIOR}, {L"pagedown", VK_NEXT}, {L"up", VK_UP},
        {L"down", VK_DOWN}, {L"left", VK_LEFT}, {L"right", VK_RIGHT},
        {L"pause", VK_PAUSE}, {L"printscreen", VK_SNAPSHOT},
        {L"capslock", VK_CAPITAL}, {L"numlock", VK_NUMLOCK}, {L"scrolllock", VK_SCROLL}
    };
    for (const auto& n : named)
        if (key == n.first) { vk = n.second; return true; }

    if (key.size() >= 2 && key[0] == L'f') {
        wchar_t* end = nullptr;
        unsigned long n = wcstoul(key.c_str() + 1, &end, 10);
        if (end && *end == L'\0' && n >= 1 && n <= 24) {
            vk = VK_F1 + (UINT)(n - 1);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- picker ---
//
// A small always-on-top panel that never takes focus. The user keeps full
// keyboard and mouse control, so they can alt-tab or click the window they
// want; whatever is in the foreground when the countdown ends is captured.

const wchar_t* PICKER_CLASS = L"NRLiveTargetPicker";

struct PickerState {
    HWND result = nullptr;
    int secondsLeft = 5;
    int tick = 0;
};

PickerState g_picker{};

LRESULT CALLBACK pickerProc(HWND h, UINT msg, WPARAM w, LPARAM lParam)
{
    switch (msg) {
    case WM_TIMER: {
        if (w != 1) return 0;
        g_picker.tick++;

        HWND front = GetForegroundWindow();
        if (isCandidate(front)) g_picker.result = front;

        if (g_picker.tick % 10 == 0 && g_picker.secondsLeft > 0) {
            g_picker.secondsLeft--;
            InvalidateRect(h, nullptr, FALSE);
        }
        if (g_picker.secondsLeft <= 0) {
            KillTimer(h, 1);
            DestroyWindow(h);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        RECT rc{};
        GetClientRect(h, &rc);

        HBRUSH bg = CreateSolidBrush(RGB(16, 16, 16));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);

        HPEN edge = CreatePen(PS_SOLID, 1, RGB(0, 180, 255));
        HBRUSH oldBrush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
        HPEN oldPen = (HPEN)SelectObject(dc, edge);
        Rectangle(dc, 0, 0, rc.right, rc.bottom);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(edge);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(235, 235, 235));

        wchar_t line[160]{};
        swprintf_s(line, L"NRLive: capturing in %d s\nBring the window to capture to the front.",
                   g_picker.secondsLeft);
        DrawTextW(dc, line, -1, &rc, DT_CENTER | DT_VCENTER | DT_WORDBREAK);

        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, lParam);
}

HWND runPicker(HINSTANCE inst, int seconds)
{
    g_picker = {};
    g_picker.secondsLeft = seconds;

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = inst;
    wc.lpfnWndProc = pickerProc;
    wc.lpszClassName = PICKER_CLASS;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    const int w = 340, h = 84;
    int x = GetSystemMetrics(SM_CXSCREEN) - w - 24;
    int y = 24;

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                PICKER_CLASS, L"", WS_POPUP,
                                x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return nullptr;

    // The launcher owns the visible countdown. Keep this window hidden while
    // silently tracking the foreground target so NRLive itself shows no picker HUD.
    SetTimer(hwnd, 1, 100, nullptr);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return g_picker.result;
}

}

HWND resolveFrontWindow()
{
    HWND h = GetForegroundWindow();
    if (!h || !IsWindow(h)) return nullptr;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return nullptr;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return nullptr;
    if (!IsWindowVisible(h)) return nullptr;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return nullptr;
    RECT r{};
    if (!GetWindowRect(h, &r)) return nullptr;
    if ((r.right - r.left) <= 0 || (r.bottom - r.top) <= 0) return nullptr;
    return h;
}

std::wstring targetUsage()
{
    return
        L"NRLive - capture and upscale a window with FSR 3\n\n"
        L"  NRLive.exe                    picker, hidden launcher countdown\n"
        L"  NRLive.exe -pid <id>          capture the window owned by PID\n"
        L"  NRLive.exe -pname <regex>     capture a process whose name matches regex\n"
        L"  NRLive.exe -window <regex>    capture a window title matching regex\n"
        L"  NRLive.exe -front             continuously follow the foreground window\n"
        L"  NRLive.exe -delay <sec>       wait before starting capture\n"
        L"  NRLive.exe -nooverlay         disable the NRLive HUD overlay\n"
        L"  NRLive.exe --key ctrl+shift+a set the global stop hotkey\n"
        L"  NRLive.exe --mv amdof|fast    select motion-vector implementation (default: fast)\n"
        L"  NRLive.exe --upscaler fsr4|fsr31|none\n"
        L"                                  upscaler after capture and DLSSNR (default: fsr4;\n"
        L"                                  none stretches the frame to the output)\n"
        L"  NRLive.exe --bindbypass home,insert,end,pageup,pagedown\n"
        L"                                  OptiScaler/ReShade menu keys (not sent to game).\n"
        L"                                  Default: Home,Insert,End,PageUp,PageDown.\n"
        L"  NRLive.exe --overlaykey ctrl+home\n"
        L"  NRLive.exe --trace frame-trace.csv\n"
        L"                                  Write per-frame CPU/capture/present timing to CSV.\n"
        L"                                  Toggle NRLive HUD overlay (Steam-style).\n"
        L"                                  Default: Ctrl+Home. When open, mouse is held\n"
        L"                                  by NRLive and not forwarded to the game.\n"
        L"  NRLive.exe -help              this message\n";
}


static void setDefaultBindBypass(TargetSpec& spec)
{
    if (spec.bindBypassExplicit) return;
    spec.bindBypass.clear();
    const UINT defs[] = { VK_HOME, VK_INSERT, VK_END, VK_PRIOR, VK_NEXT };
    for (UINT vk : defs)
        spec.bindBypass.push_back(TargetSpec::BypassKey{ 0, vk });
    spec.bindBypassText = L"Home,Insert,End,PageUp,PageDown";
}

static bool parseBypassList(const std::wstring& text, TargetSpec& spec, std::wstring& error)
{
    spec.bindBypass.clear();
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L',', start);
        std::wstring part = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        // trim
        while (!part.empty() && iswspace(part.front())) part.erase(part.begin());
        while (!part.empty() && iswspace(part.back())) part.pop_back();
        if (!part.empty()) {
            UINT mods = 0, vk = 0;
            if (!parseHotkey(part, mods, vk)) {
                error = L"invalid --bindbypass entry: " + part;
                return false;
            }
            spec.bindBypass.push_back(TargetSpec::BypassKey{ mods, vk });
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    if (spec.bindBypass.empty()) {
        error = L"--bindbypass requires at least one key (e.g. home,insert)";
        return false;
    }
    spec.bindBypassText = text;
    spec.bindBypassExplicit = true;
    return true;
}

bool parseTargetArgs(int argc, wchar_t** argv, TargetSpec& spec, std::wstring& error)
{
    setDefaultBindBypass(spec);

    bool explicitTarget = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        std::wstring opt = lower(arg);
        auto need = [&](std::wstring& out) -> bool {
            if (i + 1 >= argc) { error = L"missing value for " + arg; return false; }
            out = argv[++i];
            return true;
        };
        std::wstring value;

        if (opt == L"--help" || opt == L"-help" || opt == L"-h" || opt == L"/?") {
            spec.help = true;
        } else if (opt == L"--pid" || opt == L"-pid") {
            if (!need(value)) return false;
            spec.mode = TargetMode::Pid;
            spec.pid = wcstoul(value.c_str(), nullptr, 10);
            if (spec.pid == 0) { error = L"invalid pid: " + value; return false; }
            explicitTarget = true;
        } else if (opt == L"--window" || opt == L"-window" || opt == L"--title") {
            if (!need(value)) return false;
            spec.mode = TargetMode::Window;
            spec.text = value;
            explicitTarget = true;
        } else if (opt == L"--pname" || opt == L"-pname" || opt == L"--process" || opt == L"-process" || opt == L"--exe" || opt == L"-exe") {
            if (!need(value)) return false;
            spec.mode = TargetMode::Process;
            spec.text = value;
            explicitTarget = true;
        } else if (opt == L"--capture" || opt == L"-capture") {
            if (!need(value)) return false;
            std::wstring mode = lower(value);
            if (mode == L"dxgi" || mode == L"window") spec.captureMode = TargetSpec::CaptureMode::DxgiWindow;
            else if (mode == L"wgc") spec.captureMode = TargetSpec::CaptureMode::WgcWindow;
            else { error = L"invalid capture mode: " + value + L" (valid: dxgi, wgc)"; return false; }
        } else if (opt == L"--mv" || opt == L"-mv") {
            if (!need(value)) return false;
            std::wstring mv = lower(value);
            if (mv == L"amdof") {
                spec.motionMode = TargetSpec::MotionMode::AmdOf;
                spec.motionModeText = L"amdof";
            } else if (mv == L"fast") {
                spec.motionMode = TargetSpec::MotionMode::Fast;
                spec.motionModeText = L"fast";
            } else {
                error = L"invalid motion mode: " + value + L" (valid: amdof, fast)";
                return false;
            }
        } else if (opt == L"--upscaler" || opt == L"-upscaler") {
            if (!need(value)) return false;
            const UpscalerInfo* u = findUpscaler(value);
            if (!u) { error = L"invalid upscaler: " + value + L" (valid: " + upscalerIds() + L")"; return false; }
            spec.upscaler = u->kind;
        } else if (opt == L"--trace" || opt == L"-trace") {
            if (!need(value)) return false;
            spec.tracePath = value;
        } else if (opt == L"--front" || opt == L"-front") {
            spec.front = true;
            explicitTarget = true;
        } else if (opt == L"--nooverlay" || opt == L"-nooverlay") {
            spec.noOverlay = true;
        } else if (opt == L"--key" || opt == L"-key") {
            if (!need(value)) return false;
            if (!parseHotkey(value, spec.stopHotkeyModifiers, spec.stopHotkeyVk)) {
                error = L"invalid hotkey: " + value + L" (example: ctrl+shift+a)";
                return false;
            }
            spec.stopHotkeyText = value;
        } else if (opt == L"--delay" || opt == L"-delay") {
            if (!need(value)) return false;
            wchar_t* end = nullptr;
            unsigned long sec = wcstoul(value.c_str(), &end, 10);
            if (!end || *end != L'\0') { error = L"invalid delay: " + value; return false; }
            spec.delaySeconds = (int)sec;
            if (spec.delaySeconds < 0) spec.delaySeconds = 0;
            spec.delayExplicit = true;
        } else if (opt == L"--picker" || opt == L"-picker") {
            spec.mode = TargetMode::Picker;
            explicitTarget = true;
            // Keep backwards compatibility with: --picker 5
            if (i + 1 < argc) {
                std::wstring next = argv[i + 1];
                if (!next.empty() && next[0] >= L'0' && next[0] <= L'9') {
                    spec.delaySeconds = (int)wcstoul(next.c_str(), nullptr, 10);
                    spec.delayExplicit = true;
                    ++i;
                }
            }
            if (!spec.delayExplicit) spec.delaySeconds = 5;
            if (spec.delaySeconds < 1) spec.delaySeconds = 1;
        } else if (opt == L"--bindbypass" || opt == L"-bindbypass") {
            if (!need(value)) return false;
            if (!parseBypassList(value, spec, error)) return false;
        } else if (opt == L"--overlaykey" || opt == L"-overlaykey") {
            if (!need(value)) return false;
            if (!parseHotkey(value, spec.overlayHotkeyModifiers, spec.overlayHotkeyVk)) {
                error = L"invalid --overlaykey: " + value;
                return false;
            }
            spec.overlayHotkeyText = value;
        } else {
            error = L"unknown option: " + arg;
            return false;
        }
    }

    // -front is deliberately independent of the static target selector. It
    // wins at runtime so a user can combine it with -delay/-nooverlay.
    (void)explicitTarget;
    return true;
}

bool resolveTargetWindow(const TargetSpec& spec, HINSTANCE inst, HWND& out, std::wstring& label)
{
    if (spec.front) {
        HWND h = resolveFrontWindow();
        if (!h) { label = L"No suitable foreground window is active."; return false; }
        out = h;
        label = windowTitle(h);
        return true;
    }

    switch (spec.mode) {
    case TargetMode::Pid: {
        HWND h = bestForPid((DWORD)spec.pid);
        if (!h) { label = L"No visible window for that process id."; return false; }
        out = h;
        label = windowTitle(h);
        return true;
    }
    case TargetMode::Window: {
        HWND h = findByTitle(spec.text);
        if (!h) { label = L"No window title matches regex \"" + spec.text + L"\"."; return false; }
        out = h;
        label = windowTitle(h);
        return true;
    }
    case TargetMode::Process: {
        DWORD pid = findPidByProcessName(spec.text);
        if (!pid) { label = L"No process name matches regex: " + spec.text; return false; }
        HWND h = bestForPid(pid);
        if (!h) { label = L"Process has no visible window: " + spec.text; return false; }
        out = h;
        label = windowTitle(h);
        return true;
    }
    case TargetMode::Picker:
    default: {
        HWND h = runPicker(inst, spec.delaySeconds);
        if (!h) { label = L"No window was selected."; return false; }
        out = h;
        label = windowTitle(h);
        return true;
    }
    }
}

