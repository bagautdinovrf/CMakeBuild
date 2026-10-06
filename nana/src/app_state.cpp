#include "app_state.hpp"
#include "platform.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <stdexcept>

namespace cb {
namespace {
int integer(const std::wstring& file, const wchar_t* key, int fallback, const wchar_t* section = L"Panel") {
    const auto value = platform::readSetting(file, key, L"", section);
    if (value.empty()) return fallback;
    wchar_t* end = nullptr;
    errno = 0;
    const long parsed = std::wcstol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end || errno == ERANGE
        || parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max()) return fallback;
    return static_cast<int>(parsed);
}

bool sameProject(const std::wstring& left, const std::wstring& right) {
    if (left.empty() || right.empty()) return left == right;
    std::error_code error;
    if (std::filesystem::equivalent(left, right, error) && !error) return true;
    return platform::projectSection(left) == platform::projectSection(right);
}

bool executableExists(const std::wstring& file) {
    std::error_code error;
    return !file.empty() && std::filesystem::is_regular_file(file, error);
}

constexpr std::size_t encodedChunkUnits = 2048;
constexpr int maximumEncodedChunks = 4096;

std::wstring encoded(std::wstring_view text) {
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring value;
    value.reserve(text.size() * 4);
    for (const wchar_t character : text) {
        const auto unit = static_cast<unsigned short>(character);
        for (int shift = 12; shift >= 0; shift -= 4) value += hex[(unit >> shift) & 15];
    }
    return value;
}

// The Windows profile API trims spaces and removes surrounding quotes. Encode
// every UTF-16 unit, including newlines, so values cannot become INI syntax.
// Small independent keys also avoid readSetting's 32K buffer limit.
void writeEncoded(const std::wstring& file, const std::wstring& key,
    std::wstring_view text, const std::wstring& section) {
    const auto countKey = key + L"Chunks";
    const auto previous = std::clamp(integer(file, countKey.c_str(), 0, section.c_str()), 0, maximumEncodedChunks);
    const auto count = text.size() / encodedChunkUnits + (text.size() % encodedChunkUnits != 0);
    for (std::size_t index = 0; index < count; ++index) {
        const auto chunkKey = key + std::to_wstring(index);
        platform::writeSetting(file, chunkKey.c_str(),
            encoded(text.substr(index * encodedChunkUnits, encodedChunkUnits)), section.c_str());
    }
    // Only clear keys owned by this field; preserve every unrelated INI key.
    for (auto index = count; index < static_cast<std::size_t>(previous); ++index) {
        const auto chunkKey = key + std::to_wstring(index);
        platform::writeSetting(file, chunkKey.c_str(), L"", section.c_str());
    }
    platform::writeSetting(file, countKey.c_str(), std::to_wstring(count), section.c_str());
}

std::wstring readEncoded(const std::wstring& file, const std::wstring& key, const std::wstring& section) {
    const auto countKey = key + L"Chunks";
    const int count = integer(file, countKey.c_str(), 0, section.c_str());
    if (count < 0 || count > maximumEncodedChunks) return {};
    std::wstring result;
    for (int index = 0; index < count; ++index) {
        const auto chunkKey = key + std::to_wstring(index);
        const auto chunk = platform::readSetting(file, chunkKey.c_str(), L"", section.c_str());
        if (chunk.empty() || chunk.size() % 4 != 0 || chunk.size() > encodedChunkUnits * 4) return {};
        for (std::size_t offset = 0; offset < chunk.size(); offset += 4) {
            unsigned short unit = 0;
            for (std::size_t digit = 0; digit < 4; ++digit) {
                const auto character = chunk[offset + digit];
                const int value = character >= L'0' && character <= L'9' ? character - L'0'
                    : character >= L'A' && character <= L'F' ? character - L'A' + 10 : -1;
                if (value < 0) return {};
                unit = static_cast<unsigned short>((unit << 4) | value);
            }
            result += static_cast<wchar_t>(unit);
        }
    }
    return result;
}
}

