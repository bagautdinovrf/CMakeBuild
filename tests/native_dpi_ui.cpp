#include "ui.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Scroll.H>
#include <FL/platform.H>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct TempDirectory {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / (L"CMakeBuild-native-dpi-" + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(GetTickCount64()));
    TempDirectory() { std::filesystem::create_directories(root); }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        if (error) std::wcerr << L"Could not remove fixtures: " << root.wstring() << L'\n';
    }
};

// FLTK's show() creates the native HWND needed by its real Windows driver.
// Prevent activation while creating it, then hide it with native SW_HIDE;
// Fl_Window::hide() would destroy the HWND and stop exercising that driver.
class NoActivation {
    HHOOK hook_ = SetWindowsHookExW(WH_CBT, callback, nullptr, GetCurrentThreadId());
    static LRESULT CALLBACK callback(int code, WPARAM first, LPARAM second) {
        if (code == HCBT_ACTIVATE || code == HCBT_SETFOCUS) return 1;
        if (code == HCBT_CREATEWND) {
            const auto create = reinterpret_cast<CBT_CREATEWNDW*>(second);
            create->lpcs->dwExStyle |= WS_EX_NOACTIVATE;
        }
        return CallNextHookEx(nullptr, code, first, second);
    }
public:
    NoActivation() { require(hook_ != nullptr, "Could not prevent test-window activation"); }
    ~NoActivation() { UnhookWindowsHookEx(hook_); }
};

struct Monitor {
    HMONITOR handle{};
    RECT area{}, work{};
    UINT dpi{};
    int screen = -1;
};

BOOL CALLBACK collectMonitor(HMONITOR handle, HDC, LPRECT, LPARAM parameter) {
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(handle, &info)) return FALSE;
    reinterpret_cast<std::vector<Monitor>*>(parameter)->push_back({handle, info.rcMonitor, info.rcWork});
    return TRUE;
}

std::vector<Monitor> monitors() {
    std::vector<Monitor> result;
    require(EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&result)) != FALSE,
        "Could not enumerate Windows monitors");
    require(!result.empty(), "No Windows monitors are available");
    for (auto& monitor : result) {
        const HWND probe = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"DPI probe",
            WS_POPUP, (monitor.area.left + monitor.area.right) / 2,
            (monitor.area.top + monitor.area.bottom) / 2, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(probe != nullptr, "Could not create hidden DPI probe");
        monitor.dpi = GetDpiForWindow(probe);
        DestroyWindow(probe);
        require(monitor.dpi != 0, "Windows did not return the monitor DPI");
        for (int screen = 0; screen < Fl::screen_count(); ++screen) {
            int x = 0, y = 0, width = 0, height = 0;
            Fl::screen_xywh(x, y, width, height, screen);
            const float scale = Fl::screen_scale(screen);
            const auto close = [](double left, double right) { return std::abs(left - right) <= 3; };
            if (close(x * scale, monitor.area.left) && close(y * scale, monitor.area.top)
                && close(width * scale, monitor.area.right - monitor.area.left)
                && close(height * scale, monitor.area.bottom - monitor.area.top)) {
                monitor.screen = screen;
                break;
            }
        }
        require(monitor.screen >= 0, "Could not match Windows monitor to FLTK screen");
        std::cout << "Monitor " << monitor.screen << ": " << monitor.area.right - monitor.area.left
            << 'x' << monitor.area.bottom - monitor.area.top << ", " << monitor.dpi
            << " DPI, FLTK scale " << Fl::screen_scale(monitor.screen) << '\n';
    }
    return result;
}

void settle() {
    // Native DPI and position messages are usually synchronous. Also drain
    // posted resize/paint events without running a visible or interactive UI.
    for (int attempt = 0; attempt < 12; ++attempt) {
        Fl::check();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

HWND native(Fl_Window& window) {
    return reinterpret_cast<HWND>(fl_xid(&window));
}

void hideNative(Fl_Window& window) {
    require(native(window) != nullptr, "FLTK did not create a native window");
    ShowWindow(native(window), SW_HIDE);
    require(!IsWindowVisible(native(window)), "DPI test window must remain natively hidden");
    require(GetForegroundWindow() != native(window), "DPI test window must not become foreground");
}

class NativeTrace {
    HWND window_{};
    WNDPROC previous_{};
    static inline std::vector<NativeTrace*> active_;
    static LRESULT CALLBACK callback(HWND window, UINT message, WPARAM first, LPARAM second) {
        const auto found = std::ranges::find_if(active_, [window](const auto* trace) { return trace->window_ == window; });
        if (found == active_.end()) return DefWindowProcW(window, message, first, second);
        auto& trace = **found;
        if (message == WM_DPICHANGED) ++trace.dpiChanges;
        if (message == WM_MOVE) ++trace.moves;
        return CallWindowProcW(trace.previous_, window, message, first, second);
    }
public:
    unsigned dpiChanges = 0, moves = 0;
    explicit NativeTrace(Fl_Window& window) : window_(native(window)) {
        active_.push_back(this);
        previous_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window_, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(callback)));
        require(previous_ != nullptr, "Could not observe native DPI messages");
    }
    ~NativeTrace() {
        if (IsWindow(window_)) SetWindowLongPtrW(window_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous_));
        std::erase(active_, this);
    }
};

