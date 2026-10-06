#include "platform.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace cb::platform {
namespace {
thread_local bool comInitialized = false;
thread_local ULONG_PTR graphicsToken = 0;

struct DpiBoundsContext {
    HWND window{};
    HMONITOR monitor{};
};
thread_local DpiBoundsContext dpiBoundsContext;

struct DpiBoundsScope {
    DpiBoundsContext previous;
    explicit DpiBoundsScope(HWND window, unsigned dpi)
        : previous(std::exchange(dpiBoundsContext, DpiBoundsContext{window,
            GetDpiForWindow(window) == dpi ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) : nullptr})) {}
    ~DpiBoundsScope() { dpiBoundsContext = previous; }
};

DesktopRect stableMonitorBounds(HMONITOR monitor, DesktopRect bounds) {
    if (!monitor || bounds.width <= 0 || bounds.height <= 0) return bounds;
    const auto bounded = [](long long value) {
        return static_cast<int>(std::clamp(value, static_cast<long long>(std::numeric_limits<LONG>::min()),
            static_cast<long long>(std::numeric_limits<LONG>::max())));
    };
    const auto belongs = [&](DesktopRect candidate) {
        RECT rectangle{candidate.x, candidate.y, bounded(static_cast<long long>(candidate.x) + candidate.width),
            bounded(static_cast<long long>(candidate.y) + candidate.height)};
        return MonitorFromRect(&rectangle, MONITOR_DEFAULTTONEAREST) == monitor;
    };
    if (belongs(bounds)) return bounds;
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return bounds;
    const auto& area = info.rcMonitor;
    auto inside = bounds;
    inside.x = std::clamp(bounds.x, static_cast<int>(area.left),
        std::max(static_cast<int>(area.left), bounded(static_cast<long long>(area.right) - bounds.width)));
    inside.y = std::clamp(bounds.y, static_cast<int>(area.top),
        std::max(static_cast<int>(area.top), bounded(static_cast<long long>(area.bottom) - bounds.height)));
    if (bounds.width > area.right - area.left)
        inside.x = bounded((static_cast<long long>(area.left) + area.right - bounds.width) / 2);
    if (bounds.height > area.bottom - area.top)
        inside.y = bounded((static_cast<long long>(area.top) + area.bottom - bounds.height) / 2);
    if (!belongs(inside)) return bounds;
    // Find the first monitor-consistent position, instead of snapping the
    // entire window inside the screen when only a few pixels crossed the seam.
    double outsideFraction = 0.0, insideFraction = 1.0;
    auto result = inside;
    for (int step = 0; step < 32; ++step) {
        const double fraction = (outsideFraction + insideFraction) * .5;
        auto candidate = bounds;
        candidate.x = static_cast<int>(std::lround(std::lerp(static_cast<double>(bounds.x), static_cast<double>(inside.x), fraction)));
        candidate.y = static_cast<int>(std::lround(std::lerp(static_cast<double>(bounds.y), static_cast<double>(inside.y), fraction)));
        if (belongs(candidate)) { insideFraction = fraction; result = candidate; }
        else outsideFraction = fraction;
    }
    // Avoid a tie in the monitor intersection areas at the boundary.
    result.x += (inside.x > result.x) - (inside.x < result.x);
    result.y += (inside.y > result.y) - (inside.y < result.y);
    return result;
}

Gdiplus::Color paintColor(unsigned rgb) {
    return Gdiplus::Color(255, static_cast<BYTE>(rgb >> 16), static_cast<BYTE>(rgb >> 8), static_cast<BYTE>(rgb));
}

template<class Paint>
void scaledPaint(const void* context, double scale, int originX, int originY, Paint&& paint) {
    if (!context || !graphicsToken || !std::isfinite(scale) || scale <= 0) return;
    Gdiplus::Graphics graphics{static_cast<HDC>(const_cast<void*>(context))};
    Gdiplus::Matrix transform{static_cast<Gdiplus::REAL>(scale), 0, 0,
        static_cast<Gdiplus::REAL>(scale), static_cast<Gdiplus::REAL>(-originX), static_cast<Gdiplus::REAL>(-originY)};
    graphics.SetTransform(&transform);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    paint(graphics);
}

void configurePaintPen(Gdiplus::Pen& pen) {
    pen.SetStartCap(Gdiplus::LineCapFlat);
    pen.SetEndCap(Gdiplus::LineCapFlat);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
}

