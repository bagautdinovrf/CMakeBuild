#include "engine.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
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
    require(static_cast<bool>(stream), "Cannot write a rebuild fixture.");
}

std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "Cannot read a rebuild fixture.");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

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
        if (argument == L"--cmake") result.cmake = fs::path(value);
        else if (argument == L"--mingw-bin") result.mingwBin = fs::path(value);
        else if (argument == L"--toolchain") {
            require(value == L"msvc" || value == L"mingw", "Expected --toolchain msvc or mingw.");
            result.compiler = value == L"msvc" ? cb::CompilerMode::Msvc : cb::CompilerMode::Mingw;
        } else throw std::runtime_error("Unknown option.");
    }
    if (result.compiler == cb::CompilerMode::Mingw && result.mingwBin.empty())
        result.mingwBin = L"C:/Qt/Tools/mingw1310_64/bin";
    require(result.compiler == cb::CompilerMode::Mingw || result.mingwBin.empty(),
        "--mingw-bin requires --toolchain mingw.");
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
            require(length < count, "Cannot read PATH.");
            saved_->resize(length);
        }
        const auto path = bin.wstring() + L';' + saved_.value_or(L"");
        require(SetEnvironmentVariableW(L"PATH", path.c_str()), "Cannot set fixture PATH.");
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
        const auto completed = [application](const cb::Event& event) {
            return application ? event.kind == cb::EventKind::Finished : terminal(event);
        };
        if (!ready_.wait_for(lock, 120s, [&] { return std::ranges::any_of(events_, completed); })) {
            lock.unlock();
            engine.cancel();
            throw std::runtime_error("Rebuild did not finish within 120 seconds.");
        }
        return *std::ranges::find_if(events_, completed);
    }
    void waitUntilRebuilding(cb::Engine& engine, const fs::path& marker) {
        std::unique_lock lock(mutex_);
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        // Ninja buffers a command's stdout until it finishes. A fixture-owned
        // marker observes the running command without depending on that buffer.
        while (!fs::is_regular_file(marker)) {
            if (std::chrono::steady_clock::now() >= deadline
                || std::ranges::any_of(events_, terminal)) {
                lock.unlock();
                engine.cancel();
                throw std::runtime_error("Slow rebuild did not reach the cancellable command.");
            }
            ready_.wait_for(lock, 50ms);
        }
    }
    bool contains(std::wstring_view text) {
        std::lock_guard lock(mutex_);
        return containsUnlocked(text);
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
    void checkDuration(std::chrono::steady_clock::duration elapsed, bool allowZero = false) {
        std::lock_guard lock(mutex_);
        size_t terminalCount = 0;
        for (const auto& event : events_) {
            if (terminal(event)) {
                ++terminalCount;
                require(event.buildDuration && (allowZero ? *event.buildDuration >= 0ms : *event.buildDuration > 0ms),
                    "Operation completion did not report a valid duration.");
                require(*event.buildDuration <= elapsed, "Rebuild duration exceeds observed time.");
            } else require(!event.buildDuration, "Nonterminal event reported rebuild duration.");
        }
        require(terminalCount == 1, "Rebuild emitted more than one completion event.");
    }
    void dump() {
        std::lock_guard lock(mutex_);
        for (const auto& event : events_)
            if (!event.text.empty()) std::cout << utf8(event.text) << '\n';
    }
private:
    static bool commandArgument(const cb::Event& event, std::wstring_view argument) {
        if (event.kind != cb::EventKind::Log || !event.text.starts_with(L"> ")) return false;
        // Engine::commandLine quotes every argument, even options without
        // spaces. Check a complete token only in the process-launch record.
        const auto token = L" \"" + std::wstring(argument) + L"\"";
        auto command = std::wstring_view(event.text);
        while (!command.empty() && (command.back() == L'\r' || command.back() == L'\n')) command.remove_suffix(1);
        return command.find(token + L' ') != command.npos || command.ends_with(token);
    }
    static bool terminal(const cb::Event& event) {
        return event.kind == cb::EventKind::BuildSucceeded || event.kind == cb::EventKind::BuildFailed
            || event.kind == cb::EventKind::CleanSucceeded || event.kind == cb::EventKind::CleanFailed
            || event.kind == cb::EventKind::ConfigureSucceeded || event.kind == cb::EventKind::ConfigureFailed;
    }
    bool containsUnlocked(std::wstring_view text) const {
        return std::ranges::any_of(events_, [&](const auto& event) {
            return event.kind == cb::EventKind::Log && event.text.find(text) != event.text.npos;
        });
    }
    std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<cb::Event> events_;
};

