#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace cb {

struct BuildProgress {
    std::uint64_t completed{};
    std::uint64_t total{};
    bool operator==(const BuildProgress&) const = default;
};

// Read only a progress prefix at the beginning of a build output line. CMake
// Makefiles print [ 42%], while Ninja prints [12/80]. ANSI SGR colors may precede
// the prefix; ordinary diagnostics containing brackets are not progress.
inline std::optional<BuildProgress> parseBuildProgress(std::string_view line) noexcept {
    auto skipSpaces = [&] {
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    };
    for (;;) {
        skipSpaces();
        if (!line.starts_with("\x1b[")) break;
        std::size_t end = 2;
        while (end < line.size() && ((line[end] >= '0' && line[end] <= '9')
                || line[end] == ';' || line[end] == ':')) ++end;
        if (end == line.size() || line[end] != 'm') return {};
        line.remove_prefix(end + 1);
    }
    if (line.empty() || line.front() != '[') return {};
    line.remove_prefix(1);
    skipSpaces();
    auto number = [&]() -> std::optional<std::uint64_t> {
        if (line.empty() || line.front() < '0' || line.front() > '9') return {};
        std::uint64_t value = 0;
        while (!line.empty() && line.front() >= '0' && line.front() <= '9') {
            const auto digit = static_cast<std::uint64_t>(line.front() - '0');
            if (value > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10) return {};
            value = value * 10 + digit;
            line.remove_prefix(1);
        }
        return value;
    };
    const auto completed = number();
    if (!completed) return {};
    skipSpaces();
    if (line.empty()) return {};
    const char separator = line.front();
    line.remove_prefix(1);
    BuildProgress progress{*completed, 100};
    if (separator == '/') {
        skipSpaces();
        const auto total = number();
        if (!total) return {};
        progress.total = *total;
    } else if (separator != '%') {
        return {};
    }
    skipSpaces();
    if (line.empty() || line.front() != ']' || !progress.total
            || progress.completed > progress.total) return {};
    return progress;
}

} // namespace cb
