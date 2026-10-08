#include "engine.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace cb {
namespace {
namespace fs = std::filesystem;

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) reset(std::exchange(other.value_, nullptr));
        return *this;
    }
    void reset(HANDLE value = nullptr) noexcept {
        if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }
private:
    HANDLE value_;
};

std::wstring windowsError(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring result = length ? std::wstring(buffer, length) : L"Ошибка Windows " + std::to_wstring(code);
    if (buffer) LocalFree(buffer);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) {
        result.pop_back();
    }
    return result;
}

struct Failure : std::exception {
    std::wstring message;
    DWORD code;
    explicit Failure(std::wstring text, DWORD error = ERROR_INVALID_DATA)
        : message(std::move(text)), code(error) {}
};

std::wstring decode(std::string_view bytes, UINT codePage = CP_UTF8, DWORD flags = MB_ERR_INVALID_CHARS) {
    if (bytes.empty()) return {};
    int length = MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (!length) {
        codePage = GetOEMCP();
        flags = 0;
        length = MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    }
    if (!length) return L"[Не удалось декодировать вывод]";
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()), result.data(), length);
    return result;
}

std::wstring decodeToolOutput(std::string_view bytes) {
    // cmd.exe /u uses UTF-16 for builtins, but a shell launch error or an
    // external utility can still write an OEM byte stream to the same pipe.
    if (!bytes.empty() && bytes.size() % sizeof(wchar_t) == 0) {
        size_t zeroHighBytes = 0;
        for (size_t index = 1; index < bytes.size(); index += 2) {
            if (bytes[index] == '\0') ++zeroHighBytes;
        }
        if (zeroHighBytes > bytes.size() / 16) {
            std::wstring text(bytes.size() / sizeof(wchar_t), L'\0');
            memcpy(text.data(), bytes.data(), bytes.size());
            return text;
        }
    }
    return decode(bytes);
}

std::wstring trim(std::wstring text) {
    const auto first = text.find_first_not_of(L" \r\n\t");
    if (first == std::wstring::npos) return {};
    const auto last = text.find_last_not_of(L" \r\n\t");
    return text.substr(first, last - first + 1);
}

bool equal(std::wstring_view left, std::wstring_view right) {
    return left.size() == right.size()
        && std::equal(left.begin(), left.end(), right.begin(),
            [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
}

bool contains(std::wstring_view text, std::wstring_view fragment) {
    return std::search(text.begin(), text.end(), fragment.begin(), fragment.end(),
        [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); }) != text.end();
}

struct EnvironmentLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};
using Environment = std::map<std::wstring, std::wstring, EnvironmentLess>;

Environment currentEnvironment() {
    Environment environment;
    wchar_t* block = GetEnvironmentStringsW();
    if (!block) throw Failure(L"Не удалось прочитать окружение.", GetLastError());
    for (const wchar_t* entry = block; *entry; entry += wcslen(entry) + 1) {
        const std::wstring_view line(entry);
        const auto separator = line.find(L'=', line.front() == L'=' ? 1 : 0);
        if (separator != std::wstring_view::npos) {
            environment.emplace(std::wstring(line.substr(0, separator)), line.substr(separator + 1));
        }
    }
    FreeEnvironmentStringsW(block);
    return environment;
}

std::wstring environmentValue(const Environment& environment, const wchar_t* name) {
    const auto item = environment.find(name);
    return item == environment.end() ? std::wstring{} : item->second;
}

