#include "platform.hpp"

#include <nana/gui/programming_interface.hpp>
#include <nana/gui/widgets/form.hpp>
#include <nana/gui/widgets/textbox.hpp>
#include <nana/gui/widgets/skeletons/text_editor.hpp>
#include <nana/paint/graphics.hpp>
#include <nana/paint/pixel_buffer.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
class TestTextbox final : public nana::textbox {
public:
    using nana::textbox::textbox;
    using nana::textbox::get_drawer_trigger;
};

void expect(bool condition, const std::string& context) {
    if (!condition) throw std::runtime_error(context);
}

std::wstring normalized(std::wstring text) {
    std::erase(text, L'\r');
    return text;
}

std::size_t offset(const std::wstring& text, nana::upoint position) {
    std::size_t result{};
    for (unsigned row = 0; row < position.y; ++row) {
        const auto newline = text.find(L'\n', result);
        expect(newline != std::wstring::npos, "Invalid test line coordinate");
        result = newline + 1;
    }
    const auto end = text.find(L'\n', result);
    const auto length = (end == std::wstring::npos ? text.size() : end) - result;
    expect(position.x <= length, "Invalid test UTF-16 character coordinate");
    return result + position.x;
}

auto& editor(TestTextbox& box) {
    auto* result = box.get_drawer_trigger().editor();
    expect(result != nullptr, "Real textbox editor was not constructed");
    return *result;
}

void refresh(TestTextbox& box) {
    editor(box).try_refresh();
    nana::api::refresh_window(box);
    cb::platform::drainPreviewMessages();
}

class Fixture {
    // The form must be constructed first and outlive both widgets.
    nana::form form_;

public:
    Fixture()
        : form_{nana::rectangle{-32000, -32000, 1000, 400},
                nana::appearance{false, false, false, true, false, false, false}},
          incremental{form_, nana::rectangle{0, 0, 320, 160}},
          snapshot{form_, nana::rectangle{500, 0, 320, 160}} {
        cb::platform::preparePreviewWindow(form_.native_handle());
        for (auto* box : {&incremental, &snapshot}) {
            box->multi_lines(true);
            box->line_wrapped(false);
            box->editable(true);
            box->enable_border_focused(false);
            box->set_undo_queue_length(64);
            box->typeface(nana::paint::font{"Consolas", 9, {}, 96});
        }
    }

    void replace(std::wstring_view text) {
        incremental.caption(std::wstring{text});
        snapshot.caption(std::wstring{text});
        incremental.select(false);
        snapshot.select(false);
        refresh(incremental);
        refresh(snapshot);
    }

    // A fresh caption builds all sections again. Compare observable content,
    // scrollbar range, exposed line anchors and actual text-area pixels with
    // the editor whose sections were updated through incremental mutations.
    void check(const std::wstring& expected, const std::string& context) {
        snapshot.caption(expected);
        for (auto* box : {&incremental, &snapshot}) {
            box->select(false);
            box->caret_pos({0, 0}, false);
            editor(*box).restore_content_origin({0, 0});
            refresh(*box);
        }
        expect(normalized(incremental.caption_wstring()) == expected, context + ": incremental text differs");
        expect(normalized(snapshot.caption_wstring()) == expected, context + ": snapshot text differs");
        const auto lines = static_cast<std::size_t>(std::ranges::count(expected, L'\n')) + 1;
        expect(incremental.text_line_count() == lines && snapshot.text_line_count() == lines,
            context + ": logical line count differs");
        expect(incremental.display_line_count() == snapshot.display_line_count(), context + ": display line count differs");
        const auto firstScrolls = incremental.scroll_operation();
        const auto secondScrolls = snapshot.scroll_operation();
        expect(firstScrolls && secondScrolls, context + ": scrollbar interface is unavailable");
        expect(firstScrolls->visible(false) == secondScrolls->visible(false)
            && firstScrolls->visible(true) == secondScrolls->visible(true), context + ": scrollbar visibility differs");
        checkView({0, 0}, context + " top");
        checkView({96, 5 * static_cast<int>(incremental.line_pixels())}, context + " middle");
        checkView({1000000, 1000000}, context + " scroll limits");
        editor(incremental).restore_content_origin({0, 0});
        editor(snapshot).restore_content_origin({0, 0});
    }

    void resize(unsigned width, unsigned height) {
        incremental.size({width, height});
        snapshot.size({width, height});
    }

