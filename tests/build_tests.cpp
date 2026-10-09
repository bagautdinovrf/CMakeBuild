#include "engine.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void checkProgressParser() {
    auto parsed = [](std::string_view line, cb::BuildProgress expected) {
        require(cb::parseBuildProgress(line) == expected, "Valid build progress prefix was not parsed.");
    };
    parsed("[ 42%] Building source", {42, 100});
    parsed("[0%] Starting", {0, 100});
    parsed("[100%] Linking", {100, 100});
    parsed("[12/80] Building source", {12, 80});
    parsed(" \t[ 12 / 80 ] Building source", {12, 80});
    parsed("\x1b[1;32m\x1b[38;2;20;100;200m[ 42%]\x1b[0m source", {42, 100});
    parsed("\x1b[m[0/1] source", {0, 1});
    const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    parsed("[18446744073709551615/18446744073709551615] source", {maximum, maximum});
    for (const auto* invalid : {"message [42%]", "-- [42%]", "[42%", "[42]", "[%]",
            "[101%]", "[-1%]", "[+1%]", "[1.5%]", "[1/0]", "[2/1]", "[1/]",
            "[18446744073709551616%]", "[18446744073709551616/18446744073709551615]",
            "[0/18446744073709551616]", "\x1b[K[42%]", "\x1b[32[42%]"}) {
        require(!cb::parseBuildProgress(invalid), "Invalid or embedded progress was accepted.");
    }
}

std::string utf8(std::wstring_view value) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    if (length) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), length, nullptr, nullptr);
    return result;
}

void write(const fs::path& path, std::string_view contents) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    require(static_cast<bool>(stream), "Cannot write a temporary fixture.");
}

std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "Cannot read the configuration-invocation fixture.");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::map<std::string, std::string> readCache(const fs::path& build) {
    std::ifstream stream(build / L"CMakeCache.txt", std::ios::binary);
    require(static_cast<bool>(stream), "CMakeCache.txt was not created.");
    std::map<std::string, std::string> result;
    for (std::string line; std::getline(stream, line); ) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.starts_with('#') || line.starts_with("//")) continue;
        const auto colon = line.find(':');
        const auto equals = line.find('=');
        if (colon != line.npos && equals != line.npos && colon < equals) {
            result.emplace(line.substr(0, colon), line.substr(equals + 1));
        }
    }
    return result;
}

class PathScope {
public:
    explicit PathScope(const fs::path& bin) {
        if (bin.empty()) return;
        require(fs::is_regular_file(bin / L"g++.exe"), "MinGW g++.exe is missing.");
        const DWORD count = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        if (count) {
            saved_.emplace(count, L'\0');
            const DWORD length = GetEnvironmentVariableW(L"PATH", saved_->data(), count);
            require(length < count, "Cannot read the test process PATH.");
            saved_->resize(length);
        }
        const auto path = bin.wstring() + L';' + saved_.value_or(L"");
        require(SetEnvironmentVariableW(L"PATH", path.c_str()), "Cannot update the test process PATH.");
        changed_ = true;
    }
    ~PathScope() {
        if (changed_) SetEnvironmentVariableW(L"PATH", saved_ ? saved_->c_str() : nullptr);
    }
private:
    std::optional<std::wstring> saved_;
    bool changed_ = false;
};

