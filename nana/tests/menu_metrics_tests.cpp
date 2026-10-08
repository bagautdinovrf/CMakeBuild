#include "platform.hpp"

#include <nana/gui/programming_interface.hpp>
#include <nana/gui/widgets/form.hpp>
#include <nana/gui/widgets/menu.hpp>
#include <nana/paint/graphics.hpp>

#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct RenderState {
    bool prepared{};
    unsigned frames{}, rows{};
};

class Renderer final : public nana::menu::renderer_interface {
public:
    explicit Renderer(std::shared_ptr<RenderState> state) : state_(std::move(state)) {}
    void background(graph_reference graph, nana::window window) override {
        if (!state_->prepared)
            state_->prepared = cb::platform::preparePreviewDragWindow(nana::api::root(window), 1, 0, 0);
        ++state_->frames;
        graph.rectangle(true, nana::colors::white);
    }
    void item(graph_reference graph, const nana::rectangle& bounds, const attr& attributes) override {
        ++state_->rows;
        graph.rectangle(bounds, true, attributes.item_state == state::active
            ? nana::color{30, 100, 210} : nana::color{255, 255, 255});
    }
    void item_image(graph_reference, const nana::point&, unsigned, const nana::paint::image&) override {}
    void item_text(graph_reference graph, const nana::point& point, const std::string& text,
        unsigned, const attr& attributes) override {
        graph.string(point, text, attributes.item_state == state::active
            ? nana::color{255, 255, 255} : nana::color{0, 0, 0});
    }
    void item_text(graph_reference graph, const nana::point& point, std::u8string_view text,
        unsigned width, const attr& attributes) override {
        item_text(graph, point, std::string{reinterpret_cast<const char*>(text.data()), text.size()}, width, attributes);
    }
    void sub_arrow(graph_reference, const nana::point&, unsigned, const attr&) override {}

private:
    std::shared_ptr<RenderState> state_;
};

class Fixture {
public:
    nana::form owner{nana::rectangle{-32000, -32000, 120, 80},
        nana::appearance{false, false, false, true, false, false, false}};
    std::shared_ptr<RenderState> rendered = std::make_shared<RenderState>();
    nana::menu menu;

    explicit Fixture(unsigned items = 3, unsigned captionLength = 5) {
        cb::platform::preparePreviewWindow(owner.native_handle());
        menu.renderer(nana::pat::cloneable<nana::menu::renderer_interface>{Renderer{rendered}});
        menu.max_pixels(4096);
        for (unsigned index = 0; index < items; ++index)
            menu.append(std::string(captionLength, 'm') + std::to_string(index));
        menu.popup(owner, 0, 0);
        expect(menu.handle() && rendered->prepared, "Menu popup was not created as a transparent test window");
        nana::api::typeface(menu.handle(), nana::paint::font{"Segoe UI", 9.75, {}, 96});
        nana::api::refresh_window(menu.handle());
    }

    nana::size dimensions() const { return nana::api::window_size(menu.handle()); }

    void refresh() { nana::api::refresh_window(menu.handle()); }

    void hover(unsigned index) {
        const auto dpi = nana::api::window_dpi(menu.handle());
        const auto border = static_cast<int>(2 * dpi / 96);
        const auto unit = static_cast<int>(dpi / 96);
        const auto pitch = static_cast<int>(menu.item_pixels() * dpi / 96);
        nana::arg_mouse mouse{};
        mouse.evt_code = nana::event_code::mouse_move;
        mouse.window_handle = menu.handle();
        mouse.pos = {border + 8, border + static_cast<int>(index) * (pitch + unit) + pitch / 2};
        nana::api::emit_internal_event(nana::event_code::mouse_move, menu.handle(), mouse);
    }
};