struct Fixture { fs::path source; fs::path build; };

Fixture project(const fs::path& root) {
    Fixture fixture{root / L"проект с пробелами", root / L"каталог сборки"};
    write(fixture.source / L"CMakeLists.txt", R"cmake(cmake_minimum_required(VERSION 3.24)
project(EngineRebuildFixture LANGUAGES NONE)
file(APPEND "${CMAKE_SOURCE_DIR}/configure-invocations.txt" "configured;")
if(EXISTS "${CMAKE_SOURCE_DIR}/configure-control.txt")
    file(READ "${CMAKE_SOURCE_DIR}/configure-control.txt" configure_mode)
    if(configure_mode STREQUAL "fail")
        message(FATAL_ERROR "CMAKEBUILD_CONFIGURE_DELIBERATE_FAILURE")
    elseif(configure_mode STREQUAL "wait")
        file(WRITE "${CMAKE_SOURCE_DIR}/configure-control.txt.waiting" "waiting")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 30 COMMAND_ERROR_IS_FATAL ANY)
    endif()
endif()
set(REBUILD_KEEP "initial value" CACHE STRING "User setting preserved by clean rebuild")
foreach(name IN ITEMS selected unselected)
    add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/${name}.txt"
        COMMAND "${CMAKE_COMMAND}"
            "-DOUTPUT=${CMAKE_BINARY_DIR}/${name}.txt"
            "-DWITNESS=${CMAKE_SOURCE_DIR}/${name}-invocations.txt"
            "-DCONTROL=${CMAKE_SOURCE_DIR}/control.txt"
            -P "${CMAKE_SOURCE_DIR}/make-artifact.cmake"
        DEPENDS "${CMAKE_SOURCE_DIR}/make-artifact.cmake"
        VERBATIM)
    add_custom_target(${name} ALL DEPENDS "${CMAKE_BINARY_DIR}/${name}.txt")
endforeach()
)cmake");
    write(fixture.source / L"make-artifact.cmake", R"cmake(file(READ "${CONTROL}" mode)
if(mode STREQUAL "fail")
    message(FATAL_ERROR "CMAKEBUILD_REBUILD_DELIBERATE_FAILURE")
elseif(mode STREQUAL "wait")
    file(WRITE "${CONTROL}.waiting" "waiting")
    message(STATUS "CMAKEBUILD_REBUILD_WAITING")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 30 COMMAND_ERROR_IS_FATAL ANY)
endif()
file(APPEND "${WITNESS}" "built;")
file(WRITE "${OUTPUT}" "generated artifact\n")
)cmake");
    write(fixture.source / L"control.txt", "success");
    return fixture;
}

cb::Event build(const Fixture& fixture, const Options& options, std::optional<bool> cleanFirst = {},
        std::wstring_view target = {}, bool cancel = false, std::optional<bool> expectedConfigure = {},
        std::wstring_view configuration = L"Release") {
    Events events;
    cb::Engine engine([&](cb::Event event) { events.receive(std::move(event)); });
    cb::BuildSettings settings;
    settings.cmakeFile = (fixture.source / L"CMakeLists.txt").wstring();
    settings.buildDirectory = fixture.build.wstring();
    settings.cmakeExecutable = options.cmake.wstring();
    settings.compiler = options.compiler;
    settings.configuration = configuration;
    settings.target = target;
    // Unspecified intentionally exercises the public API default.
    if (cleanFirst) settings.cleanFirst = *cleanFirst;
    const auto started = std::chrono::steady_clock::now();
    require(engine.build(std::move(settings)), "Engine rejected rebuild.");
    if (cancel) {
        events.waitUntilRebuilding(engine, fixture.source / L"control.txt.waiting");
        engine.cancel();
    }
    const auto result = events.wait(engine);
    events.checkDuration(std::chrono::steady_clock::now() - started);
    require(events.invokedArgument(L"--clean-first") == cleanFirst.value_or(false),
        "Clean-first argument does not match the requested build action.");
    if (expectedConfigure) require(events.explicitlyConfigured() == *expectedConfigure,
        "Build did not use the expected explicit CMake configuration policy.");
    if (!cancel && result.kind == cb::EventKind::BuildFailed
        && !events.contains(L"CMAKEBUILD_REBUILD_DELIBERATE_FAILURE")) {
        events.dump();
        throw std::runtime_error("Rebuild failed before the deliberately failing command.");
    }
    if (cancel) require(result.kind == cb::EventKind::BuildFailed && result.exitCode == ERROR_CANCELLED,
        "Cancelling clean rebuild did not report cancellation.");
    return result;
}

