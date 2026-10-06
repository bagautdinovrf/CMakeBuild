#include "fixtures.hpp"
#include <atomic>
#include <functional>

namespace {
using namespace harness;

struct OwnedController {
    cb::AppState state;
    std::atomic_uint wakes{0};
    cb::na::Controller controller;
    static cb::AppState load(const fs::path& file) {
        cb::AppState state(file.wstring());
        state.load();
        return state;
    }
    explicit OwnedController(const fs::path& file)
        : state(load(file)), controller(state, [this] { ++wakes; }) {}
};

void waitUntil(cb::na::Controller& controller, const std::function<bool()>& condition, const wchar_t* message,
    std::chrono::seconds timeout = 15s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        controller.drainEvents();
        if (condition()) return;
        std::this_thread::sleep_for(5ms);
    }
    std::cerr << controller.journal().text() << '\n';
    require(false, message);
}

size_t targetIndex(const cb::AppState& state, const std::wstring& name) {
    const auto found = std::ranges::find_if(state.targets, [&](const auto& target) { return target.name == name; });
    require(found != state.targets.end(), L"Expected executable target is missing");
    return static_cast<size_t>(found - state.targets.begin());
}

void seed(const fs::path& ini, const ProjectFixture& fixture) {
    cb::AppState state(ini.wstring());
    state.load();
    state.settings = fixture.settings();
    state.targets = cb::readExecutableTargets(state.settings);
    require(state.targets.size() == 2, L"File API must exclude unbuilt and nonexecutable artifacts");
    state.chosenTarget = fixture.targets.front().name;
    state.chosenExecutable = fixture.targets.front().executable;
    state.save();
}