std::vector<wchar_t> environmentBlock(const Environment& environment) {
    std::vector<wchar_t> result;
    for (const auto& [name, value] : environment) {
        result.insert(result.end(), name.begin(), name.end());
        result.push_back(L'=');
        result.insert(result.end(), value.begin(), value.end());
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    if (result.size() == 1) result.push_back(L'\0');
    return result;
}

bool fileExists(const fs::path& path) {
    std::error_code error;
    return !path.empty() && fs::is_regular_file(path, error);
}

bool directoryExists(const fs::path& path) {
    std::error_code error;
    return !path.empty() && fs::is_directory(path, error);
}

void prependPath(Environment& environment, const fs::path& directory) {
    if (!directoryExists(directory)) return;
    std::wstring& path = environment[L"PATH"];
    const std::wstring addition = directory.lexically_normal().wstring();
    size_t begin = 0;
    while (begin <= path.size()) {
        const auto end = path.find(L';', begin);
        auto component = trim(path.substr(begin, end == std::wstring::npos ? end : end - begin));
        if (component.size() >= 2 && component.front() == L'"' && component.back() == L'"') {
            component = component.substr(1, component.size() - 2);
        }
        if (!component.empty() && equal(fs::path(component).lexically_normal().wstring(), addition)) {
            if (begin == 0) return;
            path.erase(begin, end == std::wstring::npos ? end : end - begin + 1);
            break;
        }
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    path = addition + (path.empty() ? L"" : L";" + path);
}

fs::path absolutePath(fs::path path, const fs::path& base = {}) {
    if (path.is_relative() && !base.empty()) path = base / path;
    return fs::absolute(path).lexically_normal();
}

bool samePath(const fs::path& left, const fs::path& right) {
    if (equal(absolutePath(left).wstring(), absolutePath(right).wstring())) return true;
    // Windows short names and directory junctions can name the same directory.
    std::error_code error;
    return fs::equivalent(left, right, error) && !error;
}

fs::path executableOnPath(std::wstring_view name, const Environment& environment) {
    fs::path supplied(name);
    if (supplied.has_parent_path()) {
        return fileExists(supplied) ? absolutePath(supplied) : fs::path{};
    }
    std::wstring path = environmentValue(environment, L"PATH");
    size_t begin = 0;
    while (begin <= path.size()) {
        const auto end = path.find(L';', begin);
        auto component = trim(path.substr(begin, end == std::wstring::npos ? end : end - begin));
        if (component.size() >= 2 && component.front() == L'"' && component.back() == L'"') {
            component = component.substr(1, component.size() - 2);
        }
        if (!component.empty()) {
            fs::path candidate = fs::path(component) / supplied;
            if (fileExists(candidate)) return absolutePath(candidate);
        }
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    return {};
}

void appendDirectory(std::vector<fs::path>& paths, fs::path path) {
    if (!directoryExists(path)) return;
    std::error_code error;
    path = fs::weakly_canonical(path, error);
    if (error || path == path.root_path()) return;
    if (std::ranges::none_of(paths, [&](const auto& item) { return equal(item.wstring(), path.wstring()); })) {
        paths.push_back(std::move(path));
    }
}

std::vector<fs::path> pathList(std::wstring_view value) {
    std::vector<fs::path> result;
    size_t begin = 0;
    while (begin < value.size()) {
        const auto end = value.find(L';', begin);
        auto component = trim(std::wstring(value.substr(begin, end == value.npos ? end : end - begin)));
        if (component.size() >= 2 && component.front() == L'"' && component.back() == L'"') {
            component = component.substr(1, component.size() - 2);
        }
        if (!component.empty()) result.emplace_back(component);
        if (end == value.npos) break;
        begin = end + 1;
    }
    return result;
}

std::wstring registryString(HKEY key, const wchar_t* subkey, const wchar_t* name) {
    DWORD bytes = 0;
    constexpr DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
    if (RegGetValueW(key, subkey, name, flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS
        || !bytes || bytes > 65536) return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, subkey, name, flags, nullptr, value.data(), &bytes) != ERROR_SUCCESS) return {};
    value.resize(wcsnlen(value.c_str(), value.size()));
    return value;
}

std::vector<fs::path> qtRoots(const Environment& environment) {
    std::vector<fs::path> roots;
    const DWORD drives = GetLogicalDrives();
    for (unsigned index = 0; index < 26; ++index) {
        if (!(drives & (1UL << index))) continue;
        std::wstring drive{static_cast<wchar_t>(L'A' + index), L':', L'\\'};
        if (GetDriveTypeW(drive.c_str()) == DRIVE_FIXED) appendDirectory(roots, fs::path(drive) / L"Qt");
    }
    for (const wchar_t* key : { L"USERPROFILE", L"LOCALAPPDATA", L"ProgramFiles", L"ProgramFiles(x86)" }) {
        const auto value = environmentValue(environment, key);
        if (!value.empty()) appendDirectory(roots, fs::path(value) / L"Qt");
    }
    for (const wchar_t* key : { L"QTDIR", L"QT_ROOT", L"QT_INSTALL_DIR", L"Qt6_ROOT", L"Qt5_ROOT" }) {
        auto path = fs::path(environmentValue(environment, key));
        // Accept an installation root, a version directory or an individual kit.
        for (int level = 0; level < 3 && !path.empty() && path != path.root_path(); ++level) {
            appendDirectory(roots, path);
            path = path.parent_path();
        }
    }
    for (const auto& bin : pathList(environmentValue(environment, L"PATH"))) {
        if (fileExists(bin / L"qmake.exe") || fileExists(bin / L"qtpaths.exe") || fileExists(bin / L"qtpaths6.exe")) {
            appendDirectory(roots, bin.parent_path());
            appendDirectory(roots, bin.parent_path().parent_path().parent_path());
        }
    }
    // The online installer records custom locations here (including per-user installs).
    for (HKEY hive : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
        for (const wchar_t* location : { L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" }) {
            HKEY raw = nullptr;
            if (RegOpenKeyExW(hive, location, 0, KEY_READ, &raw) != ERROR_SUCCESS) continue;
            struct KeyCloser { HKEY key; ~KeyCloser() { RegCloseKey(key); } } closer{raw};
            for (DWORD index = 0; ; ++index) {
                wchar_t name[256]{};
                DWORD length = static_cast<DWORD>(std::size(name));
                const auto status = RegEnumKeyExW(raw, index, name, &length, nullptr, nullptr, nullptr, nullptr);
                if (status == ERROR_NO_MORE_ITEMS) break;
                if (status != ERROR_SUCCESS) continue;
                const auto display = registryString(raw, name, L"DisplayName");
                if (equal(display, L"Qt") || display.starts_with(L"Qt ")) {
                    appendDirectory(roots, registryString(raw, name, L"InstallLocation"));
                }
            }
        }
    }
    return roots;
}

std::vector<unsigned> numericParts(std::wstring_view value) {
    std::vector<unsigned> parts;
    for (size_t index = 0; index < value.size(); ) {
        if (value[index] < L'0' || value[index] > L'9') { ++index; continue; }
        unsigned number = 0;
        while (index < value.size() && value[index] >= L'0' && value[index] <= L'9') {
            number = std::min(number, 100000u) * 10 + static_cast<unsigned>(value[index++] - L'0');
        }
        parts.push_back(number);
    }
    return parts;
}

fs::path discoverTool(std::wstring_view name, const Environment& environment) {
    if (auto fromPath = executableOnPath(name, environment); !fromPath.empty()) return fromPath;
    std::vector<fs::path> candidates;
    if (equal(name, L"cmake.exe")) {
        for (const wchar_t* key : { L"ProgramFiles", L"ProgramFiles(x86)" }) {
            const auto root = environmentValue(environment, key);
            if (!root.empty()) candidates.emplace_back(fs::path(root) / L"CMake/bin/cmake.exe");
        }
        for (const auto& root : qtRoots(environment)) {
            candidates.emplace_back(root / L"Tools/CMake_64/bin/cmake.exe");
            candidates.emplace_back(root / L"Tools/CMake/bin/cmake.exe");
        }
    } else if (equal(name, L"ninja.exe")) {
        for (const auto& root : qtRoots(environment)) {
            candidates.emplace_back(root / L"Tools/Ninja/ninja.exe");
        }
        candidates.emplace_back(L"C:/ninja/ninja.exe");
    } else if (equal(name, L"g++.exe")) {
        for (const auto& root : qtRoots(environment)) {
            const fs::path tools = root / L"Tools";
            std::error_code error;
            std::vector<fs::path> mingw;
            for (const auto& entry : fs::directory_iterator(tools, error)) {
                if (contains(entry.path().filename().wstring(), L"mingw")) {
                    mingw.emplace_back(entry.path() / L"bin/g++.exe");
                }
            }
            std::ranges::sort(mingw, [](const auto& left, const auto& right) {
                return numericParts(left.parent_path().parent_path().filename().wstring())
                    > numericParts(right.parent_path().parent_path().filename().wstring());
            });
            candidates.insert(candidates.end(), mingw.begin(), mingw.end());
        }
        for (const fs::path root : { fs::path(L"C:/mingw64"), fs::path(L"C:/msys64/mingw64"),
                                    fs::path(L"C:/msys64/ucrt64"), fs::path(L"C:/MinGW") }) {
            candidates.emplace_back(root / L"bin/g++.exe");
        }
    }
    for (const auto& candidate : candidates) {
        if (fileExists(candidate)) return absolutePath(candidate);
    }
    return {};
}

fs::path discoverVisualStudio(const Environment& environment) {
    const auto existing = environmentValue(environment, L"VSINSTALLDIR");
    if (!existing.empty()) {
        const fs::path candidate = fs::path(existing) / L"Common7/Tools/VsDevCmd.bat";
        if (fileExists(candidate)) return candidate;
    }
    for (const wchar_t* key : { L"ProgramFiles", L"ProgramFiles(x86)" }) {
        const auto root = environmentValue(environment, key);
        if (root.empty()) continue;
        std::error_code error;
        std::vector<fs::path> versions;
        for (const auto& entry : fs::directory_iterator(fs::path(root) / L"Microsoft Visual Studio", error)) {
            versions.emplace_back(entry.path());
        }
        std::sort(versions.rbegin(), versions.rend());
        for (const auto& version : versions) {
            for (const wchar_t* edition : { L"Community", L"Professional", L"Enterprise", L"BuildTools" }) {
                const auto candidate = version / edition / L"Common7/Tools/VsDevCmd.bat";
                if (fileExists(candidate)) return candidate;
            }
        }
    }
    return {};
}

// CreateProcess parses arguments using the Microsoft CRT quoting rules.
std::wstring quoteArgument(std::wstring_view value) {
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
        } else {
            result.append(backslashes * (character == L'"' ? 2 : 1), L'\\');
            if (character == L'"') result.push_back(L'\\');
            result.push_back(character);
            backslashes = 0;
        }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring commandLine(const fs::path& executable, const std::vector<std::wstring>& arguments) {
    std::wstring result = quoteArgument(executable.wstring());
    for (const auto& argument : arguments) {
        result.push_back(L' ');
        result += quoteArgument(argument);
    }
    if (result.size() >= 32767) throw Failure(L"Слишком длинная командная строка.");
    return result;
}

std::vector<std::wstring> configureArguments(std::wstring_view text) {
    if (text.size() >= 32767 || std::ranges::any_of(text, [](wchar_t ch) { return ch < L' ' && ch != L'\t'; })) {
        throw Failure(L"Параметры CMake должны занимать одну строку длиной менее 32767 символов.");
    }
    // Inverse of quoteArgument: CRT double quotes and backslashes, no shell.
    std::vector<std::wstring> arguments;
    size_t position = 0;
    while (position < text.size()) {
        while (position < text.size() && (text[position] == L' ' || text[position] == L'\t')) ++position;
        if (position == text.size()) break;
        std::wstring argument;
        bool quoted = false;
        while (position < text.size()) {
            if (!quoted && (text[position] == L' ' || text[position] == L'\t')) break;
            size_t backslashes = 0;
            while (position < text.size() && text[position] == L'\\') { ++backslashes; ++position; }
            if (position < text.size() && text[position] == L'"') {
                argument.append(backslashes / 2, L'\\');
                if (backslashes % 2) argument.push_back(L'"');
                else if (quoted && position + 1 < text.size() && text[position + 1] == L'"') {
                    argument.push_back(L'"');
                    ++position;
                } else quoted = !quoted;
                ++position;
            } else {
                argument.append(backslashes, L'\\');
                if (position == text.size() || (!quoted && (text[position] == L' ' || text[position] == L'\t'))) break;
                argument.push_back(text[position++]);
            }
        }
        if (quoted) throw Failure(L"В параметрах CMake не закрыта двойная кавычка.");
        arguments.push_back(std::move(argument));
    }
    const auto rejectReserved = [](const std::wstring& argument) {
        const auto option = argument.substr(0, argument.find(L'='));
        const auto invalid = [&] {
            throw Failure(L"Недопустимый параметр CMake: " + argument
                + L". Путь проекта, каталог сборки, генератор и действие задаются панелью.");
        };
        for (const auto* reserved : {L"--build", L"--install", L"--open", L"--workflow", L"--preset",
                L"--list-presets", L"--find-package", L"--help", L"--version", L"--fresh", L"--system-information"}) {
            if (option == reserved || (std::wstring_view(reserved) == L"--help" && option.starts_with(L"--help-"))) invalid();
        }
        if (argument == L"--" || argument.starts_with(L"-S") || argument.starts_with(L"-B")
            || argument.starts_with(L"-G") || argument.starts_with(L"-A") || argument.starts_with(L"-T")
            || argument.starts_with(L"-P") || argument.starts_with(L"-E") || argument.starts_with(L"-H")
            || argument == L"-N" || argument == L"-h" || argument == L"-?" || argument == L"-version"
            || argument == L"-help" || argument == L"-usage" || argument == L"/?" || argument == L"/V") invalid();
    };
    // CMake handles help and mode flags before it parses option/value pairs.
    // A token consumed as a value here must not bypass that first CMake pass.
    for (const auto& argument : arguments) rejectReserved(argument);
    const auto validateDefinition = [](std::wstring_view definition) {
        const auto name = definition.substr(0, definition.find_first_of(L":="));
        for (const auto* reserved : {L"CMAKE_GENERATOR", L"CMAKE_GENERATOR_PLATFORM", L"CMAKE_GENERATOR_TOOLSET",
                L"CMAKE_HOME_DIRECTORY", L"CMAKE_CACHEFILE_DIR", L"CMAKE_COMMAND",
                L"CMAKE_C_COMPILER", L"CMAKE_CXX_COMPILER", L"CMAKE_MAKE_PROGRAM"}) {
            if (name == reserved) {
                throw Failure(L"Параметр CMake " + std::wstring(name)
                    + L" задаётся панелью. Выберите CMake, компилятор и каталог сборки в настройках.");
            }
        }
    };
    for (size_t index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (argument.empty() || argument.front() != L'-') {
            throw Failure(L"Недопустимый параметр CMake: " + argument
                + L". Путь проекта и каталог сборки задаются панелью.");
        }
        if (argument.starts_with(L"-D") && argument.size() > 2)
            validateDefinition(std::wstring_view(argument).substr(2));
        // These configure switches consume a separate value, which may begin with '-'.
        if (argument == L"-D" || argument == L"-U" || argument == L"-C" || argument == L"-W"
            || argument == L"--log-level" || argument == L"--trace-source" || argument == L"--trace-redirect"
            || argument == L"--profiling-format" || argument == L"--profiling-output"
            || argument == L"--toolchain" || argument == L"--install-prefix") {
            if (++index == arguments.size() || arguments[index].empty())
                throw Failure(L"Укажите значение параметра CMake " + argument + L".");
            if (argument == L"-D") validateDefinition(arguments[index]);
        }
    }
    return arguments;
}

std::string readFile(const fs::path& path, size_t limit = 64 * 1024 * 1024) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw Failure(L"Не удалось открыть " + path.wstring() + L".");
    const auto size = stream.tellg();
    if (size < 0 || static_cast<unsigned long long>(size) > limit) {
        throw Failure(L"Файл слишком большой: " + path.wstring());
    }
    std::string content(static_cast<size_t>(size), '\0');
    stream.seekg(0);
    if (!content.empty() && !stream.read(content.data(), static_cast<std::streamsize>(content.size()))) {
        throw Failure(L"Не удалось прочитать " + path.wstring() + L".");
    }
    return content;
}

using Cache = std::map<std::wstring, std::wstring>;
Cache readCache(const fs::path& directory) {
    Cache result;
    const fs::path path = directory / L"CMakeCache.txt";
    if (!fileExists(path)) return result;
    const auto text = decode(readFile(path));
    size_t begin = 0;
    while (begin < text.size()) {
        const auto end = text.find(L'\n', begin);
        auto line = std::wstring_view(text).substr(begin, end == std::wstring::npos ? end : end - begin);
        if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
        const auto colon = line.find(L':');
        const auto separator = line.find(L'=');
        if (!line.empty() && line.front() != L'#' && !line.starts_with(L"//")
            && colon != line.npos && separator != line.npos && colon < separator) {
            result.emplace(std::wstring(line.substr(0, colon)), line.substr(separator + 1));
        }
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    return result;
}

std::wstring cacheValue(const Cache& cache, const wchar_t* name) {
    const auto item = cache.find(name);
    return item == cache.end() ? std::wstring{} : item->second;
}

fs::path cachedCompilerPath(const Cache& cache) {
    auto value = cacheValue(cache, L"CMAKE_CXX_COMPILER");
    if (value.empty()) value = cacheValue(cache, L"CMAKE_C_COMPILER");
    return fs::path(value);
}

std::wstring priValue(std::wstring_view text, std::wstring_view key) {
    size_t begin = 0;
    while (begin < text.size()) {
        const auto end = text.find(L'\n', begin);
        const auto line = text.substr(begin, end == text.npos ? end : end - begin);
        const auto separator = line.find(L'=');
        if (separator != line.npos && trim(std::wstring(line.substr(0, separator))) == key) {
            return trim(std::wstring(line.substr(separator + 1)));
        }
        if (end == text.npos) break;
        begin = end + 1;
    }
    return {};
}

std::wstring normalizedArchitecture(std::wstring_view value) {
    if (equal(value, L"x64") || equal(value, L"x86_64") || equal(value, L"amd64")) return L"x64";
    if (equal(value, L"x86") || equal(value, L"i386") || equal(value, L"i686") || equal(value, L"Win32")) return L"x86";
    if (equal(value, L"arm64") || equal(value, L"aarch64")) return L"arm64";
    return {};
}

struct QtInstallation {
    fs::path prefix;
    std::wstring version;
    std::wstring architecture;
    CompilerMode compiler = CompilerMode::Environment;
    unsigned gccMajor = 0;
};

std::vector<QtInstallation> discoverQt(const Environment& environment, const Cache& cache = {}) {
    auto roots = qtRoots(environment);
    for (const auto& prefix : pathList(environmentValue(environment, L"CMAKE_PREFIX_PATH"))) appendDirectory(roots, prefix);
    for (const auto& prefix : pathList(cacheValue(cache, L"CMAKE_PREFIX_PATH"))) appendDirectory(roots, prefix);
    for (const wchar_t* key : { L"Qt6_DIR", L"Qt5_DIR", L"Qt6Core_DIR", L"Qt5Core_DIR" }) {
        for (const auto& value : { cacheValue(cache, key), environmentValue(environment, key) }) {
            auto root = fs::path(value);
            for (int level = 0; level < 4 && !root.empty() && root != root.root_path(); ++level) {
                appendDirectory(roots, root);
                root = root.parent_path();
            }
        }
    }
    struct SearchDirectory { fs::path path; unsigned depth; };
    std::vector<SearchDirectory> pending;
    for (const auto& root : roots) pending.push_back({root, 0});
    std::vector<fs::path> visited;
    std::vector<QtInstallation> installations;
    // Inspect only root/version/kit. Never walk an entire drive or Qt's sources.
    for (size_t index = 0; index < pending.size() && index < 4096; ++index) {
        const auto current = pending[index];
        const auto oldSize = visited.size();
        appendDirectory(visited, current.path);
        if (visited.size() == oldSize) continue;
        const auto& prefix = visited.back();
        const bool qt6 = fileExists(prefix / L"lib/cmake/Qt6/Qt6Config.cmake");
        const bool qt5 = fileExists(prefix / L"lib/cmake/Qt5/Qt5Config.cmake");
        if (qt6 || qt5) {
            const auto pri = prefix / L"mkspecs/qconfig.pri";
            if (!fileExists(pri)) continue;
            std::wstring metadata;
            try { metadata = decode(readFile(pri, 1024 * 1024)); }
            catch (const Failure&) { continue; }
            const auto core = qt6 ? L"Qt6Core" : L"Qt5Core";
            QtInstallation installation{prefix, priValue(metadata, L"QT_VERSION"),
                normalizedArchitecture(priValue(metadata, L"QT_ARCH"))};
            // Windows import/static libraries also exclude Android, Linux and WASM kits.
            if (fileExists(prefix / L"lib" / (std::wstring(core) + L".lib"))) {
                installation.compiler = CompilerMode::Msvc;
            } else if (fileExists(prefix / L"lib" / (L"lib" + std::wstring(core) + L".dll.a"))
                || (fileExists(prefix / L"lib" / (L"lib" + std::wstring(core) + L".a"))
                    && !priValue(metadata, L"QT_GCC_MAJOR_VERSION").empty())) {
                installation.compiler = CompilerMode::Mingw;
            }
            const auto gcc = numericParts(priValue(metadata, L"QT_GCC_MAJOR_VERSION"));
            if (!gcc.empty()) installation.gccMajor = gcc.front();
            if (!installation.version.empty() && !installation.architecture.empty()
                && installation.compiler != CompilerMode::Environment
                && !contains(priValue(metadata, L"QT.global.enabled_features"), L"cross_compile")) {
                installations.push_back(std::move(installation));
            }
            continue;
        }
        if (current.depth == 2) continue;
        std::error_code error;
        fs::directory_iterator iterator(prefix, fs::directory_options::skip_permission_denied, error), end;
        for (; !error && iterator != end && pending.size() < 4096; iterator.increment(error)) {
            const auto& entry = *iterator;
            const auto name = entry.path().filename().wstring();
            if (equal(name, L"Tools") || equal(name, L"Src") || equal(name, L"Docs")
                || equal(name, L"Examples") || equal(name, L"Logs")) continue;
            const auto attributes = GetFileAttributesW(entry.path().c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)
                && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                pending.push_back({entry.path(), current.depth + 1});
            }
        }
    }
    std::ranges::stable_sort(installations, [](const auto& left, const auto& right) {
        return numericParts(left.version) > numericParts(right.version);
    });
    return installations;
}

std::wstring buildArchitecture(const Cache& cache, const Environment& environment) {
    auto architecture = normalizedArchitecture(cacheValue(cache, L"CMAKE_GENERATOR_PLATFORM"));
    if (architecture.empty()) architecture = normalizedArchitecture(environmentValue(environment, L"VSCMD_ARG_TGT_ARCH"));
    if (architecture.empty()) {
        auto compilerPath = cachedCompilerPath(cache);
        if (compilerPath.empty()) compilerPath = executableOnPath(L"cl.exe", environment);
        const auto compiler = compilerPath.wstring();
        if (contains(compiler, L"/x86/cl.exe") || contains(compiler, L"\\x86\\cl.exe")) architecture = L"x86";
        else if (contains(compiler, L"/arm64/cl.exe") || contains(compiler, L"\\arm64\\cl.exe")) architecture = L"arm64";
        else architecture = L"x64";
    }
    return architecture;
}

void addRuntimePaths(Environment& environment, const Cache& cache) {
    for (const wchar_t* key : { L"CMAKE_C_COMPILER", L"CMAKE_CXX_COMPILER", L"CMAKE_MAKE_PROGRAM" }) {
        const auto value = cacheValue(cache, key);
        if (!value.empty()) prependPath(environment, fs::path(value).parent_path());
    }
    const auto qt = environmentValue(environment, L"QTDIR");
    if (!qt.empty()) prependPath(environment, fs::path(qt) / L"bin");
    // Prepend the package actually selected by CMake last, ahead of broad hints.
    for (const wchar_t* key : { L"CMAKE_PREFIX_PATH", L"QTDIR", L"Qt5_DIR", L"Qt6_DIR", L"Qt5Core_DIR", L"Qt6Core_DIR" }) {
        const auto values = cacheValue(cache, key);
        size_t begin = 0;
        while (begin < values.size()) {
            const auto end = values.find(L';', begin);
            fs::path root(values.substr(begin, end == std::wstring::npos ? end : end - begin));
            // Qt*_DIR usually points to <prefix>/lib/cmake/Qt*. QTDIR points to <prefix>.
            for (int level = 0; level < 4 && !root.empty(); ++level, root = root.parent_path()) {
                const fs::path bin = root / L"bin";
                if (fileExists(bin / L"Qt6Core.dll") || fileExists(bin / L"Qt6Cored.dll")
                    || fileExists(bin / L"Qt5Core.dll") || fileExists(bin / L"Qt5Cored.dll")) {
                    prependPath(environment, bin);
                    break;
                }
            }
            if (end == std::wstring::npos) break;
            begin = end + 1;
        }
    }
}

// The CMake File API needs only object members, arrays and strings. Other JSON
// values are validated and discarded, avoiding a dependency just for this API.
struct Json {
    std::string string;
    std::map<std::string, Json> object;
    std::vector<Json> array;
    const Json& get(std::string_view key) const {
        static const Json empty;
        const auto item = object.find(std::string(key));
        return item == object.end() ? empty : item->second;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}
    Json parse() {
        if (text_.starts_with("\xef\xbb\xbf")) position_ = 3;
        Json value = read(0);
        whitespace();
        if (position_ != text_.size()) invalid();
        return value;
    }
private:
    std::string_view text_;
    size_t position_ = 0;
    [[noreturn]] static void invalid() { throw Failure(L"Некорректный JSON в ответе CMake File API."); }
    void whitespace() {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\n'
            || text_[position_] == '\r' || text_[position_] == '\t')) ++position_;
    }
    bool take(char expected) {
        whitespace();
        if (position_ < text_.size() && text_[position_] == expected) { ++position_; return true; }
        return false;
    }
    void expect(char expected) { if (!take(expected)) invalid(); }
    unsigned hex() {
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            if (position_ >= text_.size()) invalid();
            const char character = text_[position_++];
            value <<= 4;
            if (character >= '0' && character <= '9') value += static_cast<unsigned>(character - '0');
            else if (character >= 'a' && character <= 'f') value += static_cast<unsigned>(character - 'a' + 10);
            else if (character >= 'A' && character <= 'F') value += static_cast<unsigned>(character - 'A' + 10);
            else invalid();
        }
        return value;
    }
    static void utf8(std::string& result, unsigned code) {
        if (code <= 0x7f) result.push_back(static_cast<char>(code));
        else if (code <= 0x7ff) {
            result.push_back(static_cast<char>(0xc0 | (code >> 6)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else if (code <= 0xffff) {
            result.push_back(static_cast<char>(0xe0 | (code >> 12)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            result.push_back(static_cast<char>(0xf0 | (code >> 18)));
            result.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }
    std::string readString() {
        expect('"');
        std::string result;
        while (position_ < text_.size()) {
            const unsigned char character = static_cast<unsigned char>(text_[position_++]);
            if (character == '"') return result;
            if (character < 0x20) invalid();
            if (character != '\\') { result.push_back(static_cast<char>(character)); continue; }
            if (position_ >= text_.size()) invalid();
            const char escape = text_[position_++];
            switch (escape) {
            case '"': result += '"'; break;
            case '\\': result += '\\'; break;
            case '/': result += '/'; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            case 'u': {
                unsigned code = hex();
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (position_ + 2 > text_.size() || text_.substr(position_, 2) != "\\u") invalid();
                    position_ += 2;
                    const unsigned low = hex();
                    if (low < 0xdc00 || low > 0xdfff) invalid();
                    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                } else if (code >= 0xdc00 && code <= 0xdfff) invalid();
                utf8(result, code);
                break;
            }
            default: invalid();
            }
        }
        invalid();
    }
    Json read(unsigned depth) {
        if (depth > 128) invalid();
        whitespace();
        if (position_ == text_.size()) invalid();
        Json result;
        if (text_[position_] == '"') result.string = readString();
        else if (take('{')) {
            if (take('}')) return result;
            do {
                const auto key = readString();
                expect(':');
                result.object.insert_or_assign(key, read(depth + 1));
            } while (take(','));
            expect('}');
        } else if (take('[')) {
            if (take(']')) return result;
            do { result.array.emplace_back(read(depth + 1)); } while (take(','));
            expect(']');
        } else if (text_.substr(position_, 4) == "null" || text_.substr(position_, 4) == "true") {
            position_ += 4;
        } else if (text_.substr(position_, 5) == "false") {
            position_ += 5;
        } else {
            const size_t start = position_;
            if (text_[position_] == '-') ++position_;
            if (position_ == text_.size()) invalid();
            if (text_[position_] == '0') ++position_;
            else {
                if (text_[position_] < '1' || text_[position_] > '9') invalid();
                while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) ++position_;
            }
            if (position_ < text_.size() && text_[position_] == '.') {
                ++position_;
                const auto fraction = position_;
                while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) ++position_;
                if (position_ == fraction) invalid();
            }
            if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
                ++position_;
                if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
                const auto exponent = position_;
                while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) ++position_;
                if (position_ == exponent) invalid();
            }
            if (position_ == start) invalid();
        }
        return result;
    }
};

