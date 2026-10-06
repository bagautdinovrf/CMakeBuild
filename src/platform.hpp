#pragma once

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

// Position values retain the existing INI's physical pixel coordinates. FLTK
// owns logical dimensions and DPI scaling; these helpers never resize windows.
void constrainToMonitors(int& x, int& y, int width, int height);
void restoreNativePosition(void* nativeHandle, int x, int y);
void captureNativePosition(void* nativeHandle, int& x, int& y);

// Called during FLTK mouse dispatch. A drag keeps its last processed message
// pointer in physical desktop pixels and uses the current native window bounds.
// FLTK's root coordinates change units when a window crosses DPI boundaries.
// Edge bits are left=1, right=2, top=4, bottom=8; zero moves the window.
struct WindowDrag {
    int pointerX{}, pointerY{};
    unsigned edges{};
};
bool beginWindowDrag(void* nativeHandle, unsigned edges, WindowDrag& drag);
bool updateWindowDrag(void* nativeHandle, WindowDrag& drag,
    int minimumLogicalWidth, int minimumLogicalHeight, int fixedLogicalHeight = 0,
    double logicalScale = 0.0);

std::wstring selectPath(void* owner, bool folder, std::wstring title,
    std::wstring filterName = L"CMakeLists.txt", std::wstring filter = L"CMakeLists.txt");

} // namespace cb::platform