int textLength(std::size_t length) {
    if (length > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::length_error("Unicode text is too large");
    return static_cast<int>(length);
}
}

struct DpiChangeHandler::Impl {
    HWND window{};
    std::function<void(double, DesktopRect)> callback;
    bool handling{};
    UINT_PTR deferredTimer{};
    std::optional<std::pair<double, DesktopRect>> pending;

    void dispatchPending() {
        if (handling) return;
        handling = true;
        struct Reset { bool& flag; ~Reset() { flag = false; } } reset{handling};
        // SetWindowPos can synchronously send another WM_DPICHANGED. Always
        // yield to mouse-up/capture cancellation instead of monopolizing the UI.
        for (int count = 0; pending && window && count < 4; ++count) {
            const auto change = *std::exchange(pending, std::nullopt);
            const DpiBoundsScope boundsScope{window, static_cast<unsigned>(std::lround(change.first * 96))};
            callback(change.first, change.second);
        }
        if (pending && window && !deferredTimer)
            deferredTimer = SetTimer(window, reinterpret_cast<UINT_PTR>(this), 1, nullptr);
    }

    ~Impl() {
        if (window) {
            if (deferredTimer) KillTimer(window, deferredTimer);
            RemoveWindowSubclass(window, procedure, reinterpret_cast<UINT_PTR>(this));
        }
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR id, DWORD_PTR data) {
        auto& self = *reinterpret_cast<Impl*>(data);
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(window, procedure, id);
            self.window = nullptr;
            self.pending.reset();
        } else if (message == WM_TIMER && self.deferredTimer && wParam == self.deferredTimer) {
            KillTimer(window, std::exchange(self.deferredTimer, 0));
            self.dispatchPending();
            return 0;
        } else if (message == WM_DPICHANGED) {
            const auto dpi = LOWORD(wParam);
            if (!dpi || !lParam) return 0;
            const auto& rect = *reinterpret_cast<const RECT*>(lParam);
            self.pending = std::pair{dpi / 96.0, DesktopRect{rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top}};
            self.dispatchPending();
            return 0;
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }
};
DpiChangeHandler::DpiChangeHandler() = default;
DpiChangeHandler::~DpiChangeHandler() = default;
void DpiChangeHandler::bind(void* nativeHandle, std::function<void(double, DesktopRect)> callback) {
    impl_.reset();
    auto handler = std::make_unique<Impl>();
    handler->window = static_cast<HWND>(nativeHandle);
    handler->callback = std::move(callback);
    if (!handler->window || !handler->callback || !SetWindowSubclass(handler->window, Impl::procedure,
        reinterpret_cast<UINT_PTR>(handler.get()), reinterpret_cast<DWORD_PTR>(handler.get())))
        throw std::runtime_error("Could not install the window DPI handler");
    impl_ = std::move(handler);
}
DesktopRect monitorStableDpiBounds(void* nativeHandle, DesktopRect bounds) {
    return nativeHandle ? stableMonitorBounds(MonitorFromWindow(static_cast<HWND>(nativeHandle), MONITOR_DEFAULTTONEAREST), bounds) : bounds;
}
bool setWindowBounds(void* nativeHandle, DesktopRect bounds) {
    if (nativeHandle == dpiBoundsContext.window) bounds = stableMonitorBounds(dpiBoundsContext.monitor, bounds);
    return nativeHandle && bounds.width > 0 && bounds.height > 0 && SetWindowPos(static_cast<HWND>(nativeHandle),
        nullptr, bounds.x, bounds.y, bounds.width, bounds.height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

struct WindowDragCancelHandler::Impl {
    HWND window{};
    std::function<void()> callback;

    ~Impl() {
        if (window) RemoveWindowSubclass(window, procedure, reinterpret_cast<UINT_PTR>(this));
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR id, DWORD_PTR data) {
        auto& self = *reinterpret_cast<Impl*>(data);
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(window, procedure, id);
            self.window = nullptr;
            self.callback();
        } else if ((message == WM_CAPTURECHANGED && reinterpret_cast<HWND>(lParam) != window)
            || message == WM_CANCELMODE || (message == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE)) {
            // The owner clears its drag flag before releasing Nana capture,
            // so the resulting WM_CAPTURECHANGED cannot re-enter an active drag.
            self.callback();
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }
};
WindowDragCancelHandler::WindowDragCancelHandler() = default;
WindowDragCancelHandler::~WindowDragCancelHandler() = default;
void WindowDragCancelHandler::bind(void* nativeHandle, std::function<void()> callback) {
    impl_.reset();
    auto handler = std::make_unique<Impl>();
    handler->window = static_cast<HWND>(nativeHandle);
    handler->callback = std::move(callback);
    if (!handler->window || !handler->callback || !SetWindowSubclass(handler->window, Impl::procedure,
        reinterpret_cast<UINT_PTR>(handler.get()), reinterpret_cast<DWORD_PTR>(handler.get())))
        throw std::runtime_error("Could not install the window drag cancellation handler");
    impl_ = std::move(handler);
}

std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    // Windows replaces an isolated UTF-16 surrogate with U+FFFD. No system
    // ANSI code page is involved, including for diagnostic tool output.
    const int length = textLength(text.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
    if (!bytes) throw std::runtime_error("Could not encode UTF-8 text");
    std::string result(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), length, result.data(), bytes, nullptr, nullptr);
    return result;
}

std::wstring utf16(std::string_view text) {
    if (text.empty()) return {};
    const int length = textLength(text.size());
    const int characters = MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
    if (!characters) throw std::runtime_error("Could not decode UTF-8 text");
    std::wstring result(static_cast<std::size_t>(characters), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), length, result.data(), characters);
    return result;
}

