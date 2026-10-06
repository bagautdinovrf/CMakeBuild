#include "ui.hpp"
#include "app_state.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Choice.H>
#include <FL/fl_utf8.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

void require(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << message << L'\n';
        throw std::runtime_error("run UI assertion failed");
    }
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), count, nullptr, nullptr);
    return result;
}

std::string quoted(const std::wstring& value) {
    std::string result = "\"";
    for (const char ch : utf8(value)) {
        if (ch == '\\' || ch == '"') result += '\\';
        result += ch;
    }
    return result + '"';
}

void writeFile(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(value.data(), static_cast<std::streamsize>(value.size()));
    require(file.good(), L"Cannot write test fixture");
}

fs::path currentExecutable() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    require(length != 0 && length < buffer.size(), L"Cannot locate harness executable");
    return fs::path(std::wstring(buffer.data(), length));
}

fs::path marker(const cb::Target& target) {
    const fs::path executable(target.executable);
    return executable.parent_path() / (executable.stem().wstring() + L".launched");
}

struct TempDirectory {
    fs::path root = fs::temp_directory_path() / (L"CMakeBuild-run-ui-"
        + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    TempDirectory() { fs::create_directories(root); }
    ~TempDirectory() {
        std::error_code error;
        fs::remove_all(root, error);
        if (error) std::wcerr << L"Could not remove test fixtures: " << root.wstring() << L'\n';
    }
};

struct ProjectFixture {
    fs::path project, build;
    std::wstring buildDirectory, configuration;
    std::vector<cb::Target> targets;

    ProjectFixture(const fs::path& root, const wchar_t* folder, const wchar_t* directory,
        const wchar_t* config, const wchar_t* first, const wchar_t* second,
        const wchar_t* firstExecutable, const wchar_t* secondExecutable)
        : project(root / folder / L"CMakeLists.txt"), build(project.parent_path() / directory),
          buildDirectory(directory), configuration(config) {
        writeFile(project, "cmake_minimum_required(VERSION 3.24)\nproject(RunFixture LANGUAGES CXX)\n");
        for (const auto& [name, executable] : std::array<std::pair<const wchar_t*, const wchar_t*>, 2>{
                 std::pair{first, firstExecutable}, std::pair{second, secondExecutable}}) {
            const auto artifact = build / L"bin" / executable;
            fs::create_directories(artifact.parent_path());
            fs::copy_file(currentExecutable(), artifact);
            targets.push_back({name, artifact.wstring()});
        }
        writeReplies(false);
    }

    cb::BuildSettings settings() const {
        cb::BuildSettings result;
        result.cmakeFile = project.wstring();
        result.buildDirectory = buildDirectory;
        result.configuration = configuration;
        result.target = targets.front().name;
        return result;
    }

    void writeReplies(bool reversed, const fs::path& source = {}) const {
        const fs::path reply = build / L".cmake/api/v1/reply";
        writeFile(reply / L"index-2026-10-05.json",
            R"({"reply":{"client-cmakebuild":{"codemodel-v2":{"jsonFile":"codemodel.json"}}}})");
        std::vector<std::string> references;
        for (size_t i = 0; i < targets.size(); ++i) {
            const std::wstring filename = L"target-" + std::to_wstring(i) + L".json";
            writeFile(reply / filename, "{\"name\":" + quoted(targets[i].name)
                + ",\"type\":\"EXECUTABLE\",\"artifacts\":[{\"path\":"
                + quoted(fs::relative(fs::path(targets[i].executable), build).generic_wstring()) + "}]}");
            references.push_back("{\"jsonFile\":" + quoted(filename) + "}");
        }
        if (reversed) std::ranges::reverse(references);
        writeFile(reply / L"unbuilt.json",
            R"({"name":"Unbuilt","type":"EXECUTABLE","artifacts":[{"path":"bin/not-built.exe"}]})");
        writeFile(reply / L"library.json",
            R"({"name":"Library","type":"STATIC_LIBRARY","artifacts":[{"path":"bin/helper.lib"}]})");
        writeFile(reply / L"symbols.json",
            R"({"name":"Symbols","type":"EXECUTABLE","artifacts":[{"path":"bin/symbols.pdb"}]})");
        writeFile(build / L"bin/helper.lib", "fixture");
        writeFile(build / L"bin/symbols.pdb", "fixture");
        std::string targetReferences;
        for (const auto& reference : references) {
            if (!targetReferences.empty()) targetReferences += ',';
            targetReferences += reference;
        }
        targetReferences += R"(,{"jsonFile":"unbuilt.json"},{"jsonFile":"library.json"},{"jsonFile":"symbols.json"})";
        writeFile(reply / L"codemodel.json", "{\"paths\":{\"source\":"
            + quoted((source.empty() ? project.parent_path() : source).generic_wstring())
            + ",\"build\":" + quoted(build.generic_wstring())
            + "},\"configurations\":[{\"name\":" + quoted(configuration)
            + ",\"targets\":[" + targetReferences + "]}]}");
    }
};

void pumpEvents() {
    Fl::check();
}

struct MenuCapture {
    std::optional<size_t> choice;
    int calls = 0;
    std::vector<std::string> labels;
    std::vector<int> flags;

    const Fl_Menu_Item* popup(Fl_Menu_Button& menu) {
        ++calls;
        labels.clear();
        flags.clear();
        const auto* entries = menu.menu();
        if (entries) {
            for (const auto* entry = entries; entry->text; ++entry) {
                labels.emplace_back(entry->label());
                flags.push_back(entry->flags);
            }
        }
        return choice && *choice < labels.size() ? entries + *choice : nullptr;
    }
};

struct HiddenPanel {
    cb::AppState app;
    cb::Panel panel;
    MenuCapture menu;
    MenuCapture buildActions;
    MenuCapture projects;