Json readJson(const fs::path& path) {
    const auto content = readFile(path);
    return JsonParser(content).parse();
}

fs::path replyReference(const fs::path& directory, const std::string& relative) {
    if (relative.empty()) throw Failure(L"CMake File API не вернул имя файла.");
    const fs::path reference(decode(relative));
    // File API references are relative JSON file names in the reply directory.
    if (reference.is_absolute() || reference.has_parent_path()) {
        throw Failure(L"Неожиданный путь в CMake File API.");
    }
    return directory / reference;
}

std::vector<Target> discoverTargets(const fs::path& buildDirectory, const std::wstring& configuration,
    const fs::path& expectedSource = {}, bool* projectMatches = nullptr) {
    const auto replyDirectory = buildDirectory / L".cmake/api/v1/reply";
    std::error_code error;
    fs::path latest;
    for (const auto& entry : fs::directory_iterator(replyDirectory, error)) {
        const auto name = entry.path().filename().wstring();
        if (name.starts_with(L"index-") && name.ends_with(L".json") && entry.path().filename() > latest.filename()) {
            latest = entry.path();
        }
    }
    if (latest.empty()) throw Failure(L"CMake не создал ответ File API (нужен CMake 3.15 или новее).");
    const Json index = readJson(latest);
    const auto& reference = index.get("reply").get("client-cmakebuild").get("codemodel-v2");
    const Json model = readJson(replyReference(replyDirectory, reference.get("jsonFile").string));
    if (!expectedSource.empty()) {
        const fs::path modelSource(decode(model.get("paths").get("source").string));
        if (!modelSource.empty() && !samePath(modelSource, expectedSource)) {
            if (projectMatches) *projectMatches = false;
            return {};
        }
    }
    const auto& configurations = model.get("configurations").array;
    const Json* selected = nullptr;
    for (const auto& item : configurations) {
        if (equal(decode(item.get("name").string), configuration)) { selected = &item; break; }
    }
    if (!selected && configurations.size() == 1) selected = &configurations.front();
    if (!selected) throw Failure(L"CMake не вернул конфигурацию " + configuration + L".");
    std::vector<Target> targets;
    fs::path modelBuild(decode(model.get("paths").get("build").string));
    if (modelBuild.empty()) modelBuild = buildDirectory;
    for (const auto& item : selected->get("targets").array) {
        const Json target = readJson(replyReference(replyDirectory, item.get("jsonFile").string));
        if (target.get("type").string != "EXECUTABLE") continue;
        for (const auto& artifact : target.get("artifacts").array) {
            fs::path executable(decode(artifact.get("path").string));
            if (executable.empty() || !equal(executable.extension().wstring(), L".exe")) continue;
            executable = absolutePath(executable, modelBuild);
            targets.push_back({ decode(target.get("name").string), executable.wstring() });
            break;
        }
    }
    std::sort(targets.begin(), targets.end(),
        [](const Target& left, const Target& right) { return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0; });
    return targets;
}