void initialize() {
    if (!comInitialized) comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    if (!graphicsToken) {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&graphicsToken, &input, nullptr);
    }
}

void shutdown() {
    if (graphicsToken) {
        Gdiplus::GdiplusShutdown(graphicsToken);
        graphicsToken = 0;
    }
    if (comInitialized) {
        CoUninitialize();
        comInitialized = false;
    }
}

void ensureConfigurationFile(const std::wstring& file) {
    const auto parent = std::filesystem::path(file).parent_path();
    std::error_code error;
    if (!parent.empty()) std::filesystem::create_directories(parent, error);
    // Preserve existing settings byte-for-byte on read. New files need a BOM
    // before the profile API writes any Unicode paths.
    const HANDLE created = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (created != INVALID_HANDLE_VALUE) {
        const WORD bom = 0xFEFF;
        DWORD written = 0;
        WriteFile(created, &bom, sizeof(bom), &written, nullptr);
        CloseHandle(created);
    }
}

std::wstring configurationFile() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    const auto directory = length && length < buffer.size()
        ? std::filesystem::path(buffer.data()) / L"CMakeBuild"
        : std::filesystem::temp_directory_path() / L"CMakeBuild";
    const auto file = (directory / L"settings.ini").wstring();
    ensureConfigurationFile(file);
    return file;
}

std::wstring readSetting(const std::wstring& file, const wchar_t* key,
    const wchar_t* fallback, const wchar_t* section) {
    std::array<wchar_t, 32768> buffer{};
    GetPrivateProfileStringW(section, key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), file.c_str());
    return buffer.data();
}

void writeSetting(const std::wstring& file, const wchar_t* key,
    const std::wstring& value, const wchar_t* section) {
    WritePrivateProfileStringW(section, key, value.c_str(), file.c_str());
}

std::vector<std::wstring> settingSections(const std::wstring& file) {
    std::vector<wchar_t> buffer(32768);
    for (;;) {
        const DWORD copied = GetPrivateProfileSectionNamesW(buffer.data(), static_cast<DWORD>(buffer.size()), file.c_str());
        if (copied < buffer.size() - 2 || buffer.size() >= 1024 * 1024) break;
        buffer.resize(buffer.size() * 2);
    }
    std::vector<std::wstring> result;
    for (const wchar_t* name = buffer.data(); *name; name += std::wcslen(name) + 1) result.emplace_back(name);
    return result;
}

std::wstring projectSection(const std::wstring& file) {
    std::error_code error;
    auto path = std::filesystem::weakly_canonical(file, error);
    if (error) path = std::filesystem::path(file).lexically_normal();
    path.make_preferred();
    auto name = path.wstring();
    // Normalise existing 8.3 aliases without changing the format of ordinary
    // legacy Project:<path> section names.
    const DWORD required = GetLongPathNameW(name.c_str(), nullptr, 0);
    if (required) {
        std::wstring longName(static_cast<std::size_t>(required), L'\0');
        const DWORD copied = GetLongPathNameW(name.c_str(), longName.data(), required);
        if (copied && copied < required) {
            longName.resize(copied);
            name = std::move(longName);
        }
    }
    CharLowerBuffW(name.data(), static_cast<DWORD>(name.size()));
    std::wstring section = L"Project:";
    for (const wchar_t character : name) {
        if (character == L'%') section += L"%25";
        else if (character == L'[') section += L"%5B";
        else if (character == L']') section += L"%5D";
        else section += character;
    }
    return section;
}