    static cb::AppState load(const fs::path& ini) {
        cb::AppState state(ini.wstring());
        state.load();
        return state;
    }
    explicit HiddenPanel(const fs::path& ini) : app(load(ini)), panel(app) {
        app.pinned = false;
        panel.popupMenu = [this](Fl_Menu_Button& popup) { return menu.popup(popup); };
        panel.popupBuildMenu = [this](Fl_Menu_Button& popup) { return buildActions.popup(popup); };
        panel.popupProjectMenu = [this](Fl_Menu_Button& popup) { return projects.popup(popup); };
        require(!panel.shown(), L"Test FLTK panel must remain hidden");
    }
    ~HiddenPanel() {
        panel.closePanel();
        pumpEvents();
    }
};

size_t targetIndex(const cb::AppState& app, const std::wstring& name) {
    const auto found = std::ranges::find_if(app.targets, [&](const auto& target) { return target.name == name; });
    require(found != app.targets.end(), L"Expected built executable is missing");
    return static_cast<size_t>(found - app.targets.begin());
}

void requireInventory(const cb::AppState& app, const ProjectFixture& project) {
    require(app.targets.size() == project.targets.size(), L"Inventory must include every built EXE and exclude unbuilt/nonexecutable artifacts");
    for (const auto& target : project.targets) {
        const auto& actual = app.targets[targetIndex(app, target.name)];
        require(_wcsicmp(actual.executable.c_str(), target.executable.c_str()) == 0, L"Executable artifact path is incorrect");
    }
}

void requireNoLaunch(const ProjectFixture& first, const ProjectFixture& second) {
    require(std::ranges::none_of(first.targets, [](const auto& target) { return fs::exists(marker(target)); })
        && std::ranges::none_of(second.targets, [](const auto& target) { return fs::exists(marker(target)); }),
        L"Choosing a run target must not launch any executable");
}

void clickAndWaitForRun(HiddenPanel& hidden, const cb::Target& target) {
    fs::remove(marker(target));
    hidden.panel.button(cb::Control::Run).do_callback();
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        pumpEvents();
        if (fs::exists(marker(target)) && hidden.panel.operation() == cb::Operation::Idle) break;
        std::this_thread::sleep_for(5ms);
    }
    require(fs::exists(marker(target)), L"Main Run button did not launch the selected executable");
    require(hidden.panel.operation() == cb::Operation::Idle, L"Fixture application did not complete");
    require(!hidden.panel.failed(), L"Selected executable failed to run");
    require(!hidden.panel.shown(), L"Test panel must stay hidden");
    for (auto* window = Fl::first_window(); window; window = Fl::next_window(window))
        require(!window->shown(), L"Run must not show a Settings window or popup");
    std::ifstream file(marker(target), std::ios::binary);
    const std::string directory((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    require(directory == utf8(fs::path(target.executable).parent_path().wstring()), L"Selected EXE must run in its own directory");
}

void seedProject(const fs::path& ini, const ProjectFixture& project) {
    cb::AppState seed(ini.wstring());
    seed.load();
    seed.settings = project.settings();
    seed.targets = cb::readExecutableTargets(seed.settings);
    requireInventory(seed, project);
    seed.chosenTarget = project.targets.front().name;
    seed.chosenExecutable = project.targets.front().executable;
    seed.pinned = false;
    seed.save();
}

void checkArrow(HiddenPanel& hidden, const std::wstring& selected) {
    auto& panel = hidden.panel;
    auto& app = hidden.app;
    auto& run = panel.button(cb::Control::Run);
    auto& arrow = panel.button(cb::Control::RunMenu);
    require(arrow.w() > 0 && run.y() == arrow.y() && run.h() == arrow.h()
        && arrow.x() >= run.x() + run.w() && arrow.x() <= run.x() + run.w() + 2,
        L"Arrow must form the right-hand part of Run");
    require(arrow.active(), L"Arrow must be enabled when built executables are available");
    hidden.menu.choice.reset();
    const int previousCalls = hidden.menu.calls;
    arrow.do_callback();
    require(hidden.menu.calls == previousCalls + 1, L"Arrow callback must open the executable menu");
    require(hidden.menu.labels.size() == app.targets.size(), L"Dropdown must list all built executables");
    for (size_t i = 0; i < app.targets.size(); ++i) {
        std::string expected;
        for (const char character : utf8(app.targets[i].name + L" — "
                + fs::path(app.targets[i].executable).filename().wstring())) {
            // FLTK preserves doubled ampersands for the label renderer, while
            // add() consumes the path/leading-underscore escape characters.
            if (character == '&') expected += '&';
            expected += character;
        }
        require(hidden.menu.labels[i] == expected, L"Dropdown must identify the target and EXE in UTF-8");
        require(!(hidden.menu.flags[i] & FL_MENU_INACTIVE), L"Built executable menu entry must be selectable");
        require(((hidden.menu.flags[i] & FL_MENU_VALUE) != 0) == (app.targets[i].name == selected),
            L"Dropdown must mark the remembered target");
    }
    require(app.chosenTarget == selected, L"Dismissing the dropdown must retain its selection");
}

int sendKey(Fl_Widget& widget, int key, int modifiers = 0) {
    // Install an event context, then exercise the real FLTK widget handle().
    // No native keyboard input is sent to the user's foreground application.
    struct EventContext {
        int key = Fl::e_keysym, state = Fl::e_state, number = Fl::e_number, length = Fl::e_length;
        char* text = Fl::e_text;
        ~EventContext() {
            Fl::e_keysym = key; Fl::e_state = state; Fl::e_number = number;
            Fl::e_text = text; Fl::e_length = length;
        }
    } previous;
    char character = key == FL_Enter || key == FL_KP_Enter ? '\r' : key == ' ' ? ' ' : '\0';
    Fl::e_keysym = key;
    Fl::e_state = modifiers;
    Fl::e_number = FL_KEYDOWN;
    Fl::e_text = &character;
    Fl::e_length = character ? 1 : 0;
    return widget.handle(FL_KEYDOWN);
}

Fl_Widget* findControl(Fl_Group& group, const std::function<bool(Fl_Widget&)>& matches) {
    for (int index = 0; index < group.children(); ++index) {
        auto& widget = *group.child(index);
        if (matches(widget)) return &widget;
        if (auto* nested = widget.as_group())
            if (auto* found = findControl(*nested, matches)) return found;
    }
    return nullptr;
}

Fl_Button& settingsButton(cb::SettingsDialog& dialog, const char* caption) {
    auto* found = findControl(dialog, [&](Fl_Widget& widget) {
        return dynamic_cast<Fl_Button*>(&widget) && widget.label()
            && std::string_view(widget.label()) == caption;
    });
    require(found != nullptr, L"Expected Settings action button is missing");
    return *static_cast<Fl_Button*>(found);
}

void checkSettingsKeyboard(HiddenPanel& hidden) {
    const auto original = hidden.app.settings.configuration;
    // Visibility flags make cancellation observable without creating or mapping
    // a native window. The actual dialog handler and callbacks process the key.
    for (const auto key : {FL_Enter, FL_KP_Enter}) {
        cb::SettingsDialog cancelled(hidden.panel);
        cancelled.configurationInput().value("Discarded with Enter");
        cancelled.set_visible();
        Fl::focus(&settingsButton(cancelled, "Отмена"));
        require(sendKey(cancelled, key), L"Enter must activate the focused Cancel button");
        require(!cancelled.accepted() && !cancelled.visible()
            && hidden.app.settings.configuration == original,
            L"Enter on Cancel must close Settings without saving edits");
        Fl::focus(nullptr);
    }
    {
        cb::SettingsDialog closed(hidden.panel);
        auto* close = findControl(closed, [](Fl_Widget& widget) {
            return dynamic_cast<Fl_Button*>(&widget) && widget.tooltip()
                && std::string_view(widget.tooltip()).starts_with("Закрыть настройки");
        });
        require(close != nullptr, L"Settings must expose a discoverable Close action");
        closed.configurationInput().value("Discarded with Close");
        closed.set_visible();
        Fl::focus(close);
        require(sendKey(closed, FL_Enter) && !closed.accepted() && !closed.visible(),
            L"Enter on Settings Close must cancel rather than save");
        Fl::focus(nullptr);
    }
    {
        cb::SettingsDialog closed(hidden.panel);
        closed.configurationInput().value("Discarded with Alt+F4");
        closed.set_visible();
        Fl::focus(&closed.configurationInput());
        require(sendKey(closed, FL_F + 4, FL_ALT) && !closed.accepted() && !closed.visible(),
            L"Alt+F4 must cancel the Settings dialog");
        Fl::focus(nullptr);
    }
    {
        cb::SettingsDialog dialog(hidden.panel);
        auto* checkbox = dynamic_cast<Fl_Check_Button*>(findControl(dialog, [](Fl_Widget& widget) {
            return dynamic_cast<Fl_Check_Button*>(&widget) != nullptr;
        }));
        require(checkbox != nullptr, L"Build-tests checkbox must be available for keyboard navigation");
        const bool before = checkbox->value() != 0;
        Fl::focus(checkbox);
        require(sendKey(dialog, FL_Enter) && (checkbox->value() != 0) != before && !dialog.accepted(),
            L"Enter on the build-tests checkbox must toggle it without saving the dialog");
        for (auto* choice : {static_cast<Fl_Choice*>(findControl(dialog, [&](Fl_Widget& widget) {
                 return dynamic_cast<Fl_Choice*>(&widget) && &widget != &dialog.runChoice();
             })), &dialog.runChoice()}) {
            require(choice != nullptr, L"Settings must retain both compiler and launch choices");
            const int value = choice->value();
            Fl::focus(choice);
            require(sendKey(dialog, FL_Enter) && !dialog.accepted() && choice->value() == value,
                L"Enter on a choice must belong to the choice rather than save Settings");
        }
        dialog.configurationInput().value("  \tDebug\t  ");
        Fl::focus(&dialog.configurationInput());
        require(sendKey(dialog, FL_Enter) && dialog.accepted()
            && dialog.settingsValue().configuration == L"Debug"
            && dialog.settingsValue().buildTests != before,
            L"Enter in an input must save the normalized configuration and checkbox edit");
        require(!dialog.shown(), L"Keyboard checks must never map a Settings window");
        Fl::focus(nullptr);
    }
}

void checkLiteralMenuAndExpiredTargets(const fs::path& root) {
    HiddenPanel hidden(root / L"literal-menu.ini");
    const auto existing = currentExecutable().wstring();
    const auto absent = (root / L"expired.exe").wstring();
    const std::wstring longName = L"_tools/path\\segment & @literal " + std::wstring(5000, L'я');
    const std::wstring shortName = L"_other/path\\segment & @target";
    const auto tooltipText = [](const std::wstring& text) {
        std::string escaped;
        for (const char ch : utf8(text)) {
            if (ch == '@') escaped += ch;
            escaped += ch;
        }
        return escaped;
    };
    hidden.app.targets = {{L"Expired", absent}, {longName, existing}, {shortName, existing}};
    hidden.app.chosenTarget = longName;
    hidden.app.chosenExecutable = existing;
    hidden.panel.updateControls();
    hidden.panel.button(cb::Control::RunMenu).do_callback();
    require(hidden.menu.labels.size() == 2 && hidden.app.targets.size() == 2,
        L"Opening the arrow must filter expired EXEs before presenting the menu");
    std::string expected;
    for (const char ch : utf8(shortName + L" — " + currentExecutable().filename().wstring())) {
        if (ch == '&' || ch == '@') expected += ch;
        expected += ch;
    }
    require(hidden.menu.labels.back() == expected,
        L"Target labels must retain literal slash, backslash and underscore, and escape FLTK mnemonic/symbol characters");
    const auto& shortened = hidden.menu.labels.front();
    require(shortened.starts_with("_tools/path\\segment && @@literal ") && shortened.ends_with("…")
        && shortened.size() < utf8(longName).size(),
        L"Very long target labels must preserve their identifying prefix and show an explicit ellipsis");
    for (const char* cursor = shortened.data(); cursor < shortened.data() + shortened.size();) {
        int length = 0;
        const auto codepoint = fl_utf8decode(cursor, shortened.data() + shortened.size(), &length);
        require(length > 0 && codepoint != 0xfffd,
            L"Eliding long target names must retain complete UTF-8 characters");
        cursor += length;
    }
    require((hidden.menu.flags.front() & FL_MENU_VALUE) != 0
        && std::ranges::none_of(hidden.menu.flags, [](int flags) { return flags & (FL_SUBMENU | FL_SUBMENU_POINTER); }),
        L"Literal target text must not create submenus or lose its selection mark");
    require(hidden.panel.button(cb::Control::Run).tooltip()
        && std::string_view(hidden.panel.button(cb::Control::Run).tooltip()).find(tooltipText(longName)) != std::string_view::npos,
        L"Run tooltip must retain the complete target name when its menu label is elided");
    {
        cb::SettingsDialog dialog(hidden.panel);
        const auto* entries = dialog.runChoice().menu();
        require(entries && dialog.runChoice().value() == 1 && entries[1].text && entries[2].text && !entries[3].text,
            L"Settings choice must retain one literal row per target plus its automatic row");
        std::string shortLabel;
        for (const char ch : utf8(shortName)) {
            if (ch == '&' || ch == '@') shortLabel += ch;
            shortLabel += ch;
        }
        require(std::string_view(entries[2].label()) == shortLabel
            && std::string_view(entries[1].label()).ends_with("…"),
            L"Settings target choice must preserve literal labels and elide only the long visible text");
        dialog.runChoice().value(2);
        dialog.runChoice().do_callback();
        dialog.apply();
        require(dialog.chosenTarget() == shortName,
            L"Settings choice must resolve the selected literal row to its complete target name");
        dialog.runChoice().value(1);
        dialog.runChoice().do_callback();
        dialog.apply();
        require(dialog.chosenTarget() == longName,
            L"Selecting an elided Settings row must preserve the complete long target name");
    }
    hidden.menu.choice = 1;
    hidden.panel.button(cb::Control::RunMenu).do_callback();
    require(hidden.app.chosenTarget == shortName && hidden.panel.operation() == cb::Operation::Idle,
        L"Selection indices must refer to the filtered inventory without launching a target");
    require(hidden.panel.button(cb::Control::Run).tooltip()
        && std::string_view(hidden.panel.button(cb::Control::Run).tooltip()).find(tooltipText(shortName)) != std::string_view::npos,
        L"Run tooltip must identify the selected target");

    hidden.app.targets = {{L"Expired", absent}};
    hidden.app.chosenTarget = L"Expired";
    hidden.app.chosenExecutable = absent;
    hidden.panel.updateControls();
    const int calls = hidden.menu.calls;
    hidden.panel.button(cb::Control::RunMenu).do_callback();
    require(hidden.menu.calls == calls && hidden.app.targets.empty()
        && !hidden.panel.button(cb::Control::Run).active() && !hidden.panel.button(cb::Control::RunMenu).active(),
        L"When the final EXE disappears, opening the arrow must refresh both disabled controls without a popup");
    require(hidden.app.chosenTarget == L"Expired", L"Filtering unavailable EXEs must retain the remembered choice");
    require(!sendKey(hidden.panel.button(cb::Control::Run), FL_Down)
        && !sendKey(hidden.panel.button(cb::Control::RunMenu), FL_F + 4),
        L"Disabled launch controls must not accept menu keyboard shortcuts");
}

void checkKeyboard(HiddenPanel& hidden) {
    const int previousCalls = hidden.menu.calls;
    require(sendKey(hidden.panel.button(cb::Control::Run), FL_Down), L"Down must be handled by Run");
    require(sendKey(hidden.panel.button(cb::Control::RunMenu), FL_F + 4), L"F4 must be handled by the arrow");
    require(sendKey(hidden.panel.button(cb::Control::Run), FL_Down, FL_ALT), L"Alt+Down must be handled by Run");
    require(hidden.menu.calls == previousCalls + 3, L"Down, F4 and Alt+Down must open the actual FLTK executable menu");
    require(!sendKey(hidden.panel.button(cb::Control::Run), FL_F + 4, FL_ALT),
        L"Alt+F4 must retain normal window-close handling");
}

void checkSettingsSelection(const fs::path& ini, const ProjectFixture& project) {
    HiddenPanel hidden(ini);
    hidden.app.settings = project.settings();
    hidden.app.targets = project.targets;
    hidden.app.chosenTarget = L"Temporarily missing target";
    hidden.app.chosenExecutable = (project.build / L"missing.exe").wstring();
    checkSettingsKeyboard(hidden);
    cb::SettingsDialog dialog(hidden.panel);
    require(!dialog.shown(), L"Settings verification must use a hidden FLTK window");
    require(!dialog.border(), L"Settings must use the panel's borderless window style");
    require(dialog.configurationInput().textfont() == cb::appFont && dialog.runChoice().textfont() == cb::appFont,
        L"Settings fields must use the panel's application font");
    dialog.configurationInput().value("Debug");
    dialog.configurationInput().insert_position(1, 4);
    Fl::focus(&dialog.configurationInput());
    const int settingsX = dialog.x(), settingsY = dialog.y(), settingsWidth = dialog.w(), settingsHeight = dialog.h();
    const auto settingsFont = dialog.configurationInput().textfont();
    const auto settingsTextSize = dialog.configurationInput().textsize();
    const int settingsPosition = dialog.configurationInput().insert_position(), settingsMark = dialog.configurationInput().mark();
    auto* settingsFocus = Fl::focus();
    dialog.setTheme(false);
    const auto settingsLight = dialog.configurationInput().color();
    dialog.setTheme(true);
    require(dialog.configurationInput().color() != settingsLight,
        L"Settings fields must update colors for the active theme");
    dialog.setTheme(false);
    require(dialog.x() == settingsX && dialog.y() == settingsY && dialog.w() == settingsWidth && dialog.h() == settingsHeight,
        L"Settings theme changes must preserve window geometry");
    require(dialog.configurationInput().textfont() == settingsFont && dialog.configurationInput().textsize() == settingsTextSize,
        L"Settings theme changes must preserve fonts");
    require(std::string(dialog.configurationInput().value()) == "Debug"
        && dialog.configurationInput().insert_position() == settingsPosition && dialog.configurationInput().mark() == settingsMark
        && Fl::focus() == settingsFocus,
        L"Settings theme changes must preserve edited values, selection, and focus");
    Fl::focus(nullptr);
    dialog.apply();
    require(dialog.accepted() && dialog.settingsValue().configuration == L"Debug"
        && dialog.chosenTarget() == L"Temporarily missing target",
        L"Saving another setting must preserve the remembered run target even when its EXE is absent");
    hidden.panel.applySettings(dialog.settingsValue(), dialog.chosenTarget(), dialog.selectionChanged());
    require(hidden.app.settings.configuration == L"Debug"
        && hidden.app.chosenTarget == L"Temporarily missing target",
        L"Applying the real Settings dialog must preserve a missing launch choice");
    // Row zero is the empty/automatic selection; actual targets follow it.
    require(dialog.runChoice().value(static_cast<int>(project.targets.size())) != 0,
        L"Cannot select a built run target in hidden Settings");
    dialog.runChoice().do_callback();
    dialog.apply();
    require(dialog.chosenTarget() == project.targets.back().name,
        L"An explicit run-target change in Settings must save the requested executable");
    require(!dialog.shown(), L"Settings verification must never show its window");

    const auto before = hidden.app.settings.configuration;
    cb::SettingsDialog cancelled(hidden.panel);
    cancelled.configurationInput().value("Discarded");
    require(sendKey(cancelled, FL_Escape), L"Escape must be handled by the Settings dialog");
    require(!cancelled.accepted() && hidden.app.settings.configuration == before,
        L"Cancel must not apply Settings edits");
}

void checkLegacySettings(const fs::path& root, const ProjectFixture& first) {
    const auto ini = root / L"legacy-settings.ini";
    writeFile(ini, std::string("\xFF\xFE", 2));
    const auto setting = [&](const wchar_t* key, const std::wstring& value) {
        require(WritePrivateProfileStringW(L"Panel", key, value.c_str(), ini.c_str()) != FALSE,
            L"Cannot seed legacy UTF-16 Panel settings");
    };
    setting(L"Project", first.project.wstring());
    setting(L"BuildDirectory", first.buildDirectory);
    setting(L"Configuration", first.configuration);
    setting(L"RunTarget", first.targets.back().name);
    setting(L"Executable", first.targets.back().executable);
    setting(L"BuildTests", L"1");
    setting(L"CMake", L"C:\\legacy tools\\cmake.exe");
    setting(L"Compiler", L"2");
    setting(L"BuildTarget", L"legacy build target");
    setting(L"Pinned", L"0");
    setting(L"LogVisible", L"1");
    setting(L"LogHeight", L"480");
    setting(L"UnknownPanelKey", L"Сохранить значение");
    require(WritePrivateProfileStringW(L"UserExtension", L"PrivateValue", L"Не потерять", ini.c_str()) != FALSE,
        L"Cannot seed an unrelated legacy INI section");
    {
        HiddenPanel legacy(ini);
        requireInventory(legacy.app, first);
        require(legacy.app.chosenTarget == first.targets.back().name
            && legacy.app.settings.buildTests && legacy.app.logVisible && legacy.app.logHeight == 480
            && legacy.app.settings.cmakeExecutable == L"C:\\legacy tools\\cmake.exe"
            && legacy.app.settings.compiler == cb::CompilerMode::Mingw
            && legacy.app.settings.target == L"legacy build target",
            L"FLTK panel must load existing global Panel keys without losing values");
        legacy.panel.save();
    }
    std::array<wchar_t, 128> value{};
    GetPrivateProfileStringW(L"Panel", L"UnknownPanelKey", L"", value.data(),
        static_cast<DWORD>(value.size()), ini.c_str());
    require(std::wstring(value.data()) == L"Сохранить значение", L"Saving migrated settings must preserve unknown Panel keys");
    GetPrivateProfileStringW(L"UserExtension", L"PrivateValue", L"", value.data(),
        static_cast<DWORD>(value.size()), ini.c_str());
    require(std::wstring(value.data()) == L"Не потерять", L"Saving migrated settings must preserve unrelated INI sections");
    HiddenPanel restarted(ini);
    require(restarted.app.chosenTarget == first.targets.back().name && restarted.app.settings.buildTests
        && restarted.app.settings.cmakeExecutable == L"C:\\legacy tools\\cmake.exe"
        && restarted.app.settings.compiler == cb::CompilerMode::Mingw
        && restarted.app.settings.target == L"legacy build target",
        L"Legacy global build and launch settings must migrate into a durable complete project profile");
}

void checkRuns(const fs::path& ini, const ProjectFixture& first, const ProjectFixture& second) {
    for (const auto& wrongDirectory : {second.build.wstring(), fs::relative(second.build, first.project.parent_path()).wstring()}) {
        auto settings = first.settings();
        settings.buildDirectory = wrongDirectory;
        bool projectMatches = true;
        require(cb::readExecutableTargets(settings, &projectMatches).empty() && !projectMatches,
            L"Absolute and relative build directories owned by another project must not expose that project's EXEs");
    }
    seedProject(ini, first);
    seedProject(ini, second);
    {
        HiddenPanel hidden(ini);
        auto& app = hidden.app;
        auto& panel = hidden.panel;
        panel.selectProject(first.project.wstring());
        requireInventory(app, first);
        require(app.settings.buildDirectory == first.buildDirectory && app.settings.configuration == first.configuration,
            L"Switching projects must restore the project's build directory and configuration");
        require(app.chosenTarget == first.targets.front().name, L"Project must restore its initial remembered target");
        checkArrow(hidden, first.targets.front().name);
        checkKeyboard(hidden);
        app.chosenTarget.clear();
        const int beforeUnchosenRun = hidden.menu.calls;
        panel.button(cb::Control::Run).do_callback();
        require(hidden.menu.calls == beforeUnchosenRun + 1 && app.chosenTarget.empty(),
            L"Run without a choice must open the executable menu; cancel must leave the choice empty");
        requireNoLaunch(first, second);
        hidden.menu.choice = targetIndex(app, first.targets.back().name);
        panel.button(cb::Control::Run).do_callback();
        hidden.menu.choice.reset();
        require(app.chosenTarget == first.targets.back().name, L"Run's initial menu must choose the requested executable");
        require(panel.operation() == cb::Operation::Idle, L"Arrow selection must leave the panel idle");
        hidden.menu.choice = targetIndex(app, first.targets.front().name);
        panel.button(cb::Control::RunMenu).do_callback();
        require(app.chosenTarget == first.targets.front().name, L"Arrow must change the launch target");
        hidden.menu.choice = targetIndex(app, first.targets.back().name);
        panel.button(cb::Control::RunMenu).do_callback();
        hidden.menu.choice.reset();
        require(app.chosenTarget == first.targets.back().name, L"Arrow must remember the next chosen executable");
        require(!panel.chooseRunTarget(app.targets.size()), L"Invalid target index must be rejected");
        require(app.chosenTarget == first.targets.back().name, L"Invalid selection must not change the remembered executable");
        requireNoLaunch(first, second);

        const fs::path chosenExecutable(first.targets.back().executable);
        auto temporarilyMissing = chosenExecutable;
        temporarilyMissing += L".temporarily-missing";
        fs::rename(chosenExecutable, temporarilyMissing);
        panel.onEvent({cb::EventKind::BuildSucceeded, L"Build while chosen EXE is absent", first.targets});
        require(app.targets.size() == 1 && app.chosenTarget == first.targets.back().name,
            L"Missing remembered EXE must not silently change the saved choice to a sole other target");
        const int beforeMissingRun = hidden.menu.calls;
        panel.button(cb::Control::Run).do_callback();
        require(hidden.menu.calls == beforeMissingRun + 1 && app.chosenTarget == first.targets.back().name,
            L"Run with a temporarily missing choice must offer a menu and retain the choice on cancel");
        requireNoLaunch(first, second);
        fs::rename(temporarilyMissing, chosenExecutable);

        auto reordered = first.targets;
        std::ranges::reverse(reordered);
        app.settings.target = first.targets.front().name;
        panel.onEvent({cb::EventKind::BuildSucceeded, L"Fixture rebuild", std::move(reordered)});
        require(app.chosenTarget == first.targets.back().name, L"Rebuild and target reordering must preserve the user's launch choice over the build target");
        clickAndWaitForRun(hidden, first.targets.back());
        require(!fs::exists(marker(first.targets.front())), L"Main Run must not launch another executable");

        panel.selectProject(second.project.wstring());
        requireInventory(app, second);
        require(app.settings.buildDirectory == second.buildDirectory && app.settings.configuration == second.configuration,
            L"Second project must restore its own build directory and configuration");
        require(app.chosenTarget == second.targets.front().name, L"A project must not inherit another project's launch choice");
        require(panel.chooseRunTarget(targetIndex(app, second.targets.back().name)), L"Cannot choose the second project's executable");
        require(!fs::exists(marker(second.targets.back())), L"Selecting the second project's target must not launch it");
        clickAndWaitForRun(hidden, second.targets.back());
        require(!fs::exists(marker(second.targets.front())), L"Second project must launch its chosen executable only");

        panel.selectProject(first.project.wstring());
        requireInventory(app, first);
        require(app.chosenTarget == first.targets.back().name, L"Returning to a project must restore its independent launch choice");
        panel.save();
    }
    first.writeReplies(true);
    {
        HiddenPanel restarted(ini);
        requireInventory(restarted.app, first);
        require(restarted.app.chosenTarget == first.targets.back().name, L"Panel restart must restore the chosen target");
        require(restarted.app.configFile == ini.wstring(), L"AppState::load must keep the isolated test INI");
        checkArrow(restarted, first.targets.back().name);
        clickAndWaitForRun(restarted, first.targets.back());
        restarted.panel.selectProject(second.project.wstring());
        requireInventory(restarted.app, second);
        require(restarted.app.chosenTarget == second.targets.back().name, L"Restart must preserve the other project's independent choice too");
        auto alias = (first.project.parent_path() / L"." / first.project.filename()).wstring();
        CharUpperBuffW(alias.data(), static_cast<DWORD>(alias.size()));
        restarted.panel.selectProject(alias);
        require(restarted.app.chosenTarget == first.targets.back().name,
            L"Project choice must survive path case changes and redundant path components");
        restarted.panel.save();
    }
    first.writeReplies(false, second.project.parent_path());
    {
        HiddenPanel wrongProject(ini);
        require(wrongProject.app.targets.empty(), L"File API replies owned by another project must suppress even an existing cached EXE");
        require(wrongProject.app.chosenTarget == first.targets.back().name,
            L"A build directory ownership mismatch must retain the project's remembered launch choice");
        require(!wrongProject.panel.button(cb::Control::Run).active() && !wrongProject.panel.button(cb::Control::RunMenu).active(),
            L"Run must be disabled for a build directory owned by another project");
    }
    first.writeReplies(false);
    writeFile(first.build / L"CMakeCache.txt", "CMAKE_HOME_DIRECTORY:INTERNAL=" + utf8(second.project.parent_path().wstring()) + "\n");
    {
        HiddenPanel wrongCache(ini);
        require(wrongCache.app.targets.empty() && wrongCache.app.chosenTarget == first.targets.back().name,
            L"A cache owned by another project must suppress targets while retaining the saved choice");
    }
    fs::remove(first.build / L"CMakeCache.txt");
    fs::remove_all(first.build / L".cmake/api/v1/reply");
    bool projectMatches = false;
    require(cb::readExecutableTargets(first.settings(), &projectMatches).empty() && projectMatches,
        L"Missing File API replies must allow recovery of the existing saved executable");
    {
        HiddenPanel fallback(ini);
        require(fallback.app.targets.size() == 1 && fallback.app.targets.front().name == first.targets.back().name
            && fallback.app.targets.front().executable == first.targets.back().executable,
            L"Existing saved EXE must remain runnable when File API replies are unavailable");
        clickAndWaitForRun(fallback, first.targets.back());
    }
    fs::remove(fs::path(first.targets.back().executable));
    {
        HiddenPanel missing(ini);
        require(missing.app.targets.empty(), L"Missing saved EXE must not appear as runnable");
        require(!missing.panel.button(cb::Control::Run).active() && !missing.panel.button(cb::Control::RunMenu).active(),
            L"Run and arrow must be disabled when no executable exists");
    }
}

void checkBuildDuration(const fs::path& root) {
    HiddenPanel hidden(root / L"build-duration.ini");
    auto& panel = hidden.panel;
    const auto journalText = [&] {
        std::unique_ptr<char, decltype(&std::free)> text(panel.journalBuffer().text(), &std::free);
        return std::string(text.get());
    };
    require(!panel.lastBuildDuration(), L"A fresh panel must not show a previous build duration");
    panel.onEvent({cb::EventKind::Progress, {}, {}, 0, {}, cb::BuildProgress{1, 2}});
    require(!panel.buildProgress(), L"Idle panels must ignore build progress events");
    for (const auto& [elapsed, expected] : std::array<std::pair<std::chrono::milliseconds, const char*>, 5>{
             std::pair{0ms, "0,00 с"}, std::pair{12345ms, "12,35 с"},
             std::pair{59999ms, "1 мин 00,00 с"}, std::pair{61500ms, "1 мин 01,50 с"},
             std::pair{3661234ms, "1 ч 01 мин 01,23 с"}}) {
        panel.journal().clear();
        panel.onEvent({cb::EventKind::BuildSucceeded, L"Сборка завершена.", {}, 0, elapsed});
        require(panel.lastBuildDuration() == elapsed,
            L"Successful build must expose the duration received from the worker");
        require(journalText().find(std::string("Время сборки: ") + expected + '\n') != std::string::npos,
            L"Build duration must be readable in the journal with seconds, minutes, or hours");
    }
    panel.journal().clear();
    panel.onEvent({cb::EventKind::BuildFailed, L"Ошибка сборки", {}, ERROR_INVALID_DATA, 1230ms});
    require(panel.failed() && panel.lastBuildDuration() == 1230ms
            && journalText().find("Время сборки: 1,23 с") != std::string::npos,
        L"Failed builds must show their own duration and keep the failure state");
    panel.journal().clear();
    panel.onEvent({cb::EventKind::BuildFailed, L"Операция отменена.", {}, ERROR_CANCELLED, 2500ms});
    require(!panel.failed() && panel.lastBuildDuration() == 2500ms
            && journalText().find("Время сборки: 2,50 с") != std::string::npos,
        L"Cancelled builds must show elapsed time without becoming errors");
    panel.onEvent({cb::EventKind::BuildSucceeded, L"Legacy event without timing"});
    require(!panel.lastBuildDuration(), L"An untimed result must not retain another build's duration");
    panel.journal().clear();
    panel.onEvent({cb::EventKind::ConfigureSucceeded, L"CMake завершён.", {}, 0, 2340ms});
    require(!panel.failed() && panel.lastBuildDuration() == 2340ms
        && journalText().find("CMake: 2,34 с") != std::string::npos
        && journalText().find("Время сборки:") == std::string::npos,
        L"Configure-only results must show their own CMake duration without calling it build time");
    panel.journal().clear();
    panel.onEvent({cb::EventKind::ConfigureFailed, L"Ошибка CMake", {}, ERROR_INVALID_DATA, 1230ms});
    require(panel.failed() && panel.lastBuildDuration() == 1230ms
        && journalText().find("CMake: 1,23 с") != std::string::npos,
        L"Failed configure-only results must retain their measured CMake duration and failure state");
    panel.journal().clear();
    panel.onEvent({cb::EventKind::ConfigureFailed, L"Операция отменена.", {}, ERROR_CANCELLED, 500ms});
    require(!panel.failed() && panel.lastBuildDuration() == 500ms
        && journalText().find("CMake: 0,50 с") != std::string::npos,
        L"Configure cancellation must show CMake duration while leaving the panel usable");
}

void waitUntil(const std::function<bool()>& condition, const wchar_t* message) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline) {
        pumpEvents();
        if (condition()) return;
        std::this_thread::sleep_for(5ms);
    }
    require(false, message);
}