    TestTextbox incremental, snapshot;

private:
    void checkView(nana::point requested, const std::string& context) {
        for (auto* box : {&incremental, &snapshot}) {
            editor(*box).restore_content_origin(requested);
            refresh(*box);
        }
        expect(incremental.content_origin() == snapshot.content_origin(), context + ": clamped viewport differs");
        expect(incremental.text_position() == snapshot.text_position(), context + ": exposed UTF-16 line anchors differ");
        // Nana's textbox::text_area() delegates to text_area(false), whose
        // rectangle includes scrollbar chrome. The editor's true overload
        // returns the content viewport, so RGB assertions cover glyphs only;
        // scrollbar visibility and clamped ranges are checked independently.
        const auto firstArea = editor(incremental).text_area(true), secondArea = editor(snapshot).text_area(true);
        expect(firstArea == secondArea, context + ": text area differs");
        nana::paint::graphics firstGraphics, secondGraphics;
        expect(nana::api::window_graphics(incremental, firstGraphics)
            && nana::api::window_graphics(snapshot, secondGraphics), context + ": real drawer snapshot failed");
        nana::paint::pixel_buffer firstPixels{firstGraphics.handle(), firstArea};
        nana::paint::pixel_buffer secondPixels{secondGraphics.handle(), secondArea};
        expect(firstPixels.size() == firstArea.dimension() && secondPixels.size() == secondArea.dimension(),
            context + ": real text-area pixels are unavailable");
        for (unsigned row = 0; row < firstArea.height; ++row) {
            const auto* first = firstPixels.raw_ptr(row);
            const auto* second = secondPixels.raw_ptr(row);
            if (!std::equal(first, first + firstArea.width, second, [](const auto& left, const auto& right) {
                return (left.value & 0xffffffu) == (right.value & 0xffffffu);
            })) {
                reportPixelDifference(firstPixels, secondPixels, firstArea, context);
            }
        }
    }

    [[noreturn]] void reportPixelDifference(const nana::paint::pixel_buffer& firstPixels,
        const nana::paint::pixel_buffer& secondPixels, const nana::rectangle& area, const std::string& context) {
        unsigned minX = std::numeric_limits<unsigned>::max(), minY = minX, maxX{}, maxY{};
        unsigned firstX{}, firstY{}, firstColor{}, secondColor{};
        std::size_t changed{};
        for (unsigned row = 0; row < area.height; ++row) {
            const auto* first = firstPixels.raw_ptr(row);
            const auto* second = secondPixels.raw_ptr(row);
            for (unsigned column = 0; column < area.width; ++column) {
                const auto left = first[column].value & 0xffffffu, right = second[column].value & 0xffffffu;
                if (left == right) continue;
                if (!changed) { firstX = column; firstY = row; firstColor = left; secondColor = right; }
                ++changed;
                minX = std::min(minX, column); minY = std::min(minY, row);
                maxX = std::max(maxX, column); maxY = std::max(maxY, row);
            }
        }
        const auto directory = std::filesystem::current_path() / L"editor-diagnostics";
        std::filesystem::create_directories(directory);
        expect(firstPixels.save(directory / L"incremental.bmp") && secondPixels.save(directory / L"snapshot.bmp"),
            "Could not save mismatched real editor text-area graphics");
        const auto saveText = [&](const wchar_t* filename, const std::wstring& text) {
            std::ofstream output{directory / filename, std::ios::binary};
            output << cb::platform::utf8(text);
            expect(output.good(), "Could not save mismatched editor text");
        };
        saveText(L"incremental.txt", incremental.caption_wstring());
        saveText(L"snapshot.txt", snapshot.caption_wstring());
        const auto origin = incremental.content_origin();
        const auto font = incremental.typeface();
        std::ostringstream details;
        details << context << ": rendered text differs from full-caption oracle. First local pixel=("
            << firstX << ',' << firstY << "), widget pixel=(" << area.x + static_cast<int>(firstX)
            << ',' << area.y + static_cast<int>(firstY) << "), RGB incremental=0x" << std::hex << firstColor
            << " snapshot=0x" << secondColor << std::dec << ", changed=" << changed
            << ", bounding box=(" << minX << ',' << minY << ")..(" << maxX << ',' << maxY << ")"
            << ", origin=(" << origin.x << ',' << origin.y << "), line_pixels=" << incremental.line_pixels()
            << ", text_area=(" << area.x << ',' << area.y << ',' << area.width << ',' << area.height << ")"
            << ", font=" << font.name() << '/' << font.size() << ", caret=(" << incremental.caret_pos().x
            << ',' << incremental.caret_pos().y << "), selected=" << incremental.selected()
            << ", diagnostics=" << cb::platform::utf8(directory.wstring());
        std::ofstream output{directory / L"difference.txt", std::ios::binary};
        output << details.str() << '\n';
        output << "exposed anchors:";
        for (const auto anchor : incremental.text_position()) output << " (" << anchor.x << ',' << anchor.y << ')';
        output << '\n';
        throw std::runtime_error(details.str());
    }
};

