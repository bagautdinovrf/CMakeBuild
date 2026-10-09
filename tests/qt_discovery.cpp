#include "engine.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
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

std::string cmakePath(const fs::path& path) { return utf8(path.generic_wstring()); }
fs::path pathUtf8(std::string_view value) { return fs::path(std::u8string(value.begin(), value.end())); }

void write(const fs::path& path, std::string_view contents) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    require(static_cast<bool>(stream), "Cannot write a temporary fixture.");
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

bool samePath(const std::string& actual, const fs::path& expected) {
    std::error_code error;
    return fs::equivalent(pathUtf8(actual), expected, error) && !error;
}

std::optional<std::wstring> environment(const wchar_t* name) {
    const DWORD count = GetEnvironmentVariableW(name, nullptr, 0);
    if (!count) return std::nullopt;
    std::wstring value(count, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), count);
    require(length < count, "Cannot read the test process environment.");
    value.resize(length);
    return value;
}

class EnvironmentScope {
public:
    ~EnvironmentScope() {
        for (const auto& [name, value] : saved_) {
            SetEnvironmentVariableW(name.c_str(), value ? value->c_str() : nullptr);
        }
    }
    void set(const wchar_t* name, std::optional<std::wstring> value) {
        saved_.try_emplace(name, environment(name));
        require(SetEnvironmentVariableW(name, value ? value->c_str() : nullptr),
            "Cannot update the test process environment.");
    }
private:
    std::map<std::wstring, std::optional<std::wstring>> saved_;
};

// Remove existing package hints and Qt DLL directories so an accidental inherited
// PATH cannot make either autodiscovery or runtime restoration appear to work.
void isolateEnvironment(EnvironmentScope& scope, const fs::path& mingwBin, bool discoverMingw) {
    for (const wchar_t* key : {L"QTDIR", L"QT_ROOT", L"QT_INSTALL_DIR", L"Qt6_ROOT", L"Qt5_ROOT",
            L"Qt6_DIR", L"Qt5_DIR", L"Qt6Core_DIR", L"Qt5Core_DIR", L"CMAKE_PREFIX_PATH"}) {
        scope.set(key, std::nullopt);
    }
    const auto original = environment(L"PATH").value_or(L"");
    std::wstring cleaned;
    for (size_t begin = 0; begin < original.size(); ) {
        const auto end = original.find(L';', begin);
        auto entry = original.substr(begin, end == original.npos ? end : end - begin);
        if (entry.size() >= 2 && entry.front() == L'"' && entry.back() == L'"') {
            entry = entry.substr(1, entry.size() - 2);
        }
        std::error_code error;
        const bool hasQt = fs::is_regular_file(fs::path(entry) / L"Qt6Core.dll", error)
            || fs::is_regular_file(fs::path(entry) / L"Qt5Core.dll", error)
            || fs::is_regular_file(fs::path(entry) / L"Qt6Cored.dll", error)
            || fs::is_regular_file(fs::path(entry) / L"Qt5Cored.dll", error);
        const bool hasGcc = discoverMingw && fs::is_regular_file(fs::path(entry) / L"g++.exe", error);
        if (!entry.empty() && !hasQt && !hasGcc) {
            if (!cleaned.empty()) cleaned += L';';
            cleaned += entry;
        }
        if (end == original.npos) break;
        begin = end + 1;
    }
    if (!mingwBin.empty() && !discoverMingw) cleaned = mingwBin.wstring() + L';' + cleaned;
    scope.set(L"PATH", cleaned);
}

class Events {
public:
    void receive(cb::Event event) {
        std::lock_guard lock(mutex_);
        events_.push_back(std::move(event));
        ready_.notify_all();
    }
    cb::Event wait(cb::Engine& engine) {
        std::unique_lock lock(mutex_);
        const bool completed = ready_.wait_for(lock, 120s, [&] {
            return std::ranges::any_of(events_, terminal);
        });
        if (!completed) {
            lock.unlock();
            engine.cancel();
            throw std::runtime_error("Engine did not finish within 120 seconds.");
        }
        return *std::ranges::find_if(events_, terminal);
    }
    bool contains(std::wstring_view text) {
        std::lock_guard lock(mutex_);
        return std::ranges::any_of(events_, [&](const auto& event) {
            return event.kind == cb::EventKind::Log && event.text.find(text) != event.text.npos;
        });
    }
    void dump() {
        std::lock_guard lock(mutex_);
        for (const auto& event : events_) {
            if (!event.text.empty()) std::cout << utf8(event.text) << '\n';
        }
    }
private:
    static bool terminal(const cb::Event& event) {
        return event.kind == cb::EventKind::BuildSucceeded || event.kind == cb::EventKind::BuildFailed
            || event.kind == cb::EventKind::Finished;
    }
    std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<cb::Event> events_;
};