void runAndWait(OwnedController& owned, const cb::Target& target) {
    fs::remove(marker(target));
    require(owned.controller.run(), L"Controller must handle the selected launch");
    waitUntil(owned.controller, [&] {
        return fs::exists(marker(target)) && owned.controller.operation() == cb::na::Operation::Idle;
    }, L"Selected fixture failed to finish");
    require(!owned.controller.failed(), L"Selected fixture must complete successfully");
    std::ifstream file(marker(target), std::ios::binary);
    const std::string directory((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    require(directory == utf8(fs::path(target.executable).parent_path().wstring()),
        L"Selected executable must use its own directory as working directory");
    require(owned.wakes.load() > 0, L"Real Engine events must wake the controller's UI bridge");
}

void checkProjects(const fs::path& ini, const ProjectFixture& first, const ProjectFixture& second) {
    seed(ini, first);
    seed(ini, second);
    require(WritePrivateProfileStringW(L"PrivateSection", L"UnknownKey", L"Сохранить значение", ini.c_str()) != FALSE,
        L"Cannot seed unrelated settings");
    {
        OwnedController owned(ini);
        auto& ctl = owned.controller;
        ctl.selectProject(first.project.wstring());
        require(owned.state.settings.buildDirectory == first.buildDirectory && owned.state.settings.configuration == first.configuration,
            L"Each project must restore its independent build directory and configuration");
        require(owned.state.chosenTarget == first.targets.front().name && owned.state.targets.size() == 2,
            L"Project switch must restore its File API inventory and remembered launch choice");
        owned.state.chosenTarget.clear();
        require(!ctl.run(), L"An unchosen launch must request the actual UI menu");
        require(ctl.chooseRunTarget(targetIndex(owned.state, first.targets.back().name)), L"Cannot choose a built target");
        require(!fs::exists(marker(first.targets.back())), L"Choosing a target must not start it");
        require(!ctl.chooseRunTarget(owned.state.targets.size()), L"Invalid target selection must be rejected");
        auto reversed = first.targets;
        std::ranges::reverse(reversed);
        ctl.onEvent({cb::EventKind::BuildSucceeded, L"Fixture rebuilt", std::move(reversed)});
        require(owned.state.chosenTarget == first.targets.back().name, L"Reordered rebuild must preserve target identity");
        runAndWait(owned, first.targets.back());
        require(!fs::exists(marker(first.targets.front())), L"Controller must launch the chosen executable only");
        ctl.selectProject(second.project.wstring());
        require(owned.state.settings.buildDirectory == second.buildDirectory && owned.state.chosenTarget == second.targets.front().name,
            L"Second project must keep its own settings and target");
        require(ctl.chooseRunTarget(targetIndex(owned.state, second.targets.back().name)), L"Cannot choose the second project's executable");
        runAndWait(owned, second.targets.back());
        ctl.selectProject(first.project.wstring());
        require(owned.state.chosenTarget == first.targets.back().name, L"Returning must restore independent launch identity");
        auto alias = (first.project.parent_path() / L"." / first.project.filename()).wstring();
        CharUpperBuffW(alias.data(), static_cast<DWORD>(alias.size()));
        ctl.selectProject(alias);
        require(owned.state.chosenTarget == first.targets.back().name, L"Equivalent Windows paths must identify the same project");
        require(owned.state.recentProjects.size() == 2, L"MRU must merge Windows path aliases");
        const auto active = owned.state.settings.cmakeFile;
        ctl.selectProject((ini.parent_path() / L"unavailable/CMakeLists.txt").wstring());
        require(ctl.failed() && owned.state.settings.cmakeFile == active && owned.state.recentProjects.size() == 2,
            L"Unavailable project must preserve the current project and history");
        const cb::RunSettings firstRun{L"--first \"Русские аргументы\"", L".", {{L"MY_VALUE", L"Первый"}, {L"EMPTY", L""}}};
        const cb::RunSettings secondRun{L"--second", L"", {{L"MY_VALUE", L"Второй"}}};
        ctl.applySettings(owned.state.settings, owned.state.chosenTarget, false,
            {{first.targets.front().name, firstRun}, {first.targets.back().name, secondRun}});
        ctl.selectProject(second.project.wstring());
        require(owned.state.runSettingsFor(first.targets.front().name).arguments.empty(), L"Launch profiles must belong to their project");
        ctl.selectProject(first.project.wstring());
        require(owned.state.runSettingsFor(first.targets.front().name).arguments == firstRun.arguments
            && owned.state.runSettingsFor(first.targets.back().name).environment == secondRun.environment,
            L"Applying settings must persist all visited per-target drafts independently");
        // Restore empty launch profiles so the remaining legacy working-directory checks stay meaningful.
        ctl.applySettings(owned.state.settings, owned.state.chosenTarget, false,
            {{first.targets.front().name, {}}, {first.targets.back().name, {}}});
        ctl.save();
    }
    first.writeReplies(true);
    {
        OwnedController restarted(ini);
        require(restarted.state.targets.size() == 2 && restarted.state.chosenTarget == first.targets.back().name,
            L"Restart must restore File API targets and saved choice without configuring CMake");
        std::array<wchar_t, 128> value{};
        GetPrivateProfileStringW(L"PrivateSection", L"UnknownKey", L"", value.data(), static_cast<DWORD>(value.size()), ini.c_str());
        require(std::wstring(value.data()) == L"Сохранить значение", L"Forked state must preserve unrelated legacy INI data");
        const fs::path chosen(first.targets.back().executable);
        auto absent = chosen;
        absent += L".missing";
        fs::rename(chosen, absent);
        restarted.controller.onEvent({cb::EventKind::BuildSucceeded, L"Missing chosen EXE", first.targets});
        require(restarted.state.targets.size() == 1 && restarted.state.chosenTarget == first.targets.back().name,
            L"A temporarily missing remembered EXE must not choose the sole other executable");
        require(!restarted.controller.run(), L"Missing selected executable must request the menu");
        auto settings = restarted.state.settings;
        settings.buildTests = true;
        restarted.controller.applySettings(settings, restarted.state.chosenTarget, false);
        require(restarted.state.settings.buildTests && restarted.state.chosenTarget == first.targets.back().name,
            L"Applying another setting must preserve a missing run choice");
        fs::rename(absent, chosen);
        restarted.state.restoreTargets();
        restarted.controller.save();
    }
    fs::remove_all(first.build / L".cmake/api/v1/reply");
    {
        OwnedController fallback(ini);
        require(fallback.state.targets.size() == 1 && fallback.state.findChosenTarget(),
            L"Existing saved EXE must remain runnable without File API replies");
        runAndWait(fallback, first.targets.back());
    }
    fs::remove(first.targets.back().executable);
    {
        OwnedController missing(ini);
        require(missing.state.targets.empty() && !missing.controller.run(), L"Missing saved EXE must not be runnable");
    }
}

struct ProcessTree {
    HANDLE parent{}, child{};
    explicit ProcessTree(const cb::Target& target) {
        DWORD parentId = 0, childId = 0;
        std::ifstream file(fs::path(target.executable).parent_path() / L"process-tree.pid");
        file >> parentId >> childId;
        require(parentId && childId, L"Cannot read test-owned process tree IDs");
        parent = OpenProcess(SYNCHRONIZE, FALSE, parentId);
        child = OpenProcess(SYNCHRONIZE, FALSE, childId);
        require(parent && child, L"Cannot inspect test-owned process tree");
    }
    ~ProcessTree() { if (parent) CloseHandle(parent); if (child) CloseHandle(child); }
    bool alive() const { return WaitForSingleObject(parent, 0) == WAIT_TIMEOUT && WaitForSingleObject(child, 0) == WAIT_TIMEOUT; }
    bool stopped() const { return WaitForSingleObject(parent, 0) == WAIT_OBJECT_0 && WaitForSingleObject(child, 0) == WAIT_OBJECT_0; }
};

void checkLifecycle(const fs::path& root) {
    const auto directory = root / L"операции с дочерними процессами";
    const auto project = directory / L"CMakeLists.txt";
    const auto ini = directory / L"settings.ini";
    const auto started = directory / L"configuration-started";
    writeFile(project, "cmake_minimum_required(VERSION 3.24)\nproject(Slow LANGUAGES NONE)\n"
        "file(WRITE \"${CMAKE_CURRENT_SOURCE_DIR}/configuration-started\" \"ready\")\n"
        "execute_process(COMMAND \"${CMAKE_COMMAND}\" -E sleep 30)\n");
    const cb::Target slow{L"Медленный процесс", (directory / L"bin/run-fixture-slow.exe").wstring()};
    const cb::Target child{L"Дочерний процесс", (directory / L"bin/run-fixture-child.exe").wstring()};
    fs::create_directories(fs::path(slow.executable).parent_path());
    fs::copy_file(currentExecutable(), slow.executable);
    fs::copy_file(currentExecutable(), child.executable);
    {
        OwnedController owned(ini);
        auto& ctl = owned.controller;
        owned.state.settings.cmakeFile = project.wstring();
        owned.state.settings.buildDirectory = L"slow-build";
        owned.state.settings.compiler = cb::CompilerMode::Environment;
        owned.state.targets = {slow, child};
        owned.state.chosenTarget = slow.name;
        owned.state.chosenExecutable = slow.executable;
        ctl.build();
        require(ctl.operation() == cb::na::Operation::Building && !ctl.chooseRunTarget(1),
            L"Real building state must reject target changes");
        require(ctl.canQueueRun() && ctl.run() && ctl.runQueued(), L"First real build must accept a deferred run before any EXE is built");
        const auto queueLog = ctl.journal().text();
        ctl.run();
        require(ctl.journal().text() == queueLog, L"Repeated queue request must log only once");
        ctl.onEvent({cb::EventKind::Progress, L"", {}, 0, {}, cb::BuildProgress{42, 100}});
        ctl.onEvent({cb::EventKind::Progress, L"", {}, 0, {}, cb::BuildProgress{3, 100}});
        require(ctl.buildProgress() == cb::BuildProgress{42, 100}, L"Progress must reject a regression within the same total");
        waitUntil(ctl, [&] { return fs::exists(started) || ctl.operation() == cb::na::Operation::Idle; }, L"Slow CMake did not begin");
        require(fs::exists(started), L"Real CMake must begin before cancellation");
        ctl.build();
        require(ctl.cancellationRequested() && !ctl.runQueued() && !ctl.canQueueRun(), L"Cancel must immediately reset and disable deferred launch");
        waitUntil(ctl, [&] { return ctl.operation() == cb::na::Operation::Idle; }, L"Cancel must complete");
        require(!ctl.runQueued() && !ctl.buildProgress() && ctl.lastBuildDuration().has_value() && !fs::exists(marker(slow)),
            L"Cancelled build must retain worker duration, clear progress and never launch");
        owned.state.targets = {slow, child};
        owned.state.chosenTarget = slow.name;
        owned.state.chosenExecutable = slow.executable;
        require(ctl.run(), L"Cannot start the owned process tree");
        waitUntil(ctl, [&] { return fs::exists(marker(slow)) && fs::exists(marker(child)); }, L"Owned process tree did not start");
        ProcessTree running(slow);
        require(running.alive() && ctl.operation() == cb::na::Operation::Running && !ctl.chooseRunTarget(1),
            L"Running state must reject target changes");
        waitUntil(ctl, [&] { return ctl.journal().text().find("Русский UTF-8 вывод из рабочего процесса 😀") != std::string::npos; },
            L"Worker events must deliver actual Unicode output to the UTF-8 journal");
        ctl.run();
        waitUntil(ctl, [&] { return running.stopped() && ctl.operation() == cb::na::Operation::Idle; },
            L"Stop must terminate both owned processes");
        fs::remove(marker(slow)); fs::remove(marker(child));
        ctl.run();
        waitUntil(ctl, [&] { return fs::exists(marker(slow)) && fs::exists(marker(child)); }, L"Owned process tree did not restart");
        ProcessTree closing(slow);
        ctl.close();
        require(closing.stopped() && ctl.closing(), L"Close while running must terminate the whole tree and join Engine");
        ctl.close();
    }
    fs::remove(started); fs::remove(marker(slow)); fs::remove(marker(child));
    writeFile(project, "cmake_minimum_required(VERSION 3.24)\nproject(Slow LANGUAGES NONE)\n"
        "file(WRITE \"${CMAKE_CURRENT_SOURCE_DIR}/configuration-started\" \"ready\")\n"
        "execute_process(COMMAND \"${CMAKE_CURRENT_SOURCE_DIR}/bin/run-fixture-slow.exe\")\n");
    {
        OwnedController owned(ini);
        owned.controller.build();
        waitUntil(owned.controller, [&] { return (fs::exists(started) && fs::exists(marker(slow)) && fs::exists(marker(child)))
            || owned.controller.operation() == cb::na::Operation::Idle; }, L"Configuration process tree did not begin");
        require(owned.controller.operation() == cb::na::Operation::Building, L"Close fixture must still be configuring");
        owned.controller.run();
        require(owned.controller.runQueued(), L"Close fixture must queue launch during the build");
        ProcessTree configuring(slow);
        const auto closeStart = std::chrono::steady_clock::now();
        owned.controller.close();
        require(configuring.stopped() && std::chrono::steady_clock::now() - closeStart < 5s,
            L"Close while building must promptly join and terminate CMake's descendants");
        require(!owned.controller.runQueued(), L"Close must discard pending launch");
    }
}

void checkJournal() {
    cb::na::Journal journal;
    journal.append(L"Первая строка 😀\r\nВторая строка\n");
    require(journal.text() == "Первая строка 😀\nВторая строка\n", L"Journal must explicitly convert UTF-16 to UTF-8 and normalize newlines");
    const auto revision = journal.revision();
    std::wstring batch;
    for (int row = 0; row < 600; ++row) batch += L"Строка 😀 " + std::wstring(160, L'я') + L"\n";
    for (int count = 0; count < 8; ++count) journal.append(batch);
    require(journal.text().size() < 1000000 && journal.totalRemovedBytes() > 0 && journal.revision() > revision,
        L"Large journal must remain bounded and report prefix removals to its UI view");
    require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, journal.text().data(), static_cast<int>(journal.text().size()), nullptr, 0) > 0,
        L"Journal trimming must preserve complete UTF-8 code points");
    journal.clear();
    require(journal.text().empty(), L"Clear must remove the visible journal");
}