std::wstring cachePath(const Fixture& fixture, std::string_view key) {
    std::ifstream stream(fixture.build / L"CMakeCache.txt", std::ios::binary);
    require(static_cast<bool>(stream), "Cannot read the configured fixture cache.");
    for (std::string line; std::getline(stream, line); ) {
        if (!line.starts_with(std::string(key) + ':')) continue;
        const auto equals = line.find('=');
        if (equals == line.npos) continue;
        auto value = line.substr(equals + 1);
        if (!value.empty() && value.back() == '\r') value.pop_back();
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), nullptr, 0);
        require(length > 0, "CMake cache path is not valid UTF-8.");
        std::wstring result(static_cast<std::size_t>(length), L'\0');
        require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), length) == length, "Cannot decode CMake cache path.");
        return result;
    }
    throw std::runtime_error("Required CMake cache path is missing.");
}

cb::Event configure(const Fixture& fixture, const Options& options, bool cancel = false,
        bool failureExpected = false) {
    Events events;
    cb::Engine engine([&](cb::Event event) { events.receive(std::move(event)); });
    cb::BuildSettings settings;
    settings.cmakeFile = (fixture.source / L"CMakeLists.txt").wstring();
    settings.buildDirectory = fixture.build.wstring();
    settings.cmakeExecutable = options.cmake.wstring();
    settings.compiler = options.compiler;
    const auto started = std::chrono::steady_clock::now();
    require(engine.configure(std::move(settings)), "Engine rejected standalone configuration.");
    if (cancel) {
        events.waitUntilRebuilding(engine, fixture.source / L"configure-control.txt.waiting");
        engine.cancel();
    }
    const auto result = events.wait(engine);
    events.checkDuration(std::chrono::steady_clock::now() - started, true);
    require(events.explicitlyConfigured(), "Standalone configuration did not explicitly run CMake.");
    require(!events.invokedArgument(L"--build"), "Standalone configuration unexpectedly launched compilation.");
    require(result.kind == cb::EventKind::ConfigureSucceeded || result.kind == cb::EventKind::ConfigureFailed,
        "Standalone configuration emitted another operation's terminal event.");
    if (cancel) require(result.kind == cb::EventKind::ConfigureFailed && result.exitCode == ERROR_CANCELLED,
        "Cancelled standalone configuration did not report cancellation.");
    else if (failureExpected) require(result.kind == cb::EventKind::ConfigureFailed
            && events.contains(L"CMAKEBUILD_CONFIGURE_DELIBERATE_FAILURE"),
        "Standalone configuration missed the deliberately failing project script.");
    else if (result.kind != cb::EventKind::ConfigureSucceeded) {
        events.dump();
        throw std::runtime_error("Standalone configuration failed.");
    }
    return result;
}

