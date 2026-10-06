#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cb::platform {

// FLTK text is UTF-8; engine paths and the Windows profile API are UTF-16.
std::string utf8(std::wstring_view text);
std::wstring utf16(std::string_view text);

void initialize();
void shutdown();
std::wstring configurationFile();
void ensureConfigurationFile(const std::wstring& file);
std::wstring readSetting(const std::wstring& file, const wchar_t* key,
    const wchar_t* fallback = L"", const wchar_t* section = L"Panel");
void writeSetting(const std::wstring& file, const wchar_t* key,
    const std::wstring& value, const wchar_t* section = L"Panel");
std::vector<std::wstring> settingSections(const std::wstring& file);
std::wstring projectSection(const std::wstring& file);

bool darkTheme();
bool themeChanged(void* systemEvent);
void setWindowTheme(void* nativeHandle, bool dark);
void setTopmost(void* nativeHandle, bool enabled);
void setApplicationIcon(void* nativeHandle);
void configurePanelWindow(void* nativeHandle);
void configureDialogWindow(void* nativeHandle, void* ownerHandle);
double windowScale(void* nativeHandle);
void minimizeWindow(void* nativeHandle);
struct DesktopRect { int x{}, y{}, width{}, height{}; };
// Consume WM_DPICHANGED before Nana can resize its buffers using stale layout
// constraints. The owner applies the new scale and native bounds synchronously.
class DpiChangeHandler {
public:
    DpiChangeHandler();
    ~DpiChangeHandler();
    DpiChangeHandler(const DpiChangeHandler&) = delete;
    DpiChangeHandler& operator=(const DpiChangeHandler&) = delete;
    void bind(void* nativeHandle, std::function<void(double, DesktopRect)> callback);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Keep the resized rectangle on its current monitor with the smallest shift
// towards that monitor. DPI scaling at a monitor seam must not undo the move.
DesktopRect monitorStableDpiBounds(void* nativeHandle, DesktopRect bounds);
bool setWindowBounds(void* nativeHandle, DesktopRect bounds);
// Cancel a manual drag when Windows ends capture or cancels mouse interaction.
class WindowDragCancelHandler {
public:
    WindowDragCancelHandler();
    ~WindowDragCancelHandler();
    WindowDragCancelHandler(const WindowDragCancelHandler&) = delete;
    WindowDragCancelHandler& operator=(const WindowDragCancelHandler&) = delete;
    void bind(void* nativeHandle, std::function<void()> callback);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
DesktopRect monitorWorkArea(void* nativeHandle);
DesktopRect clientArea(void* nativeHandle);
// Test-owned preview surfaces only: stay offscreen and never activate or appear
// in the user's taskbar. Production window setup does not call these helpers.
void preparePreviewWindow(void* nativeHandle);
// Test-owned drag fixture: fully transparent and mouse-pass-through, but keep
// Nana's shown state for real native mouse dispatch near GetMessagePos.
bool preparePreviewDragWindow(void* nativeHandle, double logicalScale, int logicalX, int logicalY);
void drainPreviewMessages();
// Deliver the same native message for isolated DPI regression windows.
void dispatchPreviewDpiChange(void* nativeHandle, unsigned dpi, DesktopRect bounds);

// Position values retain the existing INI's physical pixel coordinates.
void constrainToMonitors(int& x, int& y, int width, int height);
void restoreNativePosition(void* nativeHandle, int x, int y);
void captureNativePosition(void* nativeHandle, int& x, int& y);

// Called during Nana mouse dispatch. Move origins and dispatched pointers use
// physical desktop pixels; the original clicked offset uses logical pixels.
// Rebase the physical origin after a DPI transition, retaining that offset.
// Edge bits are left=1, right=2, top=4, bottom=8; zero moves the window.
struct WindowDrag {
    int x{}, y{}, width{}, height{}, pointerX{}, pointerY{};
    unsigned edges{};
    // Original grabbed offset in logical pixels; preserved across DPI rebases.
    double anchorX{}, anchorY{};
};
bool beginWindowDrag(void* nativeHandle, unsigned edges, WindowDrag& drag, double logicalScale = 0.0);
// Moving windows retain their originally grabbed logical offset around the
// dispatched message pointer, including work-area-clamped settings dialogs.
// Edge-resizing uses suggested bounds.
DesktopRect windowDragDpiBounds(const WindowDrag& drag, DesktopRect bounds, double logicalScale);
bool rebaseWindowDrag(void* nativeHandle, WindowDrag& drag, double logicalScale = 0.0);
bool updateWindowDrag(void* nativeHandle, const WindowDrag& drag,
    int minimumLogicalWidth, int minimumLogicalHeight, int fixedLogicalHeight = 0,
    double logicalScale = 0.0);

struct PaintPoint { int x{}, y{}; };
// Physical pixels, opaque HDC, colors in 0xRRGGBB order. The caller owns context.
void paintPolygon(const void* context, const std::vector<PaintPoint>& points,
    unsigned strokeRgb, unsigned fillRgb, unsigned strokeWidth, bool filled);
// Logical coordinates and physical child origin; GDI+ applies the DPI transform.
void paintScaledPolygon(const void* context, const std::vector<PaintPoint>& points,
    unsigned strokeRgb, unsigned fillRgb, double scale, int originX, int originY, bool filled);
void paintScaledLines(const void* context, const std::vector<PaintPoint>& points,
    unsigned strokeRgb, double scale, int originX, int originY);
void paintScaledEllipse(const void* context, int x, int y, int width, int height,
    unsigned colorRgb, double scale, int originX, int originY, bool filled);
void paintFocusRectangle(const void* context, int x, int y, int width, int height,
    unsigned colorRgb, double scale, int originX, int originY);

std::wstring selectPath(void* owner, bool folder, std::wstring title,
    std::wstring filterName = L"CMakeLists.txt", std::wstring filter = L"CMakeLists.txt");

} // namespace cb::platform