void AppState::load() {
    if (configFile.empty()) configFile = platform::configurationFile();
    else platform::ensureConfigurationFile(configFile);
    settings.cmakeFile = platform::readSetting(configFile, L"Project");
    settings.buildDirectory = platform::readSetting(configFile, L"BuildDirectory");
    settings.cmakeExecutable = platform::readSetting(configFile, L"CMake");
    settings.configuration = platform::readSetting(configFile, L"Configuration", L"Release");
    settings.target = platform::readSetting(configFile, L"BuildTarget");
    settings.buildTests = integer(configFile, L"BuildTests", 0) != 0;
    settings.compiler = static_cast<CompilerMode>(std::clamp(integer(configFile, L"Compiler", 0), 0, 3));
    settings.cleanFirst = false;
    pinned = integer(configFile, L"Pinned", 1) != 0;
    logVisible = integer(configFile, L"LogVisible", 0) != 0;
    logHeight = std::clamp(integer(configFile, L"LogHeight", 365), 240, 4000);
    width = std::max(500, integer(configFile, L"Width", 620));
    x = integer(configFile, L"X", std::numeric_limits<int>::min());
    y = integer(configFile, L"Y", std::numeric_limits<int>::min());
    chosenTarget = platform::readSetting(configFile, L"RunTarget");
    chosenExecutable = platform::readSetting(configFile, L"Executable");
    loadProject();
    loadRecentProjects();
}

std::wstring AppState::sectionForProject() const {
    auto section = platform::projectSection(settings.cmakeFile);
    if (!platform::readSetting(configFile, L"Project", L"", section.c_str()).empty()) return section;
    // An old INI may name the same project through a short path or another
    // Windows alias. Keep writing that section rather than discarding its keys.
    for (const auto& candidate : platform::settingSections(configFile)) {
        if (!candidate.starts_with(L"Project:")) continue;
        const auto savedProject = platform::readSetting(configFile, L"Project", L"", candidate.c_str());
        if (!savedProject.empty() && sameProject(savedProject, settings.cmakeFile)) return candidate;
    }
    return section;
}

void AppState::loadProject(bool switching) {
    targets.clear();
    if (settings.cmakeFile.empty()) return;
    const auto section = sectionForProject();
    const auto savedProject = platform::readSetting(configFile, L"Project", L"", section.c_str());
    if (!savedProject.empty()) {
        // On startup an old project section inherits the active Panel values.
        // On switching, missing keys use defaults rather than the previous
        // project's compiler, target or testing choice. CMake is a preference.
        auto fallback = switching ? BuildSettings{} : settings;
        if (switching) fallback.cmakeExecutable = settings.cmakeExecutable;
        settings.buildDirectory = platform::readSetting(configFile, L"BuildDirectory", fallback.buildDirectory.c_str(), section.c_str());
        settings.cmakeExecutable = platform::readSetting(configFile, L"CMake", fallback.cmakeExecutable.c_str(), section.c_str());
        settings.configuration = platform::readSetting(configFile, L"Configuration", fallback.configuration.c_str(), section.c_str());
        settings.target = platform::readSetting(configFile, L"BuildTarget", fallback.target.c_str(), section.c_str());
        settings.compiler = static_cast<CompilerMode>(std::clamp(integer(configFile, L"Compiler",
            static_cast<int>(fallback.compiler), section.c_str()), 0, 3));
        settings.buildTests = integer(configFile, L"BuildTests", fallback.buildTests ? 1 : 0, section.c_str()) != 0;
        chosenTarget = platform::readSetting(configFile, L"RunTarget", L"", section.c_str());
        chosenExecutable = platform::readSetting(configFile, L"Executable", L"", section.c_str());
    } else if (switching) {
        chosenTarget.clear();
        chosenExecutable.clear();
    }
    settings.cleanFirst = false;
    // Without a project section, the active Panel section is the legacy source.
    restoreTargets();
}

void AppState::restoreTargets() {
    targets.clear();
    if (settings.cmakeFile.empty()) return;
    bool projectMatches = true;
    targets = readExecutableTargets(settings, &projectMatches);
    if (projectMatches && targets.empty() && executableExists(chosenExecutable)) {
        if (chosenTarget.empty()) chosenTarget = std::filesystem::path(chosenExecutable).stem().wstring();
        targets.push_back({chosenTarget, chosenExecutable});
    }
    if (chosenTarget.empty() && targets.size() == 1) chosenTarget = targets.front().name;
    rememberChosenExecutable();
}

void AppState::rememberChosenExecutable() {
    if (chosenTarget.empty()) {
        chosenExecutable.clear();
        return;
    }
    const auto found = std::ranges::find_if(targets, [&](const auto& target) { return target.name == chosenTarget; });
    if (found != targets.end()) chosenExecutable = found->executable;
}