struct Options {
    cb::CompilerMode compiler = cb::CompilerMode::Msvc;
    fs::path qtPrefix = L"C:/Qt/6.11.2/msvc2022_64";
    fs::path cmake;
    fs::path mingwBin;
    bool keep = false;
    bool discoverMingw = false;
};

Options parse(int argc, wchar_t** argv) {
    Options result;
    bool prefixSet = false;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--keep") { result.keep = true; continue; }
        if (argument == L"--discover-mingw") { result.discoverMingw = true; continue; }
        require(index + 1 < argc, "Missing option value.");
        const std::wstring_view value(argv[++index]);
        if (argument == L"--toolchain") {
            require(value == L"msvc" || value == L"mingw", "Expected --toolchain msvc or mingw.");
            result.compiler = value == L"msvc" ? cb::CompilerMode::Msvc : cb::CompilerMode::Mingw;
        } else if (argument == L"--qt-prefix") {
            result.qtPrefix = fs::path(value);
            prefixSet = true;
        } else if (argument == L"--cmake") result.cmake = fs::path(value);
        else if (argument == L"--mingw-bin") result.mingwBin = fs::path(value);
        else throw std::runtime_error("Unknown option.");
    }
    if (result.compiler == cb::CompilerMode::Mingw) {
        if (!prefixSet) result.qtPrefix = L"C:/Qt/6.11.2/mingw_64";
        if (result.mingwBin.empty()) result.mingwBin = L"C:/Qt/Tools/mingw1310_64/bin";
        require(fs::is_regular_file(result.mingwBin / L"g++.exe"), "MinGW g++.exe is missing.");
    }
    require(!result.discoverMingw || result.compiler == cb::CompilerMode::Mingw,
        "--discover-mingw requires --toolchain mingw.");
    result.qtPrefix = fs::absolute(result.qtPrefix).lexically_normal();
    require(fs::is_regular_file(result.qtPrefix / L"lib/cmake/Qt6/Qt6Config.cmake"),
        "Qt 6 kit is missing; pass its location using --qt-prefix.");
    return result;
}

std::string installedVersion(const fs::path& prefix) {
    std::ifstream stream(prefix / L"mkspecs/qconfig.pri", std::ios::binary);
    for (std::string line; std::getline(stream, line); ) {
        if (!line.starts_with("QT_VERSION = ")) continue;
        auto version = line.substr(13);
        if (!version.empty() && version.back() == '\r') version.pop_back();
        return version;
    }
    throw std::runtime_error("Cannot read the Qt kit version.");
}

struct Fixture { fs::path source; fs::path build; };