void requireProfile(const cb::BuildSettings& actual, const cb::BuildSettings& expected) {
    require(actual.cmakeFile == expected.cmakeFile && actual.buildDirectory == expected.buildDirectory
        && actual.cmakeExecutable == expected.cmakeExecutable && actual.configuration == expected.configuration
        && actual.compiler == expected.compiler && actual.target == expected.target
        && actual.buildTests == expected.buildTests && !actual.cleanFirst,
        L"Switching and restarting must retain every saved project setting without transient clean-first");
}

void requireRunSettings(const cb::RunSettings& actual, const cb::RunSettings& expected) {
    require(actual.arguments == expected.arguments && actual.workingDirectory == expected.workingDirectory
        && actual.environment == expected.environment,
        L"Target launch settings must round-trip arguments, working directory and Unicode environment exactly");
}

void checkLaunchSettingsDialog(const fs::path& root) {
    const auto directory = root / L"launch settings dialog";
    const ProjectFixture project(directory, L"project", L"build", L"Release", L"First", L"Second",
        L"run-fixture-dialog-first.exe", L"run-fixture-dialog-second.exe");
    const auto ini = directory / L"settings.ini";
    seedProject(ini, project);
    HiddenPanel hidden(ini);
    const cb::RunSettings originalFirst{L"--original-first", L"first directory", {{L"FIRST", L"original first"}}};
    const cb::RunSettings originalSecond{L"--original-second", L"second directory", {{L"SECOND", L"original second"}}};
    hidden.app.setRunSettings(project.targets.front().name, originalFirst);
    hidden.app.setRunSettings(project.targets.back().name, originalSecond);
    hidden.app.save();
    cb::SettingsDialog dialog(hidden.panel);
    require(std::string_view(dialog.argumentsInput().value()) == "--original-first"
        && std::string_view(dialog.workingDirectoryInput().value()) == "first directory",
        L"Settings launch inputs must display the selected target's saved launch profile");
    dialog.argumentsInput().value("  --first \"русский аргумент\"  ");
    dialog.workingDirectoryInput().value("relative first directory");
    dialog.environmentInput().value("FIRST=edited first\nUNICODE=русское значение 😀");
    dialog.runChoice().value(2);
    dialog.runChoice().do_callback();
    require(std::string_view(dialog.argumentsInput().value()) == "--original-second"
        && std::string_view(dialog.workingDirectoryInput().value()) == "second directory",
        L"Changing launch target must load its own profile while retaining the first target's edits");
    dialog.argumentsInput().value("--second \"two words\"");
    dialog.workingDirectoryInput().value("relative second directory");
    dialog.environmentInput().value("SECOND=edited second");
    dialog.argumentsInput().insert_position(1, 5);
    Fl::focus(&dialog.argumentsInput());
    const int x = dialog.x(), y = dialog.y(), width = dialog.w(), height = dialog.h();
    const int position = dialog.argumentsInput().insert_position(), mark = dialog.argumentsInput().mark();
    const auto font = dialog.argumentsInput().textfont();
    dialog.setTheme(true);
    dialog.setTheme(false);
    require(dialog.x() == x && dialog.y() == y && dialog.w() == width && dialog.h() == height
        && dialog.argumentsInput().insert_position() == position && dialog.argumentsInput().mark() == mark
        && dialog.argumentsInput().textfont() == font && Fl::focus() == &dialog.argumentsInput()
        && std::string_view(dialog.argumentsInput().value()) == "--second \"two words\""
        && std::string_view(dialog.environmentInput().value()) == "SECOND=edited second",
        L"Theme changes must preserve launch drafts, focused input, selection, fonts and dialog geometry");
    Fl::focus(nullptr);
    dialog.runChoice().value(1);
    dialog.runChoice().do_callback();
    require(std::string_view(dialog.argumentsInput().value()) == "  --first \"русский аргумент\"  "
        && std::string_view(dialog.workingDirectoryInput().value()) == "relative first directory"
        && std::string_view(dialog.environmentInput().value()) == "FIRST=edited first\nUNICODE=русское значение 😀",
        L"Returning to a launch target must restore its uncommitted drafts exactly");
    dialog.apply();
    require(dialog.accepted(), L"Valid launch profiles must be accepted by the real Settings dialog");
    const auto draftFor = [&](const std::wstring& target) -> cb::RunSettings {
        const auto& settings = dialog.launchSettings();
        const auto found = std::ranges::find_if(settings, [&](const auto& item) { return item.first == target; });
        require(found != settings.end(), L"Accepted dialog must return every edited target's launch profile");
        return found->second;
    };
    requireRunSettings(draftFor(project.targets.front().name),
        {L"  --first \"русский аргумент\"  ", L"relative first directory", {{L"FIRST", L"edited first"}, {L"UNICODE", L"русское значение 😀"}}});
    requireRunSettings(draftFor(project.targets.back().name),
        {L"--second \"two words\"", L"relative second directory", {{L"SECOND", L"edited second"}}});
    // Dialog owns its transaction. Panel commits accepted results; building or
    // cancelling a dialog must never write draft values to the shared AppState.
    requireRunSettings(hidden.app.runSettingsFor(project.targets.front().name), originalFirst);
    cb::SettingsDialog cancelled(hidden.panel);
    cancelled.argumentsInput().value("--discarded");
    cancelled.environmentInput().value("FIRST=discarded");
    cancelled.runChoice().value(2);
    cancelled.runChoice().do_callback();
    cancelled.argumentsInput().value("--discarded-second");
    require(sendKey(cancelled, FL_Escape) && !cancelled.accepted(), L"Escape must cancel launch drafts");
    cb::AppState restored(ini.wstring());
    restored.load();
    requireRunSettings(restored.runSettingsFor(project.targets.front().name), originalFirst);
    requireRunSettings(restored.runSettingsFor(project.targets.back().name), originalSecond);
}

