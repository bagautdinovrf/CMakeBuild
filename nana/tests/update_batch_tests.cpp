#include "platform.hpp"

#include <nana/gui/programming_interface.hpp>
#include <nana/gui/widgets/button.hpp>
#include <nana/gui/widgets/form.hpp>
#include <nana/gui/widgets/scroll.hpp>
#include <nana/gui/widgets/skeletons/text_editor.hpp>
#include <nana/gui/widgets/textbox.hpp>
#include <nana/paint/pixel_buffer.hpp>
#include <windows.h>

#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class TestTextbox final : public nana::textbox {
public:
    using nana::textbox::textbox;
    using nana::textbox::get_drawer_trigger;
};

auto& editor(TestTextbox& box) {
    auto* result = box.get_drawer_trigger().editor();
    expect(result != nullptr, "Real textbox editor was not constructed");
    return *result;
}

class DrawingProbe {
public:
    DrawingProbe(nana::window window, std::function<void(nana::paint::graphics&)> callback)
        : window_{window}, handle_{nana::api::drawing(window, std::move(callback))} {}
    ~DrawingProbe() { nana::api::remove_drawing(window_, handle_); }
    DrawingProbe(const DrawingProbe&) = delete;
    DrawingProbe& operator=(const DrawingProbe&) = delete;

private:
    nana::window window_;
    nana::drawing_handle handle_;
};

class FontDrawer final : public nana::drawer_trigger {
public:
    unsigned changes{}, draws{};
    void typeface_changed(graph_reference) override { ++changes; }
    void refresh(graph_reference graph) override {
        ++draws;
        graph.rectangle(true, nana::colors::white);
        graph.string({2, 2}, "Font", nana::colors::black);
    }
};

class FontProbe final : public nana::widget_object<nana::category::widget_tag, FontDrawer> {
public:
    using nana::widget_object<nana::category::widget_tag, FontDrawer>::get_drawer_trigger;
};

