#include "platform.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <utility>

namespace cb::platform {
namespace {
thread_local bool comInitialized = false;

int textLength(std::size_t length) {
    if (length > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::length_error("Unicode text is too large");
    return static_cast<int>(length);
}
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
}

void shutdown() {
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

bool beginWindowDrag(void* nativeHandle, unsigned edges, WindowDrag& drag) {
    drag = {};
    if (!nativeHandle) return false;
    RECT rectangle{};
    const DWORD messagePosition = GetMessagePos();
    const POINT pointer{static_cast<short>(LOWORD(messagePosition)), static_cast<short>(HIWORD(messagePosition))};
    const HWND window = static_cast<HWND>(nativeHandle);
    if (!GetWindowRect(window, &rectangle) || IsIconic(window)) return false;
    drag = {static_cast<int>(pointer.x), static_cast<int>(pointer.y), edges & 15u};
    return true;
}

bool updateWindowDrag(void* nativeHandle, WindowDrag& drag,
    int minimumLogicalWidth, int minimumLogicalHeight, int fixedLogicalHeight,
    double logicalScale) {
    if (!nativeHandle) return false;
    const HWND window = static_cast<HWND>(nativeHandle);
    RECT current{};
    // Mouse events can queue while the cursor moves ahead. Use the position of
    // the message FLTK is dispatching, rather than the live cursor position.
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
    const auto applyPosition = [&](int x, int y, int width, int height, UINT positionFlags) {
        if (!SetWindowPos(window, nullptr, x, y, width, height, positionFlags)) return false;
        drag.pointerX = pointer.x;
        drag.pointerY = pointer.y;
        return true;
    };
    // WM_DPICHANGED can change both native bounds and the cursor's relative
    // position inside the window. Apply only the next message's displacement
    // to those current bounds so the drag retains FLTK's DPI adjustment.
    if (!drag.edges) {
        // Let FLTK process WM_DPICHANGED and retain its logical window size.
        return applyPosition(bounded(static_cast<long long>(current.left) + dx),
            bounded(static_cast<long long>(current.top) + dy), 0, 0, flags | SWP_NOSIZE);
    }
    if (!std::isfinite(logicalScale) || logicalScale <= 0.0) {
        const UINT dpi = GetDpiForWindow(window);
        logicalScale = dpi ? dpi / 96.0 : 1.0;
    }
    const auto physicalSize = [&](int logical) {
        return static_cast<int>(std::clamp(std::ceil(std::max(1, logical) * logicalScale), 1.0,
            static_cast<double>(std::numeric_limits<int>::max())));
    };
    int width = current.right - current.left, height = current.bottom - current.top;
    int x = current.left, y = current.top;
    if (drag.edges & 3u) {
        width = std::max(physicalSize(minimumLogicalWidth),
            bounded(static_cast<long long>(width) + (drag.edges & 2u ? dx : -dx)));
        if (drag.edges & 1u) x = bounded(static_cast<long long>(current.right) - width);
    }
    if (drag.edges & 12u) {
        height = std::max(physicalSize(minimumLogicalHeight),
            bounded(static_cast<long long>(height) + (drag.edges & 8u ? dy : -dy)));
        if (drag.edges & 4u) y = bounded(static_cast<long long>(current.bottom) - height);
    }
    if (fixedLogicalHeight > 0) height = physicalSize(fixedLogicalHeight);
    // Unchanged axes retain the current native dimensions, including any
    // scaling FLTK applied after crossing to another monitor during the drag.
    return applyPosition(x, y, width, height, flags);
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