void checkCompleteProfilesAndRecentProjects(const fs::path& root) {
    const auto directory = root / L"complete profiles";
    const auto ini = directory / L"settings.ini";
    const ProjectFixture first(directory, L"первый & проект", L"out one", L"Release",
        L"First", L"Second", L"run-fixture-profile-first.exe", L"run-fixture-profile-second.exe");
    const ProjectFixture second(directory, L"второй проект", L"out two", L"Debug",
        L"Other", L"Extra", L"run-fixture-profile-other.exe", L"run-fixture-profile-extra.exe");
    auto firstSettings = first.settings();
    firstSettings.cmakeExecutable = L"C:\\Tools with spaces\\cmake-one.exe";
    firstSettings.compiler = cb::CompilerMode::Msvc;
    firstSettings.target = L"First & build";
    firstSettings.buildTests = true;
    auto secondSettings = second.settings();
    secondSettings.cmakeExecutable = L"C:\\другие инструменты\\cmake-two.exe";
    secondSettings.compiler = cb::CompilerMode::Mingw;
    secondSettings.target = L"Other build";
    const cb::RunSettings firstRun{L"  \"русский аргумент\" --name=\"quoted value\" " + std::wstring(10000, L'я') + L"  ", L"..\\рабочая папка",
        {{L"PROFILE_VALUE", L"  Unicode 😀 & = value  "}, {L"MULTILINE", L"first\nsecond"}}};
    const cb::RunSettings secondRun{L"--second", L"second working directory", {{L"PROFILE_VALUE", L"second"}}};
    {
        HiddenPanel hidden(ini);
        hidden.panel.selectProject(first.project.wstring());
        hidden.app.settings = firstSettings;
        hidden.app.targets = first.targets;
        hidden.app.setRunSettings(first.targets.front().name, firstRun);
        hidden.app.setRunSettings(first.targets.back().name, secondRun);
        hidden.panel.save();
        hidden.panel.selectProject(second.project.wstring());
        hidden.app.settings = secondSettings;
        hidden.app.targets = second.targets;
        hidden.app.setRunSettings(second.targets.front().name, secondRun);
        hidden.panel.save();
        hidden.panel.selectProject(first.project.wstring());
        requireProfile(hidden.app.settings, firstSettings);
        requireRunSettings(hidden.app.runSettingsFor(first.targets.front().name), firstRun);
        requireRunSettings(hidden.app.runSettingsFor(first.targets.back().name), secondRun);
        require(hidden.app.recentProjects.size() == 2 && hidden.app.recentProjects.front() == first.project.wstring(),
            L"Recent projects must put the selected project first without duplicating it");
        auto alias = (first.project.parent_path() / L"." / first.project.filename()).wstring();
        CharUpperBuffW(alias.data(), static_cast<DWORD>(alias.size()));
        hidden.panel.selectProject(alias);
        require(hidden.app.recentProjects.size() == 2,
            L"Windows case and dot aliases must not duplicate a recent project");
        const int menuCalls = hidden.projects.calls;
        hidden.panel.button(cb::Control::PickMenu).do_callback();
        require(hidden.projects.calls == menuCalls + 1 && hidden.projects.labels.size() == 3
            && hidden.projects.labels.front() == "Открыть проект…",
            L"Project arrow must present Open Project followed by the actual recent project inventory");
        require(std::ranges::none_of(hidden.projects.flags, [](int flags) { return flags & (FL_SUBMENU | FL_SUBMENU_POINTER); }),
            L"Recent project paths must remain literal menu rows");
        require(hidden.panel.projectMenu().textfont() == cb::appFont,
            L"Recent project menu must retain the panel's application font");
        const int keyboardMenuCalls = hidden.projects.calls;
        require(sendKey(hidden.panel.button(cb::Control::Pick), FL_Down)
            && sendKey(hidden.panel.button(cb::Control::PickMenu), FL_F + 4)
            && hidden.projects.calls == keyboardMenuCalls + 2,
            L"Project arrow keyboard handlers must open the actual recent-project menu without switching projects");
        hidden.projects.choice = 2;
        hidden.panel.button(cb::Control::PickMenu).do_callback();
        hidden.projects.choice.reset();
        requireProfile(hidden.app.settings, secondSettings);
        requireRunSettings(hidden.app.runSettingsFor(second.targets.front().name), secondRun);
        hidden.panel.save();
    }
    {
        HiddenPanel restarted(ini);
        requireProfile(restarted.app.settings, secondSettings);
        requireRunSettings(restarted.app.runSettingsFor(second.targets.front().name), secondRun);
        restarted.panel.selectProject(first.project.wstring());
        requireProfile(restarted.app.settings, firstSettings);
        requireRunSettings(restarted.app.runSettingsFor(first.targets.front().name), firstRun);
        requireRunSettings(restarted.app.runSettingsFor(first.targets.back().name), secondRun);
        requireRunSettings(restarted.app.runSettingsFor(L"Unknown target"), {});
        for (int index = 0; index < 12; ++index) {
            const auto project = directory / (L"recent " + std::to_wstring(index)) / L"CMakeLists.txt";
            writeFile(project, "cmake_minimum_required(VERSION 3.24)\nproject(Recent LANGUAGES NONE)\n");
            restarted.panel.selectProject(project.wstring());
            require(restarted.app.settings.buildDirectory.empty() && restarted.app.settings.target.empty()
                && restarted.app.settings.configuration == L"Release" && !restarted.app.settings.buildTests
                && restarted.app.settings.compiler == cb::CompilerMode::Automatic,
                L"New projects must receive defaults instead of another project's build profile");
        }
        require(restarted.app.recentProjects.size() == 10
            && restarted.app.recentProjects.front() == (directory / L"recent 11" / L"CMakeLists.txt").wstring()
            && restarted.app.recentProjects.back() == (directory / L"recent 2" / L"CMakeLists.txt").wstring(),
            L"Recent project history must keep the ten most recently selected projects in order");
        const auto current = restarted.app.settings.cmakeFile;
        const auto missing = restarted.app.recentProjects[1];
        fs::remove(fs::path(missing));
        require(!restarted.panel.chooseRecentProject(1) && restarted.app.settings.cmakeFile == current
            && restarted.app.recentProjects.size() == 10,
            L"Missing recent projects must retain their history entry without replacing the active project");
        require(!restarted.panel.chooseRecentProject(99) && restarted.app.settings.cmakeFile == current,
            L"An invalid recent project index must leave the active profile unchanged");
        restarted.panel.save();
    }
    cb::AppState restored(ini.wstring());
    restored.load();
    require(restored.recentProjects.size() == 10
        && restored.recentProjects.front() == restored.settings.cmakeFile,
        L"Recent-project order and missing entries must survive restart");
}