class Events {
public:
    void receive(cb::Event event) {
        std::lock_guard lock(mutex_);
        events_.push_back(std::move(event));
        ready_.notify_all();
    }
    cb::Event wait(cb::Engine& engine, bool application = false) {
        std::unique_lock lock(mutex_);
        const auto finished = [application](const cb::Event& event) {
            return application ? event.kind == cb::EventKind::Finished : terminal(event);
        };
        const bool completed = ready_.wait_for(lock, 120s, [&] {
            return std::ranges::any_of(events_, finished);
        });
        if (!completed) {
            lock.unlock();
            engine.cancel();
            throw std::runtime_error("Engine did not finish within 120 seconds.");
        }
        return *std::ranges::find_if(events_, finished);
    }
    bool contains(std::wstring_view text) {
        std::lock_guard lock(mutex_);
        return std::ranges::any_of(events_, [&](const auto& event) {
            return event.kind == cb::EventKind::Log && event.text.find(text) != event.text.npos;
        });
    }
    bool explicitlyConfigured() {
        std::lock_guard lock(mutex_);
        return std::ranges::any_of(events_, [](const auto& event) {
            return commandArgument(event, L"-S") && commandArgument(event, L"-B");
        });
    }
    bool invokedArgument(std::wstring_view argument) {
        std::lock_guard lock(mutex_);
        return std::ranges::any_of(events_, [&](const auto& event) { return commandArgument(event, argument); });
    }
    void checkConfigureOnlyArgument(std::wstring_view argument, bool expected) {
        std::lock_guard lock(mutex_);
        size_t occurrences = 0;
        for (const auto& event : events_) {
            if (!commandArgument(event, argument)) continue;
            ++occurrences;
            require(commandArgument(event, L"-S") && commandArgument(event, L"-B")
                && !commandArgument(event, L"--build"),
                "Additional CMake arguments escaped the configure invocation.");
        }
        require(occurrences == (expected ? 1u : 0u),
            "Additional CMake argument was omitted, duplicated, or reused without configuration.");
    }
    bool invokedCommand() {
        std::lock_guard lock(mutex_);
        return std::ranges::any_of(events_, [](const auto& event) {
            return event.kind == cb::EventKind::Log && event.text.starts_with(L"> ");
        });
    }
    void checkNoProgressForLog(std::wstring_view text) {
        std::lock_guard lock(mutex_);
        bool found = false;
        for (size_t index = 0; index < events_.size(); ++index) {
            const auto& event = events_[index];
            if (event.kind == cb::EventKind::Log && event.text.find(text) != event.text.npos) {
                found = true;
                require(index == 0 || events_[index - 1].kind != cb::EventKind::Progress,
                    "Native Ninja cleanup incorrectly updated compilation progress.");
            }
        }
        require(found, "Expected native Ninja cleanup output is missing from the log.");
    }
    void checkBuildDuration(std::chrono::steady_clock::duration elapsed) {
        std::lock_guard lock(mutex_);
        size_t terminalCount = 0;
        for (const auto& event : events_) {
            if (terminal(event)) {
                ++terminalCount;
                require(event.buildDuration.has_value(), "Build completion did not report its duration.");
                require(*event.buildDuration > 0ms, "An actual build reported a nonpositive duration.");
                require(*event.buildDuration <= elapsed, "Build duration exceeds the observed build time.");
            } else {
                require(!event.buildDuration, "A nonterminal build event unexpectedly reported a duration.");
            }
        }
        require(terminalCount == 1, "Build reported more than one terminal event.");
    }
    std::vector<cb::BuildProgress> progressValues() {
        std::lock_guard lock(mutex_);
        std::vector<cb::BuildProgress> result;
        for (const auto& event : events_) {
            if (event.kind == cb::EventKind::Progress) {
                require(event.progress && event.progress->total > 0
                    && event.progress->completed <= event.progress->total,
                    "Progress event has no valid completed/total values.");
                require(!event.buildDuration, "Progress event incorrectly reported completion time.");
                require(result.empty() || result.back() != *event.progress,
                    "Identical progress events were not suppressed.");
                result.push_back(*event.progress);
            } else {
                require(!event.progress, "An unrelated event unexpectedly carries build progress.");
            }
        }
        return result;
    }
    void dump() {
        std::lock_guard lock(mutex_);
        for (const auto& event : events_) {
            if (!event.text.empty()) std::cout << utf8(event.text) << '\n';
        }
    }
private:
    static bool commandArgument(const cb::Event& event, std::wstring_view argument) {
        if (event.kind != cb::EventKind::Log || !event.text.starts_with(L"> ")) return false;
        // Engine::commandLine always quotes its arguments; matching complete
        // tokens prevents ordinary CMake output from impersonating a launch.
        const auto token = L" \"" + std::wstring(argument) + L"\"";
        auto command = std::wstring_view(event.text);
        while (!command.empty() && (command.back() == L'\r' || command.back() == L'\n')) command.remove_suffix(1);
        return command.find(token + L' ') != command.npos || command.ends_with(token);
    }
    static bool terminal(const cb::Event& event) {
        return event.kind == cb::EventKind::BuildSucceeded || event.kind == cb::EventKind::BuildFailed
            || event.kind == cb::EventKind::ConfigureSucceeded || event.kind == cb::EventKind::ConfigureFailed;
    }
    std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<cb::Event> events_;
};

struct Options {
    cb::CompilerMode compiler = cb::CompilerMode::Msvc;
    fs::path cmake;
    fs::path mingwBin;
    bool keep = false;
};

Options parse(int argc, wchar_t** argv) {
    Options result;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--keep") { result.keep = true; continue; }
        require(index + 1 < argc, "Missing option value.");
        const std::wstring_view value(argv[++index]);
        if (argument == L"--toolchain") {
            require(value == L"msvc" || value == L"mingw", "Expected --toolchain msvc or mingw.");
            result.compiler = value == L"msvc" ? cb::CompilerMode::Msvc : cb::CompilerMode::Mingw;
        } else if (argument == L"--cmake") result.cmake = fs::path(value);
        else if (argument == L"--mingw-bin") result.mingwBin = fs::path(value);
        else throw std::runtime_error("Unknown option.");
    }
    if (result.compiler == cb::CompilerMode::Mingw && result.mingwBin.empty()) {
        result.mingwBin = L"C:/Qt/Tools/mingw1310_64/bin";
    }
    require(result.compiler == cb::CompilerMode::Mingw || result.mingwBin.empty(),
        "--mingw-bin requires --toolchain mingw.");
    return result;
}

struct Fixture { fs::path source; fs::path build; };

void testSources(const Fixture& fixture, bool broken) {
    write(fixture.source / L"ctest.cpp", broken
        ? "#error CMAKEBUILD_BUILD_TESTING_COMPILE_FAILURE\n"
        : "int main() { return 0; }\n");
    write(fixture.source / L"custom_test.cpp", broken
        ? "#error CMAKEBUILD_BUILD_TESTS_COMPILE_FAILURE\n"
        : "int main() { return 0; }\n");
}

Fixture project(const fs::path& root, std::wstring_view name, bool broken) {
    Fixture fixture{root / name / L"source", root / name / L"build"};
    write(fixture.source / L"CMakeLists.txt", R"cmake(cmake_minimum_required(VERSION 3.24)
project(EngineBuildTestsFixture LANGUAGES CXX)
file(APPEND "${CMAKE_SOURCE_DIR}/configure-invocations.txt" "configured;")
if(MSVC)
    set(CMAKE_CXX_STANDARD 23)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/std:c++latest>")
else()
    set(fixture_cxx_standards ${CMAKE_CXX_COMPILE_FEATURES})
    list(FILTER fixture_cxx_standards INCLUDE REGEX "^cxx_std_[0-9]+$")
    list(REMOVE_ITEM fixture_cxx_standards cxx_std_98)
    list(SORT fixture_cxx_standards COMPARE NATURAL)
    list(GET fixture_cxx_standards -1 fixture_latest_cxx)
    string(REPLACE "cxx_std_" "" CMAKE_CXX_STANDARD "${fixture_latest_cxx}")
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(CTest)
option(BUILD_TESTS "Build project-specific tests" ON)
add_executable(engine_fixture main.cpp)
if(BUILD_TESTING)
    add_executable(ctest_fixture ctest.cpp)
    add_test(NAME ctest_fixture COMMAND ctest_fixture)
endif()
if(BUILD_TESTS)
    add_executable(custom_test_fixture custom_test.cpp)
endif()
)cmake");
    write(fixture.source / L"main.cpp", "#include <cstdio>\nint main() { "
        "std::puts(\"[22%] application progress lookalike\"); return 0; }\n");
    testSources(fixture, broken);
    return fixture;
}