const Target* AppState::findChosenTarget() const {
    const auto found = std::ranges::find_if(targets, [&](const auto& target) { return target.name == chosenTarget; });
    return found == targets.end() || !executableExists(found->executable) ? nullptr : &*found;
}

void AppState::saveProject() {
    if (settings.cmakeFile.empty()) return;
    if (configFile.empty()) configFile = platform::configurationFile();
    rememberChosenExecutable();
    const auto section = sectionForProject();
    platform::writeSetting(configFile, L"Project", settings.cmakeFile, section.c_str());
    platform::writeSetting(configFile, L"BuildDirectory", settings.buildDirectory, section.c_str());
    platform::writeSetting(configFile, L"CMake", settings.cmakeExecutable, section.c_str());
    platform::writeSetting(configFile, L"Configuration", settings.configuration, section.c_str());
    platform::writeSetting(configFile, L"BuildTarget", settings.target, section.c_str());
    platform::writeSetting(configFile, L"BuildTests", settings.buildTests ? L"1" : L"0", section.c_str());
    platform::writeSetting(configFile, L"Compiler", std::to_wstring(static_cast<int>(settings.compiler)), section.c_str());
    platform::writeSetting(configFile, L"RunTarget", chosenTarget, section.c_str());
    platform::writeSetting(configFile, L"Executable", chosenExecutable, section.c_str());
}

void AppState::save() {
    if (configFile.empty()) configFile = platform::configurationFile();
    else platform::ensureConfigurationFile(configFile);
    rememberChosenExecutable();
    platform::writeSetting(configFile, L"Project", settings.cmakeFile);
    platform::writeSetting(configFile, L"BuildDirectory", settings.buildDirectory);
    platform::writeSetting(configFile, L"CMake", settings.cmakeExecutable);
    platform::writeSetting(configFile, L"Configuration", settings.configuration);
    platform::writeSetting(configFile, L"BuildTarget", settings.target);
    platform::writeSetting(configFile, L"BuildTests", settings.buildTests ? L"1" : L"0");
    platform::writeSetting(configFile, L"Compiler", std::to_wstring(static_cast<int>(settings.compiler)));
    platform::writeSetting(configFile, L"Pinned", pinned ? L"1" : L"0");
    platform::writeSetting(configFile, L"LogVisible", logVisible ? L"1" : L"0");
    platform::writeSetting(configFile, L"LogHeight", std::to_wstring(logHeight));
    platform::writeSetting(configFile, L"RunTarget", chosenTarget);
    platform::writeSetting(configFile, L"Executable", chosenExecutable);
    if (x != std::numeric_limits<int>::min() && y != std::numeric_limits<int>::min()) {
        platform::writeSetting(configFile, L"X", std::to_wstring(x));
        platform::writeSetting(configFile, L"Y", std::to_wstring(y));
    }
    platform::writeSetting(configFile, L"Width", std::to_wstring(width));
    saveProject();
    rememberProject(settings.cmakeFile);
    saveRecentProjects();
}

bool AppState::selectProject(const std::wstring& file) {
    if (sameProject(settings.cmakeFile, file)) return false;
    save();
    auto cmakePreference = std::move(settings.cmakeExecutable);
    settings = BuildSettings{};
    settings.cmakeFile = file;
    settings.cmakeExecutable = std::move(cmakePreference);
    chosenTarget.clear();
    chosenExecutable.clear();
    loadProject(true);
    save();
    return true;
}

bool AppState::selectRunTarget(std::size_t index) {
    if (index >= targets.size() || !executableExists(targets[index].executable)) return false;
    chosenTarget = targets[index].name;
    chosenExecutable = targets[index].executable;
    save();
    return true;
}

void AppState::rememberProject(const std::wstring& file) {
    if (file.empty()) return;
    std::erase_if(recentProjects, [&](const auto& recent) { return sameProject(recent, file); });
    recentProjects.insert(recentProjects.begin(), file);
    if (recentProjects.size() > maximumRecentProjects) recentProjects.resize(maximumRecentProjects);
}