CompilerMode cachedCompiler(const Cache& cache) {
    const fs::path compiler = cachedCompilerPath(cache);
    const auto filename = compiler.filename().wstring();
    const auto generator = cacheValue(cache, L"CMAKE_GENERATOR");
    if (equal(filename, L"cl.exe") || equal(filename, L"clang-cl.exe")
        || contains(generator, L"Visual Studio") || contains(generator, L"NMake")) return CompilerMode::Msvc;
    if (contains(filename, L"g++") || contains(filename, L"gcc") || contains(generator, L"MinGW")) return CompilerMode::Mingw;
    return CompilerMode::Environment;
}

bool cacheFlagMatches(const Cache& cache, const wchar_t* name, bool enabled) {
    const auto value = trim(cacheValue(cache, name));
    if (enabled) return equal(value, L"ON") || equal(value, L"TRUE") || equal(value, L"YES")
        || equal(value, L"Y") || value == L"1";
    return equal(value, L"OFF") || equal(value, L"FALSE") || equal(value, L"NO")
        || equal(value, L"N") || value == L"0";
}

std::string configureSignature(const BuildSettings& settings, const Cache& cache) {
    // Record both the requested settings and the resulting flag values. Explicit
    // -D/-U arguments can override the checkbox; this must not force every build
    // to reconfigure, while later checkbox/cache changes must still be detected.
    std::wstring signature = L"CMakeBuild configure arguments v1";
    for (const auto& value : {settings.cmakeArguments, std::wstring(settings.buildTests ? L"ON" : L"OFF"),
            cacheValue(cache, L"BUILD_TESTING"), cacheValue(cache, L"BUILD_TESTS")}) {
        signature.push_back(L'\0');
        signature += value;
    }
    return {reinterpret_cast<const char*>(signature.data()), signature.size() * sizeof(wchar_t)};
}

bool configureArgumentsMatch(const BuildSettings& settings, const Cache& cache, const fs::path& directory) {
    const auto marker = directory / L".cmake/cmakebuild-configure-arguments";
    if (!fileExists(marker)) {
        // Preserve reuse of trees configured by previous versions of the panel.
        return settings.cmakeArguments.empty()
            && cacheFlagMatches(cache, L"BUILD_TESTING", settings.buildTests)
            && cacheFlagMatches(cache, L"BUILD_TESTS", settings.buildTests);
    }
    try { return readFile(marker, 256 * 1024) == configureSignature(settings, cache); }
    catch (const Failure&) { return false; }
}

void rememberConfigureArguments(const BuildSettings& settings, const fs::path& directory) {
    const auto signature = configureSignature(settings, readCache(directory));
    std::ofstream marker(directory / L".cmake/cmakebuild-configure-arguments", std::ios::binary | std::ios::trunc);
    marker.write(signature.data(), static_cast<std::streamsize>(signature.size()));
    marker.close();
    if (!marker) throw Failure(L"Не удалось сохранить параметры выполненной конфигурации CMake.");
}

bool cachedConfigurationMatches(const Cache& cache, const std::wstring& configuration) {
    const auto generator = cacheValue(cache, L"CMAKE_GENERATOR");
    if (!contains(generator, L"Visual Studio") && !equal(generator, L"Ninja Multi-Config")
        && !equal(generator, L"Xcode")) {
        return equal(cacheValue(cache, L"CMAKE_BUILD_TYPE"), configuration);
    }
    const auto configurations = cacheValue(cache, L"CMAKE_CONFIGURATION_TYPES");
    size_t begin = 0;
    while (begin < configurations.size()) {
        const auto end = configurations.find(L';', begin);
        if (equal(std::wstring_view(configurations).substr(begin,
                end == configurations.npos ? end : end - begin), configuration)) return true;
        if (end == configurations.npos) break;
        begin = end + 1;
    }
    return false;
}

bool generatedBuildFilesExist(const Cache& cache, const fs::path& buildDirectory,
                             const std::wstring& configuration) {
    const auto generator = cacheValue(cache, L"CMAKE_GENERATOR");
    if (!fileExists(buildDirectory / L"CMakeFiles/cmake.check_cache")) return false;
    if (contains(generator, L"Ninja")) {
        return fileExists(buildDirectory / L"build.ninja")
            && fileExists(buildDirectory / L"CMakeFiles/rules.ninja")
            && (!equal(generator, L"Ninja Multi-Config")
                || fileExists(buildDirectory / (L"build-" + configuration + L".ninja")));
    }
    if (contains(generator, L"Makefiles")) {
        return fileExists(buildDirectory / L"Makefile")
            && fileExists(buildDirectory / L"CMakeFiles/Makefile2");
    }
    if (contains(generator, L"Visual Studio")) {
        const auto project = cacheValue(cache, L"CMAKE_PROJECT_NAME");
        return !project.empty() && fileExists(buildDirectory / L"ALL_BUILD.vcxproj")
            && (fileExists(buildDirectory / (project + L".sln"))
                || fileExists(buildDirectory / (project + L".slnx")));
    }
    // Unknown generators retain the full configure path rather than guessing
    // which generated files represent a complete build tree.
    return false;
}

bool canReuseConfiguration(const BuildSettings& settings, const Cache& cache,
                           const fs::path& source, const fs::path& buildDirectory,
                           const fs::path& cmake, const std::wstring& configuration) {
    const auto previousSource = cacheValue(cache, L"CMAKE_HOME_DIRECTORY");
    const auto previousCMake = cacheValue(cache, L"CMAKE_COMMAND");
    if (previousSource.empty() || previousCMake.empty()
        || !samePath(previousSource, source) || !samePath(previousCMake, cmake)
        || fileExists(buildDirectory / L".cmake/cmakebuild-configure-pending")
        || !configureArgumentsMatch(settings, cache, buildDirectory)
        || !cachedConfigurationMatches(cache, configuration)
        || !generatedBuildFilesExist(cache, buildDirectory, configuration)) return false;
    try {
        bool projectMatches = true;
        (void)discoverTargets(buildDirectory, configuration, source, &projectMatches);
        return projectMatches;
    } catch (...) {
        // A foreign/imported tree or deleted reply needs one configure step to
        // populate our File API query, including targets not compiled yet.
        return false;
    }
}

} // namespace

struct Engine::Impl {
    explicit Impl(Callback supplied) : callback(std::move(supplied)) {}
    Callback callback;
    std::atomic_bool active = false;
    std::atomic_bool cancelled = false;
    std::mutex launchMutex;
    std::mutex processMutex;
    HANDLE activeJob = nullptr; // borrowed; worker owns the handle
    std::jthread worker;
    Environment lastBuildEnvironment;

    struct ProcessResult {
        DWORD exitCode = 0;
        std::string output;
        bool cannotWriteOutput = false;
    };

    static bool nativeCleanProgress(std::string_view line, const BuildProgress& progress) noexcept {
        if (progress != BuildProgress{1, 1}) return false;
        const auto prefixEnd = line.find(']');
        if (prefixEnd == line.npos) return false;
        line.remove_prefix(prefixEnd + 1);
        // Ninja --clean-first reports its one-step cleanup before the actual
        // compilation. Ignore that marker rather than briefly showing 100%.
        for (;;) {
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
            if (!line.starts_with("\x1b[")) break;
            size_t end = 2;
            while (end < line.size() && ((line[end] >= '0' && line[end] <= '9')
                    || line[end] == ';' || line[end] == ':')) ++end;
            if (end == line.size() || line[end] != 'm') return false;
            line.remove_prefix(end + 1);
        }
        return line.starts_with("Cleaning all built files...");
    }