std::wstring environmentValue(const wchar_t* name) {
    const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (!length) return {};
    std::wstring value(length, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), length);
    value.resize(copied);
    return value;
}

std::string fileContents(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void checkConfiguredLaunch(const fs::path& root) {
    const auto directory = root / L"настройки запуска с пробелами";
    const auto executable = directory / L"bin/run-fixture-arguments.exe";
    fs::create_directories(executable.parent_path());
    fs::copy_file(currentExecutable(), executable);
    const auto working = directory / L"рабочая папка";
    fs::create_directories(working);
    const cb::Target target{L"Configured target", executable.wstring()};
    const auto previousValue = environmentValue(L"CMAKEBUILD_RUN_UI_VALUE");
    const auto previousTemp = environmentValue(L"TEMP");
    const cb::RunSettings options{L"\"русский аргумент 😀\" \"a b\" \"quote\\\"inside\" \"C:\\folder with spaces\\\\\"", working.wstring(),
        {{L"CMAKEBUILD_RUN_UI_VALUE", L"Unicode значение 😀 = one two"}, {L"TEMP", working.wstring()}}};
    const auto launch = [&](cb::RunSettings settings) {
        fs::remove(marker(target));
        std::atomic_bool finished = false;
        DWORD exitCode = ERROR_INVALID_DATA;
        cb::Engine engine([&](cb::Event event) {
            if (event.kind == cb::EventKind::Finished) {
                exitCode = event.exitCode;
                finished.store(true, std::memory_order_release);
            }
        });
        require(engine.runConfigured(target, std::move(settings)), L"Configured launch must start an actual fixture process");
        waitUntil([&] { return finished.load(std::memory_order_acquire); }, L"Configured fixture launch did not finish");
        require(exitCode == 0 && fs::exists(marker(target)), L"Configured fixture must report a successful actual run");
    };
    launch(options);
    require(fileContents(marker(target)) == utf8(working.wstring()),
        L"Configured working directory must reach the child process");
    std::string expectedArguments;
    for (const auto* argument : {L"русский аргумент 😀", L"a b", L"quote\"inside", L"C:\\folder with spaces\\"})
        expectedArguments += quoted(argument) + '\n';
    require(fileContents(executable.parent_path() / L"arguments.txt") == expectedArguments,
        L"Raw Windows arguments must deliver Unicode, spaces, literal quotes and trailing backslashes to argv");
    require(fileContents(executable.parent_path() / L"environment.txt")
        == utf8(options.environment.front().second) + "\n" + utf8(working.wstring()),
        L"Environment overrides must reach the child after runtime environment preparation");
    require(environmentValue(L"CMAKEBUILD_RUN_UI_VALUE") == previousValue && environmentValue(L"TEMP") == previousTemp,
        L"Launch environment overrides must never alter the parent process environment");
    auto relative = options;
    relative.workingDirectory = L"..\\рабочая папка";
    launch(relative);
    require(fileContents(marker(target)) == utf8(working.wstring()),
        L"Relative launch directory must resolve against the executable's directory");
    launch({});
    require(fileContents(marker(target)) == utf8(executable.parent_path().wstring())
        && fileContents(executable.parent_path() / L"arguments.txt").empty()
        && fileContents(executable.parent_path() / L"environment.txt") == utf8(previousValue) + "\n" + utf8(previousTemp),
        L"A later default launch must regain the default directory, empty arguments and inherited environment");
}

void checkBuildAndRun(const fs::path& root) {
    const auto directory = root / L"build and run";
    const auto ini = directory / L"settings.ini";
    const auto project = directory / L"CMakeLists.txt";
    const auto working = directory / L"chosen working directory";
    const auto ran = working / L"combined-ran";
    const auto launchCount = working / L"launch-count";
    const auto holdEnabled = directory / L"hold-enabled";
    const auto buildStarted = directory / L"build-started";
    const auto releaseBuild = directory / L"release-build";
    fs::create_directories(working);
    writeFile(directory / L"main.cpp", "#include <cstdlib>\n#include <fstream>\nint main(int argc, char**) {\n"
        "const char* value=std::getenv(\"CMAKEBUILD_CHAIN_VALUE\");\n"
        "std::ofstream(\"launch-count\",std::ios::app) << 'x';\n"
        "std::ofstream(\"combined-ran\") << argc << ':' << (value ? value : \"missing\");\nreturn 0;\n}\n");
    writeFile(directory / L"hold-build.cmake", "if(EXISTS \"${CMAKE_CURRENT_LIST_DIR}/hold-enabled\")\n"
        "file(WRITE \"${CMAKE_CURRENT_LIST_DIR}/build-started\" \"ready\")\n"
        "while(NOT EXISTS \"${CMAKE_CURRENT_LIST_DIR}/release-build\")\n"
        "execute_process(COMMAND \"${CMAKE_COMMAND}\" -E sleep 0.05)\nendwhile()\nendif()\n");
    const std::string validProject = "cmake_minimum_required(VERSION 3.24)\nproject(BuildAndRun LANGUAGES CXX)\n"
        "add_executable(chain main.cpp)\n"
        "add_custom_target(wait_step COMMAND \"${CMAKE_COMMAND}\" -P \"${CMAKE_CURRENT_SOURCE_DIR}/hold-build.cmake\")\n"
        "add_dependencies(chain wait_step)\n";
    writeFile(project, validProject);
    HiddenPanel hidden(ini);
    auto& panel = hidden.panel;
    hidden.app.settings.cmakeFile = project.wstring();
    hidden.app.settings.buildDirectory = (directory / L"build").wstring();
    hidden.app.setRunSettings(L"chain", {L"--chain \"argument with spaces\"", working.wstring(),
        {{L"CMAKEBUILD_CHAIN_VALUE", L"success"}}});
    panel.updateControls();
    const auto requireIdleRun = [&] {
        require(panel.operation() == cb::Operation::Idle
            && std::string_view(panel.button(cb::Control::Run).label()) == "Запустить"
            && (panel.button(cb::Control::Run).active() != 0) == !hidden.app.targets.empty(),
            L"Terminal build/run outcomes must restore the normal Run caption and enable it only when targets exist");
    };
    const auto requireSuccessfulChain = [&] {
        waitUntil([&] { return fs::exists(ran) && panel.operation() == cb::Operation::Idle; },
            L"Build-and-run must wait for a successful actual build and finish the selected application");
        require(!panel.failed() && fileContents(ran) == "3:success" && hidden.menu.calls == 0,
            L"Combined action must launch the remembered target with its saved arguments, directory and environment");
        require(panel.lastBuildDuration().has_value(), L"Combined action must retain its build duration after run completion");
        requireIdleRun();
    };
    const auto holdBuild = [&](bool cleanFirst = false) {
        fs::remove(ran);
        fs::remove(launchCount);
        fs::remove(buildStarted);
        fs::remove(releaseBuild);
        writeFile(holdEnabled, "hold this build until queue checks finish");
        panel.build(cleanFirst);
        waitUntil([&] { return fs::exists(buildStarted) || panel.operation() == cb::Operation::Idle; },
            L"Build target must reach the test-owned wait step before queueing a launch");
        require(panel.operation() == cb::Operation::Building && fs::exists(buildStarted),
            L"Queued-launch fixture must have a real active build process");
        require(panel.button(cb::Control::Run).active() && !panel.button(cb::Control::RunMenu).active()
            && std::string_view(panel.button(cb::Control::Run).label()) == "Запустить",
            L"Main Run must remain enabled during ordinary build/rebuild while target selection stays disabled");
    };
    const auto queueRun = [&] {
        const int menuCalls = hidden.menu.calls;
        require(!sendKey(panel.button(cb::Control::Run), FL_Down)
            && !sendKey(panel.button(cb::Control::Run), FL_F + 4) && hidden.menu.calls == menuCalls,
            L"Enabled Run during a build must not open its disabled target-selection menu from the keyboard");
        auto& run = panel.button(cb::Control::Run);
        const std::array geometry{run.x(), run.y(), run.w(), run.h()};
        run.do_callback();
        require(!run.active() && std::string_view(run.label()) == "Ожидание"
            && std::array{run.x(), run.y(), run.w(), run.h()} == geometry,
            L"The first queued Run request must immediately disable it and show Waiting without changing geometry");
        panel.button(cb::Control::Run).do_callback();
        require(sendKey(panel, FL_F + 5, FL_CTRL), L"Ctrl+F5 during a build must queue the same pending launch");
        pumpEvents();
        require(panel.operation() == cb::Operation::Building && !fs::exists(ran) && !fs::exists(launchCount),
            L"Repeated Run requests must neither interrupt the build nor launch an older executable before success");
        std::unique_ptr<char, decltype(&std::free)> text(panel.journalBuffer().text(), &std::free);
        const std::string_view journal(text.get());
        constexpr std::string_view planned = "Запуск после успешной сборки запланирован.";
        const auto message = journal.find(planned);
        require(message != journal.npos && journal.find(planned, message + planned.size()) == journal.npos
            && !panel.button(cb::Control::Run).active()
            && std::string_view(panel.button(cb::Control::Run).label()) == "Ожидание"
            && panel.button(cb::Control::Run).tooltip()
            && std::string_view(panel.button(cb::Control::Run).tooltip()).find("уже запланирован") != std::string_view::npos,
            L"Repeated requests must retain the disabled Waiting caption and give one journal acknowledgement");
    };
    require(hidden.app.targets.empty() && hidden.app.chosenTarget.empty(),
        L"The first queued-launch fixture must start before any executable or target choice exists");
    holdBuild();
    queueRun();
    writeFile(releaseBuild, "allow successful compilation and launch");
    requireSuccessfulChain();
    require(hidden.app.chosenTarget == L"chain" && fileContents(launchCount) == "x",
        L"The sole newly built executable must be selected and launched exactly once after repeated queue requests");
    fs::remove(holdEnabled);

    holdBuild(true);
    queueRun();
    writeFile(releaseBuild, "allow successful full rebuild and launch");
    requireSuccessfulChain();
    require(fileContents(launchCount) == "x", L"A queued full rebuild must launch the selected target exactly once");
    fs::remove(holdEnabled);
    fs::remove(ran);
    hidden.buildActions.choice = 3;
    panel.button(cb::Control::BuildMenu).do_callback();
    hidden.buildActions.choice.reset();
    require(panel.operation() == cb::Operation::Building, L"Combined Build menu action must start with a build");
    require(!panel.button(cb::Control::Run).active()
        && std::string_view(panel.button(cb::Control::Run).label()) == "Ожидание",
        L"Combined Build menu action must immediately display a disabled Waiting button");
    requireSuccessfulChain();

    fs::remove(ran);
    require(sendKey(panel, FL_F + 5), L"F5 must invoke the local combined build-and-run action");
    require(!panel.button(cb::Control::Run).active()
        && std::string_view(panel.button(cb::Control::Run).label()) == "Ожидание",
        L"F5 must immediately display a disabled Waiting button during its combined build");
    requireSuccessfulChain();
    fs::remove(ran);
    require(sendKey(panel, FL_F + 6), L"F6 must invoke the local build action");
    waitUntil([&] { return panel.operation() == cb::Operation::Idle; }, L"F6 build must finish");
    require(!panel.failed() && !fs::exists(ran), L"A build-only shortcut must not inherit the previous combined launch request");
    require(sendKey(panel, FL_F + 5, FL_CTRL), L"Ctrl+F5 must invoke the local run action");
    requireSuccessfulChain();

    fs::remove(ran);
    writeFile(project, "cmake_minimum_required(VERSION 3.24)\nproject(BuildAndRun LANGUAGES CXX)\n"
        "include(\"${CMAKE_CURRENT_SOURCE_DIR}/hold-build.cmake\")\n"
        "message(FATAL_ERROR \"DELIBERATE_CHAIN_FAILURE\")\n");
    holdBuild();
    queueRun();
    writeFile(releaseBuild, "allow the deliberate configure failure");
    waitUntil([&] { return panel.operation() == cb::Operation::Idle; }, L"Failing combined fixture did not finish");
    require(panel.failed() && !fs::exists(ran) && !fs::exists(launchCount),
        L"A failed ordinary build must drop queued Run requests without launching an earlier executable");
    requireIdleRun();
    fs::remove(holdEnabled);
    writeFile(project, validProject);
    panel.build();
    waitUntil([&] { return panel.operation() == cb::Operation::Idle; }, L"Ordinary build after queue failure did not finish");
    require(!panel.failed() && !fs::exists(ran) && !fs::exists(launchCount),
        L"A build after failure must not inherit the discarded queued launch");

    const auto started = directory / L"configuration-started";
    writeFile(project, "cmake_minimum_required(VERSION 3.24)\nproject(BuildAndRun LANGUAGES CXX)\n"
        "file(WRITE \"${CMAKE_CURRENT_SOURCE_DIR}/configuration-started\" \"ready\")\n"
        "execute_process(COMMAND \"${CMAKE_COMMAND}\" -E sleep 30)\nadd_executable(chain main.cpp)\n");
    panel.build();
    waitUntil([&] { return fs::exists(started) || panel.operation() == cb::Operation::Idle; },
        L"Combined build must begin its slow configure before cancellation");
    require(panel.operation() == cb::Operation::Building, L"Cancellable combined fixture must still be building");
    queueRun();
    require(sendKey(panel, FL_F + 6), L"F6 during a build must request cancellation");
    require(!panel.button(cb::Control::Run).active(), L"Requesting cancellation must immediately disable queued Run");
    const int cancelledJournalLength = panel.journalBuffer().length();
    panel.run();
    panel.button(cb::Control::Run).do_callback();
    require(sendKey(panel, FL_F + 5, FL_CTRL) && panel.journalBuffer().length() == cancelledJournalLength,
        L"Direct Run callbacks and Ctrl+F5 after cancellation must not restore the discarded launch request");
    waitUntil([&] { return panel.operation() == cb::Operation::Idle; }, L"Combined build cancellation did not finish");
    require(!panel.failed() && !fs::exists(ran) && !fs::exists(launchCount),
        L"Cancelling an ordinary build must discard queued Run requests without launching an older executable");
    requireIdleRun();

    writeFile(project, validProject);
    require(sendKey(panel, FL_F + 6), L"F6 must allow an ordinary build after combined cancellation");
    waitUntil([&] { return panel.operation() == cb::Operation::Idle; }, L"Build after combined cancellation did not finish");
    require(!panel.failed() && !fs::exists(ran) && !fs::exists(launchCount),
        L"Cancellation must clear the queued launch before the next ordinary build");
    require(sendKey(panel, FL_F + 5, FL_CTRL), L"Ctrl+F5 must allow standalone run after cancellation");
    requireSuccessfulChain();
}

void checkBuildActions(const fs::path& root) {
    const auto directory = root / L"build actions";
    const auto ini = directory / L"settings.ini";
    const auto project = directory / L"CMakeLists.txt";
    const auto buildDirectory = directory / L"build";
    const auto sentinel = buildDirectory / L"clean-sentinel";
    const auto selected = buildDirectory / L"selected-built";
    const auto other = buildDirectory / L"other-built";
    writeFile(directory / L"sentinel-new", "regenerated by full rebuild");
    // Generators may run a newly introduced custom command even if its output
    // exists. Only cleaning that output must make this fixture replace it.
    writeFile(directory / L"generate-sentinel.cmake", "if(NOT EXISTS \"${SENTINEL}\")\n"
        "file(COPY_FILE \"${SOURCE}\" \"${SENTINEL}\")\nendif()\n");
    writeFile(project, "cmake_minimum_required(VERSION 3.24)\nproject(BuildActions LANGUAGES NONE)\n"
        "add_custom_command(OUTPUT \"${CMAKE_BINARY_DIR}/clean-sentinel\" COMMAND \"${CMAKE_COMMAND}\" "
            "\"-DSENTINEL=${CMAKE_BINARY_DIR}/clean-sentinel\" \"-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/sentinel-new\" "
            "-P \"${CMAKE_CURRENT_SOURCE_DIR}/generate-sentinel.cmake\" "
            "DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/sentinel-new\" \"${CMAKE_CURRENT_SOURCE_DIR}/generate-sentinel.cmake\")\n"
        "add_custom_target(selected_target ALL DEPENDS \"${CMAKE_BINARY_DIR}/clean-sentinel\" "
            "COMMAND \"${CMAKE_COMMAND}\" -E touch \"${CMAKE_BINARY_DIR}/selected-built\")\n"
        "add_custom_target(other_target ALL DEPENDS \"${CMAKE_BINARY_DIR}/clean-sentinel\" "
            "COMMAND \"${CMAKE_COMMAND}\" -E touch \"${CMAKE_BINARY_DIR}/other-built\")\n");
    const auto sentinelContents = [&] {
        std::ifstream stream(sentinel, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };
    HiddenPanel hidden(ini);
    auto& panel = hidden.panel;
    hidden.app.settings.cmakeFile = project.wstring();
    hidden.app.settings.buildDirectory = buildDirectory.wstring();
    hidden.app.settings.compiler = cb::CompilerMode::Environment;
    hidden.app.settings.target = L"selected_target";
    hidden.app.settings.buildTests = true;
    panel.updateControls();

    auto& main = panel.button(cb::Control::Build);
    auto& arrow = panel.button(cb::Control::BuildMenu);
    require(arrow.active() && arrow.w() > 0 && arrow.y() == main.y() && arrow.h() == main.h()
            && arrow.x() >= main.x() + main.w() && arrow.x() <= main.x() + main.w() + 2,
        L"Build arrow must form an enabled right-hand part of the Build button");
    const int dismissedCalls = hidden.buildActions.calls;
    arrow.do_callback();
    require(hidden.buildActions.calls == dismissedCalls + 1
            && hidden.buildActions.labels == std::vector<std::string>{"Собрать", "Пересборка", "Очистить", "Собрать и запустить", "CMake"},
        L"Build dropdown must retain the first four actions and append configure-only CMake");
    require(std::ranges::none_of(hidden.buildActions.flags, [](int flags) { return flags & FL_MENU_INACTIVE; })
            && panel.operation() == cb::Operation::Idle && !fs::exists(selected) && !fs::exists(other),
        L"Dismissing Build actions must not start work or change its selectable entries");
    require(panel.buildMenu().textfont() == cb::appFont,
        L"Build actions must use the panel's menu font");
    const int keyboardCalls = hidden.buildActions.calls;
    require(sendKey(main, FL_Down) && sendKey(arrow, FL_F + 4) && sendKey(main, FL_Down, FL_ALT),
        L"Down, F4 and Alt+Down must open Build actions through actual button handlers");
    require(hidden.buildActions.calls == keyboardCalls + 3 && panel.operation() == cb::Operation::Idle,
        L"Keyboard-dismissed Build actions must leave the panel idle");
    require(!sendKey(main, FL_F + 4, FL_ALT),
        L"Alt+F4 on Build must retain normal window-close handling");

    const auto finish = [&](bool clean = false) {
        require(panel.operation() == cb::Operation::Building && !panel.buildProgress(),
            L"Each build or cleanup must start with unknown progress");
        if (!clean) {
            panel.onEvent({cb::EventKind::Progress, {}, {}, 0, {}, cb::BuildProgress{1, 2}});
            require(panel.buildProgress() && panel.buildProgress()->completed == 1 && panel.buildProgress()->total == 2,
                L"An active build must accept measured progress");
        }
        waitUntil([&] { return panel.operation() == cb::Operation::Idle; },
            L"Build action did not complete through the worker event bridge");
        require(!panel.failed() && !panel.buildProgress() && panel.lastBuildDuration().has_value(),
            L"Successful build or cleanup must clear progress and report its duration");
        {
            std::unique_ptr<char, decltype(&std::free)> text(panel.journalBuffer().text(), &std::free);
            require(std::string(text.get()).find(clean ? "Время очистки: " : "Время сборки: ") != std::string::npos,
                L"Successful action must name the measured build or cleanup duration in the journal");
        }
        require(hidden.app.settings.target == L"selected_target" && hidden.app.settings.buildTests
                && !hidden.app.settings.cleanFirst,
            L"Build actions must preserve the saved target and test setting without persisting clean-first");
    };

    writeFile(sentinel, "ordinary build must retain this file");
    hidden.buildActions.choice = 0;
    arrow.do_callback();
    finish();
    require(fs::exists(sentinel) && fs::exists(selected) && !fs::exists(other),
        L"Normal build action must retain artifacts and build only the selected target");

    hidden.buildActions.choice = 1;
    arrow.do_callback();
    finish();
    require(sentinelContents() == "regenerated by full rebuild" && fs::exists(selected) && fs::exists(other),
        L"Full rebuild action must actually clean stale outputs and rebuild all targets");

    writeFile(sentinel, "next ordinary build must retain this file");
    fs::remove(other);
    main.do_callback();
    finish();
    require(sentinelContents() == "next ordinary build must retain this file" && !fs::exists(other),
        L"Full rebuild must be one-shot: the next main-button build keeps artifacts and the selected target");

    fs::remove(selected);
    const auto cleanExecutable = directory / L"bin/run-fixture-clean.exe";
    fs::create_directories(cleanExecutable.parent_path());
    fs::copy_file(currentExecutable(), cleanExecutable);
    hidden.app.targets = {{L"remembered run target", cleanExecutable.wstring()}};
    hidden.app.chosenTarget = hidden.app.targets.front().name;
    hidden.app.chosenExecutable = hidden.app.targets.front().executable;
    panel.updateControls();
    hidden.buildActions.choice = 2;
    arrow.do_callback();
    require(!panel.button(cb::Control::Run).active(), L"Clean must not offer a queued launch");
    const int cleanJournalLength = panel.journalBuffer().length();
    panel.run();
    require(sendKey(panel, FL_F + 5, FL_CTRL) && panel.journalBuffer().length() == cleanJournalLength,
        L"Direct Run and Ctrl+F5 during Clean must not schedule a launch");
    finish(true);
    require(!fs::exists(sentinel) && !fs::exists(selected) && !fs::exists(other),
        L"Project cleanup must remove generated outputs without rebuilding any target");
    require(hidden.app.targets.empty() && hidden.app.chosenTarget == L"remembered run target"
            && !panel.button(cb::Control::Run).active() && !panel.button(cb::Control::RunMenu).active(),
        L"Cleanup must disable obsolete launch artifacts while retaining the remembered target choice");
    require(!fs::exists(marker({L"", cleanExecutable.wstring()})), L"Cleaning must not start the remembered executable");

    main.do_callback();
    finish();
    require(sentinelContents() == "regenerated by full rebuild" && fs::exists(selected) && !fs::exists(other),
        L"Normal build after cleanup must restore the selected target without making cleanup or full rebuild persistent");
    cb::AppState restored(ini.wstring());
    restored.load();
    require(!restored.settings.cleanFirst && restored.settings.target == L"selected_target" && restored.settings.buildTests,
        L"Restarting the panel must not turn a one-shot full rebuild into a saved preference");

    writeFile(project, "cmake_minimum_required(VERSION 3.24)\nproject(BuildActions LANGUAGES NONE)\n"
        "message(FATAL_ERROR \"BUILD_ACTIONS_CONFIGURE_FAILURE\")\n");
    main.do_callback();
    require(panel.operation() == cb::Operation::Building,
        L"Failing configure fixture must enter the building state");
    panel.onEvent({cb::EventKind::Progress, {}, {}, 0, {}, cb::BuildProgress{1, 2}});
    waitUntil([&] { return panel.operation() == cb::Operation::Idle; },
        L"Configure failure did not complete through the worker event bridge");
    require(panel.failed() && !panel.buildProgress() && panel.lastBuildDuration().has_value()
            && panel.button(cb::Control::BuildMenu).active(),
        L"Failed build must clear progress, report elapsed time and restore Build actions");
}

void checkConfigureAction(const fs::path& root) {
    const auto directory = root / L"configure without building";
    const auto ini = directory / L"settings.ini";
    const auto project = directory / L"CMakeLists.txt";
    const auto build = directory / L"build";
    const auto configureCount = directory / L"configure-count";
    const auto buildCount = directory / L"build-count";
    const auto fixtureExecutable = directory / L"bin/run-fixture-configure.exe";
    fs::create_directories(fixtureExecutable.parent_path());
    fs::copy_file(currentExecutable(), fixtureExecutable);
    const cb::Target remembered{L"Remembered run target", fixtureExecutable.wstring()};
    writeFile(directory / L"build-step.cmake", "file(APPEND \"${CMAKE_CURRENT_LIST_DIR}/build-count\" \"x\")\n");
    const auto projectText = [](const char* value, bool slow = false) {
        return std::string("cmake_minimum_required(VERSION 3.24)\nproject(ConfigureOnly LANGUAGES NONE)\n")
            + "file(APPEND \"${CMAKE_CURRENT_SOURCE_DIR}/configure-count\" \"x\")\n"
            + "set(CMAKEBUILD_UI_CONFIGURE_VALUE \"" + value + "\" CACHE STRING \"test fixture\" FORCE)\n"
            + (slow ? "file(WRITE \"${CMAKE_CURRENT_SOURCE_DIR}/configure-started\" \"ready\")\n"
                "execute_process(COMMAND \"${CMAKE_COMMAND}\" -E sleep 30)\n" : "")
            + "add_custom_target(build_step ALL COMMAND \"${CMAKE_COMMAND}\" -P \"${CMAKE_CURRENT_SOURCE_DIR}/build-step.cmake\")\n";
    };
    writeFile(project, projectText("one"));
    HiddenPanel hidden(ini);
    auto& panel = hidden.panel;
    hidden.app.settings.cmakeFile = project.wstring();
    hidden.app.settings.buildDirectory = build.wstring();
    hidden.app.settings.compiler = cb::CompilerMode::Environment;
    hidden.app.settings.target = L"build_step";
    hidden.app.settings.buildTests = true;
    hidden.app.chosenTarget = remembered.name;
    hidden.app.chosenExecutable = remembered.executable;
    hidden.app.targets = {remembered};
    hidden.app.recentProjects = {project.wstring()};
    const cb::RunSettings rememberedRun{L"--remembered \"one two\"", L"bin",
        {{L"REMEMBERED", L"Unicode 😀"}}};
    hidden.app.setRunSettings(remembered.name, rememberedRun);
    panel.updateControls();
    const auto finish = [&](const wchar_t* message) {
        waitUntil([&] { return panel.operation() == cb::Operation::Idle; }, message);
        require(!panel.failed(), L"Configure fixture action must succeed");
    };
    panel.button(cb::Control::Build).do_callback();
    finish(L"Initial ordinary build did not finish");
    require(fileContents(configureCount) == "x" && fileContents(buildCount) == "x",
        L"The first ordinary build must configure and then invoke the build tool exactly once");
    panel.button(cb::Control::Build).do_callback();
    finish(L"Repeat ordinary build did not finish");
    require(fileContents(configureCount) == "x" && fileContents(buildCount) == "xx",
        L"An unchanged ordinary build must invoke the build tool without repeating CMake configuration");

    writeFile(project, projectText("two"));
    hidden.buildActions.choice = 4;
    panel.button(cb::Control::BuildMenu).do_callback();
    hidden.buildActions.choice.reset();
    require(panel.operation() == cb::Operation::Building
        && std::string_view(panel.button(cb::Control::Build).label()) == "Отменить"
        && !panel.button(cb::Control::Run).active() && !panel.button(cb::Control::RunMenu).active()
        && !panel.button(cb::Control::PickMenu).active() && !panel.button(cb::Control::BuildMenu).active(),
        L"CMake menu action must enter the cancellable busy state and disable launch/project/action menus");
    const int configureJournalLength = panel.journalBuffer().length();
    panel.run();
    panel.button(cb::Control::Run).do_callback();
    require(sendKey(panel, FL_F + 5, FL_CTRL) && panel.journalBuffer().length() == configureJournalLength,
        L"Configure-only must reject direct Run and Ctrl+F5 rather than leave a queued launch");
    finish(L"Configure-only menu action did not finish");
    require(fileContents(configureCount) == "xx" && fileContents(buildCount) == "xx"
        && fileContents(build / L"CMakeCache.txt").find("CMAKEBUILD_UI_CONFIGURE_VALUE:STRING=two") != std::string::npos,
        L"Explicit CMake must update the real cache without compiling or invoking build targets");
    require(hidden.app.chosenTarget == remembered.name && !fs::exists(marker(remembered))
        && hidden.app.settings.target == L"build_step" && hidden.app.settings.buildTests,
        L"Configure-only must retain build and launch choices without starting the remembered executable");
    requireRunSettings(hidden.app.runSettingsFor(remembered.name), rememberedRun);
    {
        std::unique_ptr<char, decltype(&std::free)> text(panel.journalBuffer().text(), &std::free);
        require(std::string_view(text.get()).find("CMake: ") != std::string_view::npos
            && std::string_view(text.get()).find("Время сборки:") == std::string_view::npos,
            L"Real configure-only action must deliver its CMake duration through the UI event bridge");
    }
    panel.build();
    finish(L"Ordinary build after configure-only did not finish");
    require(fileContents(configureCount) == "xx" && fileContents(buildCount) == "xxx",
        L"A successful explicit CMake must make the next unchanged ordinary build skip configuration");

    const auto started = directory / L"configure-started";
    writeFile(project, projectText("slow", true));
    panel.configure();
    waitUntil([&] { return fs::exists(started) || panel.operation() == cb::Operation::Idle; },
        L"Cancellable configure-only fixture did not start");
    require(panel.operation() == cb::Operation::Building, L"Configure-only cancellation must occur while CMake is running");
    const int runMenuCalls = hidden.menu.calls;
    const int projectMenuCalls = hidden.projects.calls;
    panel.showRunTargets();
    panel.showRecentProjects();
    require(hidden.menu.calls == runMenuCalls && hidden.projects.calls == projectMenuCalls
        && !panel.chooseRecentProject(0) && !panel.chooseRunTarget(0),
        L"Active CMake must reject project and launch menu actions even through direct callbacks");
    panel.button(cb::Control::Build).do_callback();
    finish(L"Configure-only cancellation did not finish promptly");
    require(fileContents(buildCount) == "xxx" && !fs::exists(marker(remembered))
        && panel.lastBuildDuration().has_value(),
        L"Cancelling configure-only must not build, launch or lose its worker-measured duration");

    writeFile(project, projectText("recovered"));
    panel.configure();
    finish(L"Explicit CMake recovery after cancellation did not finish");
    require(fileContents(buildCount) == "xxx"
        && fileContents(build / L"CMakeCache.txt").find("CMAKEBUILD_UI_CONFIGURE_VALUE:STRING=recovered") != std::string::npos,
        L"Configure-only must recover from cancellation without invoking build targets");
    const auto configurationsBeforeBuild = fileContents(configureCount);
    panel.build();
    finish(L"Ordinary build after CMake recovery did not finish");
    require(fileContents(configureCount) == configurationsBeforeBuild && fileContents(buildCount) == "xxxx"
        && !fs::exists(marker(remembered)),
        L"Recovered configuration must be reusable and must not leave a pending launch request");
    {
        HiddenPanel restarted(ini);
        restarted.panel.build();
        waitUntil([&] { return restarted.panel.operation() == cb::Operation::Idle; },
            L"Ordinary build after panel restart did not finish");
        require(!restarted.panel.failed() && fileContents(configureCount) == configurationsBeforeBuild
            && fileContents(buildCount) == "xxxxx" && !fs::exists(marker(remembered)),
            L"An unchanged cached build must skip explicit configuration after panel restart too");
        requireRunSettings(restarted.app.runSettingsFor(remembered.name), rememberedRun);
    }

    writeFile(project, "cmake_minimum_required(VERSION 3.24)\nproject(ConfigureOnly LANGUAGES NONE)\n"
        "message(FATAL_ERROR \"DELIBERATE_CONFIGURE_ONLY_FAILURE\")\n");
    panel.configure();
    waitUntil([&] { return panel.operation() == cb::Operation::Idle; }, L"Failing configure-only action did not finish");
    require(panel.failed() && fileContents(buildCount) == "xxxxx" && !fs::exists(marker(remembered))
        && panel.lastBuildDuration().has_value() && panel.button(cb::Control::BuildMenu).active(),
        L"Failed configure-only must report failure and time, restore actions, and neither build nor run");
    requireRunSettings(hidden.app.runSettingsFor(remembered.name), rememberedRun);
}

struct ProcessTree {
    HANDLE parent{}, child{};
    explicit ProcessTree(const cb::Target& target) {
        const auto pidFile = fs::path(target.executable).parent_path() / L"process-tree.pid";
        DWORD parentId = 0, childId = 0;
        std::ifstream file(pidFile);
        file >> parentId >> childId;
        require(file.good() || file.eof(), L"Cannot read test-owned process IDs");
        require(parentId && childId, L"Both process fixture IDs must be available");
        parent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parentId);
        child = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, childId);
        require(parent && child, L"Cannot inspect the test-owned process tree");
    }
    ~ProcessTree() {
        if (parent) CloseHandle(parent);
        if (child) CloseHandle(child);
    }
    bool alive() const {
        return WaitForSingleObject(parent, 0) == WAIT_TIMEOUT && WaitForSingleObject(child, 0) == WAIT_TIMEOUT;
    }
    bool stopped() const {
        return WaitForSingleObject(parent, 0) == WAIT_OBJECT_0 && WaitForSingleObject(child, 0) == WAIT_OBJECT_0;
    }
};