std::wstring manyLines(unsigned count) {
    std::wstring result;
    for (unsigned line = 0; line < count; ++line) {
        result += L"row " + std::to_wstring(line) + L" Привет 😀 العربية 中文 ";
        result.append(12 + line % 29, L'x');
        result += L'\n';
    }
    return result;
}

void appendBatches() {
    Fixture fixture;
    fixture.incremental.set_undo_queue_length(0);
    std::wstring expected;
    const std::array<std::wstring, 5> prefix{
        L"Пер", L"вая 😀 строка العربية", L"\r\n", L"中文 second line\nthird", L" line\n"};
    for (const auto& packet : prefix) {
        fixture.incremental.append(packet, false);
        expected += normalized(packet);
        fixture.check(expected, "Unicode split batch");
    }
    for (unsigned chunk = 0; chunk < 48; ++chunk) {
        auto packet = L"chunk " + std::to_wstring(chunk) + L" 😀 العربية\n";
        packet += std::wstring(chunk == 0 ? 2048 : 10 + chunk % 40, L'a');
        packet += L"\nРусский " + std::to_wstring(chunk) + L"\n";
        fixture.incremental.append(packet, false);
        expected += packet;
        if (chunk % 8 == 0 || chunk == 47) fixture.check(expected, "Growing multiline append " + std::to_string(chunk));
    }
    fixture.incremental.append(std::wstring(3072, L'b'), false);
    expected.append(3072, L'b');
    fixture.check(expected, "Later longest line");
    fixture.incremental.append(L"!", false);
    expected += L'!';
    fixture.check(expected, "Single-character append at line end");
    expect(fixture.incremental.scroll_operation()->visible(false), "Long lines did not create horizontal scrolling");

    // Remove the later maximum, then the earlier one; old cached extents must
    // neither remain large nor disappear while another long line is retained.
    const auto last = static_cast<unsigned>(fixture.incremental.text_line_count() - 1);
    fixture.incremental.select_points({0, last}, {3073, last});
    fixture.incremental.del();
    expected.erase(expected.find_last_of(L'\n') + 1);
    fixture.check(expected, "Erase later longest line");
    expect(fixture.incremental.scroll_operation()->visible(false), "Earlier long line lost its horizontal scrollbar");
    fixture.incremental.select_points({0, 4}, {2048, 4});
    fixture.incremental.del();
    expected.erase(offset(expected, {0, 4}), 2048);
    fixture.check(expected, "Erase earlier longest line");
    fixture.replace(L"short\ntext");
    fixture.check(L"short\ntext", "Trim/reset to short retained tail");
    expect(!fixture.incremental.scroll_operation()->visible(false), "Reset retained stale horizontal extent");
    fixture.replace(L"");
    fixture.check(L"", "Clear editor");
}

void middleEdits() {
    Fixture fixture;
    // Keep an unambiguous longest line before every edited row. Nana's
    // existing textbase maximum-line index can drift when lines are inserted
    // before that index; this fixture targets incremental sections/undo while
    // preserving the same maximum-line semantics as the full-caption oracle.
    auto expected = std::wstring(2048, L'm') + L'\n' + manyLines(36);
    fixture.replace(expected);
    const auto initial = expected;
    const nana::upoint insertion{4, 12};
    const std::wstring packet = L"中文 😀\nالعربية inserted\nUnicode tail";
    fixture.incremental.caret_pos(insertion, false);
    fixture.incremental.append(packet, true);
    expected.insert(offset(expected, insertion), packet);
    fixture.check(expected, "Multiline middle insertion");
    editor(fixture.incremental).undo(false);
    fixture.check(initial, "Undo multiline insertion");
    editor(fixture.incremental).undo(true);
    fixture.check(expected, "Redo multiline insertion");

    const auto beforeReplacement = expected;
    const nana::upoint begin{2, 5}, end{4, 9};
    fixture.incremental.select_points(begin, end);
    const std::wstring replacement = L"замена 😀\nالعربية\nend";
    fixture.incremental.append(replacement, true);
    const auto first = offset(expected, begin), last = offset(expected, end);
    expected.replace(first, last - first, replacement);
    fixture.check(expected, "Replace multiline selection");
    editor(fixture.incremental).undo(false);
    fixture.check(beforeReplacement, "Undo selection replacement");
    editor(fixture.incremental).undo(true);
    fixture.check(expected, "Redo selection replacement");

    const auto beforeDelete = expected;
    fixture.incremental.select_points({1, 3}, {6, 7});
    fixture.incremental.del();
    const auto eraseBegin = offset(expected, {1, 3}), eraseEnd = offset(expected, {6, 7});
    expected.erase(eraseBegin, eraseEnd - eraseBegin);
    fixture.check(expected, "Delete across line boundaries");
    editor(fixture.incremental).undo(false);
    fixture.check(beforeDelete, "Undo multiline deletion");
    editor(fixture.incremental).undo(true);
    fixture.check(expected, "Redo multiline deletion");

    const auto beforeJoin = expected;
    const nana::upoint join{0, 11};
    fixture.incremental.caret_pos(join, false);
    editor(fixture.incremental).backspace(true, true);
    expected.erase(offset(expected, join) - 1, 1);
    fixture.check(expected, "Backspace joins adjacent lines");
    editor(fixture.incremental).undo(false);
    fixture.check(beforeJoin, "Undo line join");
    editor(fixture.incremental).undo(true);
    fixture.check(expected, "Redo line join");
}