Fixture project(const fs::path& root, std::string_view name, bool qt, bool dynamic = false,
        bool externalOutput = false) {
    Fixture fixture{root / pathUtf8(name) / L"source", root / pathUtf8(name) / L"build"};
    std::string cmake = "cmake_minimum_required(VERSION 3.24)\nproject(EngineFixture LANGUAGES CXX)\n"
        "if(MSVC)\n  set(CMAKE_CXX_STANDARD 23)\n"
        "  add_compile_options(\"$<$<COMPILE_LANGUAGE:CXX>:/std:c++latest>\")\nelse()\n"
        "  set(fixture_cxx_standards ${CMAKE_CXX_COMPILE_FEATURES})\n"
        "  list(FILTER fixture_cxx_standards INCLUDE REGEX \"^cxx_std_[0-9]+$\")\n"
        "  list(REMOVE_ITEM fixture_cxx_standards cxx_std_98)\n"
        "  list(SORT fixture_cxx_standards COMPARE NATURAL)\n"
        "  list(GET fixture_cxx_standards -1 fixture_latest_cxx)\n"
        "  string(REPLACE \"cxx_std_\" \"\" CMAKE_CXX_STANDARD \"${fixture_latest_cxx}\")\n"
        "endif()\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\n";
    if (qt) {
        cmake += "include(cmake/Dependency.cmake)\n";
        write(fixture.source / L"cmake/Dependency.cmake", dynamic
            ? "set(qt_names Qt6 Qt5)\nfind_package(QT NAMES ${qt_names} REQUIRED COMPONENTS Core)\n"
              "find_package(Qt${QT_VERSION_MAJOR} 6.11 REQUIRED COMPONENTS Core)\n"
            : "find_package(Qt6 6.11 REQUIRED COMPONENTS Core)\n");
    }
    cmake += "add_executable(engine_fixture main.cpp)\n";
    if (qt) cmake += "target_link_libraries(engine_fixture PRIVATE Qt6::Core)\n";
    if (externalOutput) {
        cmake += "set_target_properties(engine_fixture PROPERTIES RUNTIME_OUTPUT_DIRECTORY "
            "\"${CMAKE_CURRENT_SOURCE_DIR}/run outside build\")\n";
    }
    write(fixture.source / L"CMakeLists.txt", cmake);
    write(fixture.source / L"main.cpp", qt
        ? "#include <QtCore/QCoreApplication>\n#include <QtCore/QtGlobal>\n#include <iostream>\n"
          "int main(int argc, char** argv) { QCoreApplication app(argc, argv); "
          "std::cout << \"QT_VERSION=\" << qVersion() << \";COMPILED=\" << QT_VERSION_STR << '\\n'; }\n"
        : "#include <iostream>\nint main() { std::cout << \"PLAIN_OK\\n\"; }\n");
    return fixture;
}

cb::Target build(const Fixture& fixture, const Options& options) {
    Events events;
    cb::Engine engine([&](cb::Event event) { events.receive(std::move(event)); });
    cb::BuildSettings settings;
    settings.cmakeFile = (fixture.source / L"CMakeLists.txt").wstring();
    settings.buildDirectory = fixture.build.wstring();
    settings.cmakeExecutable = options.cmake.wstring();
    settings.compiler = options.compiler;
    require(engine.build(std::move(settings)), "Engine rejected the build.");
    const auto result = events.wait(engine);
    if (result.kind != cb::EventKind::BuildSucceeded) {
        events.dump();
        throw std::runtime_error("Engine build failed.");
    }
    require(result.targets.size() == 1, "Expected one CMake File API executable target.");
    require(fs::is_regular_file(result.targets.front().executable), "Built executable is missing.");
    return result.targets.front();
}

void runWithFreshEngine(const cb::Target& target, const Fixture& fixture, std::string_view expected,
        bool explicitBuildDirectory) {
    Events events;
    cb::Engine engine([&](cb::Event event) { events.receive(std::move(event)); });
    require(engine.run(target, {}, explicitBuildDirectory ? fixture.build.wstring() : std::wstring{}),
        "Fresh Engine rejected the run.");
    const auto result = events.wait(engine);
    if (result.kind != cb::EventKind::Finished || result.exitCode != 0
            || !events.contains(std::wstring(expected.begin(), expected.end()))) {
        events.dump();
        throw std::runtime_error("Fresh Engine could not run the target with the correct Qt runtime.");
    }
}

// A distinct user-selected package location forwards to the genuine native kit.
// This proves precedence without copying Qt, modifying its installation, or
// depending on a second compatible kit being installed.
fs::path forwardingPrefix(const fs::path& root, std::string_view name, const Options& options) {
    const auto prefix = root / pathUtf8(name) / L"explicit Qt prefix";
    const auto real = options.qtPrefix / L"lib/cmake/Qt6";
    write(prefix / L"lib/cmake/Qt6/Qt6Config.cmake",
        "set(CMAKEBUILD_TEST_EXPLICIT_QT \"" + std::string(name) + "\" CACHE STRING \"\" FORCE)\n"
        "include(\"" + cmakePath(real / L"Qt6Config.cmake") + "\")\n");
    write(prefix / L"lib/cmake/Qt6/Qt6ConfigVersion.cmake",
        "include(\"" + cmakePath(real / L"Qt6ConfigVersion.cmake") + "\")\n");
    return prefix;
}