void checkLifecycle(const fs::path& root) {
    const auto directory = root / L"операции с дочерними процессами";
    const auto ini = directory / L"settings.ini";
    const auto project = directory / L"CMakeLists.txt";
    const auto started = directory / L"configuration-started";
    writeFile(project, "cmake_minimum_required(VERSION 3.24)\nproject(SlowConfigure LANGUAGES NONE)\n"
        "file(WRITE \"${CMAKE_CURRENT_SOURCE_DIR}/configuration-started\" \"ready\")\n"
        "execute_process(COMMAND \"${CMAKE_COMMAND}\" -E sleep 30)\n");
    const cb::Target slow{L"Медленный процесс", (directory / L"bin/run-fixture-slow.exe").wstring()};
    const cb::Target child{L"Дочерний процесс", (directory / L"bin/run-fixture-child.exe").wstring()};
    fs::create_directories(fs::path(slow.executable).parent_path());
    fs::copy_file(currentExecutable(), slow.executable);
    fs::copy_file(currentExecutable(), child.executable);
    {
        HiddenPanel hidden(ini);
        auto& panel = hidden.panel;
        hidden.app.settings.cmakeFile = project.wstring();
        hidden.app.settings.buildDirectory = L"slow-build";
        hidden.app.settings.compiler = cb::CompilerMode::Environment;
        hidden.app.targets = {slow, child};
        hidden.app.chosenTarget = slow.name;
        hidden.app.chosenExecutable = slow.executable;
        hidden.app.recentProjects = {project.wstring()};
        panel.updateControls();

        panel.onEvent({cb::EventKind::BuildSucceeded, L"Previous build", {}, 0, 1230ms});
        require(panel.lastBuildDuration() == 1230ms, L"Cancellation fixture must start with a previous duration");
        panel.button(cb::Control::Build).do_callback();
        require(panel.operation() == cb::Operation::Building,
            L"Build callback must immediately enter the building state");
        require(!panel.button(cb::Control::RunMenu).active() && panel.button(cb::Control::Run).active(),
            L"Ordinary builds must enable queued Run while target selection remains disabled");
        require(!panel.button(cb::Control::BuildMenu).active(),
            L"Build actions must be disabled throughout configuration/build");
        const int buildMenuCalls = hidden.buildActions.calls;
        panel.button(cb::Control::BuildMenu).do_callback();
        panel.showBuildActions();
        require(hidden.buildActions.calls == buildMenuCalls
                && !sendKey(panel.button(cb::Control::BuildMenu), FL_F + 4),
            L"Busy Build actions must reject callbacks and keyboard menu shortcuts");
        require(!panel.chooseRunTarget(1), L"Target changes must be rejected during a real build");
        const int projectMenuCalls = hidden.projects.calls;
        panel.showRecentProjects();
        require(!panel.button(cb::Control::PickMenu).active() && hidden.projects.calls == projectMenuCalls
            && !panel.chooseRecentProject(0), L"Recent project menus and changes must be rejected during a real build");
        require(std::string(panel.button(cb::Control::Build).label()) == "Отменить",
            L"Real build must label its button Cancel");
        waitUntil([&] { return fs::exists(started) || panel.operation() == cb::Operation::Idle; },
            L"Slow configuration did not begin");
        require(fs::exists(started), L"Real CMake configuration must begin before cancellation");
        require(!panel.lastBuildDuration(), L"Starting a build must clear its previous duration");
        require(!panel.buildProgress(), L"Configuration must use unknown progress until the build tool reports work");
        panel.onEvent({cb::EventKind::Progress, {}, {}, 0, {}, cb::BuildProgress{0, 0}});
        panel.onEvent({cb::EventKind::Progress, {}, {}, 0, {}, cb::BuildProgress{3, 2}});
        require(!panel.buildProgress(), L"Zero-total and over-total progress must be rejected");
        panel.onEvent({cb::EventKind::Progress, {}, {}, 0, {}, cb::BuildProgress{0, 4}});
        require(panel.buildProgress() && panel.buildProgress()->completed == 0 && panel.buildProgress()->total == 4,
            L"Measured progress may begin at zero completed steps");
        panel.onEvent({cb::EventKind::Progress, {}, {}, 0, {}, cb::BuildProgress{2, 4}});
        panel.onEvent({cb::EventKind::Progress, {}, {}, 0});
        panel.onEvent({cb::EventKind::Progress, {}, {}, 0, {}, cb::BuildProgress{1, 4}});
        panel.onEvent({cb::EventKind::Progress, {}, {}, 0, {}, cb::BuildProgress{9, 4}});
        require(panel.buildProgress() && panel.buildProgress()->completed == 2 && panel.buildProgress()->total == 4,
            L"Invalid and regressing progress must preserve the last valid reading");
        panel.onEvent({cb::EventKind::Progress, {}, {}, 0, {}, cb::BuildProgress{1, 3}});
        require(panel.buildProgress() && panel.buildProgress()->completed == 1 && panel.buildProgress()->total == 3,
            L"A changed build-tool total must update the progress reading");
        panel.button(cb::Control::Build).do_callback();
        waitUntil([&] { return panel.operation() == cb::Operation::Idle; },
            L"Build cancellation must complete without blocking the UI");
        require(std::string(panel.button(cb::Control::Build).label()) == "Собрать",
            L"Build completion must restore its button label");
        require(panel.lastBuildDuration() && *panel.lastBuildDuration() > 0ms,
            L"Real cancellation must deliver the worker's measured duration to the panel");
        require(!panel.buildProgress() && panel.button(cb::Control::BuildMenu).active(),
            L"Cancelled build completion must clear progress and restore Build actions");
        panel.onEvent({cb::EventKind::Progress, {}, {}, 0, {}, cb::BuildProgress{1, 2}});
        require(!panel.buildProgress(), L"Delayed progress after build completion must be ignored");
        {
            std::unique_ptr<char, decltype(&std::free)> text(panel.journalBuffer().text(), &std::free);
            require(std::string(text.get()).find("Время сборки: ") != std::string::npos,
                L"Real cancellation must append elapsed time through the FLTK event bridge");
        }

        // Cancellation has refreshed no artifacts: restore test-owned runtime
        // targets, then validate stop and close against actual live children.
        hidden.app.targets = {slow, child};
        hidden.app.chosenTarget = slow.name;
        hidden.app.chosenExecutable = slow.executable;
        panel.updateControls();
        panel.button(cb::Control::Run).do_callback();
        waitUntil([&] { return fs::exists(marker(slow)) && fs::exists(marker(child)); },
            L"Test application and its child did not start");
        ProcessTree running(slow);
        require(running.alive(), L"Process-tree fixtures must be alive before stop");
        waitUntil([&] {
            std::unique_ptr<char, decltype(&std::free)> text(panel.journalBuffer().text(), &std::free);
            return std::string(text.get()).find("Русский UTF-8 вывод из рабочего процесса 😀") != std::string::npos;
        }, L"FLTK event bridge must deliver real Unicode process output to its UTF-8 journal");
        require(panel.operation() == cb::Operation::Running && !panel.button(cb::Control::RunMenu).active(),
            L"Run selection must be disabled during an actual running application");
        require(!panel.button(cb::Control::BuildMenu).active(),
            L"Build actions must be disabled during a running application");
        const int runningBuildMenuCalls = hidden.buildActions.calls;
        panel.button(cb::Control::BuildMenu).do_callback();
        panel.showBuildActions();
        panel.onEvent({cb::EventKind::Progress, {}, {}, 0, {}, cb::BuildProgress{1, 2}});
        require(hidden.buildActions.calls == runningBuildMenuCalls && !panel.buildProgress()
                && !sendKey(panel.button(cb::Control::BuildMenu), FL_Down),
            L"Running application must reject Build actions and stray build progress");
        require(!panel.chooseRunTarget(1), L"Target changes must be rejected while running");
        panel.showRecentProjects();
        require(!panel.button(cb::Control::PickMenu).active() && hidden.projects.calls == projectMenuCalls
            && !panel.chooseRecentProject(0), L"Recent project menus and changes must be rejected while running");
        require(std::string(panel.button(cb::Control::Run).label()) == "Остановить",
            L"Real run must label its button Stop");
        require(sendKey(panel, FL_F + 5, FL_CTRL), L"Ctrl+F5 must stop the running application and its child");
        waitUntil([&] { return running.stopped() && panel.operation() == cb::Operation::Idle; },
            L"Stop must terminate the entire test-owned process tree");
        require(std::string(panel.button(cb::Control::Run).label()) == "Запустить",
            L"Run completion must restore its button label");

        fs::remove(marker(slow));
        fs::remove(marker(child));
        panel.button(cb::Control::Run).do_callback();
        waitUntil([&] { return fs::exists(marker(slow)) && fs::exists(marker(child)); },
            L"Test application did not restart before closing the panel");
        ProcessTree closing(slow);
        require(closing.alive(), L"Process fixtures must be alive before close");
        panel.closePanel();
        require(closing.stopped(), L"Closing the panel must join the engine and terminate its process tree");
        pumpEvents();
        require(!panel.shown(), L"Closed test panel must remain hidden");
    }

    // Reuse the same temporary project, but run the owned parent/child fixture
    // from execute_process() so close while configuring has observable children.
    fs::remove(started);
    fs::remove(marker(slow));
    fs::remove(marker(child));
    writeFile(project, "cmake_minimum_required(VERSION 3.24)\nproject(SlowConfigure LANGUAGES NONE)\n"
        "file(WRITE \"${CMAKE_CURRENT_SOURCE_DIR}/configuration-started\" \"ready\")\n"
        "execute_process(COMMAND \"${CMAKE_CURRENT_SOURCE_DIR}/bin/run-fixture-slow.exe\")\n");
    {
        HiddenPanel hidden(ini);
        hidden.panel.button(cb::Control::Build).do_callback();
        waitUntil([&] {
            return (fs::exists(started) && fs::exists(marker(slow)) && fs::exists(marker(child)))
                || hidden.panel.operation() == cb::Operation::Idle;
        }, L"CMake configuration and its child fixtures must begin before closing the panel");
        require(hidden.panel.operation() == cb::Operation::Building,
            L"Close-during-build test must still have a live CMake configuration");
        ProcessTree configuring(slow);
        require(configuring.alive(), L"CMake's test-owned descendants must be alive before closing");
        const auto closeStart = std::chrono::steady_clock::now();
        hidden.panel.closePanel();
        require(std::chrono::steady_clock::now() - closeStart < 5s,
            L"Closing while configuring must cancel and join without waiting for the slow command");
        require(configuring.stopped(), L"Closing during configuration must terminate CMake's whole descendant tree");
        pumpEvents();
        require(!hidden.panel.shown(), L"Close during configuration must keep the test panel hidden");
    }
}

