#include "controller.hpp"
#include "platform.hpp"

#include <algorithm>
#include <deque>
#include <exception>
#include <filesystem>
#include <format>
#include <mutex>
#include <utility>

namespace cb::na {
namespace {
bool executableExists(const Target& target) {
    std::error_code error;
    return std::filesystem::is_regular_file(target.executable, error);
}

std::wstring readyStatus(const AppState& state) {
    return state.settings.cmakeFile.empty() ? L"Выберите CMakeLists.txt"
        : L"Готов к сборке · " + state.settings.configuration;
}
std::wstring formatElapsed(std::chrono::milliseconds elapsed) {
    const auto hundredths = (std::max(elapsed.count(), std::chrono::milliseconds::rep{0}) + 5) / 10;
    const auto seconds = hundredths / 100, fraction = hundredths % 100;
    if (seconds >= 3600) return std::format(L"{} ч {:02} мин {:02},{:02} с", seconds / 3600, seconds / 60 % 60, seconds % 60, fraction);
    if (seconds >= 60) return std::format(L"{} мин {:02},{:02} с", seconds / 60, seconds % 60, fraction);
    return std::format(L"{},{:02} с", seconds, fraction);
}
}

void Journal::append(std::wstring_view value) {
    if (value.empty()) return;
    auto bytes = platform::utf8(value);
    std::erase(bytes, '\r');
    if (bytes.empty()) return;
    text_ += bytes;
    if (text_.size() > 900000) {
        std::size_t removed = text_.size() - 450000;
        const auto lineEnd = text_.find('\n', removed);
        if (lineEnd != std::string::npos) removed = lineEnd + 1;
        else {
            // A single very long line may have no newline. Move the cut to a
            // complete UTF-8 code point rather than retaining continuation bytes.
            while (removed < text_.size()
                && (static_cast<unsigned char>(text_[removed]) & 0xc0) == 0x80) ++removed;
        }
        text_.erase(0, removed);
        removedBytes_ += removed;
    }
    ++revision_;
}

void Journal::clear() {
    removedBytes_ += text_.size();
    text_.clear();
    ++revision_;
}

struct Controller::EventQueue {
    explicit EventQueue(std::function<void()> suppliedWake) : wake(std::move(suppliedWake)) {}
    std::mutex mutex;
    std::deque<Event> pending;
    std::function<void()> wake;
    bool accepting = true;
    bool wakePosted = false;
};

Controller::Controller(AppState& state, std::function<void()> wake)
    : state_(state), events_(std::make_shared<EventQueue>(std::move(wake))), status_(readyStatus(state)) {
    engine_ = std::make_unique<Engine>([queue = events_](Event event) {
        bool wakeNeeded = false;
        {
            std::lock_guard lock(queue->mutex);
            if (!queue->accepting) return;
            if (event.kind == EventKind::Log && !queue->pending.empty()
                && queue->pending.back().kind == EventKind::Log
                && queue->pending.back().text.size() + event.text.size() < 32768)
                queue->pending.back().text += event.text;
            else queue->pending.push_back(std::move(event));
            if (!queue->wakePosted) {
                queue->wakePosted = true;
                wakeNeeded = true;
            }
        }
        // The worker owns the queue and an optional wake function, never the
        // Controller or a widget. Waking does not wait for the UI thread.
        if (wakeNeeded && queue->wake) queue->wake();
    });
}

Controller::~Controller() { close(); }

void Controller::drainEvents() {
    if (closing_) return;
    std::deque<Event> ready;
    {
        std::lock_guard lock(events_->mutex);
        ready.swap(events_->pending);
        events_->wakePosted = false;
    }
    std::wstring log;
    for (auto& event : ready) {
        if (event.kind == EventKind::Log) {
            log += event.text;
            if (log.size() >= 65536) {
                journal_.append(log);
                log.clear();
            }
        }
        else {
            if (!log.empty()) {
                journal_.append(log);
                log.clear();
            }
            onEvent(std::move(event));
        }
    }
    if (!log.empty()) journal_.append(log);
}

void Controller::onEvent(Event event) {
    if (closing_) return;
    switch (event.kind) {
    case EventKind::Log:
        journal_.append(event.text);
        return;
    case EventKind::Status:
        status_ = std::move(event.text);
        break;
    case EventKind::Targets:
        state_.targets = std::move(event.targets);
        break;
    case EventKind::Progress:
        if (operation_ != Operation::Building || buildAction_ == BuildAction::Configure
            || !event.progress || !event.progress->total || event.progress->completed > event.progress->total) return;
        if (buildProgress_ && buildProgress_->total == event.progress->total
            && event.progress->completed < buildProgress_->completed) return;
        buildProgress_ = event.progress;
        return;
    case EventKind::BuildSucceeded:
        operation_ = Operation::Idle;
        cancelling_ = false;
        buildProgress_.reset();
        if (!event.targets.empty()) state_.targets = std::move(event.targets);
        std::erase_if(state_.targets, [](const Target& target) { return !executableExists(target); });
        if (state_.chosenTarget.empty() && state_.targets.size() == 1) state_.selectRunTarget(0);
        status_ = event.text.empty() ? L"Сборка завершена" : std::move(event.text);
        if (state_.targets.empty()) status_ += L" · нет исполняемых целей";
        failed_ = false;
        recordDuration(event);
        save();
        if (std::exchange(runAfterBuild_, false)) {
            if (state_.findChosenTarget()) run();
            else {
                runSelectionRequested_ = state_.chosenTarget.empty() && !state_.targets.empty();
                if (runSelectionRequested_) status_ = L"Выберите цель для запуска после сборки";
                else journal_.append(state_.targets.empty() ? L"Запуск пропущен: нет собранных исполняемых целей.\n"
                    : L"Запуск пропущен: выберите доступную цель запуска.\n");
            }
        }
        break;
    case EventKind::CleanSucceeded:
    case EventKind::ConfigureSucceeded:
        operation_ = Operation::Idle;
        cancelling_ = runAfterBuild_ = false;
        buildProgress_.reset();
        state_.targets = event.kind == EventKind::CleanSucceeded ? std::vector<Target>{} : std::move(event.targets);
        std::erase_if(state_.targets, [](const Target& target) { return !executableExists(target); });
        if (state_.chosenTarget.empty() && state_.targets.size() == 1) state_.selectRunTarget(0);
        status_ = event.text.empty() ? event.kind == EventKind::CleanSucceeded ? L"Очистка завершена" : L"CMake завершён" : std::move(event.text);
        failed_ = false;
        recordDuration(event);
        save();
        break;
    case EventKind::BuildFailed: case EventKind::CleanFailed: case EventKind::ConfigureFailed:
        operation_ = Operation::Idle;
        cancelling_ = runAfterBuild_ = false;
        buildProgress_.reset();
        status_ = event.text.empty() ? event.kind == EventKind::CleanFailed ? L"Очистка завершилась с ошибкой"
            : event.kind == EventKind::ConfigureFailed ? L"CMake завершился с ошибкой" : L"Сборка завершилась с ошибкой" : std::move(event.text);
        failed_ = event.exitCode != ERROR_CANCELLED;
        recordDuration(event);
        if (failed_ && !state_.logVisible) toggleLog();
        break;
    case EventKind::Started:
        status_ = event.text.empty() ? L"Приложение запущено" : std::move(event.text);
        break;
    case EventKind::Finished:
        // The engine already reports build completion with the dedicated events.
        if (operation_ != Operation::Running) return;
        operation_ = Operation::Idle;
        failed_ = event.exitCode != 0 && event.exitCode != ERROR_CANCELLED;
        status_ = event.text.empty() ? L"Приложение завершено · код " + std::to_wstring(event.exitCode)
            : std::move(event.text);
        if (failed_ && !state_.logVisible) toggleLog();
        break;
    }
}

void Controller::recordDuration(const Event& event) {
    lastBuildDuration_ = event.buildDuration;
    durationAction_ = event.kind == EventKind::CleanSucceeded || event.kind == EventKind::CleanFailed ? BuildAction::Clean
        : event.kind == EventKind::ConfigureSucceeded || event.kind == EventKind::ConfigureFailed ? BuildAction::Configure : BuildAction::Build;
    if (lastBuildDuration_) journal_.append(std::wstring(durationAction_ == BuildAction::Clean ? L"\nВремя очистки: "
        : durationAction_ == BuildAction::Configure ? L"\nВремя CMake: " : L"\nВремя сборки: ") + formatElapsed(*lastBuildDuration_) + L"\n");
}

std::wstring Controller::durationText() const {
    if (!lastBuildDuration_) return {};
    const auto prefix = durationAction_ == BuildAction::Clean ? L"Очистка: " : durationAction_ == BuildAction::Configure ? L"CMake: " : L"Сборка: ";
    return prefix + formatElapsed(*lastBuildDuration_);
}

bool Controller::canQueueRun() const noexcept {
    return !closing_ && operation_ == Operation::Building && buildAction_ != BuildAction::Clean
        && buildAction_ != BuildAction::Configure && !cancelling_;
}

void Controller::selectProject(std::wstring file) {
    if (closing_ || operation_ != Operation::Idle) return;
    if (_wcsicmp(std::filesystem::path(file).filename().c_str(), L"CMakeLists.txt") != 0) {
        failed_ = true;
        status_ = L"Нужен файл CMakeLists.txt";
        return;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) {
        failed_ = true;
        status_ = L"Файл проекта недоступен: " + file;
        journal_.append(status_ + L"\n");
        return;
    }
    if (state_.selectProject(file)) { journal_.clear(); lastBuildDuration_.reset(); buildProgress_.reset(); }
    runSelectionRequested_ = false;
    status_ = readyStatus(state_);
    failed_ = false;
    save();
}

void Controller::build(BuildAction action) {
    if (closing_) return;
    if (operation_ == Operation::Building) {
        runAfterBuild_ = false;
        cancelling_ = true;
        engine_->cancel();
        status_ = buildAction_ == BuildAction::Clean ? L"Отмена очистки…" : buildAction_ == BuildAction::Configure ? L"Отмена CMake…" : L"Отмена сборки…";
        return;
    }
    if (operation_ != Operation::Idle) return;
    if (state_.settings.cmakeFile.empty()) {
        status_ = L"Выберите CMakeLists.txt";
        return;
    }
    journal_.clear();
    lastBuildDuration_.reset();
    buildProgress_.reset();
    buildAction_ = action;
    runAfterBuild_ = action == BuildAction::BuildAndRun;
    cancelling_ = runSelectionRequested_ = false;
    failed_ = false;
    status_ = action == BuildAction::Clean ? L"Подготовка очистки…" : action == BuildAction::Configure ? L"Подготовка CMake…"
        : action == BuildAction::Rebuild ? L"Подготовка пересборки…" : L"Подготовка сборки…";
    operation_ = Operation::Building;
    try {
        auto settings = state_.settings;
        settings.cleanFirst = action == BuildAction::Rebuild;
        if (settings.cleanFirst) settings.target.clear();
        if (action == BuildAction::Clean ? engine_->clean(std::move(settings))
            : action == BuildAction::Configure ? engine_->configure(std::move(settings)) : engine_->build(std::move(settings))) return;
    } catch (const std::exception&) {
        // Thread creation failure should restore the controls like a rejected
        // engine start, leaving the panel usable for another attempt.
    }
    operation_ = Operation::Idle;
    runAfterBuild_ = false;
    failed_ = true;
    status_ = action == BuildAction::Clean ? L"Не удалось начать очистку" : action == BuildAction::Configure ? L"Не удалось начать CMake" : L"Не удалось начать сборку";
}

bool Controller::run() {
    if (closing_) return true;
    if (operation_ == Operation::Running) {
        engine_->cancel();
        status_ = L"Остановка приложения…";
        return true;
    }
    if (canQueueRun()) {
        if (!std::exchange(runAfterBuild_, true)) journal_.append(L"Запуск после успешной сборки запланирован.\n");
        return true;
    }
    if (operation_ != Operation::Idle) return true;
    std::erase_if(state_.targets, [](const Target& target) { return !executableExists(target); });
    if (state_.chosenTarget.empty() && state_.targets.size() == 1) state_.selectRunTarget(0);
    const auto* found = state_.findChosenTarget();
    if (!found) return false;
    const auto target = *found;
    save();
    failed_ = false;
    status_ = L"Запуск " + target.name + L"…";
    operation_ = Operation::Running;
    const auto source = std::filesystem::absolute(state_.settings.cmakeFile).parent_path();
    const auto directory = state_.settings.buildDirectory.empty() ? source / L"build-cmakebuild"
        : std::filesystem::path(state_.settings.buildDirectory).is_absolute()
            ? std::filesystem::path(state_.settings.buildDirectory) : source / state_.settings.buildDirectory;
    try {
        auto settings = state_.runSettingsFor(target.name);
        if (!settings.workingDirectory.empty() && std::filesystem::path(settings.workingDirectory).is_relative())
            settings.workingDirectory = (source / settings.workingDirectory).lexically_normal().wstring();
        if (engine_->runConfigured(target, std::move(settings), directory.wstring())) return true;
    } catch (const std::exception&) {}
    operation_ = Operation::Idle;
    failed_ = true;
    status_ = L"Не удалось запустить приложение";
    return true;
}

bool Controller::chooseRunTarget(std::size_t index) {
    if (closing_ || operation_ != Operation::Idle || !state_.selectRunTarget(index)) return false;
    status_ = L"Цель запуска: " + state_.chosenTarget;
    failed_ = false;
    save();
    if (std::exchange(runSelectionRequested_, false)) run();
    return true;
}

void Controller::toggleLog() {
    if (closing_) return;
    state_.logVisible = !state_.logVisible;
    save();
}

void Controller::applySettings(BuildSettings settings, std::wstring chosen, bool selectionChanged,
    std::vector<std::pair<std::wstring, RunSettings>> runDrafts) {
    if (closing_ || operation_ != Operation::Idle) return;
    const bool changed = state_.settings.buildDirectory != settings.buildDirectory
        || state_.settings.configuration != settings.configuration;
    state_.settings = std::move(settings);
    if (selectionChanged) {
        state_.chosenTarget = std::move(chosen);
        state_.chosenExecutable.clear();
    }
    for (const auto& [target, runSettings] : runDrafts) state_.setRunSettings(target, runSettings);
    if (changed) { state_.restoreTargets(); lastBuildDuration_.reset(); buildProgress_.reset(); }
    status_ = readyStatus(state_);
    failed_ = false;
    save();
}

void Controller::save() { state_.save(); }

void Controller::close() {
    if (closing_) return;
    closing_ = true;
    runAfterBuild_ = runSelectionRequested_ = false;
    cancelling_ = true;
    buildProgress_.reset();
    {
        std::lock_guard lock(events_->mutex);
        events_->accepting = false;
    }
    // A job cancellation and worker join must never hold the event mutex: the
    // worker can finish emitting diagnostics while shutdown is in progress.
    if (engine_) {
        engine_->cancel();
        engine_.reset();
    }
    {
        std::lock_guard lock(events_->mutex);
        events_->pending.clear();
        events_->wakePosted = false;
    }
    operation_ = Operation::Idle;
    save();
}

} // namespace cb::na