struct WidgetGeometry {
    Fl_Widget* widget{};
    int x{}, y{}, width{}, height{}, labelSize{};
    Fl_Font font{};
};

std::vector<WidgetGeometry> widgetGeometry(Fl_Group& window) {
    std::vector<WidgetGeometry> result;
    const auto collect = [&](auto&& self, Fl_Group& group) -> void {
        for (int index = 0; index < group.children(); ++index) {
            auto& widget = *group.child(index);
            result.push_back({&widget, widget.x(), widget.y(), widget.w(), widget.h(), widget.labelsize(), widget.labelfont()});
            if (auto* nested = dynamic_cast<Fl_Group*>(&widget)) self(self, *nested);
        }
    };
    collect(collect, window);
    return result;
}

void checkWidgets(Fl_Window& window, const std::vector<WidgetGeometry>& expected) {
    for (const auto& geometry : expected) {
        auto& widget = *geometry.widget;
        require(std::abs(widget.x() - geometry.x) <= 1 && std::abs(widget.y() - geometry.y) <= 1
            && std::abs(widget.w() - geometry.width) <= 1 && std::abs(widget.h() - geometry.height) <= 1,
            "DPI transition changed logical control geometry");
        require(widget.labelsize() == geometry.labelSize && widget.labelfont() == geometry.font,
            "DPI transition changed control fonts");
        // Settings' scroll content intentionally extends beyond its viewport.
        // The viewport, header/footer buttons and all panel controls must fit.
        if (widget.parent() == &window && widget.visible()) {
            require(widget.x() >= 0 && widget.y() >= 0 && widget.w() > 0 && widget.h() > 0
                && widget.x() + widget.w() <= window.w() && widget.y() + widget.h() <= window.h(),
                "A visible control is clipped by the native window bounds");
        }
    }
}

void checkWindow(Fl_Window& window, const Monitor& monitor, int width, int height,
    const std::vector<WidgetGeometry>& widgets, const char* scenario, bool actualScale = true) {
    RECT client{};
    require(GetClientRect(native(window), &client) != FALSE, "Could not measure native client area");
    const float scale = Fl::screen_scale(window.screen_num());
    const int expectedWidth = static_cast<int>(std::lround(width * scale));
    const int expectedHeight = static_cast<int>(std::lround(height * scale));
    const bool dimensionsMatch = window.w() == width && window.h() == height
        && std::abs(client.right - expectedWidth) <= 1 && std::abs(client.bottom - expectedHeight) <= 1;
    if (!dimensionsMatch) {
        std::cerr << scenario << ": screen " << window.screen_num() << ", logical " << window.w()
            << 'x' << window.h() << " (expected " << width << 'x' << height << "), native "
            << client.right << 'x' << client.bottom << " (expected " << expectedWidth << 'x' << expectedHeight
            << "), scale " << scale << ", Windows DPI " << GetDpiForWindow(native(window)) << '\n';
    }
    require(dimensionsMatch, "DPI transition changed logical size or clipped the scaled client area");
    require(MonitorFromWindow(native(window), MONITOR_DEFAULTTONEAREST) == monitor.handle,
        "Native window did not reach the requested monitor");
    require(window.screen_num() == monitor.screen, "FLTK retained the previous monitor number");
    require(GetDpiForWindow(native(window)) == monitor.dpi, "Native window retained the previous monitor DPI");
    if (actualScale) require(std::abs(scale - monitor.dpi / 96.0f) < 0.01f,
        "FLTK scale disagrees with the actual Windows monitor DPI");
    require(!IsWindowVisible(native(window)), "DPI test unexpectedly displayed a native window");
    require(GetForegroundWindow() != native(window), "DPI test unexpectedly activated a native window");
    checkWidgets(window, widgets);
}