int runFixture(const fs::path& executable, int argc, wchar_t** argv) {
    const cb::Target fixture{L"", executable.wstring()};
    if (executable.filename() == L"run-fixture-arguments.exe") {
        std::string arguments;
        for (int index = 1; index < argc; ++index) arguments += quoted(argv[index]) + '\n';
        writeFile(executable.parent_path() / L"arguments.txt", arguments);
        writeFile(executable.parent_path() / L"environment.txt", utf8(environmentValue(L"CMAKEBUILD_RUN_UI_VALUE"))
            + "\n" + utf8(environmentValue(L"TEMP")));
    }
    if (executable.filename() == L"run-fixture-slow.exe") {
        const auto childExecutable = executable.parent_path() / L"run-fixture-child.exe";
        auto command = L"\"" + childExecutable.wstring() + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION child{};
        require(CreateProcessW(childExecutable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup, &child) != FALSE,
            L"Cannot create the test-owned child process");
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        writeFile(executable.parent_path() / L"process-tree.pid", std::to_string(GetCurrentProcessId())
            + " " + std::to_string(child.dwProcessId));
        std::cout << "Русский UTF-8 вывод из рабочего процесса 😀\n" << std::flush;
    }
    writeFile(marker(fixture), utf8(fs::current_path().wstring()));
    if (executable.filename() == L"run-fixture-slow.exe" || executable.filename() == L"run-fixture-child.exe")
        std::this_thread::sleep_for(30s);
    return 0;
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        const auto executable = currentExecutable();
        if (executable.filename().wstring().starts_with(L"run-fixture-")) return runFixture(executable, argc, argv);
        Fl::lock();
        TempDirectory temporary;
        const fs::path ini = temporary.root / L"settings.ini";
        writeFile(ini, std::string("\xFF\xFE", 2));
        const ProjectFixture first(temporary.root, L"проект [one] 100%", L"custom-build", L"Release",
            L"Alpha", L"Beta & tool", L"run-fixture-alpha.exe", L"run-fixture-beta.exe");
        const ProjectFixture second(temporary.root, L"проект two", L"debug-output", L"Debug",
            L"Delta", L"Gamma приложение", L"run-fixture-delta.exe", L"run-fixture-gamma.exe");
        checkSettingsSelection(ini, first);
        checkLegacySettings(temporary.root, first);
        checkLiteralMenuAndExpiredTargets(temporary.root);
        checkRuns(ini, first, second);
        checkCompleteProfilesAndRecentProjects(temporary.root);
        checkLaunchSettingsDialog(temporary.root);
        checkConfiguredLaunch(temporary.root);
        checkBuildAndRun(temporary.root);
        checkBuildDuration(temporary.root);
        checkBuildActions(temporary.root);
        checkConfigureAction(temporary.root);
        checkLifecycle(temporary.root);
        std::wcout << L"FLTK Run dropdown and per-project selection checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