void checkConfigureReuse(const fs::path& root, const Options& options) {
    const auto fixture = project(root / L"configuration reuse");
    const auto configured = [&] { return read(fixture.source / L"configure-invocations.txt"); };
    std::cout << "[ RUN ] standalone_configure_reruns_without_building" << std::endl;
    configure(fixture, options);
    require(configured() == "configured;" && fs::is_regular_file(fixture.build / L"CMakeCache.txt")
        && !fs::exists(fixture.build / L"selected.txt") && !fs::exists(fixture.build / L"unselected.txt")
        && !fs::exists(fixture.source / L"selected-invocations.txt")
        && !fs::exists(fixture.source / L"unselected-invocations.txt"),
        "Standalone configuration compiled outputs or failed to prepare the cache.");
    configure(fixture, options);
    require(configured() == "configured;configured;"
        && !fs::exists(fixture.source / L"selected-invocations.txt"),
        "Explicit CMake did not rerun unchanged project scripts without building.");
    std::cout << "[ PASS ] standalone_configure_reruns_without_building" << std::endl;

    std::cout << "[ RUN ] reused_configuration_survives_engine_restart_target_and_clean_first" << std::endl;
    const auto beforeBuild = configured();
    require(build(fixture, options, {}, {}, false, false).kind == cb::EventKind::BuildSucceeded,
        "Build could not reuse the standalone configuration.");
    require(configured() == beforeBuild, "Build unnecessarily configured unchanged scripts.");
    const auto selectedBefore = read(fixture.source / L"selected-invocations.txt");
    const auto unselectedBefore = read(fixture.source / L"unselected-invocations.txt");
    require(build(fixture, options, false, L"selected", false, false).kind == cb::EventKind::BuildSucceeded,
        "A target-only change invalidated the usable build tree.");
    require(configured() == beforeBuild
        && read(fixture.source / L"selected-invocations.txt") == selectedBefore
        && read(fixture.source / L"unselected-invocations.txt") == unselectedBefore,
        "A restarted Engine reconfigured or rebuilt unchanged outputs after changing the target.");
    require(build(fixture, options, true, L"selected", false, false).kind == cb::EventKind::BuildSucceeded,
        "Clean-first could not reuse an existing configuration.");
    require(configured() == beforeBuild
        && read(fixture.source / L"selected-invocations.txt") == selectedBefore + "built;"
        && !fs::exists(fixture.build / L"unselected.txt"),
        "Clean-first configured CMake or failed to clean/rebuild the selected output.");
    std::cout << "[ PASS ] reused_configuration_survives_engine_restart_target_and_clean_first" << std::endl;

    std::cout << "[ RUN ] missing_generated_files_and_cache_force_configuration" << std::endl;
    const auto generated = fs::is_regular_file(fixture.build / L"build.ninja")
        ? fixture.build / L"build.ninja" : fixture.build / L"Makefile";
    require(fs::is_regular_file(generated), "Fixture has no recognized generated build file.");
    require(fs::remove(generated), "Cannot remove the generated build file in the isolated fixture.");
    auto beforeRecovery = configured();
    require(build(fixture, options, {}, {}, false, true).kind == cb::EventKind::BuildSucceeded
        && configured() == beforeRecovery + "configured;" && fs::is_regular_file(generated),
        "Missing generated files did not trigger a usable explicit reconfiguration.");
    require(fs::remove(fixture.build / L"CMakeCache.txt"), "Cannot remove the isolated fixture cache.");
    beforeRecovery = configured();
    require(build(fixture, options, {}, {}, false, true).kind == cb::EventKind::BuildSucceeded
        && configured() == beforeRecovery + "configured;",
        "Missing cache did not force configuration before compilation.");
    std::cout << "[ PASS ] missing_generated_files_and_cache_force_configuration" << std::endl;

    std::cout << "[ RUN ] failed_and_cancelled_explicit_configure_invalidate_existing_tree" << std::endl;
    write(fixture.source / L"configure-control.txt", "fail");
    configure(fixture, options, false, true);
    write(fixture.source / L"configure-control.txt", "success");
    auto beforeRetry = configured();
    require(build(fixture, options, {}, {}, false, true).kind == cb::EventKind::BuildSucceeded
        && configured() == beforeRetry + "configured;",
        "Build reused a cache after an explicitly failed configuration instead of retrying.");
    write(fixture.source / L"configure-control.txt", "wait");
    configure(fixture, options, true);
    write(fixture.source / L"configure-control.txt", "success");
    beforeRetry = configured();
    require(build(fixture, options, {}, {}, false, true).kind == cb::EventKind::BuildSucceeded
        && configured() == beforeRetry + "configured;",
        "Build reused an incomplete cache after cancelled explicit configuration.");
    beforeRetry = configured();
    require(build(fixture, options, {}, {}, false, false).kind == cb::EventKind::BuildSucceeded
        && configured() == beforeRetry, "Successful retry did not restore configuration reuse.");
    std::cout << "[ PASS ] failed_and_cancelled_explicit_configure_invalidate_existing_tree" << std::endl;

    std::cout << "[ RUN ] reusable_cache_still_rejects_another_source_project" << std::endl;
    const auto foreignSource = root / L"foreign configuration source";
    write(foreignSource / L"CMakeLists.txt",
        "cmake_minimum_required(VERSION 3.24)\nproject(ForeignConfiguredProject LANGUAGES NONE)\n");
    const auto cacheBeforeForeign = read(fixture.build / L"CMakeCache.txt");
    const auto configuredBeforeForeign = configured();
    Events foreignEvents;
    cb::Engine foreignEngine([&](cb::Event event) { foreignEvents.receive(std::move(event)); });
    cb::BuildSettings foreignSettings;
    foreignSettings.cmakeFile = (foreignSource / L"CMakeLists.txt").wstring();
    foreignSettings.buildDirectory = fixture.build.wstring();
    foreignSettings.cmakeExecutable = options.cmake.wstring();
    foreignSettings.compiler = options.compiler;
    const auto foreignStarted = std::chrono::steady_clock::now();
    require(foreignEngine.build(std::move(foreignSettings)), "Engine rejected starting the foreign-cache check.");
    require(foreignEvents.wait(foreignEngine).kind == cb::EventKind::BuildFailed,
        "Build reused a cache belonging to another source project.");
    foreignEvents.checkDuration(std::chrono::steady_clock::now() - foreignStarted, true);
    require(!foreignEvents.invokedArgument(L"--build")
        && read(fixture.build / L"CMakeCache.txt") == cacheBeforeForeign
        && configured() == configuredBeforeForeign,
        "Rejected foreign-cache build compiled targets or changed the original project/cache.");
    std::cout << "[ PASS ] reusable_cache_still_rejects_another_source_project" << std::endl;

    std::cout << "[ RUN ] multi_config_cache_reuses_debug_and_release" << std::endl;
    const auto nativeMake = cachePath(fixture, "CMAKE_MAKE_PROGRAM");
    if (_wcsicmp(fs::path(nativeMake).filename().c_str(), L"ninja.exe") != 0
        && _wcsicmp(fs::path(nativeMake).filename().c_str(), L"ninja") != 0) {
        std::cout << "[ SKIP ] multi_config_cache_reuses_debug_and_release: Ninja is unavailable" << std::endl;
        return;
    }
    const auto multi = project(root / L"Ninja Multi-Config reuse");
    Events seedEvents;
    cb::Engine seedEngine([&](cb::Event event) { seedEvents.receive(std::move(event)); });
    const auto cmake = cachePath(fixture, "CMAKE_COMMAND");
    cb::RunSettings seed;
    seed.arguments = L"-S \"" + multi.source.wstring() + L"\" -B \"" + multi.build.wstring()
        + L"\" -G \"Ninja Multi-Config\" \"-DCMAKE_MAKE_PROGRAM=" + nativeMake
        + L"\" -DBUILD_TESTING:BOOL=OFF -DBUILD_TESTS:BOOL=OFF";
    require(seedEngine.runConfigured({L"CMake Multi-Config fixture", cmake}, std::move(seed)),
        "Cannot start native CMake to seed a multi-configuration cache.");
    const auto seeded = seedEvents.wait(seedEngine, true);
    if (seeded.exitCode) seedEvents.dump();
    require(seeded.exitCode == 0 && fs::is_regular_file(multi.build / L"build-Debug.ninja")
        && fs::is_regular_file(multi.build / L"build-Release.ninja"),
        "Native Ninja Multi-Config fixture did not create both configurations.");
    configure(multi, options);
    const auto multiCount = read(multi.source / L"configure-invocations.txt");
    require(build(multi, options, {}, {}, false, false, L"Debug").kind == cb::EventKind::BuildSucceeded,
        "Engine could not reuse the configured Debug build tree.");
    require(build(multi, options, {}, {}, false, false, L"Release").kind == cb::EventKind::BuildSucceeded,
        "Switching a multi-configuration tree to Release forced configuration or failed.");
    require(build(multi, options, {}, {}, false, false, L"Debug").kind == cb::EventKind::BuildSucceeded
        && read(multi.source / L"configure-invocations.txt") == multiCount,
        "Selecting an existing Debug/Release configuration reran CMake scripts.");
    std::cout << "[ PASS ] multi_config_cache_reuses_debug_and_release" << std::endl;
}