void checkSuccessfulBuild(const fs::path& root) {
    const auto directory = root / L"успешная сборка C++";
    const auto project = directory / L"CMakeLists.txt";
    const auto ini = directory / L"settings.ini";
    writeFile(project,
        "cmake_minimum_required(VERSION 3.24)\nproject(ControllerBridge LANGUAGES CXX)\n"
        "set(CMAKE_CXX_STANDARD 23)\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
        "add_executable(BridgeHello hello.cpp)\nadd_executable(BridgeTool tool.cpp)\n"
        "if(MSVC)\n  target_compile_options(BridgeHello PRIVATE /utf-8)\nendif()\n");
    const std::string helloSource = "#include <iostream>\n#include <fstream>\n#include <filesystem>\n#include <cstdlib>\nint main(int argc, char** argv) { std::cout << \"Настоящая сборка и запуск UTF-8\\n\"; if(argc>1) std::cout << argv[1] << '\\n'; if(auto* v=std::getenv(\"CB_VARIANT_VALUE\")) std::cout << v << '\\n'; const auto d=std::filesystem::current_path().generic_u8string(); std::cout.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size())); std::cout << '\\n'; std::ofstream(\"bridge.launched\") << \"ran\"; }\n";
    writeFile(directory / L"hello.cpp", helloSource);
    writeFile(directory / L"tool.cpp", "int main() { return 0; }\n");
    {
        OwnedController owned(ini);
        owned.controller.selectProject(project.wstring());
        owned.state.settings.compiler = cb::CompilerMode::Msvc;
        owned.state.settings.buildDirectory = L"successful-build";
        owned.controller.build(cb::na::BuildAction::BuildAndRun);
        require(owned.controller.operation() == cb::na::Operation::Building,
            L"Actual successful-build fixture must enter Building");
        waitUntil(owned.controller, [&] { return owned.controller.operation() == cb::na::Operation::Idle; },
            L"Actual two-target C++ build must complete", 60s);
        if (owned.controller.failed()) std::cerr << owned.controller.journal().text() << '\n';
        require(!owned.controller.failed() && owned.state.targets.size() == 2,
            L"Real BuildSucceeded and Targets events must deliver both newly built executables");
        require(owned.controller.lastBuildDuration().has_value() && owned.controller.runSelectionRequested()
            && !owned.controller.runQueued(), L"First multi-target build must retain duration and request deferred target selection");
        require(owned.wakes.load() > 0, L"Actual successful build must wake the UI bridge");
        owned.state.setRunSettings(L"BridgeHello", {L"--saved-argument", L".", {{L"CB_VARIANT_VALUE", L"saved-environment"}}});
        require(owned.controller.chooseRunTarget(targetIndex(owned.state, L"BridgeHello")),
            L"Actual build result must be selectable");
        require(owned.controller.operation() == cb::na::Operation::Running && !owned.controller.runSelectionRequested(),
            L"Choosing the deferred target must launch exactly once");
        waitUntil(owned.controller, [&] { return owned.controller.operation() == cb::na::Operation::Idle; },
            L"Freshly built target must complete");
        require(!owned.controller.failed() && owned.controller.journal().text().find("Настоящая сборка и запуск UTF-8") != std::string::npos,
            L"Freshly compiled executable output must reach the real UTF-8 journal");
        require(owned.controller.journal().text().find("--saved-argument") != std::string::npos
            && owned.controller.journal().text().find("saved-environment") != std::string::npos
            && owned.controller.journal().text().find(utf8(directory.generic_wstring())) != std::string::npos,
            L"Queued launch must use saved target arguments and environment");
        owned.state.settings.target = L"BridgeHello";
        owned.controller.build(cb::na::BuildAction::Rebuild);
        require(owned.controller.run() && owned.controller.runQueued(), L"Rebuild must also accept queued run");
        waitUntil(owned.controller, [&] { return owned.controller.operation() == cb::na::Operation::Idle; }, L"Rebuild and queued run must finish", 60s);
        require(!owned.controller.failed() && owned.state.settings.target == L"BridgeHello"
            && owned.controller.journal().text().find("saved-environment") != std::string::npos,
            L"Rebuild must keep saved target and execute saved launch profile");
        owned.controller.build(cb::na::BuildAction::Clean);
        owned.controller.run();
        require(!owned.controller.canQueueRun() && !owned.controller.runQueued(), L"Clean must never accept queued launch");
        waitUntil(owned.controller, [&] { return owned.controller.operation() == cb::na::Operation::Idle; }, L"Clean must finish", 30s);
        require(!owned.controller.failed() && owned.state.targets.empty() && owned.state.chosenTarget == L"BridgeHello"
            && owned.controller.durationText().starts_with(L"Очистка:"), L"Clean must clear inventory and preserve launch identity/duration kind");
        owned.controller.build(cb::na::BuildAction::Configure);
        owned.controller.run();
        require(!owned.controller.canQueueRun() && !owned.controller.runQueued(), L"Configure must never accept queued launch");
        waitUntil(owned.controller, [&] { return owned.controller.operation() == cb::na::Operation::Idle; }, L"Configure must finish", 30s);
        require(!owned.controller.failed() && owned.controller.durationText().starts_with(L"CMake:"), L"Configure must report its own duration");
        owned.controller.build(cb::na::BuildAction::BuildAndRun);
        waitUntil(owned.controller, [&] { return owned.controller.operation() == cb::na::Operation::Idle; }, L"Final queued build must finish", 60s);
        require(!owned.controller.failed() && !owned.controller.runQueued()
            && owned.controller.journal().text().find("saved-environment") != std::string::npos,
            L"Final build must restore missing EXE and execute remembered target");
    }
    OwnedController restarted(ini);
    require(restarted.state.targets.size() == 1 && restarted.state.chosenTarget == L"BridgeHello"
        && restarted.state.runSettingsFor(L"BridgeHello").arguments == L"--saved-argument",
        L"Restart must restore the actual built File API result and selected target");
    fs::remove(directory / L"bridge.launched");
    writeFile(directory / L"hello.cpp", "This is an intentional compiler error;\n");
    restarted.controller.build(cb::na::BuildAction::BuildAndRun);
    waitUntil(restarted.controller, [&] {return restarted.controller.operation()==cb::na::Operation::Idle;}, L"Queued compiler failure must finish", 60s);
    require(restarted.controller.failed() && !restarted.controller.runQueued() && !fs::exists(directory / L"bridge.launched"),
        L"Compiler failure with an existing old EXE must discard launch and never run the old program");
    writeFile(directory / L"hello.cpp", helloSource);
    restarted.controller.build();
    waitUntil(restarted.controller, [&] {return restarted.controller.operation()==cb::na::Operation::Idle;}, L"Recovery ordinary build must finish", 60s);
    require(!restarted.controller.failed() && !fs::exists(directory / L"bridge.launched"),
        L"Ordinary build after failure must not inherit a queued launch");
}