bool darkTheme() {
    DWORD light = 1, bytes = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes);
    return light == 0;
}

bool themeChanged(void* systemEvent) {
    if (!systemEvent) return false;
    const auto* message = static_cast<const MSG*>(systemEvent);
    return message->message == WM_SETTINGCHANGE || message->message == WM_THEMECHANGED;
}

void setWindowTheme(void* nativeHandle, bool dark) {
    if (!nativeHandle) return;
    const BOOL enabled = dark;
    DwmSetWindowAttribute(static_cast<HWND>(nativeHandle), 20, &enabled, sizeof(enabled));
    const int rounded = 2;
    DwmSetWindowAttribute(static_cast<HWND>(nativeHandle), 33, &rounded, sizeof(rounded));
}

void setTopmost(void* nativeHandle, bool enabled) {
    if (nativeHandle) SetWindowPos(static_cast<HWND>(nativeHandle), enabled ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void setApplicationIcon(void* nativeHandle) {
    if (!nativeHandle) return;
    const HWND window = static_cast<HWND>(nativeHandle);
    UINT dpi = GetDpiForWindow(window);
    if (!dpi) dpi = 96;
    const HINSTANCE module = GetModuleHandleW(nullptr);
    const auto loadIcon = [&](int widthMetric, int heightMetric) {
        return static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(101), IMAGE_ICON,
            GetSystemMetricsForDpi(widthMetric, dpi), GetSystemMetricsForDpi(heightMetric, dpi), LR_SHARED));
    };
    // Icons loaded with LR_SHARED belong to Windows and remain valid for the
    // process lifetime. Test harnesses without the application resource simply
    // keep their existing icon.
    if (const HICON largeIcon = loadIcon(SM_CXICON, SM_CYICON)) {
        SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(largeIcon));
        SetClassLongPtrW(window, GCLP_HICON, reinterpret_cast<LONG_PTR>(largeIcon));
    }
    if (const HICON smallIcon = loadIcon(SM_CXSMICON, SM_CYSMICON)) {
        SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
        SetClassLongPtrW(window, GCLP_HICONSM, reinterpret_cast<LONG_PTR>(smallIcon));
    }
}

