#include "ui.hpp"
#include "platform.hpp"
#include <exception>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    cb::platform::initialize();
    int result = 1;
    try {
        cb::AppState state; state.load();
        cb::na::Panel panel{state}; result = panel.run();
    } catch (const std::exception& error) {
        const auto detail = cb::platform::utf16(error.what());
        MessageBoxW(nullptr, (L"Не удалось запустить CMakeBuild Nana.\n\n" + detail).c_str(), L"CMakeBuild", MB_OK | MB_ICONERROR);
    }
    cb::platform::shutdown();
    return result;
}