POINT destination(Fl_Window& window, const Monitor& monitor) {
    const float scale = Fl::screen_scale(monitor.screen);
    return {monitor.work.left + std::max(8L, (monitor.work.right - monitor.work.left
                - static_cast<LONG>(std::lround(window.w() * scale))) / 2),
        monitor.work.top + std::max(8L, (monitor.work.bottom - monitor.work.top
                - static_cast<LONG>(std::lround(window.h() * scale))) / 2)};
}

void moveNative(Fl_Window& window, const Monitor& monitor) {
    const POINT point = destination(window, monitor);
    require(SetWindowPos(native(window), nullptr, point.x, point.y, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER) != FALSE,
        "Could not move hidden test window to another monitor");
    settle();
}

void continueDragAfterTransition(Fl_Window& window, cb::platform::WindowDrag& drag) {
    RECT before{}, after{};
    require(GetWindowRect(native(window), &before) != FALSE, "Could not measure corrected DPI drag geometry");
    const DWORD pointer = GetMessagePos();
    drag.pointerX = static_cast<short>(LOWORD(pointer));
    drag.pointerY = static_cast<short>(HIWORD(pointer));
    require(cb::platform::updateWindowDrag(native(window), drag, 500, cb::minimumLogHeight,
        window.h() == cb::compactHeight ? cb::compactHeight : 0, Fl::screen_scale(window.screen_num())),
        "Could not continue a native drag after a DPI transition");
    settle();
    require(GetWindowRect(native(window), &after) != FALSE, "Could not measure continued DPI drag geometry");
    require(after.left == before.left && after.top == before.top && after.right == before.right && after.bottom == before.bottom,
        "Continuing a drag reverted FLTK's corrected DPI position or size");

    // A known message-relative delta checks that the next update is relative
    // to the corrected rectangle. No cursor movement or injected input occurs.
    const DWORD nextPointer = GetMessagePos();
    drag.pointerX = static_cast<short>(LOWORD(nextPointer)) - 17;
    drag.pointerY = static_cast<short>(HIWORD(nextPointer)) - 11;
    require(cb::platform::updateWindowDrag(native(window), drag, 500, cb::minimumLogHeight,
        window.h() == cb::compactHeight ? cb::compactHeight : 0, Fl::screen_scale(window.screen_num())),
        "Could not apply a known native drag delta");
    settle();
    require(GetWindowRect(native(window), &after) != FALSE, "Could not measure native drag delta");
    require(after.left == before.left + 17 && after.top == before.top + 11
        && after.right == before.right + 17 && after.bottom == before.bottom + 11,
        "Native drag delta was not applied to the corrected DPI rectangle");
}

void initializeState(cb::AppState& state, const Monitor& monitor, bool expanded) {
    state.load();
    state.pinned = false;
    state.logVisible = expanded;
    state.width = 620;
    state.logHeight = cb::defaultLogHeight;
    state.x = monitor.work.left + 24;
    state.y = monitor.work.top + 24;
}

void crossRepeatedly(Fl_Window& window, const std::vector<Monitor>& displays, NativeTrace& trace,
    const char* scenario) {
    const int width = window.w(), height = window.h();
    const auto widgets = widgetGeometry(window);
    checkWindow(window, displays.front(), width, height, widgets, scenario);
    for (int iteration = 0; iteration < 3; ++iteration) {
        for (std::size_t target = 1; target < displays.size(); ++target) {
            for (const auto* monitor : {&displays[target], &displays.front()}) {
                const UINT oldDpi = GetDpiForWindow(native(window));
                const auto before = trace.dpiChanges;
                cb::platform::WindowDrag drag;
                require(cb::platform::beginWindowDrag(native(window), 0, drag), "Could not begin hidden native drag");
                moveNative(window, *monitor);
                checkWindow(window, *monitor, width, height, widgets, scenario);
                continueDragAfterTransition(window, drag);
                checkWindow(window, *monitor, width, height, widgets, scenario);
                if (oldDpi != monitor->dpi) require(trace.dpiChanges > before,
                    "Windows did not deliver WM_DPICHANGED during a mixed-DPI crossing");
            }
        }
    }
}