cb::Event build(const Fixture& fixture, const Options& options, std::optional<bool> buildTests = {},
        std::wstring_view configuration = L"Release", std::optional<bool> expectedConfigure = {}) {
    Events events;
    cb::Engine engine([&](cb::Event event) { events.receive(std::move(event)); });
    cb::BuildSettings settings;
    settings.cmakeFile = (fixture.source / L"CMakeLists.txt").wstring();
    settings.buildDirectory = fixture.build.wstring();
    settings.cmakeExecutable = options.cmake.wstring();
    settings.compiler = options.compiler;
    settings.configuration = configuration;
    // Empty selects the public API default; this catches regressions to that default.
    if (buildTests) settings.buildTests = *buildTests;
    const auto started = std::chrono::steady_clock::now();
    require(engine.build(std::move(settings)), "Engine rejected the build.");
    const auto result = events.wait(engine);
    events.checkBuildDuration(std::chrono::steady_clock::now() - started);
    events.progressValues();
    if (expectedConfigure) require(events.explicitlyConfigured() == *expectedConfigure,
        "Build did not follow the expected explicit configuration policy.");
    if (result.kind != cb::EventKind::BuildSucceeded) events.dump();
    if (result.kind == cb::EventKind::BuildFailed && buildTests == true) {
        require(events.contains(L"CMAKEBUILD_BUILD_TESTING_COMPILE_FAILURE")
            || events.contains(L"CMAKEBUILD_BUILD_TESTS_COMPILE_FAILURE"),
            "Opt-in build failed before compiling the deliberately broken tests.");
    }
    return result;
}

// This harness also acts as a tiny CMake custom-command child. Direct writes
// exercise split prefixes, CRLF, UTF-8 and the long-line continuation path.
int emitProgress(bool fail) {
    const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
    auto emit = [&](std::string_view bytes) {
        DWORD written = 0;
        require(WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
            && written == bytes.size(), "Cannot write progress fixture output.");
    };
    emit("[1/");
    Sleep(70);
    emit("8] split count\r");
    Sleep(70);
    emit("\n[1/8] duplicate count\r\n[ 37%] ");
    const auto unicode = utf8(L"Прогресс сборки");
    emit(std::string_view(unicode).substr(0, 1));
    Sleep(70);
    emit(std::string_view(unicode).substr(1));
    emit("\r\n\x1b[1;32m[ 6 / 8 ] colored count\x1b[0m\n[9/8] invalid count\n");
    emit("[1/1] Cleaning all built files...\n"
        "\x1b[32m[1/1]\x1b[0m Cleaning all built files...\n");
    emit("long-line " + std::string(70000, 'x'));
    Sleep(70);
    emit("[94%] continuation lookalike\n[100%] not terminal success\n");
    Sleep(70);
    emit("[8/8] final count without newline");
    return fail ? 7 : 0;
}

void checkProgressBuild(const fs::path& root, const Options& options, bool fail) {
    const auto name = fail ? L"progress failure" : L"progress success";
    const Fixture fixture{root / name / L"source", root / name / L"build"};
    std::wstring helper(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, helper.data(), static_cast<DWORD>(helper.size()));
    require(length && length < helper.size(), "Cannot locate the progress-output helper.");
    helper.resize(length);
    write(fixture.source / L"CMakeLists.txt",
        "cmake_minimum_required(VERSION 3.24)\n"
        "project(EngineProgressFixture LANGUAGES NONE)\n"
        "message(\"[73%] configure progress lookalike\")\n"
        "add_custom_target(emitted_progress ALL\n  COMMAND \""
        + utf8(fs::path(helper).generic_wstring()) + "\" --emit-progress " + (fail ? "--fail" : "")
        + "\n  USES_TERMINAL VERBATIM)\n");
    Events events;
    cb::Engine engine([&](cb::Event event) { events.receive(std::move(event)); });
    cb::BuildSettings settings;
    settings.cmakeFile = (fixture.source / L"CMakeLists.txt").wstring();
    settings.buildDirectory = fixture.build.wstring();
    settings.cmakeExecutable = options.cmake.wstring();
    settings.compiler = options.compiler;
    const auto started = std::chrono::steady_clock::now();
    require(engine.build(std::move(settings)), "Engine rejected the progress fixture.");
    const auto result = events.wait(engine);
    events.checkBuildDuration(std::chrono::steady_clock::now() - started);
    require(result.kind == (fail ? cb::EventKind::BuildFailed : cb::EventKind::BuildSucceeded),
        "A progress prefix was confused with the actual build exit status.");
    const auto values = events.progressValues();
    events.checkNoProgressForLog(L"[1/1] Cleaning all built files...");
    events.checkNoProgressForLog(L"\x1b[32m[1/1]\x1b[0m Cleaning all built files...");
    const std::vector<cb::BuildProgress> expectedValues{{1, 8}, {37, 100}, {6, 8}, {100, 100}, {8, 8}};
    for (const auto expected : expectedValues) {
        require(std::ranges::count(values, expected) == 1,
            "Build did not report each progress value once or repeated duplicate progress.");
    }
    require(std::ranges::find(values, cb::BuildProgress{73, 100}) == values.end(),
        "Configure output incorrectly updated build progress.");
    require(std::ranges::find(values, cb::BuildProgress{94, 100}) == values.end(),
        "A long-line continuation incorrectly updated build progress.");
    require(events.contains(L"[73%] configure progress lookalike")
        && events.contains(L"[1/8] duplicate count") && events.contains(L"[ 37%] Прогресс сборки")
        && events.contains(L"\x1b[1;32m[ 6 / 8 ] colored count\x1b[0m")
        && events.contains(L"[9/8] invalid count") && events.contains(L"[8/8] final count without newline"),
        "Parsing build progress changed or discarded process log text.");
}

