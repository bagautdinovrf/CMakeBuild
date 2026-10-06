#pragma once
#include "controller.hpp"
#include "platform.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace harness {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

void require(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << message << L'\n';
        throw std::runtime_error("run UI assertion failed");
    }
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), count, nullptr, nullptr);
    return result;
}

std::string quoted(const std::wstring& value) {
    std::string result = "\"";
    for (const char ch : utf8(value)) {
        if (ch == '\\' || ch == '"') result += '\\';
        result += ch;
    }
    return result + '"';
}

void writeFile(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(value.data(), static_cast<std::streamsize>(value.size()));
    require(file.good(), L"Cannot write test fixture");
}

fs::path currentExecutable() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    require(length != 0 && length < buffer.size(), L"Cannot locate harness executable");
    return fs::path(std::wstring(buffer.data(), length));
}

fs::path marker(const cb::Target& target) {
    const fs::path executable(target.executable);
    return executable.parent_path() / (executable.stem().wstring() + L".launched");
}

struct TempDirectory {
    fs::path root = fs::temp_directory_path() / (L"CMakeBuild-nana-tests-"
        + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    TempDirectory() { fs::create_directories(root); }
    ~TempDirectory() {
        std::error_code error;
        fs::remove_all(root, error);
        if (error) std::wcerr << L"Could not remove test fixtures: " << root.wstring() << L'\n';
    }
};

struct ProjectFixture {
    fs::path project, build;
    std::wstring buildDirectory, configuration;
    std::vector<cb::Target> targets;

    ProjectFixture(const fs::path& root, const wchar_t* folder, const wchar_t* directory,
        const wchar_t* config, const wchar_t* first, const wchar_t* second,
        const wchar_t* firstExecutable, const wchar_t* secondExecutable)
        : project(root / folder / L"CMakeLists.txt"), build(project.parent_path() / directory),
          buildDirectory(directory), configuration(config) {
        writeFile(project, "cmake_minimum_required(VERSION 3.24)\nproject(RunFixture LANGUAGES CXX)\n");
        for (const auto& [name, executable] : std::array<std::pair<const wchar_t*, const wchar_t*>, 2>{
                 std::pair{first, firstExecutable}, std::pair{second, secondExecutable}}) {
            const auto artifact = build / L"bin" / executable;
            fs::create_directories(artifact.parent_path());
            fs::copy_file(currentExecutable(), artifact);
            targets.push_back({name, artifact.wstring()});
        }
        writeReplies(false);
    }

    cb::BuildSettings settings() const {
        cb::BuildSettings result;
        result.cmakeFile = project.wstring();
        result.buildDirectory = buildDirectory;
        result.configuration = configuration;
        result.target = targets.front().name;
        return result;
    }

    void writeReplies(bool reversed, const fs::path& source = {}) const {
        const fs::path reply = build / L".cmake/api/v1/reply";
        writeFile(reply / L"index-2026-10-05.json",
            R"({"reply":{"client-cmakebuild":{"codemodel-v2":{"jsonFile":"codemodel.json"}}}})");
        std::vector<std::string> references;
        for (size_t i = 0; i < targets.size(); ++i) {
            const std::wstring filename = L"target-" + std::to_wstring(i) + L".json";
            writeFile(reply / filename, "{\"name\":" + quoted(targets[i].name)
                + ",\"type\":\"EXECUTABLE\",\"artifacts\":[{\"path\":"
                + quoted(fs::relative(fs::path(targets[i].executable), build).generic_wstring()) + "}]}");
            references.push_back("{\"jsonFile\":" + quoted(filename) + "}");
        }
        if (reversed) std::ranges::reverse(references);
        writeFile(reply / L"unbuilt.json",
            R"({"name":"Unbuilt","type":"EXECUTABLE","artifacts":[{"path":"bin/not-built.exe"}]})");
        writeFile(reply / L"library.json",
            R"({"name":"Library","type":"STATIC_LIBRARY","artifacts":[{"path":"bin/helper.lib"}]})");
        writeFile(reply / L"symbols.json",
            R"({"name":"Symbols","type":"EXECUTABLE","artifacts":[{"path":"bin/symbols.pdb"}]})");
        writeFile(build / L"bin/helper.lib", "fixture");
        writeFile(build / L"bin/symbols.pdb", "fixture");
        std::string targetReferences;
        for (const auto& reference : references) {
            if (!targetReferences.empty()) targetReferences += ',';
            targetReferences += reference;
        }
        targetReferences += R"(,{"jsonFile":"unbuilt.json"},{"jsonFile":"library.json"},{"jsonFile":"symbols.json"})";
        writeFile(reply / L"codemodel.json", "{\"paths\":{\"source\":"
            + quoted((source.empty() ? project.parent_path() : source).generic_wstring())
            + ",\"build\":" + quoted(build.generic_wstring())
            + "},\"configurations\":[{\"name\":" + quoted(configuration)
            + ",\"targets\":[" + targetReferences + "]}]}");
    }
};


}
