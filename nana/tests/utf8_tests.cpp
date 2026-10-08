#include "platform.hpp"

#include <nana/deploy.hpp>
#include <nana/paint/graphics.hpp>

#include <windows.h>

#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool windowsAccepts(std::string_view text) {
    return text.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0) != 0;
}

std::string hexBytes(std::string_view text) {
    std::ostringstream output;
    for (const unsigned char byte : text)
        output << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte) << ' ';
    return output.str();
}

void agreesWithWindows(std::string_view text) {
    if (nana::is_utf8(text) != windowsAccepts(text))
        throw std::runtime_error("UTF-8 validation differs from Win32 for bytes: " + hexBytes(text));
}

std::wstring scalarUtf16(std::uint32_t scalar) {
    if (scalar <= 0xFFFF) return std::wstring(1, static_cast<wchar_t>(scalar));
    scalar -= 0x10000;
    return {static_cast<wchar_t>(0xD800 + (scalar >> 10)),
        static_cast<wchar_t>(0xDC00 + (scalar & 0x3FF))};
}

constexpr std::array scalarBoundaries{
    0u, 1u, 0x7Eu, 0x7Fu, 0x80u, 0x81u, 0x7FEu, 0x7FFu,
    0x800u, 0x801u, 0xD7FEu, 0xD7FFu, 0xE000u, 0xE001u, 0xFFFEu, 0xFFFFu,
    0x10000u, 0x10001u, 0x1F600u, 0x3FFFFu, 0x40000u, 0xFFFFFu, 0x100000u, 0x10FFFEu, 0x10FFFFu
};

void validatorBoundaries() {
    // A default string_view may have a null data pointer.
    expect(nana::is_utf8(std::string_view{}), "Empty UTF-8 was rejected");
    expect(nana::is_utf8(std::string_view{"A\0B", 3}), "Embedded NUL was rejected");
    expect(!nana::is_utf8(std::string_view{"A\0\xFF", 3}), "Validation stopped at embedded NUL");
    const std::array invalid{
        std::string_view{"\x80"}, std::string_view{"\xBF"},
        std::string_view{"\xC0\x80"}, std::string_view{"\xC1\xBF"},
        std::string_view{"\xC2"}, std::string_view{"\xC2\x7F"}, std::string_view{"\xC2\xC0"},
        std::string_view{"\xE0\x9F\xBF"}, std::string_view{"\xED\xA0\x80"},
        std::string_view{"\xED\xBF\xBF"}, std::string_view{"\xE1\x80"},
        std::string_view{"\xE1\x80\x7F"}, std::string_view{"\xF0\x8F\xBF\xBF"},
        std::string_view{"\xF4\x90\x80\x80"}, std::string_view{"\xF5\x80\x80\x80"},
        std::string_view{"\xF0\x90\x80"}, std::string_view{"\xF0\x90\x80\xC0"},
        std::string_view{"\xFE"}, std::string_view{"\xFF"}
    };
    for (const auto bytes : invalid) {
        expect(!nana::is_utf8(bytes), "Invalid UTF-8 boundary was accepted");
        agreesWithWindows(bytes);
    }

    // Exhaustive one/two-byte inputs independently cover all lead and
    // continuation-byte combinations, including ASCII and embedded NUL.
    std::array<char, 2> pair{};
    for (unsigned first = 0; first < 256; ++first) {
        pair[0] = static_cast<char>(first);
        agreesWithWindows(std::string_view{pair.data(), 1});
        for (unsigned second = 0; second < 256; ++second) {
            pair[1] = static_cast<char>(second);
            agreesWithWindows(std::string_view{pair.data(), pair.size()});
        }
    }

    for (const auto scalar : scalarBoundaries) {
        const auto valid = cb::platform::utf8(scalarUtf16(scalar));
        expect(nana::is_utf8(valid), "Valid Unicode scalar boundary was rejected");
        agreesWithWindows(valid);
        for (std::size_t length = 0; length <= valid.size(); ++length)
            agreesWithWindows(std::string_view{valid.data(), length});
        for (std::size_t index = 0; index < valid.size(); ++index) {
            auto mutation = valid;
            for (unsigned byte = 0; byte < 256; ++byte) {
                mutation[index] = static_cast<char>(byte);
                agreesWithWindows(mutation);
                agreesWithWindows(std::string{"prefix\0", 7} + mutation + "suffix");
            }
        }
    }
}

struct ThrowUtf8Errors {
    bool previous = nana::utf8_Error::use_throw;
    ThrowUtf8Errors() { nana::utf8_Error::use_throw = true; }
    ~ThrowUtf8Errors() { nana::utf8_Error::use_throw = previous; }
};

void supplementaryText() {
    ThrowUtf8Errors errors;
    const std::u8string_view menuText = u8"Длинное название — 😀 и полный текст после emoji WWWWWW";
    const std::string text{reinterpret_cast<const char*>(menuText.data()), menuText.size()};
    nana::throw_not_utf8(text);
    const auto wide = cb::platform::utf16(text);
    expect(nana::to_wstring(std::string_view{text}) == wide, "UTF-8 conversion lost the emoji or its suffix");
    expect(nana::to_wstring(menuText) == wide, "char8_t conversion lost the emoji or its suffix");
    expect(nana::to_utf8(std::wstring_view{wide}) == text, "UTF-16 supplementary roundtrip changed the text");

    for (const auto scalar : scalarBoundaries) {
        const auto expected = std::wstring{L"prefix\0", 7} + scalarUtf16(scalar) + L"suffix";
        const auto encoded = cb::platform::utf8(expected);
        expect(nana::to_wstring(std::string_view{encoded}) == expected, "Unicode scalar decode differs from Win32");
        expect(nana::to_utf8(std::wstring_view{expected}) == encoded, "Unicode scalar encode differs from Win32");
    }

    nana::paint::graphics graph{nana::size{1, 1}};
    graph.typeface(nana::paint::font{"Segoe UI", 10, {}, 96});
    const auto narrowExtent = graph.text_extent_size(std::string_view{text});
    const auto wideExtent = graph.text_extent_size(std::wstring_view{wide});
    expect(narrowExtent == wideExtent, "Menu UTF-8 measurement differs from full UTF-16 caption");
    const auto prefix = wide.substr(0, wide.find(L" и полный"));
    expect(wideExtent.width > graph.text_extent_size(std::wstring_view{prefix}).width,
        "Caption measurement did not include the suffix after emoji");

    bool rejected{};
    try { nana::throw_not_utf8("\xF4\x90\x80\x80"); }
    catch (const nana::utf8_Error&) { rejected = true; }
    expect(rejected, "Malformed UTF-8 did not follow the configured error policy");
}
} // namespace

int wmain() {
    cb::platform::initialize();
    int result{};
    try {
        validatorBoundaries();
        supplementaryText();
        std::cout << "Nana UTF-8 passed (Win32 boundary oracle, supplementary roundtrip and full text metrics)\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    cb::platform::shutdown();
    return result;
}