void checkApplicationIgnoresProgress(const cb::Target& target) {
    Events events;
    cb::Engine engine([&](cb::Event event) { events.receive(std::move(event)); });
    require(engine.run(target, fs::path(target.executable).parent_path().wstring()),
        "Engine rejected the application-output fixture.");
    const auto result = events.wait(engine, true);
    require(result.exitCode == 0 && events.contains(L"[22%] application progress lookalike"),
        "Application-output fixture did not run successfully.");
    require(events.progressValues().empty(), "Application output incorrectly updated build progress.");
}

void check(const Fixture& fixture, const cb::Event& result, bool buildTests) {
    require(result.kind == cb::EventKind::BuildSucceeded, "Engine build failed.");
    std::set<std::wstring> actual;
    for (const auto& target : result.targets) {
        actual.emplace(target.name);
        require(fs::is_regular_file(target.executable), "Built executable is missing.");
    }
    std::set<std::wstring> expected{L"engine_fixture"};
    if (buildTests) expected.insert({L"ctest_fixture", L"custom_test_fixture"});
    require(actual == expected, "Configured and built targets do not match the test setting.");
    const auto cache = readCache(fixture.build);
    for (const auto* option : {"BUILD_TESTING", "BUILD_TESTS"}) {
        const auto found = cache.find(option);
        require(found != cache.end() && found->second == (buildTests ? "ON" : "OFF"),
            "Test setting did not override both CMake cache options.");
    }
}

cb::Event configure(const Fixture& fixture, const Options& options, bool buildTests) {
    Events events;
    cb::Engine engine([&](cb::Event event) { events.receive(std::move(event)); });
    cb::BuildSettings settings;
    settings.cmakeFile = (fixture.source / L"CMakeLists.txt").wstring();
    settings.buildDirectory = fixture.build.wstring();
    settings.cmakeExecutable = options.cmake.wstring();
    settings.compiler = options.compiler;
    settings.buildTests = buildTests;
    const auto started = std::chrono::steady_clock::now();
    require(engine.configure(std::move(settings)), "Engine rejected the standalone CXX configuration.");
    const auto result = events.wait(engine);
    events.checkBuildDuration(std::chrono::steady_clock::now() - started);
    if (result.kind != cb::EventKind::ConfigureSucceeded) events.dump();
    require(result.kind == cb::EventKind::ConfigureSucceeded && events.explicitlyConfigured(),
        "Standalone CXX configuration did not finish successfully through explicit CMake.");
    require(!events.invokedArgument(L"--build") && events.progressValues().empty(),
        "Standalone CXX configuration compiled targets or reported compilation progress.");
    return result;
}

void checkStandaloneConfiguration(const fs::path& root, const Options& options) {
    const auto fixture = project(root, L"standalone configuration", false);
    const auto count = [&] { return read(fixture.source / L"configure-invocations.txt"); };
    std::cout << "[ RUN ] standalone_configure_exposes_unbuilt_executable_inventory" << std::endl;
    const auto result = configure(fixture, options, true);
    std::set<std::wstring> targets;
    for (const auto& target : result.targets) {
        targets.emplace(target.name);
        require(!fs::exists(target.executable), "Standalone configuration unexpectedly compiled an executable.");
    }
    require(targets == std::set<std::wstring>{L"engine_fixture", L"ctest_fixture", L"custom_test_fixture"},
        "Configuration did not expose all executable targets before they were compiled.");
    const auto configuredCache = readCache(fixture.build);
    require(configuredCache.at("BUILD_TESTING") == "ON" && configuredCache.at("BUILD_TESTS") == "ON"
        && count() == "configured;", "Standalone configuration did not apply both test options once.");
    configure(fixture, options, true);
    require(count() == "configured;configured;",
        "Explicit configure reused the existing tree instead of rerunning unchanged project scripts.");
    for (const auto& target : result.targets)
        require(!fs::exists(target.executable), "Repeated explicit configuration compiled a target.");
    std::cout << "[ PASS ] standalone_configure_exposes_unbuilt_executable_inventory" << std::endl;

    std::cout << "[ RUN ] build_after_configure_reuses_tree_across_engine_restart" << std::endl;
    const auto beforeBuild = count();
    check(fixture, build(fixture, options, true, L"Release", false), true);
    require(count() == beforeBuild, "Build after configuration unnecessarily reran project scripts.");
    check(fixture, build(fixture, options, true, L"Release", false), true);
    require(count() == beforeBuild, "A restarted Engine reconfigured an unchanged CXX project.");
    std::cout << "[ PASS ] build_after_configure_reuses_tree_across_engine_restart" << std::endl;

    std::cout << "[ RUN ] configuration_and_test_options_invalidate_reused_tree" << std::endl;
    check(fixture, build(fixture, options, true, L"Debug", true), true);
    require(count() == beforeBuild + "configured;" && readCache(fixture.build).at("CMAKE_BUILD_TYPE") == "Debug",
        "Configuration change did not explicitly update the CMake build type.");
    const auto beforeOptions = count();
    check(fixture, build(fixture, options, false, L"Debug", true), false);
    require(count() == beforeOptions + "configured;", "Test-option change did not configure the existing cache.");
    const auto beforeUnchanged = count();
    check(fixture, build(fixture, options, false, L"Debug", false), false);
    require(count() == beforeUnchanged, "Unchanged configuration/test options did not restore build-tree reuse.");
    std::cout << "[ PASS ] configuration_and_test_options_invalidate_reused_tree" << std::endl;
}