    void emit(EventKind kind, std::wstring text = {}, std::vector<Target> targets = {}, DWORD exitCode = 0,
              std::optional<std::chrono::milliseconds> buildDuration = {},
              std::optional<BuildProgress> progress = {}) noexcept {
        try { if (callback) callback(Event{ kind, std::move(text), std::move(targets), exitCode, buildDuration, progress }); }
        catch (...) { /* A failed UI callback must not terminate the worker. */ }
    }

    void log(std::wstring text, bool completeLine = true) noexcept {
        if (completeLine) text += L"\r\n";
        emit(EventKind::Log, std::move(text));
    }

    void checkCancellation() const {
        if (cancelled.load()) throw Failure(L"Операция отменена.", ERROR_CANCELLED);
    }

    ProcessResult process(const fs::path& executable, const std::vector<std::wstring>& arguments,
                          const fs::path& workingDirectory, const Environment& environment,
                          bool logOutput = true, const std::wstring& rawCommand = {},
                          bool notifyStarted = false, bool trackBuildProgress = false) {
        checkCancellation();
        std::wstring command = rawCommand.empty() ? commandLine(executable, arguments) : rawCommand;
        if (logOutput) log(L"> " + command);
        SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &security, 65536)) {
            const DWORD error = GetLastError();
            throw Failure(L"Не удалось создать канал вывода: " + windowsError(error), error);
        }
        Handle readPipe(read), writePipe(write);
        if (!SetHandleInformation(readPipe.get(), HANDLE_FLAG_INHERIT, 0)) {
            const DWORD error = GetLastError();
            throw Failure(L"Не удалось настроить канал вывода: " + windowsError(error), error);
        }
        Handle nullInput(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!nullInput) {
            const DWORD error = GetLastError();
            throw Failure(L"Не удалось создать вход процесса: " + windowsError(error), error);
        }
        Handle job(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            const DWORD error = GetLastError();
            throw Failure(L"Не удалось создать группу процессов: " + windowsError(error), error);
        }
        SIZE_T attributeSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
        std::vector<unsigned char> attributeStorage(attributeSize);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize)) {
            const DWORD error = GetLastError();
            throw Failure(L"Не удалось настроить наследование дескрипторов: " + windowsError(error), error);
        }
        struct AttributeCleanup {
            LPPROC_THREAD_ATTRIBUTE_LIST list;
            ~AttributeCleanup() { DeleteProcThreadAttributeList(list); }
        } attributeCleanup{ attributes };
        HANDLE inherited[] = { writePipe.get(), nullInput.get() };
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited, sizeof(inherited), nullptr, nullptr)) {
            const DWORD error = GetLastError();
            throw Failure(L"Не удалось настроить наследование дескрипторов: " + windowsError(error), error);
        }
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdOutput = writePipe.get();
        startup.StartupInfo.hStdError = writePipe.get();
        startup.StartupInfo.hStdInput = nullInput.get();
        startup.lpAttributeList = attributes;
        auto block = environmentBlock(environment);
        PROCESS_INFORMATION information{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
            block.data(), workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
            &startup.StartupInfo, &information)) {
            const DWORD error = GetLastError();
            throw Failure(L"Не удалось запустить " + executable.wstring() + L": " + windowsError(error), error);
        }
        Handle child(information.hProcess), thread(information.hThread);
        writePipe.reset();
        if (!AssignProcessToJobObject(job.get(), child.get())) {
            const DWORD error = GetLastError();
            TerminateProcess(child.get(), error);
            throw Failure(L"Не удалось управлять деревом процесса: " + windowsError(error), error);
        }
        {
            std::lock_guard lock(processMutex);
            activeJob = job.get();
            if (cancelled.load()) TerminateJobObject(activeJob, ERROR_CANCELLED);
        }
        struct ActiveCleanup {
            Impl& owner;
            ~ActiveCleanup() {
                std::lock_guard lock(owner.processMutex);
                owner.activeJob = nullptr;
            }
        } activeCleanup{ *this };
        if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
            const DWORD error = GetLastError();
            TerminateJobObject(job.get(), error);
            throw Failure(L"Не удалось продолжить процесс: " + windowsError(error), error);
        }
        if (notifyStarted) emit(EventKind::Started, L"Программа запущена.");
        ProcessResult result;
        std::string pending;
        std::optional<BuildProgress> lastProgress;
        bool outputLineStart = true;
        bool ended = false;
        auto flushLines = [&](bool final) {
            auto writeLine = [&](std::string_view bytes, bool completeLine = true) {
                if (bytes.find("LNK1168") != bytes.npos) result.cannotWriteOutput = true;
                if (trackBuildProgress && outputLineStart) {
                    const auto progress = parseBuildProgress(bytes);
                    if (progress && !nativeCleanProgress(bytes, *progress) && progress != lastProgress) {
                        emit(EventKind::Progress, {}, {}, 0, {}, progress);
                        lastProgress = progress;
                    }
                }
                outputLineStart = completeLine;
                log(decode(bytes), completeLine);
            };
            size_t begin = 0;
            while (true) {
                const auto end = pending.find_first_of("\r\n", begin);
                if (end == std::string::npos) break;
                if (!final && pending[end] == '\r' && end + 1 == pending.size()) break;
                writeLine(std::string_view(pending).substr(begin, end - begin));
                begin = end + 1;
                if (pending[end] == '\r' && begin < pending.size() && pending[begin] == '\n') ++begin;
            }
            pending.erase(0, begin);
            // Keep incomplete UTF-8 sequences across reads, including a long line.
            if (pending.size() > 65536) {
                size_t boundary = pending.size();
                size_t lead = boundary;
                while (lead > 0 && (static_cast<unsigned char>(pending[lead - 1]) & 0xc0) == 0x80) --lead;
                if (lead > 0) {
                    const auto byte = static_cast<unsigned char>(pending[lead - 1]);
                    const size_t count = byte >= 0xf0 ? 4 : byte >= 0xe0 ? 3 : byte >= 0xc0 ? 2 : 1;
                    if (pending.size() - (lead - 1) < count) boundary = lead - 1;
                }
                writeLine(std::string_view(pending).substr(0, boundary), false);
                pending.erase(0, boundary);
            }
            if (final && !pending.empty()) { writeLine(pending); pending.clear(); }
        };
        while (true) {
            if (cancelled.load()) TerminateJobObject(job.get(), ERROR_CANCELLED);
            DWORD available = 0;
            if (!PeekNamedPipe(readPipe.get(), nullptr, 0, nullptr, &available, nullptr)) {
                const DWORD error = GetLastError();
                if (error != ERROR_BROKEN_PIPE) {
                    TerminateJobObject(job.get(), error);
                    throw Failure(L"Ошибка чтения вывода процесса: " + windowsError(error), error);
                }
                available = 0;
            }
            if (available) {
                char buffer[16384];
                DWORD received = 0;
                if (!ReadFile(readPipe.get(), buffer, (std::min)(available, static_cast<DWORD>(sizeof(buffer))), &received, nullptr)) {
                    const DWORD error = GetLastError();
                    if (error != ERROR_BROKEN_PIPE) {
                        TerminateJobObject(job.get(), error);
                        throw Failure(L"Ошибка чтения вывода процесса: " + windowsError(error), error);
                    }
                } else if (received) {
                    if (logOutput) {
                        pending.append(buffer, received);
                        flushLines(false);
                    } else {
                        if (result.output.size() + received > 8 * 1024 * 1024) {
                            TerminateJobObject(job.get(), ERROR_BUFFER_OVERFLOW);
                            throw Failure(L"Слишком большой служебный вывод.", ERROR_BUFFER_OVERFLOW);
                        }
                        result.output.append(buffer, received);
                    }
                    continue;
                }
            }
            ended = WaitForSingleObject(child.get(), 25) == WAIT_OBJECT_0;
            if (ended) {
                // Drain bytes already written before observing process termination.
                DWORD remaining = 0;
                if (PeekNamedPipe(readPipe.get(), nullptr, 0, nullptr, &remaining, nullptr) && remaining) continue;
                break;
            }
        }
        if (logOutput) flushLines(true);
        if (!GetExitCodeProcess(child.get(), &result.exitCode)) {
            const DWORD error = GetLastError();
            throw Failure(L"Не удалось получить код завершения: " + windowsError(error), error);
        }
        if (cancelled.load()) result.exitCode = ERROR_CANCELLED;
        return result;
    }

    fs::path visualStudioEnvironmentScript(const Environment& environment) {
        const auto installer = environmentValue(environment, L"ProgramFiles(x86)");
        const fs::path vswhere = fs::path(installer) / L"Microsoft Visual Studio/Installer/vswhere.exe";
        if (!installer.empty() && fileExists(vswhere)) {
            auto result = process(vswhere, { L"-latest", L"-products", L"*", L"-requires",
                L"Microsoft.VisualStudio.Component.VC.Tools.x86.x64", L"-property", L"installationPath", L"-utf8" },
                {}, environment, false);
            checkCancellation();
            if (!result.exitCode) {
                const fs::path root(trim(decode(result.output)));
                if (!root.empty()) {
                    const auto candidate = root / L"Common7/Tools/VsDevCmd.bat";
                    if (fileExists(candidate)) return candidate;
                }
            }
        }
        return discoverVisualStudio(environment);
    }

    Environment msvcEnvironment(Environment environment, const fs::path& script, const Cache& cache) {
        if (script.empty()) {
            if (!executableOnPath(L"cl.exe", environment).empty()) return environment;
            throw Failure(L"Не найден MSVC. Установите Visual Studio Build Tools с компонентом «Разработка на C++» "
                L"или запустите панель из Developer Command Prompt.", ERROR_FILE_NOT_FOUND);
        }
        const auto path = fs::path(script).make_preferred().wstring();
        if (path.find_first_of(L"\"%\r\n") != path.npos) {
            throw Failure(L"Путь к Visual Studio содержит символы, несовместимые с VsDevCmd.");
        }
        fs::path shell = fs::path(environmentValue(environment, L"SystemRoot")) / L"System32/cmd.exe";
        // cmd parses its own argv[0]. A slash in that path can be mistaken for
        // an option even though CreateProcess itself accepts the same path.
        shell.make_preferred();
        if (!fileExists(shell)) throw Failure(L"Не найден cmd.exe.", ERROR_FILE_NOT_FOUND);
        std::wstring architecture = L"x64";
        std::wstring versionArgument;
        const auto platform = cacheValue(cache, L"CMAKE_GENERATOR_PLATFORM");
        const auto compiler = cachedCompilerPath(cache).wstring();
        // Keep the toolset recorded in an existing build tree, even when another
        // Visual Studio toolset is now the newest one on this computer.
        fs::path ancestor = fs::path(compiler).parent_path();
        while (!ancestor.empty() && ancestor != ancestor.parent_path()) {
            if (equal(ancestor.parent_path().filename().wstring(), L"MSVC")) {
                const auto version = ancestor.filename().wstring();
                const auto secondDot = version.find(L'.', version.find(L'.') + 1);
                const auto shortVersion = version.substr(0, secondDot);
                if (!shortVersion.empty() && std::all_of(shortVersion.begin(), shortVersion.end(),
                    [](wchar_t character) { return (character >= L'0' && character <= L'9') || character == L'.'; })) {
                    versionArgument = L" -vcvars_ver=" + shortVersion;
                }
                break;
            }
            ancestor = ancestor.parent_path();
        }
        if (equal(platform, L"Win32") || contains(compiler, L"/x86/cl.exe") || contains(compiler, L"\\x86\\cl.exe")) {
            architecture = L"x86";
        } else if (equal(platform, L"ARM64") || contains(compiler, L"/arm64/cl.exe")
                   || contains(compiler, L"\\arm64\\cl.exe")) architecture = L"arm64";
        emit(EventKind::Status, L"Подготовка окружения MSVC…");
        // Only the discovered Visual Studio script enters cmd.exe. Project paths,
        // targets and user settings are always direct CreateProcess arguments.
        constexpr std::wstring_view environmentMarker = L"__CMAKEBUILD_ENVIRONMENT_BEGIN__";
        const auto raw = quoteArgument(shell.wstring()) + L" /d /u /v:off /s /c \"call \""
            + path + L"\" -no_logo -arch=" + architecture + L" -host_arch=x64" + versionArgument
            + L" && echo " + std::wstring(environmentMarker) + L" && set\"";
        auto result = process(shell, {}, script.parent_path(), environment, false, raw);
        checkCancellation();
        if (result.exitCode) {
            auto diagnostics = decodeToolOutput(result.output);
            if (const auto marker = diagnostics.find(environmentMarker); marker != diagnostics.npos) {
                diagnostics.resize(marker); // Never put environment values into the UI log.
            }
            std::wstring safeDiagnostics;
            size_t begin = 0;
            while (begin < diagnostics.size() && safeDiagnostics.size() < 2048) {
                const auto end = diagnostics.find(L'\n', begin);
                auto line = trim(diagnostics.substr(begin, end == diagnostics.npos ? end : end - begin));
                if (!line.empty() && line.find(L'=') == line.npos) safeDiagnostics += line + L"\r\n";
                if (end == diagnostics.npos) break;
                begin = end + 1;
            }
            safeDiagnostics.resize((std::min)(safeDiagnostics.size(), size_t{ 2048 }));
            auto message = L"Не удалось подготовить окружение MSVC (код " + std::to_wstring(result.exitCode) + L").";
            if (!safeDiagnostics.empty()) message += L"\r\n" + safeDiagnostics;
            throw Failure(std::move(message), result.exitCode);
        }
        if (result.output.size() % sizeof(wchar_t)) throw Failure(L"Некорректный ответ VsDevCmd.");
        std::wstring text(result.output.size() / sizeof(wchar_t), L'\0');
        if (!result.output.empty()) memcpy(text.data(), result.output.data(), result.output.size());
        const auto marker = text.find(environmentMarker);
        if (marker == text.npos) throw Failure(L"VsDevCmd не вернул окружение после успешного завершения.");
        size_t begin = text.find(L'\n', marker);
        if (begin == text.npos) throw Failure(L"VsDevCmd вернул пустое окружение.");
        ++begin;
        while (begin < text.size()) {
            const auto end = text.find(L'\n', begin);
            auto line = text.substr(begin, end == text.npos ? end : end - begin);
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            const auto separator = line.find(L'=', !line.empty() && line.front() == L'=' ? 1 : 0);
            if (separator != line.npos) environment.insert_or_assign(line.substr(0, separator), line.substr(separator + 1));
            if (end == text.npos) break;
            begin = end + 1;
        }
        if (executableOnPath(L"cl.exe", environment).empty()) {
            throw Failure(L"Visual Studio найден, но C++ toolset для выбранной архитектуры не установлен.");
        }
        return environment;
    }

    std::vector<Target> doBuild(const BuildSettings& settings, bool configureOnly = false) {
        emit(EventKind::Started, configureOnly ? L"Конфигурация CMake запущена." : L"Сборка запущена.");
        checkCancellation();
        const auto extraArguments = configureArguments(settings.cmakeArguments);
        const fs::path file = absolutePath(settings.cmakeFile);
        if (!equal(file.filename().wstring(), L"CMakeLists.txt") || !fileExists(file)) {
            throw Failure(L"Выберите существующий CMakeLists.txt.", ERROR_FILE_NOT_FOUND);
        }
        const fs::path source = file.parent_path();
        const fs::path buildDirectory = settings.buildDirectory.empty()
            ? source / L"build-cmakebuild" : absolutePath(settings.buildDirectory, source);
        if (samePath(source, buildDirectory)) {
            throw Failure(L"Выберите отдельную папку сборки, отличную от папки исходников.");
        }
        const std::wstring configuration = settings.configuration.empty() ? L"Release" : settings.configuration;
        Environment environment = currentEnvironment();
        const fs::path cmake = settings.cmakeExecutable.empty() ? discoverTool(L"cmake.exe", environment)
            : executableOnPath(settings.cmakeExecutable, environment);
        if (cmake.empty()) {
            throw Failure(L"Не найден cmake.exe. Укажите путь к CMake в настройках.", ERROR_FILE_NOT_FOUND);
        }
        prependPath(environment, cmake.parent_path());
        const Cache cache = readCache(buildDirectory);
        const auto generator = cacheValue(cache, L"CMAKE_GENERATOR");
        const auto previousSource = cacheValue(cache, L"CMAKE_HOME_DIRECTORY");
        if (!previousSource.empty() && !samePath(previousSource, source)) {
            throw Failure(L"Папка сборки уже принадлежит другому проекту. Выберите новую папку сборки.");
        }
        CompilerMode mode = settings.compiler;
        const CompilerMode previousCompiler = cachedCompiler(cache);
        if (!generator.empty() && mode != CompilerMode::Automatic && mode != CompilerMode::Environment
            && previousCompiler != CompilerMode::Environment && mode != previousCompiler) {
            throw Failure(L"Выбранный компилятор отличается от CMakeCache.txt. Для смены компилятора выберите новую папку сборки.");
        }
        const bool needsConfigure = configureOnly
            || !canReuseConfiguration(settings, cache, source, buildDirectory, cmake, configuration);
        fs::path msvcScript;
        fs::path mingw;
        if (mode == CompilerMode::Automatic) {
            if (!generator.empty()) mode = previousCompiler;
            else {
                msvcScript = visualStudioEnvironmentScript(environment);
                if (!msvcScript.empty() || !executableOnPath(L"cl.exe", environment).empty()) mode = CompilerMode::Msvc;
                else {
                    mingw = discoverTool(L"g++.exe", environment);
                    if (!mingw.empty()) mode = CompilerMode::Mingw;
                    else if (!executableOnPath(L"clang++.exe", environment).empty()) mode = CompilerMode::Environment;
                    else throw Failure(L"Не найден C++ компилятор. Установите MSVC Build Tools или MinGW.", ERROR_FILE_NOT_FOUND);
                }
            }
        }
        if (mode == CompilerMode::Msvc) {
            // Prefer the Visual Studio installation that owns the cached compiler.
            fs::path ancestor = cachedCompilerPath(cache).parent_path();
            for (int level = 0; level < 12 && !ancestor.empty(); ++level) {
                const fs::path candidate = ancestor / L"Common7/Tools/VsDevCmd.bat";
                if (fileExists(candidate)) { msvcScript = candidate; break; }
                if (ancestor == ancestor.parent_path()) break;
                ancestor = ancestor.parent_path();
            }
            if (msvcScript.empty()) msvcScript = visualStudioEnvironmentScript(environment);
            environment = msvcEnvironment(std::move(environment), msvcScript, cache);
        } else if (mode == CompilerMode::Mingw) {
            const fs::path cached = cachedCompilerPath(cache);
            if (!cached.empty() && fileExists(cached)) mingw = cached;
            if (mingw.empty()) mingw = discoverTool(L"g++.exe", environment);
            if (mingw.empty()) throw Failure(L"Не найден MinGW g++.exe. Добавьте папку MinGW/bin в PATH.", ERROR_FILE_NOT_FOUND);
            prependPath(environment, mingw.parent_path());
        }
        addRuntimePaths(environment, cache);
        const auto cachedMake = executableOnPath(cacheValue(cache, L"CMAKE_MAKE_PROGRAM"), environment);
        if (!cachedMake.empty()) prependPath(environment, cachedMake.parent_path());
        if (needsConfigure) {
            const auto ninja = discoverTool(L"ninja.exe", environment);
            if (!ninja.empty()) prependPath(environment, ninja.parent_path());
            std::vector<std::wstring> configure{ L"-S", source.wstring(), L"-B", buildDirectory.wstring(),
                L"-DCMAKE_BUILD_TYPE=" + configuration };
            // Explicit values also override an existing cache where CTest enabled tests.
            const std::wstring testSwitch = settings.buildTests ? L"ON" : L"OFF";
            configure.emplace_back(L"-DBUILD_TESTING:BOOL=" + testSwitch);
            configure.emplace_back(L"-DBUILD_TESTS:BOOL=" + testSwitch);
            log(settings.buildTests ? L"BUILD_TESTING и BUILD_TESTS включены (без запуска тестов)."
                : L"BUILD_TESTING и BUILD_TESTS выключены.");
            if (generator.empty()) {
                if (!ninja.empty()) {
                    configure.insert(configure.end(), { L"-G", L"Ninja", L"-DCMAKE_MAKE_PROGRAM=" + ninja.wstring() });
                } else if (mode == CompilerMode::Msvc) {
                    if (executableOnPath(L"nmake.exe", environment).empty()) {
                        throw Failure(L"Не найден Ninja или nmake. Установите Ninja или C++ tools для MSVC.", ERROR_FILE_NOT_FOUND);
                    }
                    configure.insert(configure.end(), { L"-G", L"NMake Makefiles" });
                } else if (mode == CompilerMode::Mingw) {
                    const auto make = mingw.parent_path() / L"mingw32-make.exe";
                    if (!fileExists(make)) throw Failure(L"Не найден Ninja или mingw32-make.exe.", ERROR_FILE_NOT_FOUND);
                    configure.insert(configure.end(), { L"-G", L"MinGW Makefiles", L"-DCMAKE_MAKE_PROGRAM=" + make.wstring() });
                }
                if (mode == CompilerMode::Mingw) {
                    configure.emplace_back(L"-DCMAKE_CXX_COMPILER=" + mingw.wstring());
                    const auto gcc = mingw.parent_path() / L"gcc.exe";
                    if (fileExists(gcc)) configure.emplace_back(L"-DCMAKE_C_COMPILER=" + gcc.wstring());
                }
            } else {
                log(L"Сохраняем генератор из CMakeCache.txt: " + generator);
            }
            emit(EventKind::Status, L"Поиск установленных Qt…");
            CompilerMode qtCompiler = mode;
            if (qtCompiler == CompilerMode::Environment) {
                // An existing build tree determines the ABI even in Environment mode.
                // A Developer Command Prompt may contain both cl and g++.
                if (!cachedCompilerPath(cache).empty()) {
                    qtCompiler = previousCompiler;
                    if (qtCompiler == CompilerMode::Mingw) mingw = cachedCompilerPath(cache);
                } else if (!environmentValue(environment, L"CXX").empty()) {
                    const auto compiler = executableOnPath(environmentValue(environment, L"CXX"), environment);
                    const auto name = compiler.filename().wstring();
                    if (equal(name, L"cl.exe") || equal(name, L"clang-cl.exe")) qtCompiler = CompilerMode::Msvc;
                    else if (equal(name, L"g++.exe")) { qtCompiler = CompilerMode::Mingw; mingw = compiler; }
                } else if (!executableOnPath(L"cl.exe", environment).empty()
                    || !executableOnPath(L"clang-cl.exe", environment).empty()) qtCompiler = CompilerMode::Msvc;
                else {
                    mingw = executableOnPath(L"g++.exe", environment);
                    if (!mingw.empty()) qtCompiler = CompilerMode::Mingw;
                }
            }
            auto architecture = buildArchitecture(cache, environment);
            unsigned gccMajor = 0;
            if (qtCompiler == CompilerMode::Mingw) {
                const auto machine = process(mingw, { L"-dumpmachine" }, source, environment, false);
                const auto version = process(mingw, { L"-dumpfullversion", L"-dumpversion" }, source, environment, false);
                const auto triple = trim(decode(machine.output));
                architecture.clear();
                if (!machine.exitCode && contains(triple, L"mingw")) {
                    architecture = normalizedArchitecture(triple.substr(0, triple.find(L'-')));
                }
                const auto numbers = numericParts(trim(decode(version.output)));
                if (!version.exitCode && !numbers.empty()) gccMajor = numbers.front();
            }
            checkCancellation();
            const auto qtInstallations = discoverQt(environment, cache);
            checkCancellation();
            // Environment hints follow explicit project/cache paths. Let find_package
            // enforce the requested Qt major/version, including nested CMake files.
            auto& prefixes = environment[L"CMAKE_PREFIX_PATH"];
            for (const auto& qt : qtInstallations) {
                if (qt.compiler != qtCompiler || qt.architecture != architecture) continue;
                if (qt.compiler == CompilerMode::Mingw && (!gccMajor || qt.gccMajor != gccMajor)) continue;
                const auto system = cacheValue(cache, L"CMAKE_SYSTEM_NAME");
                if (!system.empty() && !equal(system, L"Windows")) continue;
                const auto existing = pathList(prefixes);
                if (std::ranges::any_of(existing, [&](const auto& prefix) {
                    return equal(prefix.lexically_normal().wstring(), qt.prefix.lexically_normal().wstring());
                })) continue;
                if (!prefixes.empty() && prefixes.back() != L';') prefixes += L';';
                prefixes += qt.prefix.wstring();
                log(L"Qt " + qt.version + L" (" + architecture + L") доступен для CMake: " + qt.prefix.wstring());
            }
            checkCancellation();
            // Project-specific options follow panel defaults and are passed as
            // individual process arguments; shell metacharacters remain data.
            configure.insert(configure.end(), extraArguments.begin(), extraArguments.end());
            // The Configuration field also controls --config and File API target
            // selection; keep it authoritative after custom definitions/unsets.
            configure.emplace_back(L"-DCMAKE_BUILD_TYPE=" + configuration);
            if (!extraArguments.empty()) log(L"Применяем дополнительные параметры CMake.");
            fs::create_directories(buildDirectory / L".cmake/api/v1/query/client-cmakebuild");
            {
                std::ofstream query(buildDirectory / L".cmake/api/v1/query/client-cmakebuild/codemodel-v2", std::ios::binary);
                if (!query) throw Failure(L"Не удалось создать запрос CMake File API.");
            }
            const auto pending = buildDirectory / L".cmake/cmakebuild-configure-pending";
            {
                // Keep this marker on failed/cancelled configuration, even if CMake
                // leaves a cache and older generated files that appear usable.
                std::ofstream marker(pending, std::ios::binary | std::ios::trunc);
                if (!marker) throw Failure(L"Не удалось отметить начало конфигурации CMake.");
            }
            emit(EventKind::Status, L"Конфигурирование CMake…");
            auto configured = process(cmake, configure, source, environment);
            checkCancellation();
            if (configured.exitCode) throw Failure(L"CMake configure завершился с кодом " + std::to_wstring(configured.exitCode) + L".", configured.exitCode);
            rememberConfigureArguments(settings, buildDirectory);
            fs::remove(pending);
        } else {
            log(L"Используем подготовленный каталог сборки: поиск Qt и отдельная конфигурация CMake пропущены.");
        }
        Cache configuredCache = readCache(buildDirectory);
        addRuntimePaths(environment, configuredCache);
        if (!configureOnly) {
            emit(EventKind::Status, L"Сборка " + configuration + L"…");
            std::vector<std::wstring> build{ L"--build", buildDirectory.wstring(), L"--config", configuration, L"--parallel" };
            // NMake explicitly does not support CMake's parallel build option.
            if (contains(cacheValue(configuredCache, L"CMAKE_GENERATOR"), L"NMake")) build.pop_back();
            if (settings.cleanFirst) build.push_back(L"--clean-first");
            if (!settings.target.empty()) build.insert(build.end(), { L"--target", settings.target });
            const auto compiled = process(cmake, build, source, environment, true, {}, false, true);
            checkCancellation();
            if (compiled.exitCode && compiled.cannotWriteOutput) {
                throw Failure(L"LNK1168: не удалось перезаписать выходной файл. Закройте запущенное приложение и повторите сборку.", compiled.exitCode);
            }
            if (compiled.exitCode) throw Failure(L"Сборка завершилась с кодом " + std::to_wstring(compiled.exitCode) + L".", compiled.exitCode);
            // The native build tool can regenerate CMake after script changes.
            // Recover its refreshed package/runtime choices after compilation.
            configuredCache = readCache(buildDirectory);
        }
        for (const wchar_t* key : { L"Qt6_DIR", L"Qt5_DIR" }) {
            const auto selected = cacheValue(configuredCache, key);
            if (!selected.empty() && directoryExists(selected)) log(std::wstring(L"CMake выбрал ") + key + L": " + selected);
        }
        std::vector<Target> targets;
        try { targets = discoverTargets(buildDirectory, configuration); }
        catch (const Failure& failure) {
            log(std::wstring(configureOnly ? L"Конфигурация успешна" : L"Сборка успешна")
                + L", но цели запуска не определены: " + failure.message);
        }
        checkCancellation();
        addRuntimePaths(environment, configuredCache);
        lastBuildEnvironment = std::move(environment);
        emit(EventKind::Targets, {}, targets);
        if (targets.empty()) log(configureOnly ? L"В проекте нет исполняемых целей .exe. Конфигурация завершена."
            : L"В проекте нет исполняемых целей .exe. Проект успешно собран.");
        return targets;
    }

    void doClean(const BuildSettings& settings) {
        emit(EventKind::Started, L"Очистка запущена.");
        checkCancellation();
        const fs::path file = absolutePath(settings.cmakeFile);
        if (!equal(file.filename().wstring(), L"CMakeLists.txt") || !fileExists(file)) {
            throw Failure(L"Выберите существующий CMakeLists.txt.", ERROR_FILE_NOT_FOUND);
        }
        const fs::path source = file.parent_path();
        const fs::path buildDirectory = settings.buildDirectory.empty()
            ? source / L"build-cmakebuild" : absolutePath(settings.buildDirectory, source);
        if (samePath(source, buildDirectory)) {
            throw Failure(L"Выберите отдельную папку сборки, отличную от папки исходников.");
        }
        if (!fileExists(buildDirectory / L"CMakeCache.txt")) {
            throw Failure(L"В папке сборки нет CMakeCache.txt. Сначала выполните сборку проекта.", ERROR_FILE_NOT_FOUND);
        }
        const Cache cache = readCache(buildDirectory);
        const auto cachedSource = cacheValue(cache, L"CMAKE_HOME_DIRECTORY");
        const auto generator = cacheValue(cache, L"CMAKE_GENERATOR");
        if (cachedSource.empty() || generator.empty()) {
            throw Failure(L"CMakeCache.txt не содержит данных проекта и генератора для очистки.");
        }
        if (!samePath(cachedSource, source)) {
            throw Failure(L"Папка сборки принадлежит другому проекту. Выберите его папку сборки для очистки.");
        }
        Environment environment = currentEnvironment();
        const fs::path cmake = settings.cmakeExecutable.empty() ? discoverTool(L"cmake.exe", environment)
            : executableOnPath(settings.cmakeExecutable, environment);
        if (cmake.empty()) {
            throw Failure(L"Не найден cmake.exe. Укажите путь к CMake в настройках.", ERROR_FILE_NOT_FOUND);
        }
        prependPath(environment, cmake.parent_path());
        // CMake keeps the native build tool in its existing cache. Restore its
        // directory without selecting another generator or searching for Qt.
        const fs::path make = executableOnPath(cacheValue(cache, L"CMAKE_MAKE_PROGRAM"), environment);
        if (!make.empty()) prependPath(environment, make.parent_path());
        const fs::path compiler = cachedCompilerPath(cache);
        if (!compiler.empty() && fileExists(compiler)) prependPath(environment, compiler.parent_path());
        const auto compilerName = compiler.filename().wstring();
        if (contains(generator, L"NMake") || contains(generator, L"Visual Studio")
                || equal(compilerName, L"cl.exe") || equal(compilerName, L"clang-cl.exe")) {
            fs::path script;
            fs::path ancestor = compiler.parent_path();
            for (int level = 0; level < 12 && !ancestor.empty(); ++level) {
                const fs::path candidate = ancestor / L"Common7/Tools/VsDevCmd.bat";
                if (fileExists(candidate)) { script = candidate; break; }
                if (ancestor == ancestor.parent_path()) break;
                ancestor = ancestor.parent_path();
            }
            if (script.empty()) script = visualStudioEnvironmentScript(environment);
            environment = msvcEnvironment(std::move(environment), script, cache);
        }
        addRuntimePaths(environment, cache);
        checkCancellation();
        const std::wstring configuration = settings.configuration.empty() ? L"Release" : settings.configuration;
        log(L"Очистка проекта через генератор " + generator + L".");
        emit(EventKind::Status, L"Очистка проекта…");
        const auto cleaned = process(cmake,
            {L"--build", buildDirectory.wstring(), L"--config", configuration, L"--target", L"clean"},
            source, environment);
        checkCancellation();
        if (cleaned.exitCode) {
            throw Failure(L"Очистка завершилась с кодом " + std::to_wstring(cleaned.exitCode) + L".", cleaned.exitCode);
        }
    }

    enum class ProjectOperation { Build, Clean, Configure };

    void projectTask(const BuildSettings& settings, ProjectOperation operation = ProjectOperation::Build) noexcept {
        const auto started = std::chrono::steady_clock::now();
        const bool cleaning = operation == ProjectOperation::Clean;
        const bool configuring = operation == ProjectOperation::Configure;
        const auto failed = cleaning ? EventKind::CleanFailed
            : configuring ? EventKind::ConfigureFailed : EventKind::BuildFailed;
        Event result{ cleaning ? EventKind::CleanSucceeded
                : configuring ? EventKind::ConfigureSucceeded : EventKind::BuildSucceeded,
            cleaning ? L"Очистка завершена." : configuring ? L"Конфигурация CMake завершена." : L"Сборка завершена.", {} };
        try {
            if (cleaning) doClean(settings);
            else result.targets = doBuild(settings, configuring);
        }
        catch (const Failure& failure) {
            active.store(false);
            log(failure.message);
            result = Event{ failed, failure.message, {}, failure.code };
        } catch (const std::exception& exception) {
            active.store(false);
            const auto message = (cleaning ? L"Ошибка очистки: "
                : configuring ? L"Ошибка конфигурации CMake: " : L"Ошибка сборки: ") + decode(exception.what());
            log(message);
            result = Event{ failed, message, {}, ERROR_INVALID_DATA };
        } catch (...) {
            active.store(false);
            result = Event{ failed, cleaning ? L"Неизвестная ошибка очистки."
                    : configuring ? L"Неизвестная ошибка конфигурации CMake." : L"Неизвестная ошибка сборки.",
                {}, ERROR_INVALID_DATA };
        }
        active.store(false);
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        emit(result.kind, std::move(result.text), std::move(result.targets), result.exitCode, duration);
    }

    void runTask(const Target& target, const RunSettings& settings, const std::wstring& buildDirectory) noexcept {
        DWORD exitCode = ERROR_INVALID_DATA;
        std::wstring message;
        try {
            const auto hasControlCharacters = [](std::wstring_view text) {
                return std::ranges::any_of(text, [](wchar_t character) {
                    return character == L'\0' || character == L'\r' || character == L'\n';
                });
            };
            if (hasControlCharacters(settings.arguments)) {
                throw Failure(L"Аргументы запуска не должны содержать нулевой символ или переносы строк.");
            }
            if (hasControlCharacters(settings.workingDirectory)) {
                throw Failure(L"Рабочая папка запуска не должна содержать нулевой символ или переносы строк.");
            }
            for (const auto& [name, value] : settings.environment) {
                if (name.empty() || name.find(L'=') != name.npos || hasControlCharacters(name)) {
                    throw Failure(L"Имя переменной окружения запуска не должно быть пустым или содержать '=', нулевой символ и переносы строк.");
                }
                if (hasControlCharacters(value)) {
                    throw Failure(L"Значение переменной окружения запуска не должно содержать нулевой символ или переносы строк.");
                }
            }
            const fs::path executable = absolutePath(target.executable);
            if (target.executable.empty() || !fileExists(executable)) {
                throw Failure(L"Исполняемый файл не найден. Сначала соберите выбранную цель.", ERROR_FILE_NOT_FOUND);
            }
            const fs::path directory = settings.workingDirectory.empty() ? executable.parent_path()
                : absolutePath(settings.workingDirectory, executable.parent_path());
            if (!directoryExists(directory)) throw Failure(L"Рабочая папка не существует.", ERROR_PATH_NOT_FOUND);
            Environment environment = lastBuildEnvironment.empty() ? currentEnvironment() : lastBuildEnvironment;
            bool recoveredRuntime = false;
            if (lastBuildEnvironment.empty() && !buildDirectory.empty()) {
                const fs::path explicitBuildDirectory = absolutePath(buildDirectory);
                if (fileExists(explicitBuildDirectory / L"CMakeCache.txt")) {
                    try {
                        addRuntimePaths(environment, readCache(explicitBuildDirectory));
                        recoveredRuntime = true;
                    } catch (const Failure& failure) {
                        log(L"Не удалось восстановить окружение из папки сборки: " + failure.message);
                    }
                }
            }
            // Executable targets may be restored from settings after restarting
            // the panel. An explicit build directory also covers output artifacts
            // placed in the source tree; otherwise inspect the executable's parents.
            fs::path ancestor = executable.parent_path();
            for (int level = 0; !recoveredRuntime && level < 16 && !ancestor.empty(); ++level) {
                if (fileExists(ancestor / L"CMakeCache.txt")) {
                    try { addRuntimePaths(environment, readCache(ancestor)); }
                    catch (const Failure& failure) { log(L"Не удалось восстановить окружение: " + failure.message); }
                    break;
                }
                if (ancestor == ancestor.parent_path()) break;
                ancestor = ancestor.parent_path();
            }
            prependPath(environment, executable.parent_path());
            for (const auto& [name, value] : settings.environment) environment.insert_or_assign(name, value);
            std::wstring command = quoteArgument(executable.wstring());
            if (!settings.arguments.empty()) {
                command.push_back(L' ');
                command += settings.arguments;
            }
            if (command.size() >= 32767) throw Failure(L"Слишком длинная командная строка запуска.");
            emit(EventKind::Status, L"Запуск " + target.name + L"…");
            const auto result = process(executable, {}, directory, environment, true, command, true);
            exitCode = result.exitCode;
            message = cancelled.load() ? L"Программа остановлена." : L"Программа завершилась с кодом " + std::to_wstring(exitCode) + L".";
        } catch (const Failure& failure) {
            exitCode = failure.code;
            message = failure.message;
        } catch (const std::exception& exception) {
            message = L"Ошибка запуска: " + decode(exception.what());
        } catch (...) {
            message = L"Неизвестная ошибка запуска.";
        }
        active.store(false);
        log(message);
        emit(EventKind::Finished, std::move(message), {}, exitCode);
    }

    template<class Task>
    bool start(Task task) {
        std::lock_guard lock(launchMutex);
        if (active.exchange(true)) return false;
        // A finished thread only has its final, nonblocking event callback left.
        if (worker.joinable()) worker.join();
        cancelled.store(false);
        try { worker = std::jthread([this, task = std::move(task)] { task(); }); }
        catch (...) { active.store(false); throw; }
        return true;
    }

    void cancel() noexcept {
        cancelled.store(true);
        std::lock_guard lock(processMutex);
        if (activeJob) TerminateJobObject(activeJob, ERROR_CANCELLED);
    }
};

