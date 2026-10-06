#pragma once

#include <array>
#include <string>
#include <string_view>

// Shared data for actual Nana preview rendering. These are fixture inputs,
// not an alternate UI or a bitmap scaled after it has been rendered.
namespace harness::visual {
inline constexpr std::array scales{1.0, 1.25, 1.5, 2.0};
inline constexpr std::array percentages{100, 125, 150, 200};
inline constexpr int panelWidth = 500, compactHeight = 150, journalHeight = 365;
inline constexpr int settingsWidth = 505, settingsHeight = 660;
inline constexpr std::wstring_view projectFile = L"C:\\GitRepos\\Демонстрационный проект\\CMakeLists.txt";
inline constexpr std::wstring_view buildDirectory = L"build-release", configuration = L"Release";
inline constexpr std::array targetNames{L"ExampleApp", L"ExampleTool"};
inline constexpr std::wstring_view journalText =
    L"CMake: конфигурация Release\n"
    L"Собраны ExampleApp.exe и ExampleTool.exe\n"
    L"Русский UTF-8: Привет, мир! 😀\n";

enum class View { Panel, BuildAndRun, States, Journal, Settings, Waiting };
inline constexpr std::array comparableViews{View::Panel, View::BuildAndRun, View::States, View::Journal, View::Settings};

inline std::wstring imageStem(View view, bool dark, int percent) {
    std::wstring name = view == View::Settings ? L"settings-" : L"panel-";
    name += dark ? L"dark" : L"light";
    if (view == View::States) name += L"-states";
    else if (view == View::BuildAndRun) name += L"-build-and-run";
    else if (view == View::Journal) name += L"-journal";
    else if (view == View::Waiting) name += L"-waiting";
    return name + L"-" + std::to_wstring(percent);
}
} // namespace harness::visual