Fixture customArgumentsProject(const fs::path& root, std::wstring_view name) {
    const Fixture fixture{root / name / L"source", root / name / L"build"};
    write(fixture.source / L"CMakeLists.txt", R"cmake(cmake_minimum_required(VERSION 3.24)
project(EngineCustomArgumentsFixture LANGUAGES CXX)
file(APPEND "${CMAKE_SOURCE_DIR}/configure-invocations.txt" "configured;")
if(MSVC)
    set(CMAKE_CXX_STANDARD 23)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/std:c++latest>")
else()
    set(fixture_cxx_standards ${CMAKE_CXX_COMPILE_FEATURES})
    list(FILTER fixture_cxx_standards INCLUDE REGEX "^cxx_std_[0-9]+$")
    list(REMOVE_ITEM fixture_cxx_standards cxx_std_98)
    list(SORT fixture_cxx_standards COMPARE NATURAL)
    list(GET fixture_cxx_standards -1 fixture_latest_cxx)
    string(REPLACE "cxx_std_" "" CMAKE_CXX_STANDARD "${fixture_latest_cxx}")
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(DESIGNER_BUILD_TESTS "Build project-specific designer tests" ON)
set(CUSTOM_TEXT "default text" CACHE STRING "Additional configure argument")
file(WRITE "${CMAKE_BINARY_DIR}/custom-text.txt" "${CUSTOM_TEXT}")
add_executable(engine_fixture main.cpp)
if(DESIGNER_BUILD_TESTS)
    add_executable(designer_test_fixture designer_test.cpp)
endif()
)cmake");
    write(fixture.source / L"main.cpp", "int main() { return 0; }\n");
    write(fixture.source / L"designer_test.cpp", "#error CMAKEBUILD_DESIGNER_TEST_COMPILE_FAILURE\n");
    return fixture;
}

cb::BuildSettings customArgumentsSettings(const Fixture& fixture, const Options& options,
        std::wstring_view arguments) {
    cb::BuildSettings settings;
    settings.cmakeFile = (fixture.source / L"CMakeLists.txt").wstring();
    settings.buildDirectory = fixture.build.wstring();
    settings.cmakeExecutable = options.cmake.wstring();
    settings.compiler = options.compiler;
    settings.cmakeArguments = arguments;
    return settings;
}

cb::Event buildArguments(const Fixture& fixture, const Options& options,
        std::wstring_view arguments, bool expectedConfigure, bool brokenTests = false,
        bool configureOnly = false) {
    Events events;
    cb::Engine engine([&](cb::Event event) { events.receive(std::move(event)); });
    auto settings = customArgumentsSettings(fixture, options, arguments);
    const auto started = std::chrono::steady_clock::now();
    require(configureOnly ? engine.configure(std::move(settings)) : engine.build(std::move(settings)),
        "Engine rejected the custom CMake argument fixture.");
    const auto result = events.wait(engine);
    events.checkBuildDuration(std::chrono::steady_clock::now() - started);
    require(events.explicitlyConfigured() == expectedConfigure,
        "Custom CMake arguments did not follow the expected configuration reuse policy.");
    const auto expected = configureOnly ? cb::EventKind::ConfigureSucceeded
        : brokenTests ? cb::EventKind::BuildFailed : cb::EventKind::BuildSucceeded;
    if (result.kind != expected) events.dump();
    require(result.kind == expected, "Custom CMake argument fixture returned an unexpected result.");
    if (brokenTests) {
        require(events.contains(L"CMAKEBUILD_DESIGNER_TEST_COMPILE_FAILURE"),
            "Custom test-option control failed before compiling the deliberately broken test.");
    }
    events.checkConfigureOnlyArgument(L"-DDESIGNER_BUILD_TESTS=OFF",
        expectedConfigure && arguments.find(L"-DDESIGNER_BUILD_TESTS=OFF") != arguments.npos);
    if (configureOnly) {
        require(!events.invokedArgument(L"--build") && events.progressValues().empty(),
            "Additional CMake arguments caused configure-only to build a target.");
    }
    return result;
}