Engine::Engine(Callback callback) : impl_(std::make_unique<Impl>(std::move(callback))) {}

Engine::~Engine() {
    impl_->cancel();
    if (impl_->worker.joinable()) impl_->worker.join();
}

bool Engine::build(BuildSettings settings) {
    return impl_->start([state = impl_.get(), settings = std::move(settings)] { state->projectTask(settings); });
}

bool Engine::configure(BuildSettings settings) {
    return impl_->start([state = impl_.get(), settings = std::move(settings)] {
        state->projectTask(settings, Impl::ProjectOperation::Configure);
    });
}

bool Engine::clean(BuildSettings settings) {
    return impl_->start([state = impl_.get(), settings = std::move(settings)] {
        state->projectTask(settings, Impl::ProjectOperation::Clean);
    });
}

bool Engine::run(const Target& target, std::wstring workingDirectory, std::wstring buildDirectory) {
    RunSettings settings;
    settings.workingDirectory = std::move(workingDirectory);
    return runConfigured(target, std::move(settings), std::move(buildDirectory));
}

bool Engine::runConfigured(const Target& target, RunSettings settings, std::wstring buildDirectory) {
    return impl_->start([state = impl_.get(), target, settings = std::move(settings),
                        build = std::move(buildDirectory)] {
        state->runTask(target, settings, build);
    });
}