void checkFailuresAndDuration(const fs::path& root) {
    const auto project = root / L"failed-config/CMakeLists.txt";
    writeFile(project, "cmake_minimum_required(VERSION 3.24)\nproject(Failure LANGUAGES NONE)\nmessage(FATAL_ERROR \"Intentional failure\")\n");
    OwnedController owned(root / L"failed-config/settings.ini");
    owned.controller.selectProject(project.wstring());
    owned.state.settings.compiler = cb::CompilerMode::Environment;
    owned.controller.build(cb::na::BuildAction::BuildAndRun);
    waitUntil(owned.controller, [&] {return owned.controller.operation() == cb::na::Operation::Idle;}, L"Failed queued build must finish");
    require(owned.controller.failed() && !owned.controller.runQueued() && owned.controller.lastBuildDuration().has_value(),
        L"Failed queued build must reset pending launch and retain duration");
    owned.controller.onEvent({cb::EventKind::ConfigureFailed, L"Duration fixture", {}, ERROR_CANCELLED, 3723456ms});
    require(owned.controller.durationText() == L"CMake: 1 ч 02 мин 03,46 с"
        && owned.controller.journal().text().find("Время CMake: 1 ч 02 мин 03,46 с") != std::string::npos,
        L"Worker duration must round hundredths and distinguish hours and operation kinds");
    require(cb::parseBuildProgress("\x1b[32m[ 42%] compiling") == cb::BuildProgress{42, 100}
        && cb::parseBuildProgress("[12/80] compiling") == cb::BuildProgress{12, 80}
        && !cb::parseBuildProgress("warning [42%]") && !cb::parseBuildProgress("[3/0]"),
        L"Native progress parser must accept ANSI/Make/Ninja prefixes and reject diagnostics or invalid totals");
}