void checkCustomCMakeArguments(const fs::path& root, const Options& options) {
    const auto fixture = customArgumentsProject(root, L"custom CMake параметры");
    const auto count = [&] { return read(fixture.source / L"configure-invocations.txt"); };
    constexpr std::wstring_view disableTests = L"-DDESIGNER_BUILD_TESTS=OFF";

    std::cout << "[ RUN ] custom_test_option_overrides_project_default" << std::endl;
    buildArguments(fixture, options, {}, true, true);
    const auto controlCache = readCache(fixture.build);
    require(controlCache.at("BUILD_TESTING") == "OFF" && controlCache.at("BUILD_TESTS") == "OFF"
        && controlCache.at("DESIGNER_BUILD_TESTS") == "ON",
        "Control fixture did not reproduce the independent project-specific test option.");
    const auto result = buildArguments(fixture, options, disableTests, true);
    require(result.targets.size() == 1 && result.targets.front().name == L"engine_fixture"
        && fs::is_regular_file(result.targets.front().executable)
        && readCache(fixture.build).at("DESIGNER_BUILD_TESTS") == "OFF",
        "The explicit project-specific argument did not exclude the broken test target.");
    std::cout << "[ PASS ] custom_test_option_overrides_project_default" << std::endl;

    std::cout << "[ RUN ] custom_arguments_preserve_quoted_unicode_values" << std::endl;
    const std::wstring text = L"Значение с пробелами & %PATH% ; данные";
    const auto arguments = std::wstring(disableTests) + L" -DCUSTOM_TEXT:STRING=\"" + text + L"\""
        LR"( -DLITERAL_QUOTE:STRING="value \"quoted\"" -DTRAILING_PATH:STRING="C:\Пример\\")";
    const auto beforeArguments = count();
    buildArguments(fixture, options, arguments, true);
    require(count() == beforeArguments + "configured;"
        && readCache(fixture.build).at("CUSTOM_TEXT") == utf8(text)
        && read(fixture.build / L"custom-text.txt") == utf8(text)
        && readCache(fixture.build).at("LITERAL_QUOTE") == "value \"quoted\""
        && readCache(fixture.build).at("TRAILING_PATH") == utf8(L"C:\\Пример\\"),
        "Quoted spaces, Unicode, or shell metacharacters changed in the CMake argument value.");
    std::cout << "[ PASS ] custom_arguments_preserve_quoted_unicode_values" << std::endl;

    std::cout << "[ RUN ] custom_arguments_reuse_change_and_clear_across_engine_restart" << std::endl;
    const auto beforeReuse = count();
    buildArguments(fixture, options, arguments, false);
    require(count() == beforeReuse, "Identical custom arguments reconfigured after an Engine restart.");
    const std::wstring changedText = L"Изменённое значение проекта";
    const auto changed = std::wstring(disableTests) + L" -DCUSTOM_TEXT:STRING=\"" + changedText + L"\"";
    buildArguments(fixture, options, changed, true);
    require(count() == beforeReuse + "configured;"
        && readCache(fixture.build).at("CUSTOM_TEXT") == utf8(changedText),
        "Changed custom arguments did not reconfigure and update the existing cache.");
    const auto beforeClear = count();
    buildArguments(fixture, options, {}, true);
    // Removing CLI definitions reruns configuration; CMake retains existing cache
    // values until explicitly changed or unset, including the disabled tests.
    require(count() == beforeClear + "configured;"
        && readCache(fixture.build).at("DESIGNER_BUILD_TESTS") == "OFF",
        "Clearing custom arguments did not reconfigure the existing build tree.");
    const auto afterClear = count();
    buildArguments(fixture, options, {}, false);
    require(count() == afterClear, "Cleared custom arguments did not restore configuration reuse.");
    std::cout << "[ PASS ] custom_arguments_reuse_change_and_clear_across_engine_restart" << std::endl;

    std::cout << "[ RUN ] custom_cache_overrides_and_unsets_reuse_successful_configuration" << std::endl;
    const auto overrides = std::wstring(disableTests)
        + L" -D BUILD_TESTING:BOOL=ON -DBUILD_TESTS:BOOL=ON -U CUSTOM_TEXT";
    const auto beforeOverrides = count();
    buildArguments(fixture, options, overrides, true);
    const auto overridden = readCache(fixture.build);
    require(count() == beforeOverrides + "configured;" && overridden.at("BUILD_TESTING") == "ON"
        && overridden.at("BUILD_TESTS") == "ON" && overridden.at("CUSTOM_TEXT") == "default text",
        "Explicit cache definitions did not override the test checkbox or the cache unset was ignored.");
    const auto beforeOverrideReuse = count();
    buildArguments(fixture, options, overrides, false);
    require(count() == beforeOverrideReuse,
        "Explicit test-option overrides caused repeated configuration despite unchanged arguments.");
    buildArguments(fixture, options, disableTests, true);
    require(readCache(fixture.build).at("BUILD_TESTING") == "OFF"
        && readCache(fixture.build).at("BUILD_TESTS") == "OFF",
        "Removing explicit test-option overrides did not restore the checkbox defaults.");
    std::cout << "[ PASS ] custom_cache_overrides_and_unsets_reuse_successful_configuration" << std::endl;

    std::cout << "[ RUN ] configuration_field_wins_over_custom_build_type_and_unset" << std::endl;
    for (const auto* buildTypeArgument : {L" -DCMAKE_BUILD_TYPE=Debug", L" -U CMAKE_BUILD_TYPE"}) {
        const auto buildTypeArguments = std::wstring(disableTests) + buildTypeArgument;
        const auto beforeBuildType = count();
        buildArguments(fixture, options, buildTypeArguments, true);
        require(count() == beforeBuildType + "configured;"
            && readCache(fixture.build).at("CMAKE_BUILD_TYPE") == "Release",
            "Custom build-type definition or unset replaced the panel configuration field.");
        const auto beforeBuildTypeReuse = count();
        buildArguments(fixture, options, buildTypeArguments, false);
        require(count() == beforeBuildTypeReuse && readCache(fixture.build).at("CMAKE_BUILD_TYPE") == "Release",
            "Reasserting the panel build type prevented reuse of unchanged custom arguments.");
    }
    std::cout << "[ PASS ] configuration_field_wins_over_custom_build_type_and_unset" << std::endl;

    std::cout << "[ RUN ] standalone_configure_accepts_custom_arguments_without_building" << std::endl;
    const auto standalone = customArgumentsProject(root, L"standalone custom CMake");
    const auto preload = standalone.source / L"начальные параметры.cmake";
    write(preload, "set(CUSTOM_PRELOADED \"cache script value\" CACHE STRING \"Preloaded fixture\")\n");
    const auto installPrefix = (standalone.build / L"install prefix").generic_wstring();
    const auto standaloneArguments = arguments + L" -C \"" + preload.wstring()
        + L"\" --no-warn-unused-cli --install-prefix \"" + installPrefix + L"\"";
    const auto configured = buildArguments(standalone, options, standaloneArguments, true, false, true);
    require(configured.targets.size() == 1 && !fs::exists(configured.targets.front().executable)
        && read(standalone.build / L"custom-text.txt") == utf8(text)
        && readCache(standalone.build).at("CUSTOM_PRELOADED") == "cache script value"
        && readCache(standalone.build).at("CMAKE_INSTALL_PREFIX") == utf8(installPrefix),
        "Standalone configure did not apply the custom argument without compiling the executable.");
    const auto beforeBuild = read(standalone.source / L"configure-invocations.txt");
    buildArguments(standalone, options, standaloneArguments, false);
    require(read(standalone.source / L"configure-invocations.txt") == beforeBuild,
        "Build could not reuse custom arguments recorded by standalone configure.");
    std::cout << "[ PASS ] standalone_configure_accepts_custom_arguments_without_building" << std::endl;
}