void invalidation() {
    Fixture fixture;
    const auto initial = fixture.dimensions();
    const auto frames = fixture.rendered->frames;
    for (unsigned index = 0; index < 80; ++index) fixture.hover(index % 2);
    expect(fixture.rendered->frames >= frames + 79, "Hover did not exercise real menu drawing");
    expect(fixture.dimensions() == initial, "Hover changed menu geometry");

    const auto original = fixture.menu.text(0);
    const std::string wide(100, 'W');
    fixture.menu.text(0, wide); fixture.hover(0);
    const auto widened = fixture.dimensions();
    expect(widened.width > initial.width && widened.height == initial.height,
        "Text mutation did not invalidate cached menu width");
    fixture.menu.text(0, original); fixture.hover(1);
    expect(fixture.dimensions() == initial, "Shorter text did not restore menu width");

    fixture.menu.text(0, std::u8string_view{u8"Длинное название пункта — Unicode 😀"}); fixture.refresh();
    const auto unicodePrefix = fixture.dimensions();
    nana::paint::graphics metrics{{1, 1}};
    metrics.typeface(nana::api::typeface(fixture.menu.handle()));
    const auto prefixExtent = metrics.text_extent_size(std::wstring_view{L"Длинное название пункта — Unicode 😀"}).width;
    fixture.menu.text(0, std::u8string_view{u8"Длинное название пункта — Unicode 😀 и весь текст после emoji"}); fixture.refresh();
    const auto fullExtent = metrics.text_extent_size(std::wstring_view{L"Длинное название пункта — Unicode 😀 и весь текст после emoji"}).width;
    expect(unicodePrefix.width > initial.width && fullExtent > prefixExtent
        && fixture.dimensions().width == unicodePrefix.width + fullExtent - prefixExtent,
        "UTF-8 menu width lost text following a supplementary character");
    fixture.menu.text(0, original); fixture.refresh();

    auto inserted = fixture.menu.insert(1, wide); fixture.refresh();
    expect(fixture.dimensions().height > initial.height && fixture.dimensions().width > initial.width,
        "Inserted item did not invalidate menu dimensions");
    inserted.text("short"); fixture.refresh();
    expect(fixture.dimensions().width == initial.width, "Item proxy text kept stale menu width");
    fixture.menu.erase(1); fixture.refresh();
    expect(fixture.dimensions() == initial, "Erasing an item did not restore menu dimensions");

    fixture.menu.append_splitter(); fixture.refresh();
    expect(fixture.dimensions().height > initial.height, "Appending a splitter kept stale menu height");
    fixture.menu.erase(fixture.menu.size() - 1); fixture.refresh();
    expect(fixture.dimensions() == initial, "Erasing a splitter kept stale menu height");
    fixture.menu.append(wide); fixture.refresh();
    expect(fixture.dimensions().height > initial.height && fixture.dimensions().width > initial.width,
        "Appending a caption kept stale menu dimensions");
    fixture.menu.erase(fixture.menu.size() - 1); fixture.refresh();

    const auto rowHeight = fixture.menu.item_pixels();
    fixture.menu.item_pixels(rowHeight + 12); fixture.refresh();
    expect(fixture.dimensions().height > initial.height, "Row-height mutation kept stale menu dimensions");
    fixture.menu.item_pixels(rowHeight); fixture.refresh();
    expect(fixture.dimensions() == initial, "Restoring row height kept stale menu dimensions");

    fixture.menu.text(0, wide); fixture.refresh();
    fixture.menu.max_pixels(120); fixture.refresh();
    expect(fixture.dimensions().width < widened.width, "Maximum-width mutation kept stale menu dimensions");
    fixture.menu.max_pixels(4096); fixture.refresh();
    expect(fixture.dimensions() == widened, "Restoring maximum width kept stale menu dimensions");
    fixture.menu.text(0, original); fixture.refresh();

    nana::api::typeface(fixture.menu.handle(), nana::paint::font{"Segoe UI", 18, {}, 96});
    fixture.refresh();
    expect(fixture.dimensions().width > initial.width, "A new font did not invalidate measured captions");
    nana::api::typeface(fixture.menu.handle(), nana::paint::font{"Segoe UI", 9.75, {}, 96});
    fixture.refresh();
    expect(fixture.dimensions() == initial, "Restoring a font did not restore measured captions");

    nana::api::window_size(fixture.menu.handle(), {initial.width + 30, initial.height + 30});
    fixture.refresh();
    expect(fixture.dimensions() == initial, "Cached metrics failed to correct an external resize");

    {
        // GetDpiForWindow reports the actual monitor DPI; a synthetic message
        // cannot change it. Use the application's real DPI handler contract:
        // consume message DPI and explicitly update font and layout metrics.
        const auto native = nana::api::root(fixture.menu.handle());
        unsigned deliveredDpi{};
        cb::platform::DpiChangeHandler dpiHandler;
        dpiHandler.bind(native, [&](double scale, cb::platform::DesktopRect) {
            deliveredDpi = static_cast<unsigned>(std::lround(scale * 96));
            fixture.menu.item_pixels(static_cast<unsigned>(std::lround(rowHeight * scale)));
            nana::api::typeface(fixture.menu.handle(), nana::paint::font{"Segoe UI", 9.75, {}, deliveredDpi});
        });
        for (const unsigned dpi : {144u, 192u, 96u}) {
            cb::platform::dispatchPreviewDpiChange(native, dpi,
                {100, 100, static_cast<int>(initial.width), static_cast<int>(initial.height)});
            fixture.refresh();
            expect(deliveredDpi == dpi, "DPI fixture did not deliver its requested scale");
            const auto current = fixture.dimensions();
            if (dpi != 96)
                expect(current.height > initial.height && current.width > initial.width,
                    "Message DPI change kept stale font or row metrics");
            else expect(current == initial, "Restoring message DPI kept stale menu dimensions");
        }
    }

    fixture.menu.clear();
    fixture.menu.append("one"); fixture.menu.append("two"); fixture.refresh();
    expect(fixture.dimensions().height < initial.height, "Clearing and replacing items kept stale menu height");
}

void benchmark() {
    // Actual hover handlers and drawing, with no wall-time pass/fail threshold.
    // Run the same binary fixture against both dependency revisions to compare.
    Fixture fixture{24, 72};
    for (unsigned index = 0; index < 40; ++index) fixture.hover(index % 2);
    constexpr unsigned iterations = 2000;
    const auto frames = fixture.rendered->frames;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned index = 0; index < iterations; ++index) fixture.hover(index % 2);
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    expect(fixture.rendered->frames >= frames + iterations - 1, "Benchmark did not exercise real hover redraws");
    std::cout << "{\"benchmark\":\"menu_hover\",\"items\":24,\"caption_length\":72,\"iterations\":"
        << iterations << ",\"frames\":" << fixture.rendered->frames - frames << ",\"milliseconds\":" << elapsed << "}\n";
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    cb::platform::initialize();
    int result{};
    try {
        if (argc > 1 && std::wstring_view{argv[1]} == L"--benchmark") benchmark();
        else { invalidation(); std::cout << "Nana menu metrics passed (hover, mutations, font, DPI and resize)\n"; }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; result = 1;
    }
    cb::platform::shutdown();
    return result;
}