void geometryAndFont() {
    Fixture fixture;
    auto expected = manyLines(60) + std::wstring(2048, L'w');
    fixture.replace(expected);
    for (const auto dimensions : std::array<nana::size, 4>{{{180, 100}, {420, 220}, {256, 140}, {320, 160}}}) {
        fixture.resize(dimensions.width, dimensions.height);
        fixture.check(expected, "Resize invalidates display sections");
    }
    for (const unsigned dpi : {96u, 120u, 144u, 192u, 96u}) {
        for (auto* box : {&fixture.incremental, &fixture.snapshot}) {
            box->typeface(nana::paint::font{"Consolas", 9, {}, dpi});
        }
        fixture.incremental.append(L"\nfont DPI 😀 العربية", false);
        expected += L"\nfont DPI 😀 العربية";
        fixture.check(expected, "Font/DPI metrics then append " + std::to_string(dpi));
    }
    for (const bool wrapped : {true, false, true, false}) {
        fixture.incremental.line_wrapped(wrapped);
        fixture.snapshot.line_wrapped(wrapped);
        fixture.incremental.append(L"\nwrapping 中文 😀 العربية", false);
        expected += L"\nwrapping 中文 😀 العربية";
        fixture.check(expected, wrapped ? "Wrapped append keeps full recalculation" : "Unwrapped append after wrapping");
        if (wrapped) expect(!fixture.incremental.scroll_operation()->visible(false), "Wrapped text retained horizontal scrolling");
    }
}

void retainedSelectionAndViewport() {
    Fixture fixture;
    auto expected = manyLines(60) + std::wstring(2048, L'x');
    fixture.replace(expected);
    auto& box = fixture.incremental;
    box.caret_pos({8, 12}, false);
    box.select_points({13, 8}, {18, 12});
    editor(box).restore_content_origin({96, static_cast<int>(5 * box.line_pixels())});
    refresh(box);
    const auto selection = box.selection();
    const auto caret = box.caret_pos();
    const auto origin = box.content_origin();
    const auto selectedText = normalized(editor(box).make_select_string());
    expect(!selectedText.empty() && selectedText.find(L"😀") != std::wstring::npos,
        "Selection fixture must cover non-BMP Unicode");
    for (unsigned batch = 0; batch < 12; ++batch) {
        const std::wstring packet = L"\nretained view 😀 العربية " + std::to_wstring(batch);
        // This is the transaction used by JournalBox: raw textbox append may
        // replace a selection, so release it and restore the user's anchors.
        box.select(false);
        box.append(packet, false);
        box.caret_pos(caret, false);
        box.select_points(selection.first, selection.second);
        editor(box).restore_content_origin(origin);
        refresh(box);
        expected += packet;
        expect(normalized(box.caption_wstring()) == expected, "Append with restored selection lost text");
        expect(box.selection() == selection, "Append restoration moved UTF-16 selection anchors");
        expect(normalized(editor(box).make_select_string()) == selectedText, "Append restoration changed selected Unicode text");
        expect(box.caret_pos() == caret, "Append restoration moved caret");
        expect(box.content_origin() == origin, "Append restoration moved horizontal/vertical viewport");
    }
    fixture.check(expected, "Selection-preserving append content and drawing");
}
} // namespace

int wmain() {
    cb::platform::initialize();
    try {
        appendBatches();
        middleEdits();
        geometryAndFont();
        retainedSelectionAndViewport();
        cb::platform::shutdown();
        std::cout << "Nana incremental editor correctness passed (text, undo/redo, scroll extents, rendering, resize/font/DPI/wrap)\n";
        return 0;
    } catch (const std::exception& error) {
        cb::platform::shutdown();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