void checkRejectedCMakeArguments(const fs::path& root, const Options& options) {
    std::cout << "[ RUN ] custom_arguments_reject_cmake_mode_and_directory_overrides" << std::endl;
    const auto fixture = customArgumentsProject(root, L"rejected custom CMake arguments");
    const auto redirected = root / L"unexpected redirected build";
    const auto script = fixture.source / L"unexpected-script.cmake";
    const auto scriptMarker = root / L"unexpected-script-ran.txt";
    write(script, "file(WRITE \"" + utf8(scriptMarker.generic_wstring()) + "\" \"unexpected\")\n");
    std::vector<std::wstring> rejected{
        L"-S \"" + fixture.source.wstring() + L"\"",
        L"-S\"" + fixture.source.wstring() + L"\"",
        L"-B \"" + redirected.wstring() + L"\"",
        L"-B\"" + redirected.wstring() + L"\"",
        L"--build \"" + redirected.wstring() + L"\"",
        L"--install \"" + redirected.wstring() + L"\"",
        L"-P \"" + script.wstring() + L"\"",
        L"-P\"" + script.wstring() + L"\"",
        L"-E touch \"" + scriptMarker.wstring() + L"\"",
        L"--workflow --preset fixture",
        L"--preset fixture",
        L"-G Ninja",
        L"-A x64",
        L"-T host=x64",
        L"--open \"" + redirected.wstring() + L"\"",
        L"--find-package",
        L"--help",
        L"--version",
        L"\"" + fixture.source.wstring() + L"\"",
        L"--system-information",
        L"-help",
        L"-usage",
        L"/?",
        L"/V",
        L"-D --help",
        L"--trace-source --help",
        L"--log-level --system-information",
        L"-C -N",
        L"-U -P",
        L"-D",
        L"-U",
        L"-C",
        L"-DCUSTOM_TEXT=\"unclosed",
        L"-DCUSTOM_TEXT=line\nsecond line",
        std::wstring(32767, L'x'),
    };
    for (const auto* reserved : {L"CMAKE_GENERATOR", L"CMAKE_GENERATOR_PLATFORM", L"CMAKE_GENERATOR_TOOLSET",
            L"CMAKE_HOME_DIRECTORY", L"CMAKE_CACHEFILE_DIR", L"CMAKE_COMMAND", L"CMAKE_C_COMPILER",
            L"CMAKE_CXX_COMPILER", L"CMAKE_MAKE_PROGRAM"}) {
        rejected.push_back(std::wstring(L"-D") + reserved + L"=unexpected");
        rejected.push_back(std::wstring(L"-D ") + reserved + L"=unexpected");
        rejected.push_back(std::wstring(L"-D") + reserved + L":STRING=unexpected");
        rejected.push_back(std::wstring(L"-D ") + reserved + L":STRING=unexpected");
    }
    for (const auto& arguments : rejected) {
        Events events;
        cb::Engine engine([&](cb::Event event) { events.receive(std::move(event)); });
        require(engine.build(customArgumentsSettings(fixture, options, arguments)),
            "Engine did not dispatch custom argument validation asynchronously.");
        const auto result = events.wait(engine);
        if (result.kind != cb::EventKind::BuildFailed) events.dump();
        require(result.kind == cb::EventKind::BuildFailed && result.text.find(L"CMake") != result.text.npos,
            "A custom argument was allowed to replace the Engine CMake operation or directories.");
        require(!events.invokedCommand() && !fs::exists(fixture.build)
            && !fs::exists(fixture.source / L"configure-invocations.txt")
            && !fs::exists(redirected) && !fs::exists(scriptMarker),
            "Invalid custom CMake arguments caused process or filesystem side effects before rejection.");
    }
    std::cout << "[ PASS ] custom_arguments_reject_cmake_mode_and_directory_overrides" << std::endl;
}