void qtCase(const fs::path& root, std::string_view name, const Options& options,
        std::string_view version, bool dynamic, bool externalOutput, int explicitHint = 0) {
    std::cout << "[ RUN ] " << name << std::endl;
    const auto fixture = project(root, name, true, dynamic, externalOutput);
    auto expectedDirectory = options.qtPrefix / L"lib/cmake/Qt6";
    EnvironmentScope scope;
    std::string originalPrefix;
    if (explicitHint) {
        const auto prefix = forwardingPrefix(root, name, options);
        expectedDirectory = prefix / L"lib/cmake/Qt6";
        if (explicitHint == 1) {
            write(fixture.build / L"CMakeCache.txt", "Qt6_DIR:PATH=" + cmakePath(expectedDirectory) + "\n");
        } else if (explicitHint == 2) {
            originalPrefix = cmakePath(root / L"unrelated prefix") + ';' + cmakePath(prefix);
            write(fixture.build / L"CMakeCache.txt", "CMAKE_PREFIX_PATH:STRING=" + originalPrefix + "\n");
        } else scope.set(L"CMAKE_PREFIX_PATH", prefix.wstring());
    }
    const auto originalEnvironmentPrefix = environment(L"CMAKE_PREFIX_PATH");
    const auto target = build(fixture, options);
    auto cache = readCache(fixture.build);
    if (options.discoverMingw) require(samePath(cache["CMAKE_CXX_COMPILER"], options.mingwBin / L"g++.exe"),
        "Engine autodiscovered an unexpected MinGW compiler.");
    if (!samePath(cache["Qt6_DIR"], expectedDirectory)) {
        std::cout << "Selected Qt6_DIR: " << cache["Qt6_DIR"] << '\n';
        throw std::runtime_error("CMake selected an unexpected Qt kit or replaced an explicit package path.");
    }
    require(samePath(cache["Qt6Core_DIR"], options.qtPrefix / L"lib/cmake/Qt6Core"),
        "Qt Core does not belong to the expected compiler kit.");
    if (explicitHint) require(cache["CMAKEBUILD_TEST_EXPLICIT_QT"] == name, "Explicit package was bypassed.");
    if (explicitHint == 2) require(cache["CMAKE_PREFIX_PATH"] == originalPrefix, "Cached prefix list was modified.");
    require(environment(L"CMAKE_PREFIX_PATH") == originalEnvironmentPrefix, "Parent process prefix was modified.");
    runWithFreshEngine(target, fixture, "QT_VERSION=" + std::string(version) + ";COMPILED=" + std::string(version),
        externalOutput);
    std::cout << "[ PASS ] " << name << std::endl;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    fs::path root;
    try {
        const auto options = parse(argc, argv);
        const auto temp = fs::absolute(fs::temp_directory_path()).lexically_normal();
        root = temp / (L"CMakeBuild Qt tests " + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(GetTickCount64()));
        require(fs::create_directory(root), "Cannot create a unique temporary test directory.");
        std::cout << "Temporary fixtures: " << utf8(root.wstring()) << std::endl;
        EnvironmentScope environmentScope;
        isolateEnvironment(environmentScope, options.mingwBin, options.discoverMingw);
        const auto version = installedVersion(options.qtPrefix);

        std::cout << "[ RUN ] non_qt" << std::endl;
        const auto plain = project(root, "non_qt", false);
        const auto target = build(plain, options);
        const auto cache = readCache(plain.build);
        require(!cache.contains("Qt6_DIR") && !cache.contains("Qt5_DIR"), "Non-Qt project received a Qt cache entry.");
        runWithFreshEngine(target, plain, "PLAIN_OK", false);
        std::cout << "[ PASS ] non_qt" << std::endl;

        qtCase(root, "nested_qt_external_output", options, version, false, true);
        qtCase(root, "dynamic_qt_names", options, version, true, false);
        qtCase(root, "explicit_qt_cache", options, version, false, false, 1);
        qtCase(root, "explicit_prefix_cache", options, version, false, false, 2);
        qtCase(root, "explicit_prefix_environment", options, version, false, false, 3);

        std::cout << "All 6 Engine integration cases passed." << std::endl;
        if (!options.keep) {
            std::error_code cleanupError;
            require(root.filename().wstring().starts_with(L"CMakeBuild Qt tests ")
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
