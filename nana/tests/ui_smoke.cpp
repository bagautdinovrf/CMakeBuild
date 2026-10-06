#include "ui.hpp"
#include "platform.hpp"
#include <filesystem>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { std::cerr << "Usage: NanaUiSmoke <isolated-directory> | --render-preview <directory>\n"; return 2; }
    cb::platform::initialize();
    try {
        const bool preview=std::wstring_view{argv[1]}==L"--render-preview";
        if(preview && argc<3) throw std::runtime_error("preview directory is required");
        const auto root = std::filesystem::absolute(argv[preview ? 2 : 1]);
        std::filesystem::create_directories(root);
        const auto result = preview ? cb::na::runVisualPreviews((root / L"visual-settings.ini").wstring(),root.wstring())
            : cb::na::runUiSmoke((root / L"settings.ini").wstring(), (root / L"images").wstring());
        cb::platform::shutdown();
        std::cout << (preview ? "Nana actual widget previews saved\n" : "Nana widget and settings smoke passed\n");
        return result;
    } catch (const std::exception& error) {
        cb::platform::shutdown(); std::cerr << error.what() << '\n'; return 1;
    }
}