int runFixture(const fs::path& executable) {
    const cb::Target fixture{L"", executable.wstring()};
    if (executable.filename() == L"run-fixture-slow.exe") {
        const auto childExecutable = executable.parent_path() / L"run-fixture-child.exe";
        auto command = L"\"" + childExecutable.wstring() + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION child{};
        require(CreateProcessW(childExecutable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
            nullptr, executable.parent_path().c_str(), &startup, &child) != FALSE, L"Cannot create the owned child fixture");
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
        writeFile(executable.parent_path() / L"process-tree.pid", std::to_string(GetCurrentProcessId()) + " " + std::to_string(child.dwProcessId));
        std::cout << "Русский UTF-8 вывод из рабочего процесса 😀\n" << std::flush;
    }
    writeFile(marker(fixture), utf8(fs::current_path().wstring()));
    if (executable.filename() == L"run-fixture-slow.exe" || executable.filename() == L"run-fixture-child.exe") std::this_thread::sleep_for(30s);
    return 0;
}
}

int wmain() {
    try {
        const auto executable = harness::currentExecutable();
        if (executable.filename().wstring().starts_with(L"run-fixture-")) return runFixture(executable);
        harness::TempDirectory temporary;
        const harness::ProjectFixture first(temporary.root, L"проект [one] 100%", L"custom-build", L"Release",
            L"Alpha", L"Beta & tool", L"run-fixture-alpha.exe", L"run-fixture-beta.exe");
        const harness::ProjectFixture second(temporary.root, L"проект two", L"debug-output", L"Debug",
            L"Delta", L"Gamma приложение", L"run-fixture-delta.exe", L"run-fixture-gamma.exe");
        checkProjects(temporary.root / L"settings.ini", first, second);
        checkLifecycle(temporary.root);
        checkJournal();
        checkSuccessfulBuild(temporary.root);
        checkFailuresAndDuration(temporary.root);
        std::wcout << L"Nana variant controller state, lifecycle, Unicode and journal checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