void Engine::cancel() { impl_->cancel(); }
bool Engine::busy() const noexcept { return impl_->active.load(); }

std::vector<Target> readExecutableTargets(const BuildSettings& settings, bool* projectMatches) noexcept {
    if (projectMatches) *projectMatches = true;
    try {
        if (settings.cmakeFile.empty()) return {};
        const fs::path source = absolutePath(settings.cmakeFile).parent_path();
        const fs::path buildDirectory = settings.buildDirectory.empty()
            ? source / L"build-cmakebuild" : absolutePath(settings.buildDirectory, source);
        const auto cachedSource = cacheValue(readCache(buildDirectory), L"CMAKE_HOME_DIRECTORY");
        if (!cachedSource.empty() && !samePath(cachedSource, source)) {
            if (projectMatches) *projectMatches = false;
            return {};
        }
        const std::wstring configuration = settings.configuration.empty() ? L"Release" : settings.configuration;
        auto targets = discoverTargets(buildDirectory, configuration, source, projectMatches);
        std::erase_if(targets, [](const Target& target) {
            std::error_code error;
            return !fs::is_regular_file(fs::path(target.executable), error);
        });
        return targets;
    } catch (...) {
        return {};
    }
}

std::wstring describeDetectedTools() {
    try {
        const Environment environment = currentEnvironment();
        const auto cmake = discoverTool(L"cmake.exe", environment);
        const auto ninja = discoverTool(L"ninja.exe", environment);
        const auto msvc = discoverVisualStudio(environment);
        const auto mingw = discoverTool(L"g++.exe", environment);
        auto describe = [](const wchar_t* name, const fs::path& path) {
            return std::wstring(name) + L": " + (path.empty() ? L"не найден" : path.wstring()) + L"\r\n";
        };
        auto description = describe(L"CMake", cmake) + describe(L"Ninja", ninja) + describe(L"MSVC", msvc) + describe(L"MinGW", mingw);
        const auto installations = discoverQt(environment);
        if (installations.empty()) description += L"Qt: не найден; при необходимости задайте QTDIR или CMAKE_PREFIX_PATH.\r\n";
        for (const auto& qt : installations) {
            description += L"Qt " + qt.version + L" (" + (qt.compiler == CompilerMode::Msvc ? L"MSVC" : L"MinGW")
                + L", " + qt.architecture + L"): " + qt.prefix.wstring() + L"\r\n";
        }
        return description;
    } catch (...) {
        return L"Не удалось определить инструменты. Можно указать CMake вручную.";
    }
}

} // namespace cb
