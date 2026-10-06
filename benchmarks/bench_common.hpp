#pragma once

// Both adapters exercise production widgets; timing, payloads and validation
// live here so a toolkit cannot silently get a cheaper workload.
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cbbench {
using Clock = std::chrono::steady_clock;

inline double milliseconds(Clock::duration elapsed) {
    return std::chrono::duration<double, std::milli>(elapsed).count();
}

inline std::string utf8(std::wstring_view value) {
    const auto length = static_cast<int>(value.size());
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), length, nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("UTF-8 fixture conversion failed");
    std::string result(static_cast<std::size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
        result.data(), count, nullptr, nullptr))
        throw std::runtime_error("UTF-8 fixture conversion failed");
    return result;
}

inline std::string quoted(std::string_view value) {
    std::string result = "\"";
    for (const unsigned char c : value) {
        if (c == '\\' || c == '"') { result += '\\'; result += static_cast<char>(c); }
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else if (c == '\t') result += "\\t";
        else if (c < 32) throw std::runtime_error("Unsupported JSON control character");
        else result += static_cast<char>(c);
    }
    return result + '"';
}

inline double cpuMilliseconds() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        throw std::runtime_error("GetProcessTimes failed");
    const auto ticks = [](FILETIME time) {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
}

struct Memory {
    double privateMiB{}, workingMiB{};
    DWORD handles{}, gdi{}, user{};
};

inline Memory memory() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
        throw std::runtime_error("GetProcessMemoryInfo failed");
    Memory result{static_cast<double>(counters.PrivateUsage) / 1048576.0,
        static_cast<double>(counters.WorkingSetSize) / 1048576.0};
    if (!GetProcessHandleCount(GetCurrentProcess(), &result.handles))
        throw std::runtime_error("GetProcessHandleCount failed");
    result.gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    result.user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    return result;
}

struct Metric {
    std::string name, unit;
    double value{};
    std::vector<double> samples;
};

inline double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) throw std::runtime_error("Empty timing samples");
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(values.size()))) - 1;
    return values[std::min(index, values.size() - 1)];
}