cb::Event clean(const Fixture& fixture, const Options& options) {
    Events events;
    cb::Engine engine([&](cb::Event event) { events.receive(std::move(event)); });
    cb::BuildSettings settings;
    settings.cmakeFile = (fixture.source / L"CMakeLists.txt").wstring();
    settings.buildDirectory = fixture.build.wstring();
    settings.cmakeExecutable = options.cmake.wstring();
    settings.compiler = options.compiler;
    // Cleanup applies to the project even if its ordinary build selects a target.
    settings.target = L"selected";
    const auto started = std::chrono::steady_clock::now();
    require(engine.clean(std::move(settings)), "Engine rejected standalone cleanup.");
    const auto result = events.wait(engine);
    events.checkDuration(std::chrono::steady_clock::now() - started, true);
    require(result.kind == cb::EventKind::CleanSucceeded || result.kind == cb::EventKind::CleanFailed,
        "Standalone cleanup reported a build completion event.");
    require(!events.invokedArgument(L"--clean-first"), "Standalone cleanup unexpectedly requested rebuilding.");
    return result;
}

void checkPreserved(const Fixture& fixture, const std::string& sources) {
    require(read(fixture.source / L"CMakeLists.txt") == sources, "Clean rebuild changed project sources.");
    require(read(fixture.build / L"unrelated.keep") == "user data", "Clean rebuild removed unrelated data.");
    require(read(fixture.build / L"CMakeCache.txt").find("REBUILD_KEEP:STRING=user cached value")
        != std::string::npos, "Clean rebuild reset the user's CMake cache value.");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    fs::path root;
    try {
        const auto options = parse(argc, argv);
        PathScope pathScope(options.mingwBin);
        const auto temp = fs::absolute(fs::temp_directory_path()).lexically_normal();
        root = temp / (L"CMakeBuild rebuild tests " + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(GetTickCount64()));
        require(fs::create_directory(root), "Cannot create rebuild test directory.");
        std::cout << "Temporary fixtures: " << utf8(root.wstring()) << std::endl;
        checkConfigureReuse(root, options);
        const auto fixture = project(root);
        const auto sources = read(fixture.source / L"CMakeLists.txt");
        require(build(fixture, options).kind == cb::EventKind::BuildSucceeded, "Initial fixture build failed.");
        require(read(fixture.source / L"selected-invocations.txt") == "built;"
            && read(fixture.source / L"unselected-invocations.txt") == "built;",
            "Initial fixture did not build all outputs exactly once.");
        auto cache = read(fixture.build / L"CMakeCache.txt");
        const std::string cachedSetting = "REBUILD_KEEP:STRING=initial value";
        const auto position = cache.find(cachedSetting);
        require(position != cache.npos, "Fixture cache value is missing.");
        cache.replace(position, cachedSetting.size(), "REBUILD_KEEP:STRING=user cached value");
        write(fixture.build / L"CMakeCache.txt", cache);
        write(fixture.build / L"unrelated.keep", "user data");

        std::cout << "[ RUN ] default_build_stays_incremental" << std::endl;
        require(build(fixture, options).kind == cb::EventKind::BuildSucceeded, "Incremental build failed.");
        require(read(fixture.source / L"selected-invocations.txt") == "built;"
            && read(fixture.source / L"unselected-invocations.txt") == "built;",
            "Default build unexpectedly cleaned or rebuilt up-to-date outputs.");
        checkPreserved(fixture, sources);
        std::cout << "[ PASS ] default_build_stays_incremental" << std::endl;

        std::cout << "[ RUN ] clean_first_regenerates_all_outputs_and_preserves_cache" << std::endl;
        require(build(fixture, options, true).kind == cb::EventKind::BuildSucceeded, "Clean rebuild failed.");
        require(read(fixture.source / L"selected-invocations.txt") == "built;built;"
            && read(fixture.source / L"unselected-invocations.txt") == "built;built;",
            "Clean rebuild did not regenerate previously up-to-date outputs.");
        checkPreserved(fixture, sources);
        std::cout << "[ PASS ] clean_first_regenerates_all_outputs_and_preserves_cache" << std::endl;

        std::cout << "[ RUN ] clean_first_respects_selected_target" << std::endl;
        require(build(fixture, options, true, L"selected").kind == cb::EventKind::BuildSucceeded,
            "Selected target clean rebuild failed.");
        require(fs::is_regular_file(fixture.build / L"selected.txt")
            && !fs::exists(fixture.build / L"unselected.txt")
            && read(fixture.source / L"selected-invocations.txt") == "built;built;built;"
            && read(fixture.source / L"unselected-invocations.txt") == "built;built;",
            "Selected target was ignored or clean-first did not clean old outputs.");
        checkPreserved(fixture, sources);
        std::cout << "[ PASS ] clean_first_respects_selected_target" << std::endl;

        std::cout << "[ RUN ] clean_first_failure_reports_duration_and_preserves_cache" << std::endl;
        write(fixture.source / L"control.txt", "fail");
        require(build(fixture, options, true, L"selected").kind == cb::EventKind::BuildFailed,
            "Clean rebuild ignored the deliberately failing command.");
        require(!fs::exists(fixture.build / L"selected.txt"), "Failed rebuild left the old output.");
        checkPreserved(fixture, sources);
        std::cout << "[ PASS ] clean_first_failure_reports_duration_and_preserves_cache" << std::endl;

        std::cout << "[ RUN ] clean_first_cancel_reports_duration_and_recovers" << std::endl;
        write(fixture.source / L"control.txt", "wait");
        build(fixture, options, true, L"selected", true);
        require(!fs::exists(fixture.build / L"selected.txt"), "Cancelled rebuild completed the slow output.");
        checkPreserved(fixture, sources);
        write(fixture.source / L"control.txt", "success");
        require(build(fixture, options, false, L"selected").kind == cb::EventKind::BuildSucceeded,
            "Incremental build did not recover after cancelled clean rebuild.");
        require(fs::is_regular_file(fixture.build / L"selected.txt"), "Recovery output is missing.");
        checkPreserved(fixture, sources);
        std::cout << "[ PASS ] clean_first_cancel_reports_duration_and_recovers" << std::endl;

        std::cout << "[ RUN ] standalone_clean_removes_outputs_without_configuring_or_building" << std::endl;
        require(build(fixture, options).kind == cb::EventKind::BuildSucceeded,
            "Cannot prepare all outputs for standalone cleanup.");
        const auto selectedBeforeClean = read(fixture.source / L"selected-invocations.txt");
        const auto unselectedBeforeClean = read(fixture.source / L"unselected-invocations.txt");
        const auto configureBeforeClean = read(fixture.source / L"configure-invocations.txt");
        const auto cacheBeforeClean = read(fixture.build / L"CMakeCache.txt");
        require(clean(fixture, options).kind == cb::EventKind::CleanSucceeded, "Standalone cleanup failed.");
        require(!fs::exists(fixture.build / L"selected.txt") && !fs::exists(fixture.build / L"unselected.txt"),
            "Standalone cleanup did not remove the project's generated outputs.");
        require(read(fixture.source / L"selected-invocations.txt") == selectedBeforeClean
            && read(fixture.source / L"unselected-invocations.txt") == unselectedBeforeClean,
            "Standalone cleanup unexpectedly rebuilt a target.");
        require(read(fixture.source / L"configure-invocations.txt") == configureBeforeClean,
            "Standalone cleanup unexpectedly configured CMake.");
        require(read(fixture.build / L"CMakeCache.txt") == cacheBeforeClean,
            "Standalone cleanup changed CMakeCache.txt.");
        checkPreserved(fixture, sources);
        std::cout << "[ PASS ] standalone_clean_removes_outputs_without_configuring_or_building" << std::endl;

        std::cout << "[ RUN ] repeated_standalone_clean_succeeds" << std::endl;
        require(clean(fixture, options).kind == cb::EventKind::CleanSucceeded, "Repeated standalone cleanup failed.");
        require(!fs::exists(fixture.build / L"selected.txt") && !fs::exists(fixture.build / L"unselected.txt")
            && read(fixture.source / L"selected-invocations.txt") == selectedBeforeClean
            && read(fixture.source / L"unselected-invocations.txt") == unselectedBeforeClean
            && read(fixture.source / L"configure-invocations.txt") == configureBeforeClean
            && read(fixture.build / L"CMakeCache.txt") == cacheBeforeClean,
            "Repeated cleanup changed the project or unexpectedly ran configuration/build.");
        checkPreserved(fixture, sources);
        std::cout << "[ PASS ] repeated_standalone_clean_succeeds" << std::endl;

        std::cout << "[ RUN ] ordinary_build_restores_outputs_after_standalone_clean" << std::endl;
        require(build(fixture, options).kind == cb::EventKind::BuildSucceeded,
            "Ordinary build failed after standalone cleanup.");
        require(fs::is_regular_file(fixture.build / L"selected.txt")
            && fs::is_regular_file(fixture.build / L"unselected.txt")
            && read(fixture.source / L"selected-invocations.txt") == selectedBeforeClean + "built;"
            && read(fixture.source / L"unselected-invocations.txt") == unselectedBeforeClean + "built;",
            "Ordinary build did not regenerate cleaned outputs exactly once.");
        checkPreserved(fixture, sources);
        std::cout << "[ PASS ] ordinary_build_restores_outputs_after_standalone_clean" << std::endl;

        std::cout << "[ RUN ] standalone_clean_requires_existing_cache_without_creating_directory" << std::endl;
        const Fixture missingCache{fixture.source, root / L"never configured"};
        const auto configureBeforeMissing = read(fixture.source / L"configure-invocations.txt");
        require(!fs::exists(missingCache.build), "Missing-cache fixture already exists.");
        require(clean(missingCache, options).kind == cb::EventKind::CleanFailed,
            "Standalone cleanup unexpectedly accepted a directory without a cache.");
        require(!fs::exists(missingCache.build)
            && read(fixture.source / L"configure-invocations.txt") == configureBeforeMissing,
            "Failed standalone cleanup created/configured a build directory.");
        checkPreserved(fixture, sources);
        std::cout << "[ PASS ] standalone_clean_requires_existing_cache_without_creating_directory" << std::endl;

        std::cout << "[ RUN ] standalone_clean_rejects_another_projects_cache" << std::endl;
        const Fixture foreignProject{root / L"другой проект", fixture.build};
        write(foreignProject.source / L"CMakeLists.txt",
            "cmake_minimum_required(VERSION 3.24)\nproject(ForeignProject LANGUAGES NONE)\n");
        const auto cacheBeforeForeign = read(fixture.build / L"CMakeCache.txt");
        const auto selectedBeforeForeign = read(fixture.source / L"selected-invocations.txt");
        const auto unselectedBeforeForeign = read(fixture.source / L"unselected-invocations.txt");
        require(clean(foreignProject, options).kind == cb::EventKind::CleanFailed,
            "Standalone cleanup accepted another source project's cache.");
        require(fs::is_regular_file(fixture.build / L"selected.txt")
            && fs::is_regular_file(fixture.build / L"unselected.txt")
            && read(fixture.source / L"selected-invocations.txt") == selectedBeforeForeign
            && read(fixture.source / L"unselected-invocations.txt") == unselectedBeforeForeign
            && read(fixture.source / L"configure-invocations.txt") == configureBeforeMissing
            && read(fixture.build / L"CMakeCache.txt") == cacheBeforeForeign,
            "Rejected standalone cleanup changed the other project's build.");
        checkPreserved(fixture, sources);
        std::cout << "[ PASS ] standalone_clean_rejects_another_projects_cache" << std::endl;

        std::cout << "All 16 configure/rebuild/cleanup integration cases completed." << std::endl;
        if (!options.keep) {
            std::error_code cleanupError;
            require(root.filename().wstring().starts_with(L"CMakeBuild rebuild tests ")
                && fs::equivalent(root.parent_path(), temp, cleanupError) && !cleanupError,
                "Refusing cleanup outside the test workspace.");
            fs::remove_all(root);
        }
        return 0;
    } catch (const std::exception& exception) {
        std::cout << "[ FAIL ] " << exception.what() << '\n';
        if (!root.empty()) std::cout << "Fixtures preserved: " << utf8(root.wstring()) << '\n';
        return 1;
    }
}