void configurePanelWindow(void* nativeHandle) {
    if (!nativeHandle) return;
    const HWND window = static_cast<HWND>(nativeHandle);
    // FLTK makes borderless windows tool windows by default. The panel is the
    // application's main window and must remain available in the taskbar and
    // Alt+Tab while its FLTK controls provide the title and resize handles.
    const LONG_PTR extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    SetWindowLongPtrW(window, GWL_EXSTYLE, (extended & ~static_cast<LONG_PTR>(WS_EX_TOOLWINDOW)) | WS_EX_APPWINDOW);
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    const LONG_PTR borderless = style & ~static_cast<LONG_PTR>(WS_CAPTION | WS_THICKFRAME);
    SetWindowLongPtrW(window, GWL_STYLE, borderless | WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    setApplicationIcon(nativeHandle);
}

void configureDialogWindow(void* nativeHandle, void* ownerHandle) {
    if (!nativeHandle) return;
    const HWND window = static_cast<HWND>(nativeHandle);
    const HWND owner = static_cast<HWND>(ownerHandle);
    // A top-level WS_POPUP uses GWLP_HWNDPARENT as its owner, not as a child
    // parent. FLTK retains the HWND and its own modal/focus bookkeeping.
    if (owner != window)
        SetWindowLongPtrW(window, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
    const LONG_PTR extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    SetWindowLongPtrW(window, GWL_EXSTYLE,
        (extended & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW)) | WS_EX_TOOLWINDOW);
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    SetWindowLongPtrW(window, GWL_STYLE,
        (style & ~static_cast<LONG_PTR>(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX))
        | WS_POPUP | WS_SYSMENU);
    const bool ownerTopmost = owner && (GetWindowLongPtrW(owner, GWL_EXSTYLE) & WS_EX_TOPMOST);
    // Keep the modal dialog above a pinned panel without creating an extra
    // taskbar/Alt+Tab entry or taking activation away from FLTK's focus logic.
    SetWindowPos(window, ownerTopmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    setApplicationIcon(nativeHandle);
    setWindowTheme(nativeHandle, darkTheme());
}

double windowScale(void* nativeHandle) {
    const UINT dpi = nativeHandle ? GetDpiForWindow(static_cast<HWND>(nativeHandle)) : 96;
    return dpi ? static_cast<double>(dpi) / 96.0 : 1.0;
}
void minimizeWindow(void* nativeHandle) {
    if (nativeHandle) ShowWindow(static_cast<HWND>(nativeHandle), SW_MINIMIZE);
}
DesktopRect monitorWorkArea(void* nativeHandle) {
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfoW(MonitorFromWindow(static_cast<HWND>(nativeHandle), MONITOR_DEFAULTTONEAREST), &monitor))
        return {monitor.rcWork.left, monitor.rcWork.top, monitor.rcWork.right - monitor.rcWork.left, monitor.rcWork.bottom - monitor.rcWork.top};
    return {0, 0, 1024, 768};
}
DesktopRect clientArea(void* nativeHandle) {
    RECT rectangle{};
    if (nativeHandle && GetClientRect(static_cast<HWND>(nativeHandle), &rectangle))
        return {rectangle.left, rectangle.top, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top};
    return {};
}
void preparePreviewWindow(void* nativeHandle) {
    if (!nativeHandle) return;
    const HWND window = static_cast<HWND>(nativeHandle);
    const auto extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    SetWindowLongPtrW(window, GWL_EXSTYLE,
        (extended & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW)) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    SetWindowPos(window, HWND_NOTOPMOST, -32000, -32000, 0, 0,
        SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
bool preparePreviewDragWindow(void* nativeHandle, double logicalScale, int logicalX, int logicalY) {
    if (!nativeHandle || !std::isfinite(logicalScale) || logicalScale <= 0.0) return false;
    const HWND window = static_cast<HWND>(nativeHandle);
    const auto extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    SetWindowLongPtrW(window, GWL_EXSTYLE,
        extended | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    if (!SetLayeredWindowAttributes(window, 0, 0, LWA_ALPHA)) return false;
    const DWORD messagePosition = GetMessagePos();
    const int x = static_cast<short>(LOWORD(messagePosition)) - static_cast<int>(std::lround(logicalX * logicalScale));
    const int y = static_cast<short>(HIWORD(messagePosition)) - static_cast<int>(std::lround(logicalY * logicalScale));
    return SetWindowPos(window, HWND_NOTOPMOST, x, y, 0, 0,
        SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED) != FALSE;
}
void drainPreviewMessages() {
    MSG message{};
    for (int count = 0; count < 2000 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
        if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); break; }
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
void dispatchPreviewDpiChange(void* nativeHandle, unsigned dpi, DesktopRect bounds) {
    const RECT rect{bounds.x, bounds.y, bounds.x + bounds.width, bounds.y + bounds.height};
    SendMessageW(static_cast<HWND>(nativeHandle), WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&rect));
}

void constrainToMonitors(int& x, int& y, int width, int height) {
    const int unset = std::numeric_limits<int>::min();
    if (x == unset || y == unset) return;
    const auto bounded = [](long long value) {
        return static_cast<LONG>(std::clamp(value, static_cast<long long>(std::numeric_limits<LONG>::min()),
            static_cast<long long>(std::numeric_limits<LONG>::max())));
    };
    RECT rectangle{x, y, bounded(static_cast<long long>(x) + width), bounded(static_cast<long long>(y) + height)};
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromRect(&rectangle, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    const auto& work = monitor.rcWork;
    x = std::clamp(x, static_cast<int>(work.left), std::max(static_cast<int>(work.left), static_cast<int>(work.right) - width));
    y = std::clamp(y, static_cast<int>(work.top), std::max(static_cast<int>(work.top), static_cast<int>(work.bottom) - height));
}

void restoreNativePosition(void* nativeHandle, int x, int y) {
    if (!nativeHandle || x == std::numeric_limits<int>::min() || y == std::numeric_limits<int>::min()) return;
    RECT rectangle{};
    const HWND window = static_cast<HWND>(nativeHandle);
    if (!GetWindowRect(window, &rectangle)) return;
    constrainToMonitors(x, y, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top);
    SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void captureNativePosition(void* nativeHandle, int& x, int& y) {
    if (!nativeHandle) return;
    const HWND window = static_cast<HWND>(nativeHandle);
    RECT rectangle{};
    if (!IsIconic(window) && GetWindowRect(window, &rectangle)) {
        x = rectangle.left;
        y = rectangle.top;
    }
}

bool beginWindowDrag(void* nativeHandle, unsigned edges, WindowDrag& drag, double logicalScale) {
    drag = {};
    if (!nativeHandle) return false;
    RECT rectangle{};
    const DWORD messagePosition = GetMessagePos();
    const POINT pointer{static_cast<short>(LOWORD(messagePosition)), static_cast<short>(HIWORD(messagePosition))};
    const HWND window = static_cast<HWND>(nativeHandle);
    if (!GetWindowRect(window, &rectangle) || IsIconic(window)) return false;
    drag = {static_cast<int>(rectangle.left), static_cast<int>(rectangle.top),
        static_cast<int>(rectangle.right - rectangle.left), static_cast<int>(rectangle.bottom - rectangle.top),
        static_cast<int>(pointer.x), static_cast<int>(pointer.y), edges & 15u};
    if (drag.width <= 0 || drag.height <= 0) return false;
    if (!std::isfinite(logicalScale) || logicalScale <= 0.0) {
        const UINT dpi = GetDpiForWindow(window);
        logicalScale = dpi ? dpi / 96.0 : 1.0;
    }
    drag.anchorX = (static_cast<double>(pointer.x) - drag.x) / logicalScale;
    drag.anchorY = (static_cast<double>(pointer.y) - drag.y) / logicalScale;
    return true;
}

DesktopRect windowDragDpiBounds(const WindowDrag& drag, DesktopRect bounds, double logicalScale) {
    if (drag.edges || bounds.width <= 0 || bounds.height <= 0
        || !std::isfinite(logicalScale) || logicalScale <= 0.0) return bounds;
    const DWORD messagePosition = GetMessagePos();
    const POINT pointer{static_cast<short>(LOWORD(messagePosition)), static_cast<short>(HIWORD(messagePosition))};
    const auto bounded = [](long double value) {
        return static_cast<int>(std::clamp(std::round(value),
            static_cast<long double>(std::numeric_limits<int>::min()),
            static_cast<long double>(std::numeric_limits<int>::max())));
    };
    bounds.x = bounded(static_cast<long double>(pointer.x) - drag.anchorX * logicalScale);
    bounds.y = bounded(static_cast<long double>(pointer.y) - drag.anchorY * logicalScale);
    return bounds;
}

bool rebaseWindowDrag(void* nativeHandle, WindowDrag& drag, double logicalScale) {
    WindowDrag rebased;
    if (!beginWindowDrag(nativeHandle, drag.edges, rebased, logicalScale)) return false;
    // Keep the original logical offset instead of recomputing rounded pixels
    // after every crossing; repeated transitions must not accumulate drift.
    const auto anchored = windowDragDpiBounds(drag, {rebased.x, rebased.y, rebased.width, rebased.height}, logicalScale);
    if (!drag.edges && anchored.x == rebased.x && anchored.y == rebased.y) {
        rebased.anchorX = drag.anchorX;
        rebased.anchorY = drag.anchorY;
    }
    // A correction at the monitor seam establishes a new actual grab offset.
    // Retaining the rejected offset would start the same DPI oscillation again.
    drag = rebased;
    return true;
}

bool updateWindowDrag(void* nativeHandle, const WindowDrag& drag,
    int minimumLogicalWidth, int minimumLogicalHeight, int fixedLogicalHeight,
    double logicalScale) {
    if (!nativeHandle) return false;
    const HWND window = static_cast<HWND>(nativeHandle);
    if (GetCapture() != window) return false;
    RECT current{};
    // Mouse events can queue while the cursor moves ahead. Use the position of
    // the message Nana is dispatching, rather than the live cursor position.
    const DWORD messagePosition = GetMessagePos();
    const POINT pointer{static_cast<short>(LOWORD(messagePosition)), static_cast<short>(HIWORD(messagePosition))};
    if (!GetWindowRect(window, &current) || IsIconic(window)) return false;
    const auto bounded = [](long long value) {
        return static_cast<int>(std::clamp(value, static_cast<long long>(std::numeric_limits<int>::min()),
            static_cast<long long>(std::numeric_limits<int>::max())));
    };
    const long long dx = static_cast<long long>(pointer.x) - drag.pointerX;
    const long long dy = static_cast<long long>(pointer.y) - drag.pointerY;
    constexpr UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if (!drag.edges) {
        // The DPI handler retains the panel's logical window size.
        return SetWindowPos(window, nullptr, bounded(static_cast<long long>(drag.x) + dx),
            bounded(static_cast<long long>(drag.y) + dy), 0, 0, flags | SWP_NOSIZE) != FALSE;
    }
    if (!std::isfinite(logicalScale) || logicalScale <= 0.0) {
        const UINT dpi = GetDpiForWindow(window);
        logicalScale = dpi ? dpi / 96.0 : 1.0;
    }
    const auto physicalSize = [&](int logical) {
        return static_cast<int>(std::clamp(std::floor(std::max(1, logical) * logicalScale), 1.0,
            static_cast<double>(std::numeric_limits<int>::max())));
    };
    int width = current.right - current.left, height = current.bottom - current.top;
    int x = current.left, y = current.top;
    if (drag.edges & 3u) {
        width = std::max(physicalSize(minimumLogicalWidth),
            bounded(static_cast<long long>(drag.width) + (drag.edges & 2u ? dx : -dx)));
        x = drag.edges & 1u ? bounded(static_cast<long long>(drag.x) + drag.width - width) : drag.x;
    }
    if (drag.edges & 12u) {
        height = std::max(physicalSize(minimumLogicalHeight),
            bounded(static_cast<long long>(drag.height) + (drag.edges & 8u ? dy : -dy)));
        y = drag.edges & 4u ? bounded(static_cast<long long>(drag.y) + drag.height - height) : drag.y;
    }
    if (fixedLogicalHeight > 0) height = physicalSize(fixedLogicalHeight);
    // Unchanged axes retain the current native dimensions, including any
    // scaling applied after crossing to another monitor during the drag.
    return SetWindowPos(window, nullptr, x, y, width, height, flags) != FALSE;
}

void paintPolygon(const void* context, const std::vector<PaintPoint>& points,
    unsigned strokeRgb, unsigned fillRgb, unsigned strokeWidth, bool filled) {
    if (!context || points.size() < (filled ? 3u : 2u)) return;
    auto dc = static_cast<HDC>(const_cast<void*>(context));
    std::vector<POINT> vertices;
    vertices.reserve(points.size() + (filled ? 0u : 1u));
    for (const auto& point : points) vertices.push_back({point.x, point.y});
    if (!filled) vertices.push_back(vertices.front());
    const auto color = [](unsigned value) {
        return RGB((value >> 16) & 255u, (value >> 8) & 255u, value & 255u);
    };
    LOGBRUSH description{BS_SOLID, color(strokeRgb), 0};
    const auto pen = ExtCreatePen(PS_GEOMETRIC | PS_ENDCAP_FLAT | PS_JOIN_ROUND,
        std::max(1u, strokeWidth), &description, 0, nullptr);
    if (!pen) return;
    const auto brush = filled ? CreateSolidBrush(color(fillRgb)) : nullptr;
    if (filled && !brush) { DeleteObject(pen); return; }
    const auto previousPen = SelectObject(dc, pen);
    const auto previousBrush = filled ? SelectObject(dc, brush) : nullptr;
    if (filled) Polygon(dc, vertices.data(), static_cast<int>(vertices.size()));
    else Polyline(dc, vertices.data(), static_cast<int>(vertices.size()));
    if (filled) SelectObject(dc, previousBrush);
    SelectObject(dc, previousPen);
    if (brush) DeleteObject(brush);
    DeleteObject(pen);
}

void paintScaledPolygon(const void* context, const std::vector<PaintPoint>& points,
    unsigned strokeRgb, unsigned fillRgb, double scale, int originX, int originY, bool filled) {
    if (points.size() < (filled ? 3u : 2u)) return;
    std::vector<Gdiplus::Point> vertices;
    vertices.reserve(points.size());
    for (const auto& point : points) vertices.emplace_back(point.x, point.y);
    scaledPaint(context, scale, originX, originY, [&](Gdiplus::Graphics& graphics) {
        Gdiplus::GraphicsPath path;
        if (filled) path.AddPolygon(vertices.data(), static_cast<INT>(vertices.size()));
        else path.AddLines(vertices.data(), static_cast<INT>(vertices.size()));
        path.CloseFigure();
        if (filled) {
            Gdiplus::SolidBrush brush{paintColor(fillRgb)};
            graphics.FillPath(&brush, &path);
        } else {
            Gdiplus::Pen pen{paintColor(strokeRgb), 1.0f};
            configurePaintPen(pen);
            graphics.DrawPath(&pen, &path);
        }
    });
}

void paintScaledLines(const void* context, const std::vector<PaintPoint>& points,
    unsigned strokeRgb, double scale, int originX, int originY) {
    if (points.size() < 2) return;
    scaledPaint(context, scale, originX, originY, [&](Gdiplus::Graphics& graphics) {
        Gdiplus::Pen pen{paintColor(strokeRgb), 1.0f};
        configurePaintPen(pen);
        for (std::size_t index = 1; index < points.size(); ++index) {
            const auto& first = points[index - 1];
            const auto& second = points[index];
            graphics.SetSmoothingMode(first.x == second.x || first.y == second.y
                ? Gdiplus::SmoothingModeNone : Gdiplus::SmoothingModeAntiAlias);
            graphics.DrawLine(&pen, first.x, first.y, second.x, second.y);
        }
    });
}

void paintScaledEllipse(const void* context, int x, int y, int width, int height,
    unsigned colorRgb, double scale, int originX, int originY, bool filled) {
    scaledPaint(context, scale, originX, originY, [&](Gdiplus::Graphics& graphics) {
        if (filled) {
            Gdiplus::SolidBrush brush{paintColor(colorRgb)};
            graphics.FillPie(&brush, x, y, width, height, 0, 360);
        } else {
            Gdiplus::Pen pen{paintColor(colorRgb), 1.0f};
            configurePaintPen(pen);
            graphics.DrawArc(&pen, x, y, width, height, 0, 360);
        }
    });
}

void paintFocusRectangle(const void* context, int x, int y, int width, int height,
    unsigned colorRgb, double scale, int originX, int originY) {
    if (!context || width <= 0 || height <= 0 || !std::isfinite(scale) || scale <= 0) return;
    const int stroke = std::max(1, static_cast<int>(scale));
    const int offset = stroke / 2;
    const auto pixel = [scale](int value) {
        const int magnitude = static_cast<int>(std::abs(value) * scale + .001);
        return value >= 0 ? magnitude : -magnitude;
    };
    const int left = pixel(x) + offset - originX, top = pixel(y) + offset - originY;
    const int right = left + pixel(x + width) - pixel(x) - stroke;
    const int bottom = top + pixel(y + height) - pixel(y) - stroke;
    auto dc = static_cast<HDC>(const_cast<void*>(context));
    LOGBRUSH brush{BS_SOLID, RGB((colorRgb >> 16) & 255u, (colorRgb >> 8) & 255u, colorRgb & 255u), 0};
    const auto pen = ExtCreatePen(PS_GEOMETRIC | PS_ENDCAP_FLAT | PS_JOIN_ROUND | PS_DOT,
        static_cast<DWORD>(stroke), &brush, 0, nullptr);
    if (!pen) return;
    const auto previousPen = SelectObject(dc, pen);
    const auto previousMode = SetBkMode(dc, TRANSPARENT);
    POINT previousPoint{};
    MoveToEx(dc, left, top, &previousPoint);
    LineTo(dc, right, top);
    LineTo(dc, right, bottom);
    LineTo(dc, left, bottom);
    LineTo(dc, left, top);
    MoveToEx(dc, previousPoint.x, previousPoint.y, nullptr);
    SetBkMode(dc, previousMode);
    SelectObject(dc, previousPen);
    DeleteObject(pen);
}

std::wstring selectPath(void* owner, bool folder, std::wstring title,
    std::wstring filterName, std::wstring filter) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    dialog->SetTitle(title.c_str());
    if (!folder) {
        const COMDLG_FILTERSPEC spec{filterName.c_str(), filter.c_str()};
        dialog->SetFileTypes(1, &spec);
    }
    std::wstring selected;
    if (SUCCEEDED(dialog->Show(static_cast<HWND>(owner)))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                selected = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return selected;
}

} // namespace cb::platform