template<class Adapter>
int run(int argc, wchar_t** argv) {
    const auto entry = Clock::now();
    if (argc != 2) { std::cerr << "Usage: UI-benchmark <new-result.json>\n"; return 2; }
    bool initialized = false;
    try {
        const auto output = std::filesystem::absolute(argv[1]);
        auto ready = output; ready.replace_extension(L".ready");
        auto ini = output; ini.replace_extension(L".ini");
        if (std::filesystem::exists(output) || std::filesystem::exists(ready)
            || std::filesystem::exists(ini))
            throw std::runtime_error("Each benchmark process needs fresh result/ready/INI paths");
        std::filesystem::create_directories(output.parent_path());
        Adapter::initialize(); initialized = true;
        auto adapter = std::make_unique<Adapter>(ini);
        adapter->show(); adapter->flush();
        const double startup = milliseconds(Clock::now() - entry);
        const HWND window = adapter->hwnd();
        RECT client{};
        if (!IsWindowVisible(window) || !GetClientRect(window, &client))
            throw std::runtime_error("Benchmark needs a visible native window");
        const UINT dpi = GetDpiForWindow(window);
        if (!dpi || GetUpdateRect(window, nullptr, FALSE))
            throw std::runtime_error("First native paint did not finish");
        { std::ofstream signal(ready); signal << "first native paint completed\n";
          if (!signal) throw std::runtime_error("Could not write ready signal"); }

        // Let the initial show/focus messages settle before measuring idle CPU.
        adapter->wait(std::chrono::milliseconds{250});
        const double idleCpuStart = cpuMilliseconds();
        const auto idleStart = Clock::now();
        adapter->wait(std::chrono::milliseconds{1500});
        const double idleSeconds = milliseconds(Clock::now() - idleStart) / 1000.0;
        const double idleCpu = (cpuMilliseconds() - idleCpuStart) / idleSeconds;
        const Memory idleMemory = memory();

        constexpr std::size_t chunkCount = 128, linesPerChunk = 64;
        std::vector<std::wstring> chunks;
        chunks.reserve(chunkCount);
        std::string expected;
        std::size_t inputBytes = 0;
        for (std::size_t chunk = 0; chunk < chunkCount; ++chunk) {
            std::wstring packet;
            for (std::size_t line = 0; line < linesPerChunk; ++line) {
                packet += L"[12/80] line " + std::to_wstring(chunk * linesPerChunk + line)
                    + L" C:/benchmark/Проект/src/widget.cpp : warning: Unicode 😀 "
                    + std::wstring(96, L'x') + L"\n";
            }
            const auto bytes = utf8(packet);
            inputBytes += bytes.size(); expected += bytes;
            // Independent reference for the existing 900000 -> 450000 policy.
            if (expected.size() > 900000) {
                const auto end = expected.find('\n', expected.size() - 450000);
                if (end == std::string::npos) throw std::runtime_error("Fixture missing newline");
                expected.erase(0, end + 1);
            }
            chunks.push_back(std::move(packet));
        }

        adapter->append(L"Warm-up: Русский текст 😀\n"); adapter->flush();
        adapter->clear(); adapter->flush();
        if (!adapter->text().empty()) throw std::runtime_error("Journal clear failed");
        std::vector<double> logSamples; logSamples.reserve(chunkCount);
        const double logCpuStart = cpuMilliseconds();
        const auto logStart = Clock::now();
        for (const auto& packet : chunks) {
            const auto start = Clock::now();
            adapter->append(packet); adapter->flush();
            logSamples.push_back(milliseconds(Clock::now() - start));
        }
        const double logElapsed = milliseconds(Clock::now() - logStart);
        const double logCpu = cpuMilliseconds() - logCpuStart;
        const Memory logMemory = memory();
        const auto retained = adapter->byteCount();
        // Nana's public text getter uses CRLF; its adapter normalizes to LF.
        if (retained != expected.size() || adapter->text() != expected)
            throw std::runtime_error("Widget lost or changed log text under load");

        constexpr std::size_t redrawCount = 64, themeCount = 32;
        adapter->redraw();
        std::vector<double> redrawSamples; redrawSamples.reserve(redrawCount);
        for (std::size_t i = 0; i < redrawCount; ++i) {
            const auto start = Clock::now(); adapter->redraw();
            redrawSamples.push_back(milliseconds(Clock::now() - start));
        }
        adapter->theme(false); adapter->flush();
        std::vector<double> themeSamples; themeSamples.reserve(themeCount);
        for (std::size_t i = 0; i < themeCount; ++i) {
            const auto start = Clock::now(); adapter->theme(i % 2 == 0); adapter->flush();
            themeSamples.push_back(milliseconds(Clock::now() - start));
        }
        RECT finalClient{};
        if (!GetClientRect(window, &finalClient)
            || finalClient.right != client.right || finalClient.bottom != client.bottom
            || GetDpiForWindow(window) != dpi || adapter->text() != expected)
            throw std::runtime_error("Geometry, DPI or journal changed during benchmark");

        std::vector<Metric> metrics{
            {"idle_cpu_ms_per_second", "ms/s", idleCpu},
            {"idle_private_mib", "MiB", idleMemory.privateMiB},
            {"idle_working_set_mib", "MiB", idleMemory.workingMiB},
            {"idle_handles", "count", static_cast<double>(idleMemory.handles)},
            {"idle_gdi_objects", "count", static_cast<double>(idleMemory.gdi)},
            {"idle_user_objects", "count", static_cast<double>(idleMemory.user)},
            {"log_total_ms", "ms", logElapsed}, {"log_cpu_ms", "ms", logCpu},
            {"log_chunk_p50_ms", "ms", percentile(logSamples, .5), logSamples},
            {"log_chunk_p95_ms", "ms", percentile(logSamples, .95)},
            {"log_chunk_max_ms", "ms", *std::max_element(logSamples.begin(), logSamples.end())},
            {"log_private_mib", "MiB", logMemory.privateMiB},
            {"log_working_set_mib", "MiB", logMemory.workingMiB},
            {"redraw_p50_ms", "ms", percentile(redrawSamples, .5), redrawSamples},
            {"redraw_p95_ms", "ms", percentile(redrawSamples, .95)},
            {"theme_p50_ms", "ms", percentile(themeSamples, .5), themeSamples},
            {"theme_p95_ms", "ms", percentile(themeSamples, .95)}
        };
#if defined(_MSC_VER)
        const std::string compiler = "MSVC " + std::to_string(_MSC_VER);
#elif defined(__GNUC__)
        const std::string compiler = "GCC " + std::to_string(__GNUC__) + '.' + std::to_string(__GNUC_MINOR__);
#else
        const std::string compiler = "unknown";
#endif
#if defined(NDEBUG)
        constexpr std::string_view configuration = "Release";
#else
        constexpr std::string_view configuration = "Debug";
#endif
        // Destruct/close before publishing success, including isolated INI save.
        adapter.reset(); Adapter::shutdown(); initialized = false;
        std::ofstream result(output, std::ios::binary);
        result.imbue(std::locale::classic()); result << std::setprecision(12);
        result << "{\n  \"schema_version\":1,\n  \"toolkit\":" << quoted(Adapter::name)
            << ",\n  \"app_version\":" << quoted(Adapter::appVersion)
            << ",\n  \"toolkit_revision\":" << quoted(Adapter::toolkitRevision)
            << ",\n  \"compiler\":" << quoted(compiler)
            << ",\n  \"configuration\":" << quoted(configuration)
            << ",\n  \"startup_ms\":" << startup
            << ",\n  \"metadata\":{\"dpi\":" << dpi
            << ",\"client_width\":" << client.right << ",\"client_height\":" << client.bottom
            << ",\"log_input_bytes\":" << inputBytes << ",\"log_retained_bytes\":" << retained
            << ",\"log_chunks\":" << chunkCount << ",\"lines_per_chunk\":" << linesPerChunk
            << ",\"redraw_iterations\":" << redrawCount << ",\"theme_iterations\":" << themeCount
            << ",\"idle_requested_ms\":1500,\"idle_seconds\":" << idleSeconds << ",\"log_validated\":true},\n  \"metrics\":[\n";
        for (std::size_t i = 0; i < metrics.size(); ++i) {
            const auto& metric = metrics[i];
            result << "    {\"name\":" << quoted(metric.name) << ",\"unit\":" << quoted(metric.unit)
                << ",\"value\":" << metric.value;
            if (!metric.samples.empty()) {
                result << ",\"samples\":[";
                for (std::size_t j = 0; j < metric.samples.size(); ++j)
                    result << (j ? "," : "") << metric.samples[j];
                result << ']';
            }
            result << '}' << (i + 1 < metrics.size() ? "," : "") << '\n';
        }
        result << "  ]\n}\n"; result.close();
        if (!result) throw std::runtime_error("Could not write benchmark result");
        std::cout << Adapter::name << " benchmark passed workload validation\n";
        return 0;
    } catch (const std::exception& error) {
        if (initialized) Adapter::shutdown();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
} // namespace cbbench
