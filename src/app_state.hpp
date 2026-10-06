#pragma once

#include "engine.hpp"

#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace cb {

struct AppState {
    BuildSettings settings;
    std::vector<Target> targets;
    std::wstring chosenTarget;
    std::wstring chosenExecutable;
    // Most recently used first; unavailable projects remain visible in history.
    std::vector<std::wstring> recentProjects;
    static constexpr std::size_t maximumRecentProjects = 10;
    bool pinned = true;
    bool logVisible = false;
    int logHeight = 365;
    int x = std::numeric_limits<int>::min();
    int y = std::numeric_limits<int>::min();
    int width = 620;
    std::wstring configFile;

    explicit AppState(std::wstring file = {}) : configFile(std::move(file)) {}

    void load();
    void save();
    void saveProject();
    void loadProject(bool switching = false);
    // Returns false for the same project, including equivalent Windows paths.
    bool selectProject(const std::wstring& file);
    void restoreTargets();
    const Target* findChosenTarget() const;
    bool selectRunTarget(std::size_t index);
    RunSettings runSettingsFor(const std::wstring& target) const;
    void setRunSettings(const std::wstring& target, const RunSettings&);

private:
    std::wstring sectionForProject() const;
    std::wstring sectionForRunTarget(const std::wstring& target) const;
    void rememberChosenExecutable();
    void rememberProject(const std::wstring& file);
    void loadRecentProjects();
    void saveRecentProjects();
};

} // namespace cb