// A suspended test-owned process still maps and locks its executable image.
// It cannot run arbitrary work, and its destructor always closes that process.
class ExecutableLock {
public:
    explicit ExecutableLock(const fs::path& executable) {
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        auto command = L'"' + executable.wstring() + L'"';
        require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(),
            &startup, &process_), "Cannot create the test-owned executable image lock.");
    }
    ~ExecutableLock() {
        TerminateProcess(process_.hProcess, 0);
        WaitForSingleObject(process_.hProcess, 5000);
        CloseHandle(process_.hThread);
        CloseHandle(process_.hProcess);
    }
    ExecutableLock(const ExecutableLock&) = delete;
    ExecutableLock& operator=(const ExecutableLock&) = delete;
private:
    PROCESS_INFORMATION process_{};
};
} // namespace

int wmain(int argc, wchar_t** argv) {
    fs::path root;
    try {
        if (argc >= 2 && std::wstring_view(argv[1]) == L"--emit-progress") {
            return emitProgress(argc >= 3 && std::wstring_view(argv[2]) == L"--fail");
        }
        const auto options = parse(argc, argv);
        std::cout << "[ RUN ] checked_progress_prefix_parser" << std::endl;
        checkProgressParser();
        std::cout << "[ PASS ] checked_progress_prefix_parser" << std::endl;
        const auto temp = fs::absolute(fs::temp_directory_path()).lexically_normal();
        root = temp / (L"CMakeBuild build options tests " + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(GetTickCount64()));
        require(fs::create_directory(root), "Cannot create a unique temporary test directory.");
        std::cout << "Temporary fixtures: " << utf8(root.wstring()) << std::endl;
        PathScope pathScope(options.mingwBin);
        checkStandaloneConfiguration(root, options);
        checkCustomCMakeArguments(root, options);
        checkRejectedCMakeArguments(root, options);

        std::cout << "[ RUN ] fresh_default_excludes_broken_tests" << std::endl;
        const auto fresh = project(root, L"fresh default", true);
        const auto freshResult = build(fresh, options);
        check(fresh, freshResult, false);
        std::cout << "[ PASS ] fresh_default_excludes_broken_tests" << std::endl;

        std::cout << "[ RUN ] application_output_does_not_report_build_progress" << std::endl;
        checkApplicationIgnoresProgress(freshResult.targets.front());
        std::cout << "[ PASS ] application_output_does_not_report_build_progress" << std::endl;

        std::cout << "[ RUN ] build_progress_preserves_process_output" << std::endl;
        checkProgressBuild(root, options, false);
        std::cout << "[ PASS ] build_progress_preserves_process_output" << std::endl;

        std::cout << "[ RUN ] full_progress_does_not_mask_build_failure" << std::endl;
        checkProgressBuild(root, options, true);
        std::cout << "[ PASS ] full_progress_does_not_mask_build_failure" << std::endl;

        std::cout << "[ RUN ] opt_in_builds_both_test_conventions" << std::endl;
        const auto cached = project(root, L"cached tests enabled", false);
        check(cached, build(cached, options, true), true);
        std::cout << "[ PASS ] opt_in_builds_both_test_conventions" << std::endl;

        std::cout << "[ RUN ] default_overrides_existing_on_cache" << std::endl;
        testSources(cached, true);
        const auto beforeDisableTests = read(cached.source / L"configure-invocations.txt");
        check(cached, build(cached, options, {}, L"Release", true), false);
        require(read(cached.source / L"configure-invocations.txt") == beforeDisableTests + "configured;",
            "Disabling tests did not explicitly configure the existing ON cache.");
        std::cout << "[ PASS ] default_overrides_existing_on_cache" << std::endl;

        std::cout << "[ RUN ] opt_in_reaches_broken_test_compilation" << std::endl;
        require(build(fresh, options, true, L"Release", true).kind == cb::EventKind::BuildFailed,
            "Opt-in build unexpectedly ignored deliberately broken test targets.");
        std::cout << "[ PASS ] opt_in_reaches_broken_test_compilation" << std::endl;

        if (options.compiler == cb::CompilerMode::Msvc) {
            std::cout << "[ RUN ] locked_executable_explains_lnk1168" << std::endl;
            const auto locked = project(root, L"locked executable", true);
            const auto initial = build(locked, options, {}, L"Debug");
            check(locked, initial, false);
            {
                ExecutableLock executableLock(initial.targets.front().executable);
                write(locked.source / L"main.cpp", "int main() { return 1; }\n");
                const auto result = build(locked, options, {}, L"Debug");
                require(result.kind == cb::EventKind::BuildFailed
                    && result.text.find(L"LNK1168") != result.text.npos
                    && result.text.find(L"Закройте запущенное приложение") != result.text.npos,
                    "A locked executable did not produce the actionable LNK1168 diagnostic.");
            }
            check(locked, build(locked, options, {}, L"Debug"), false);
            std::cout << "[ PASS ] locked_executable_explains_lnk1168" << std::endl;
        }

        std::cout << "All " << (options.compiler == cb::CompilerMode::Msvc ? 19 : 18)
            << " Engine progress/build-option cases passed." << std::endl;
        if (!options.keep) {
            std::error_code cleanupError;
            require(root.filename().wstring().starts_with(L"CMakeBuild build options tests ")
                && fs::equivalent(root.parent_path(), temp, cleanupError) && !cleanupError,
                "Refusing to remove a directory outside the test workspace.");
            fs::remove_all(root);
        }
        return 0;
    } catch (const std::exception& exception) {
        std::cout << "[ FAIL ] " << exception.what() << '\n';
        if (!root.empty()) std::cout << "Fixtures preserved: " << utf8(root.wstring()) << '\n';
        return 1;
    }
}