void actualMonitorTransitions(const TempDirectory& temporary, const std::vector<Monitor>& displays) {
    for (const bool expanded : {false, true}) {
        cb::AppState state((temporary.root / (expanded ? L"expanded.ini" : L"compact.ini")).wstring());
        initializeState(state, displays.front(), expanded);
        cb::Panel panel(state);
        panel.screen_num(displays.front().screen);
        panel.show();
        hideNative(panel);
        settle();
        NativeTrace panelTrace(panel);
        crossRepeatedly(panel, displays, panelTrace, expanded ? "expanded panel" : "compact panel");
        {
            cb::SettingsDialog settings(panel);
            settings.show();
            hideNative(settings);
            settle();
            NativeTrace settingsTrace(settings);
            const std::string edited = "Debug DPI draft";
            settings.configurationInput().value(edited.c_str());
            crossRepeatedly(settings, displays, settingsTrace, "settings dialog");
            require(std::string(settings.configurationInput().value()) == edited,
                "Monitor transitions lost an edited Settings value");
            settings.cancel();
        }
        if (displays.size() > 1) {
            const auto& secondary = displays[1];
            const int width = panel.w(), height = panel.h();
            const auto widgets = widgetGeometry(panel);
            const POINT point = destination(panel, secondary);
            cb::platform::restoreNativePosition(native(panel), point.x, point.y);
            settle();
            checkWindow(panel, secondary, width, height, widgets, "restore position on secondary monitor");
            panel.save();
            cb::AppState restored(state.configFile);
            restored.load();
            require(restored.width == width && restored.logVisible == expanded
                && (!expanded || restored.logHeight == height), "Isolated settings saved scaled physical dimensions");
            const auto restoredWidgets = widgetGeometry(panel);
            cb::platform::restoreNativePosition(native(panel), restored.x, restored.y);
            settle();
            checkWindow(panel, secondary, width, height, restoredWidgets, "reapply saved secondary position");
        }
    }
}

// This is process-local application scaling, not a change to Windows display
// settings. It deterministically exercises #1439 even when both displays have
// the same Windows DPI: WM_MOVE updates FLTK's screen number before the real
// native WndProc receives WM_DPICHANGED. FLTK 1.4.5 then ignores that message
// and leaves a client area too small for the destination's application scale.
class ScreenScale {
    int screen_;
    float original_;
public:
    explicit ScreenScale(int screen) : screen_(screen), original_(Fl::screen_scale(screen)) {
        // No test HWND is mapped to this screen while calling screen_scale().
        Fl::screen_scale(screen_, original_ < 1.4f ? 1.5f : 1.0f);
    }
    ~ScreenScale() { Fl::screen_scale(screen_, original_); }
};

void orderedDpiRegression(const TempDirectory& temporary, const std::vector<Monitor>& displays) {
    if (displays.size() < 2) {
        std::cout << "SKIP: ordered WM_MOVE/WM_DPICHANGED regression requires a second monitor\n";
        return;
    }
    const auto& source = displays.front();
    const auto& target = displays[1];
    ScreenScale targetScale(target.screen);
    for (const bool expanded : {false, true}) {
        cb::AppState state((temporary.root / (expanded ? L"ordered-expanded.ini" : L"ordered-compact.ini")).wstring());
        initializeState(state, source, expanded);
        cb::Panel panel(state);
        panel.screen_num(source.screen);
        panel.show();
        hideNative(panel);
        settle();
        NativeTrace trace(panel);
        const int width = panel.w(), height = panel.h();
        const auto widgets = widgetGeometry(panel);
        cb::platform::WindowDrag drag;
        require(cb::platform::beginWindowDrag(native(panel), 0, drag), "Could not begin ordered DPI drag");
        moveNative(panel, target);
        require(panel.screen_num() == target.screen, "WM_MOVE did not update the FLTK screen before WM_DPICHANGED");
        RECT suggested{};
        require(GetWindowRect(native(panel), &suggested) != FALSE, "Could not get synthetic DPI suggested rectangle");
        const auto before = trace.dpiChanges;
        SendMessageW(native(panel), WM_DPICHANGED, MAKELONG(target.dpi, target.dpi), reinterpret_cast<LPARAM>(&suggested));
        settle();
        require(trace.dpiChanges == before + 1, "Synthetic WM_DPICHANGED did not enter the actual native WndProc");
        checkWindow(panel, target, width, height, widgets, "ordered WM_MOVE then WM_DPICHANGED", false);
        continueDragAfterTransition(panel, drag);
        checkWindow(panel, target, width, height, widgets, "continued ordered DPI drag", false);
    }
}
}

int wmain() {
    try {
        cb::platform::initialize();
        NoActivation noActivation;
        Fl::lock();
        Fl::screen_count();
        TempDirectory temporary;
        const auto displays = monitors();
        actualMonitorTransitions(temporary, displays);
        const bool mixedDpi = std::ranges::any_of(displays, [&](const auto& monitor) { return monitor.dpi != displays.front().dpi; });
        if (!mixedDpi) std::cout << "SKIP: real mixed-DPI crossings need monitors with different Windows scales\n";
        orderedDpiRegression(temporary, displays);
        cb::platform::shutdown();
        std::cout << "Native FLTK DPI, monitor transitions, control bounds and secondary restore checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        cb::platform::shutdown();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