std::uint64_t pixelHash(nana::paint::graphics& graph, const nana::rectangle& area) {
    nana::paint::pixel_buffer pixels{graph.handle(), area};
    expect(pixels.size() == area.dimension(), "Could not read the rendered pixels");
    std::uint64_t hash{14695981039346656037ull};
    for (unsigned y = 0; y < pixels.size().height; ++y) {
        const auto* row = pixels.raw_ptr(y);
        for (unsigned x = 0; x < pixels.size().width; ++x) {
            // The unused alpha byte of Windows drawer bitmaps is unspecified.
            hash ^= row[x].value & 0xffffffu;
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

std::uint64_t rootHash(nana::window window) {
    nana::paint::graphics graph;
    expect(nana::api::root_graphics(window, graph) && !graph.empty(),
        "The shown root has no composed graphics");
    return pixelHash(graph, nana::rectangle{graph.size()});
}

std::uint64_t textHash(TestTextbox& box) {
    nana::paint::graphics graph;
    expect(nana::api::window_graphics(box, graph), "The textbox has no rendered graphics");
    return pixelHash(graph, editor(box).text_area(true));
}

void expectScrollbars(TestTextbox& box) {
    const auto origin = box.content_origin();
    unsigned found{};
    nana::api::enum_widgets<nana::scroll<true>>(box, false, [&](auto& scrollbar) {
        ++found;
        expect(scrollbar.value() == static_cast<std::size_t>(origin.y),
            "Batched restore left the vertical thumb at another position");
    });
    nana::api::enum_widgets<nana::scroll<false>>(box, false, [&](auto& scrollbar) {
        ++found;
        expect(scrollbar.value() == static_cast<std::size_t>(origin.x),
            "Batched restore left the horizontal thumb at another position");
    });
    expect(found == 2, "The fixture did not create both real scrollbars");
}

class Fixture {
public:
    nana::form form{nana::rectangle{-32000, -32000, 480, 220},
        nana::appearance{false, false, false, true, false, false, false}};
    TestTextbox box{form, nana::rectangle{8, 8, 330, 195}};
    nana::button sibling{form, nana::rectangle{350, 12, 115, 35}};

    Fixture() {
        cb::platform::preparePreviewWindow(form.native_handle());
        box.multi_lines(true);
        box.line_wrapped(false);
        box.enable_border_focused(false);
        box.typeface(nana::paint::font{"Consolas", 9, {}, 96});
        std::wstring text;
        for (unsigned row = 0; row != 100; ++row)
            text += L"Line " + std::to_wstring(row) + L": " + std::wstring(100, L'W') + L"\n";
        box.caption(text);
        sibling.caption("Initial");
        form.show();
        // STARTUPINFO can override the first native ShowWindow call. Ensure
        // both Nana and Windows regard this offscreen fixture as shown.
        ShowWindow(native(), SW_SHOWNOACTIVATE);
        cb::platform::drainPreviewMessages();
        box.caret_pos({0, static_cast<unsigned>(box.text_line_count() - 1)});
        editor(box).restore_content_origin({70, 12 * static_cast<int>(box.line_pixels())});
        nana::api::refresh_window_tree(form);
        cb::platform::drainPreviewMessages();
        expect(IsWindowVisible(native()) != FALSE, "Batch fixture is not really shown");
        expect(nana::api::visible(form), "Nana does not regard the fixture as shown");
        expect((GetWindowLongPtrW(native(), GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0,
            "Batch fixture may activate a user window");
        RECT bounds{};
        expect(GetWindowRect(native(), &bounds) != FALSE && bounds.right < -10000,
            "Batch fixture is not offscreen");
        expectScrollbars(box);
    }

    HWND native() const { return reinterpret_cast<HWND>(form.native_handle()); }
};

void nestedUpdates(Fixture& fixture) {
    auto& box = fixture.box;
    const auto origin = box.content_origin();
    const auto before = rootHash(fixture.form);
    const auto thread = GetCurrentThreadId();
    unsigned calls{};
    constexpr UINT sentinel = WM_APP + 147;
    expect(PostMessageW(fixture.native(), sentinel, 0, 0) != FALSE,
        "Could not post the no-message-pump sentinel");
    nana::api::batch_updates(box, [&] {
        ++calls;
        expect(GetCurrentThreadId() == thread, "Batch callback changed GUI threads");
        box.append(L"First appended entry\n", false);
        expect(rootHash(fixture.form) == before, "Append composed an intermediate root frame");
        editor(box).restore_content_origin(origin);
        expect(rootHash(fixture.form) == before, "Scrollbar restore composed an intermediate root frame");
        fixture.sibling.caption("Outer");
        nana::api::refresh_window(fixture.sibling);
        expect(rootHash(fixture.form) == before, "Sibling redraw escaped the root batch");

        nana::api::batch_updates(fixture.sibling, [&] {
            ++calls;
            box.append(L"Nested appended entry\n", false);
            editor(box).restore_content_origin(origin);
            fixture.sibling.caption("Final batch");
            nana::api::refresh_window(fixture.sibling);
            expect(rootHash(fixture.form) == before, "Nested batch composed an intermediate root frame");
        });
        expect(rootHash(fixture.form) == before, "Inner batch flushed the still-active outer batch");
        expect(box.content_origin() == origin, "Batch changed the reading position");
        expectScrollbars(box);
    });
    expect(calls == 2, "Batch callbacks were not synchronous and exactly once");
    MSG queued{};
    expect(PeekMessageW(&queued, fixture.native(), sentinel, sentinel, PM_REMOVE) != FALSE,
        "Batch entered a nested message loop");
    const auto after = rootHash(fixture.form);
    expect(after != before, "Outer batch did not compose its final changes before returning");
    // A normal complete repaint must produce the same frame: this checks that
    // flushing included the textbox, its nested scrollbars and its sibling.
    nana::api::refresh_window_tree(fixture.form);
    expect(rootHash(fixture.form) == after, "Batch flush omitted part of the final root frame");
    expectScrollbars(box);
}

struct ExpectedFailure { const void* identity; };

void synchronousNativeEventInBatch(Fixture& fixture) {
    nana::button source{fixture.form, nana::rectangle{350, 65, 115, 30}};
    nana::button pending{fixture.form, nana::rectangle{350, 110, 115, 30}};
    pending.transparent(true);
    source.caption("Event source");
    pending.caption("Before batch");
    nana::api::refresh_window_tree(fixture.form);
    // Establish hover before measuring. Only this offscreen test window
    // receives the synchronous message; no physical input is generated.
    SendMessageW(fixture.native(), WM_MOUSEMOVE, 0, MAKELPARAM(360, 75));
    const auto before = rootHash(fixture.form);
    unsigned draws{}, events{};
    DrawingProbe drawing{pending, [&](nana::paint::graphics&) { ++draws; }};
    source.events().mouse_move([&] {
        ++events;
        pending.caption("Inside event");
        nana::api::refresh_window(pending);
    });
    nana::api::batch_updates(fixture.form, [&] {
        pending.caption("Before event");
        nana::api::refresh_window(pending);
        expect(draws == 0 && rootHash(fixture.form) == before,
            "Pending transparent requester escaped the outer batch");
        SendMessageW(fixture.native(), WM_MOUSEMOVE, 0, MAKELPARAM(361, 75));
        expect(events == 1, "Synchronous native event did not invoke its callback exactly once");
        expect(draws == 0 && rootHash(fixture.form) == before,
            "Synchronous native event flushed a still-active batch");
        // Exiting root_guard must preserve the outer batch's lazy state.
        pending.caption("After event");
        nana::api::refresh_window(pending);
        expect(draws == 0 && rootHash(fixture.form) == before,
            "Native root guard disabled the enclosing batch after its event");
    });
    expect(draws == 1, "Outer batch lost or repeated its requester after a native event");
    const auto after = rootHash(fixture.form);
    expect(after != before, "Outer batch did not publish its final state after a native event");
    nana::api::refresh_window_tree(fixture.form);
    expect(rootHash(fixture.form) == after, "Native event left the outer batch's final frame incomplete");
}

void reentrantFlush(Fixture& fixture, bool failNestedAction) {
    nana::button first{fixture.form, nana::rectangle{350, 65, 115, 30}};
    nana::button second{fixture.form, nana::rectangle{350, 100, 115, 30}};
    nana::button inner{fixture.form, nana::rectangle{350, 135, 115, 30}};
    first.transparent(true);
    second.transparent(true);
    nana::api::refresh_window_tree(fixture.form);
    std::vector<unsigned> order;
    bool active{};
    unsigned secondDraws{}, firstDraws{}, token{};
    bool caught{};
    DrawingProbe firstDrawing{first, [&](nana::paint::graphics&) {
        if (!active) return;
        // Stop generating work even on a broken recursive implementation.
        // Assertions below report the repeated callback without hanging.
        if (++firstDraws > 8) return;
        order.push_back(1);
        try {
            nana::api::batch_updates(inner, [&] {
                inner.caption("Inner flush");
                nana::api::refresh_window(first);
                nana::api::refresh_window(second);
                order.push_back(2);
                if (failNestedAction) throw ExpectedFailure{&token};
            });
        } catch (const ExpectedFailure& error) {
            caught = error.identity == &token;
        }
        // The original pending queue still contains second. A nested flush
        // must not consume that queue while first is being drawn.
        expect(secondDraws == 0, "Reentrant batch flushed another batch's pending transparent widget");
        order.push_back(3);
    }};
    DrawingProbe secondDrawing{second, [&](nana::paint::graphics&) {
        if (!active) return;
        ++secondDraws;
        order.push_back(4);
    }};
    active = true;
    nana::api::batch_updates(fixture.form, [&] {
        nana::api::refresh_window(first);
        nana::api::refresh_window(second);
        expect(order.empty(), "Transparent drawing did not wait for the batch flush");
    });
    active = false;
    expect(caught == failNestedAction, "Nested drawing batch replaced or swallowed its callback exception");
    expect(order == std::vector<unsigned>{1, 2, 3, 4} && firstDraws == 1 && secondDraws == 1,
        "Reentrant flush lost, repeated or reordered a pending redraw");
    const auto after = rootHash(fixture.form);
    nana::api::refresh_window_tree(fixture.form);
    expect(rootHash(fixture.form) == after, "Reentrant batch left an incomplete final root frame");
}

void selfRefresh(Fixture& fixture, bool nativeEvent) {
    nana::button source{fixture.form, nana::rectangle{350, 65, 115, 30}};
    source.transparent(true);
    source.caption("Self refresh");
    nana::api::refresh_window_tree(fixture.form);
    bool active{};
    unsigned draws{}, events{};
    DrawingProbe drawing{source, [&](nana::paint::graphics&) {
        if (!active) return;
        // This is a finite test even if self-refresh starts scheduling itself.
        if (++draws > 8) return;
        nana::api::refresh_window(source);
        nana::api::batch_updates(source, [&] {
            nana::api::refresh_window(source);
        });
    }};
    const auto update = [&] {
        active = true;
        nana::api::refresh_window(source);
    };
    if (nativeEvent) {
        source.events().mouse_move([&] {
            ++events;
            update();
        });
        SendMessageW(fixture.native(), WM_MOUSEMOVE, 0, MAKELPARAM(360, 75));
    } else {
        nana::api::batch_updates(source, update);
    }
    active = false;
    expect(events == (nativeEvent ? 1u : 0u) && draws == 1,
        "Drawing self-refresh repeated the active requester");
    const auto after = rootHash(fixture.form);
    nana::api::refresh_window_tree(fixture.form);
    expect(rootHash(fixture.form) == after, "Self-refresh left an incomplete root frame");
}

void mutualRefresh(Fixture& fixture) {
    nana::button first{fixture.form, nana::rectangle{350, 65, 115, 30}};
    nana::button second{fixture.form, nana::rectangle{350, 110, 115, 30}};
    first.transparent(true);
    second.transparent(true);
    first.caption("First requester");
    second.caption("Second requester");
    nana::api::refresh_window_tree(fixture.form);
    bool active{};
    unsigned firstDraws{}, secondDraws{}, events{};
    std::vector<unsigned> order;
    DrawingProbe firstDrawing{first, [&](nana::paint::graphics&) {
        if (!active) return;
        ++firstDraws;
        order.push_back(1);
        // A swapped queue used to alternate these callbacks forever. Bound
        // the generated requests, then check counts after the native flush.
        if (firstDraws <= 8) nana::api::refresh_window(second);
    }};
    DrawingProbe secondDrawing{second, [&](nana::paint::graphics&) {
        if (!active) return;
        ++secondDraws;
        order.push_back(2);
        if (secondDraws <= 8) nana::api::refresh_window(first);
    }};
    first.events().mouse_move([&] {
        ++events;
        active = true;
        nana::api::refresh_window(first);
    });
    SendMessageW(fixture.native(), WM_MOUSEMOVE, 0, MAKELPARAM(360, 75));
    active = false;
    expect(events == 1 && firstDraws == 1 && secondDraws == 1
        && order == std::vector<unsigned>{1, 2},
        "Mutual drawing refresh repeated requesters within the same native flush");
    const auto after = rootHash(fixture.form);
    nana::api::refresh_window_tree(fixture.form);
    expect(rootHash(fixture.form) == after, "Mutual refresh left an incomplete root frame");

    // Deduplication must end with the flush: a later independent update is
    // still required to draw both widgets and finish its own cycle.
    firstDraws = secondDraws = events = 0;
    order.clear();
    SendMessageW(fixture.native(), WM_MOUSEMOVE, 0, MAKELPARAM(361, 75));
    active = false;
    expect(events == 1 && firstDraws == 1 && secondDraws == 1,
        "Completed native flush suppressed a later independent refresh");
}

void pendingRequesterDestruction(Fixture& fixture) {
    nana::button source{fixture.form, nana::rectangle{350, 65, 115, 30}};
    nana::button closing{fixture.form, nana::rectangle{350, 110, 115, 30}};
    source.transparent(true);
    closing.transparent(true);
    nana::api::refresh_window_tree(fixture.form);
    const auto closingHandle = closing.handle();
    bool active{}, closedDuringDraw{};
    unsigned sourceDraws{}, closingDraws{};
    DrawingProbe sourceDrawing{source, [&](nana::paint::graphics&) {
        if (!active) return;
        if (++sourceDraws == 1) {
            closing.close();
            closedDuringDraw = !nana::api::is_window(closingHandle);
        }
    }};
    DrawingProbe closingDrawing{closing, [&](nana::paint::graphics&) {
        if (active) ++closingDraws;
    }};
    active = true;
    nana::api::batch_updates(fixture.form, [&] {
        nana::api::refresh_window(source);
        nana::api::refresh_window(closing);
    });
    active = false;
    const bool handleAlive = nana::api::is_window(closingHandle);
    const auto observed = " (sourceDraws=" + std::to_string(sourceDraws)
        + ", closingDraws=" + std::to_string(closingDraws)
        + ", closedDuringDraw=" + std::to_string(closedDuringDraw)
        + ", handleAlive=" + std::to_string(handleAlive) + ")";
    // Closing a child also queues its parent. Composing that parent can
    // redraw the surviving transparent source to refresh its background;
    // this is separate from draining the destroyed pending requester.
    expect(sourceDraws > 0, "Destroying drawing callback did not run" + observed);
    expect(closedDuringDraw && !handleAlive,
        "Drawing callback did not synchronously destroy the pending requester" + observed);
    expect(closingDraws == 0,
        "Flush redrew a requester destroyed by an earlier drawing callback" + observed);
    source.caption("After destruction");
    const auto after = rootHash(fixture.form);
    nana::api::refresh_window_tree(fixture.form);
    expect(rootHash(fixture.form) == after, "Requester destruction left later redraws incomplete");
}

void nativeEventFlush(Fixture& fixture) {
    nana::button source{fixture.form, nana::rectangle{350, 65, 115, 30}};
    nana::button target{fixture.form, nana::rectangle{350, 110, 115, 30}};
    source.transparent(true);
    source.caption("Native event");
    target.caption("Before event");
    nana::api::refresh_window_tree(fixture.form);
    unsigned events{}, sourceDraws{}, targetDraws{}, updates{};
    bool armed{};
    DrawingProbe sourceDrawing{source, [&](nana::paint::graphics&) {
        if (!armed || updates) return;
        ++sourceDraws;
        ++updates; // The test callback changes its sibling exactly once.
        nana::api::batch_updates(target, [&] { target.caption("After event"); });
    }};
    DrawingProbe targetDrawing{target, [&](nana::paint::graphics&) {
        if (armed) ++targetDraws;
    }};
    source.events().mouse_move([&] {
        ++events;
        armed = true;
        nana::api::refresh_window(source);
    });

    // Dispatch through the actual Nana Win32 procedure and its root_guard.
    // The synthetic client coordinate changes only this test's hover state;
    // no mouse input, focus or physical cursor is sent to the user's desktop.
    SendMessageW(fixture.native(), WM_MOUSEMOVE, 0, MAKELPARAM(360, 75));
    armed = false;
    expect(events == 1 && sourceDraws == 1 && targetDraws > 0 && updates == 1,
        "Native-event fixture did not redraw a sibling from a transparent callback");
    expect(target.caption() == "After event", "Native draw callback did not finish its nested batch");
    const auto after = rootHash(fixture.form);
    // No message pump or extra paint is allowed before the snapshot: a later
    // WM_PAINT would hide requests accidentally discarded by root_guard.
    nana::api::refresh_window_tree(fixture.form);
    expect(rootHash(fixture.form) == after,
        "Native root guard discarded redraws enqueued during the previous flush");
}

void exceptionRecovery(Fixture& fixture) {
    const auto before = rootHash(fixture.form);
    const auto origin = fixture.box.content_origin();
    unsigned token{};
    bool caught{};
    try {
        nana::api::batch_updates(fixture.form, [&] {
            fixture.sibling.caption("Exception");
            nana::api::batch_updates(fixture.box, [&] {
                fixture.box.append(L"Entry before exception\n", false);
                editor(fixture.box).restore_content_origin(origin);
                expect(rootHash(fixture.form) == before, "Nested failing batch exposed an intermediate frame");
                throw ExpectedFailure{&token};
            });
        });
    } catch (const ExpectedFailure& error) {
        caught = error.identity == &token;
    }
    expect(caught, "Batch swallowed or replaced the original callback exception");
    const auto after = rootHash(fixture.form);
    expect(after != before, "Failing batch did not flush final widget state");
    nana::api::refresh_window_tree(fixture.form);
    expect(rootHash(fixture.form) == after, "Failing batch left an incomplete final frame");
    fixture.sibling.caption("After exception");
    nana::api::refresh_window(fixture.sibling);
    expect(rootHash(fixture.form) != after, "Exception left normal redraws stuck in a batch");
    expectScrollbars(fixture.box);
}

void unchangedTypeface(Fixture& fixture) {
    auto& box = fixture.box;
    FontProbe probe;
    expect(probe.create(fixture.form, nana::rectangle{350, 65, 115, 35}),
        "Could not create the real font notification probe");
    box.caret_pos({10, 18}, false);
    box.select_points({3, 12}, {10, 18});
    editor(box).restore_content_origin({70, 12 * static_cast<int>(box.line_pixels())});
    const auto content = box.caption_wstring();
    const auto caret = box.caret_pos();
    const auto selection = box.selection();
    const auto selected = editor(box).make_select_string();
    expect(!selected.empty(), "Font fixture must contain a real text selection");

    // Rebuild the second editor from a complete caption at each new font. Its
    // text-area pixels are an independent oracle for the existing editor's
    // invalidated metrics; no test expectation depends on a font handle.
    nana::form oracleForm{nana::rectangle{-32000, -32000, 330, 195},
        nana::appearance{false, false, false, true, false, false, false}};
    cb::platform::preparePreviewWindow(oracleForm.native_handle());
    TestTextbox oracle{oracleForm, nana::rectangle{0, 0, 330, 195}};
    oracle.multi_lines(true);
    oracle.line_wrapped(false);
    oracle.enable_border_focused(false);
    // Both focused and unfocused selections use the same test palette, so
    // the hidden oracle does not need to take focus from the user's window.
    for (auto* text : {&box, &oracle}) {
        text->scheme().selection = nana::color{190, 210, 240};
        text->scheme().selection_unfocused = nana::color{190, 210, 240};
        text->scheme().selection_text = text->scheme().foreground.get_color();
    }
    nana::api::refresh_window(box);

    struct FontCase {
        const char* family;
        double size;
        nana::paint::font::font_style style;
        unsigned dpi;
        bool changesLineHeight;
    };
    const FontCase cases[]{
        {"Consolas", 9, {}, 96, false},
        {"Consolas", 13, {}, 96, true},
        {"Consolas", 13, {}, 144, true},
        {"Consolas", 13, {700, true, true}, 144, false},
        {"Arial", 13, {}, 144, false},
    };
    unsigned textboxDraws{};
    DrawingProbe drawing{box, [&](nana::paint::graphics&) { ++textboxDraws; }};
    unsigned index{};
    for (const auto& font : cases) {
        const auto oldHeight = box.line_pixels();
        const auto oldText = textHash(box);
        const auto previousChanges = probe.get_drawer_trigger().changes;
        textboxDraws = 0;
        box.typeface(nana::paint::font{font.family, font.size, font.style, font.dpi});
        probe.typeface(nana::paint::font{font.family, font.size, font.style, font.dpi});
        if (index++) {
            expect(probe.get_drawer_trigger().changes == previousChanges + 1 && textboxDraws != 0,
                "A genuine font change skipped notification or real textbox drawing");
            if (font.changesLineHeight)
                expect(box.line_pixels() != oldHeight, "Font size/DPI change left stale line geometry");
            expect(textHash(box) != oldText, "A genuine font change left the old text rendering");
        }
        expect(box.caption_wstring() == content && box.caret_pos() == caret
            && box.selection() == selection && editor(box).make_select_string() == selected,
            "A genuine font change altered text, caret or selection");

        oracle.typeface(nana::paint::font{font.family, font.size, font.style, font.dpi});
        oracle.caption(content);
        oracle.caret_pos(caret, false);
        oracle.select_points(selection.first, selection.second);
        editor(oracle).restore_content_origin(box.content_origin());
        editor(oracle).try_refresh();
        expect(oracle.line_pixels() == box.line_pixels()
            && editor(oracle).text_area(true) == editor(box).text_area(true)
            && oracle.content_origin() == box.content_origin() && textHash(oracle) == textHash(box),
            "Changed-font editor differs from a freshly laid-out full-caption oracle");

        const auto origin = box.content_origin();
        const auto height = box.line_pixels();
        const auto positions = box.text_position();
        const auto pixels = rootHash(fixture.form);
        const auto changes = probe.get_drawer_trigger().changes;
        const auto draws = probe.get_drawer_trigger().draws;
        textboxDraws = 0;
        for (unsigned repeat = 0; repeat != 64; ++repeat) {
            // Each request constructs a new public font object with the same
            // family, size, style and DPI, as repeated UI layout does.
            box.typeface(nana::paint::font{font.family, font.size, font.style, font.dpi});
            probe.typeface(nana::paint::font{font.family, font.size, font.style, font.dpi});
        }
        expect(probe.get_drawer_trigger().changes == changes
            && probe.get_drawer_trigger().draws == draws && textboxDraws == 0,
            "Equivalent font requests repeated typeface_changed or widget drawing");
        expect(box.caption_wstring() == content && box.caret_pos() == caret
            && box.selection() == selection && editor(box).make_select_string() == selected
            && box.content_origin() == origin && box.line_pixels() == height
            && box.text_position() == positions && rootHash(fixture.form) == pixels,
            "Equivalent font requests changed editor content, geometry or pixels");
        expectScrollbars(box);
    }
}

void graphicsTypefaceState() {
    const nana::size size{300, 80};
    const nana::paint::font::font_style style{700, true};
    const nana::paint::font font{"Consolas", 11, style, 144};
    nana::paint::font emptyFont;
    emptyFont.release();
    nana::paint::graphics pending, reference{size};
    pending.typeface(emptyFont);
    expect(pending.empty() && pending.typeface().empty(), "Empty graphics lost its empty shadow font");
    pending.typeface(font);
    expect(pending.empty() && !pending.typeface().empty(), "Unrealized graphics lost its pending font");
    pending.make(size);
    reference.typeface(font);
    const std::wstring text = L"W\ti  W\tfont";
    const auto expected = reference.text_extent_size(text);
    expect(expected.width != 0 && pending.text_extent_size(text) == expected,
        "Realizing graphics did not apply its pending font metrics");
    for (unsigned repeat = 0; repeat != 32; ++repeat)
        pending.typeface(nana::paint::font{"Consolas", 11, style, 144});
    expect(pending.text_extent_size(text) == expected,
        "Equivalent graphics fonts changed glyph, space or tab metrics");
    pending.typeface(emptyFont);
    expect(!pending.typeface().empty() && pending.text_extent_size(text) == expected,
        "Empty font replaced the currently realized graphics font");
    for (auto* graph : {&pending, &reference}) {
        graph->rectangle(true, nana::colors::white);
        graph->string({3, 3}, text, nana::colors::black);
    }
    expect(pixelHash(pending, nana::rectangle{size}) == pixelHash(reference, nana::rectangle{size}),
        "Graphics font reuse or empty assignment changed final text pixels");
}

void invalidAndClosedRoots(Fixture& context) {
    unsigned calls{};
    nana::api::batch_updates(nullptr, [&] { ++calls; });
    const auto before = rootHash(context.form);
    nana::api::batch_updates(context.form, {});
    expect(calls == 0 && rootHash(context.form) == before,
        "Invalid window or empty callback was not a no-op");
    for (const bool throwAfterClose : {false, true}) {
        nana::form closing{nana::rectangle{-32000, -32000, 80, 60},
            nana::appearance{false, false, false, true, false, false, false}};
        cb::platform::preparePreviewWindow(closing.native_handle());
        const auto handle = closing.handle();
        unsigned token{};
        bool caught{};
        try {
            nana::api::batch_updates(handle, [&] {
                ++calls;
                closing.close();
                expect(!nana::api::is_window(handle), "Callback did not destroy its batch root");
                if (throwAfterClose) throw ExpectedFailure{&token};
            });
        } catch (const ExpectedFailure& error) {
            caught = error.identity == &token;
        }
        expect(caught == throwAfterClose, "Closing root changed exception propagation");
        const auto previous = calls;
        nana::api::batch_updates(handle, [&] { ++calls; });
        expect(calls == previous, "Destroyed handle still invoked a batch callback");
    }
    expect(calls == 2, "Root destruction callback did not run exactly once");
    context.sibling.caption("After close");
    nana::api::refresh_window(context.sibling);
    expect(rootHash(context.form) != before, "Destroying another batch root blocked normal redraw");
}
} // namespace

int wmain() {
    cb::platform::initialize();
    int result{};
    try {
        Fixture fixture;
        nestedUpdates(fixture);
        synchronousNativeEventInBatch(fixture);
        exceptionRecovery(fixture);
        reentrantFlush(fixture, false);
        reentrantFlush(fixture, true);
        selfRefresh(fixture, false);
        selfRefresh(fixture, true);
        mutualRefresh(fixture);
        pendingRequesterDestruction(fixture);
        nativeEventFlush(fixture);
        unchangedTypeface(fixture);
        graphicsTypefaceState();
        invalidAndClosedRoots(fixture);
        std::cout << "Nana root update batches passed (intermediate frames, nesting, exceptions and destruction)\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    } catch (...) {
        std::cerr << "Unexpected exception escaped the batch regression\n";
        result = 1;
    }
    cb::platform::shutdown();
    return result;
}
