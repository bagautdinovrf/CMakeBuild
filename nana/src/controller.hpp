#pragma once

#include "app_state.hpp"

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace cb::na {

enum class Operation { Idle, Building, Running };
enum class BuildAction { Build, Rebuild, Clean, Configure, BuildAndRun };

// UI-independent UTF-8 journal. Absolute byte offsets allow a view to retain
// selections and scroll anchors when an old prefix is removed.
class Journal {
public:
    const std::string& text() const noexcept { return text_; }
    void append(std::wstring_view text);
    void clear();
    std::uint64_t revision() const noexcept { return revision_; }
    std::uint64_t totalRemovedBytes() const noexcept { return removedBytes_; }

private:
    std::string text_;
    std::uint64_t revision_ = 0;
    std::uint64_t removedBytes_ = 0;
};

class Controller {
public:
    explicit Controller(AppState& state, std::function<void()> wake = {});
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    AppState& state() noexcept { return state_; }
    const AppState& state() const noexcept { return state_; }
    Operation operation() const noexcept { return operation_; }
    bool failed() const noexcept { return failed_; }
    const std::wstring& status() const noexcept { return status_; }
    bool closing() const noexcept { return closing_; }
    BuildAction buildAction() const noexcept { return buildAction_; }
    bool cancellationRequested() const noexcept { return cancelling_; }
    bool canQueueRun() const noexcept;
    bool runQueued() const noexcept { return runAfterBuild_; }
    bool runSelectionRequested() const noexcept { return runSelectionRequested_; }
    void dismissRunSelectionRequest() noexcept { runSelectionRequested_ = false; }
    std::optional<BuildProgress> buildProgress() const noexcept { return buildProgress_; }
    std::optional<std::chrono::milliseconds> lastBuildDuration() const noexcept { return lastBuildDuration_; }
    std::wstring durationText() const;
    Journal& journal() noexcept { return journal_; }
    const Journal& journal() const noexcept { return journal_; }

    // Called only by the UI thread. Engine callbacks only enqueue owned events.
    void drainEvents();
    void onEvent(Event event);
    void selectProject(std::wstring file);
    void build(BuildAction action = BuildAction::Build);
    // False asks the UI to show its executable menu. True means handled,
    // including stop/cancel, ignored busy calls, or a reported launch failure.
    bool run();
    // A pending post-build selection launches once after a successful choice;
    // ordinary target-menu selection only remembers the target.
    bool chooseRunTarget(std::size_t index);
    void toggleLog();
    void applySettings(BuildSettings settings, std::wstring chosen, bool selectionChanged,
        std::vector<std::pair<std::wstring, RunSettings>> runDrafts = {});
    void save();
    void close();

private:
    struct EventQueue;
    AppState& state_;
    std::shared_ptr<EventQueue> events_;
    std::unique_ptr<Engine> engine_;
    Journal journal_;
    Operation operation_ = Operation::Idle;
    bool failed_ = false;
    bool closing_ = false;
    BuildAction buildAction_ = BuildAction::Build;
    BuildAction durationAction_ = BuildAction::Build;
    bool cancelling_ = false, runAfterBuild_ = false, runSelectionRequested_ = false;
    std::optional<BuildProgress> buildProgress_;
    std::optional<std::chrono::milliseconds> lastBuildDuration_;
    std::wstring status_;
    void recordDuration(const Event&);
};

} // namespace cb::na