void AppState::loadRecentProjects() {
    constexpr auto section = L"RecentProjects";
    recentProjects.clear();
    const auto append = [this](std::wstring file) {
        if (file.empty() || recentProjects.size() >= maximumRecentProjects
            || std::ranges::any_of(recentProjects, [&](const auto& recent) { return sameProject(recent, file); })) return;
        recentProjects.push_back(std::move(file));
    };
    append(settings.cmakeFile);
    const int count = integer(configFile, L"Count", -1, section);
    if (count >= 0) {
        for (int index = 0; index < std::min(count, static_cast<int>(maximumRecentProjects)); ++index) {
            const auto key = L"Project" + std::to_wstring(index);
            append(platform::readSetting(configFile, key.c_str(), L"", section));
        }
    } else {
        // Older versions already remember project sections without an MRU list.
        for (const auto& projectSection : platform::settingSections(configFile)) {
            if (projectSection.starts_with(L"Project:"))
                append(platform::readSetting(configFile, L"Project", L"", projectSection.c_str()));
        }
    }
}

void AppState::saveRecentProjects() {
    constexpr auto section = L"RecentProjects";
    for (std::size_t index = 0; index < maximumRecentProjects; ++index) {
        const auto key = L"Project" + std::to_wstring(index);
        platform::writeSetting(configFile, key.c_str(), index < recentProjects.size() ? recentProjects[index] : L"", section);
    }
    platform::writeSetting(configFile, L"Count", std::to_wstring(recentProjects.size()), section);
}

std::wstring AppState::sectionForRunTarget(const std::wstring& target) const {
    // Encoding makes arbitrary target names safe and preserves case identity
    // despite Windows INI's case-insensitive section-name comparison.
    return L"Run:" + sectionForProject() + L":Target:" + encoded(target);
}

RunSettings AppState::runSettingsFor(const std::wstring& target) const {
    RunSettings result;
    if (configFile.empty() || settings.cmakeFile.empty() || target.empty()) return result;
    const auto section = sectionForRunTarget(target);
    result.arguments = readEncoded(configFile, L"Arguments", section);
    result.workingDirectory = readEncoded(configFile, L"WorkingDirectory", section);
    const int count = std::clamp(integer(configFile, L"EnvironmentCount", 0, section.c_str()), 0, 4096);
    for (int index = 0; index < count; ++index) {
        const auto prefix = L"Environment" + std::to_wstring(index);
        result.environment.emplace_back(readEncoded(configFile, prefix + L"Name", section),
            readEncoded(configFile, prefix + L"Value", section));
    }
    return result;
}

void AppState::setRunSettings(const std::wstring& target, const RunSettings& runSettings) {
    if (settings.cmakeFile.empty() || target.empty()) return;
    const auto tooLarge=[](std::wstring_view text) {return text.size()>encodedChunkUnits*maximumEncodedChunks;};
    if(runSettings.environment.size()>4096 || tooLarge(runSettings.arguments) || tooLarge(runSettings.workingDirectory)
        || std::ranges::any_of(runSettings.environment,[&](const auto& entry){return tooLarge(entry.first) || tooLarge(entry.second);}))
        throw std::length_error("Launch settings exceed the supported INI storage limits");
    if (configFile.empty()) configFile = platform::configurationFile();
    else platform::ensureConfigurationFile(configFile);
    const auto section = sectionForRunTarget(target);
    writeEncoded(configFile, L"Arguments", runSettings.arguments, section);
    writeEncoded(configFile, L"WorkingDirectory", runSettings.workingDirectory, section);
    const auto previous = std::clamp(integer(configFile, L"EnvironmentCount", 0, section.c_str()), 0, 4096);
    for (std::size_t index = 0; index < runSettings.environment.size(); ++index) {
        const auto prefix = L"Environment" + std::to_wstring(index);
        writeEncoded(configFile, prefix + L"Name", runSettings.environment[index].first, section);
        writeEncoded(configFile, prefix + L"Value", runSettings.environment[index].second, section);
    }
    for (auto index = runSettings.environment.size(); index < static_cast<std::size_t>(previous); ++index) {
        const auto prefix = L"Environment" + std::to_wstring(index);
        writeEncoded(configFile, prefix + L"Name", L"", section);
        writeEncoded(configFile, prefix + L"Value", L"", section);
    }
    platform::writeSetting(configFile, L"EnvironmentCount", std::to_wstring(runSettings.environment.size()), section.c_str());
}

} // namespace cb
