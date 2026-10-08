#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "build_progress.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cb {

enum class CompilerMode { Automatic, Msvc, Mingw, Environment };

struct BuildSettings {
    std::wstring cmakeFile;
    std::wstring buildDirectory;
    std::wstring cmakeExecutable;
    // Additional configure arguments, using Windows command-line quoting.
    std::wstring cmakeArguments;
    std::wstring configuration = L"Release";
    CompilerMode compiler = CompilerMode::Automatic;
    std::wstring target;
    bool buildTests = false;
    bool cleanFirst = false;
};

struct Target {
    std::wstring name;
    std::wstring executable;
};

struct RunSettings {
    // Raw Windows command-line arguments; passed directly to the executable,
    // without a shell. The caller supplies any quoting needed by that program.
    std::wstring arguments;
    // Empty means the executable's directory; relative paths use that directory.
    std::wstring workingDirectory;
    // Overrides apply only to this run, after automatic runtime PATH preparation.
    std::vector<std::pair<std::wstring, std::wstring>> environment;
};

enum class EventKind {
    Log, Status, BuildSucceeded, BuildFailed, Started, Finished, Targets, Progress,
    CleanSucceeded, CleanFailed, ConfigureSucceeded, ConfigureFailed
};

struct Event {
    EventKind kind;
    std::wstring text;
    std::vector<Target> targets;
    DWORD exitCode = 0;
    // Total elapsed build, clean or configure task time, including tool
    // preparation. Present only on the corresponding terminal events,
    // including cancellation.
    std::optional<std::chrono::milliseconds> buildDuration{};
    // Only Progress events carry the native build tool's completed/total count.
    std::optional<BuildProgress> progress{};
};

// Callbacks arrive on a worker thread. Queue them and marshal to the UI thread;
// the callback must remain nonblocking and must not access GUI widgets.
class Engine {
public:
    using Callback = std::function<void(Event)>;

    explicit Engine(Callback callback);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool build(BuildSettings settings);
    // Always rerun CMake configuration and refresh File API/runtime information,
    // without compiling, cleaning or running any target.
    bool configure(BuildSettings settings);
    // Run the generated clean target in an existing, matching build tree only.
    // No configure step or removal of the source/cache directories is performed.
    bool clean(BuildSettings settings);
    // Supplying the build directory restores cached runtime paths after a panel
    // restart, including targets whose output directory is outside the build tree.
    bool run(const Target& target, std::wstring workingDirectory, std::wstring buildDirectory = {});
    bool runConfigured(const Target& target, RunSettings settings, std::wstring buildDirectory = {});
    void cancel();
    bool busy() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A quick, read-only inventory for the settings dialog; it never starts tools.
std::wstring describeDetectedTools();

// Restore built executables from existing CMake File API replies without starting
// tools. Missing or invalid replies return an empty list. projectMatches is false
// only when the cache or codemodel identifies a different source project.
std::vector<Target> readExecutableTargets(const BuildSettings& settings, bool* projectMatches = nullptr) noexcept;

} // namespace cb
