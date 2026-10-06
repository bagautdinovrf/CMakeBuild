#include "ui.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_Graphics_Driver.H>
#include <FL/fl_draw.H>
#include <FL/fl_utf8.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << message << L'\n';
        throw std::runtime_error("journal UI assertion failed");
    }
}

struct TempDirectory {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / (L"CMakeBuild-journal-ui-" + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(GetTickCount64()));
    TempDirectory() { std::filesystem::create_directories(root); }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        if (error) std::wcerr << L"Could not remove fixtures: " << root.wstring() << L'\n';
    }
};

void requireSelection(Fl_Text_Buffer& buffer, int expectedStart, int expectedEnd) {
    int start = 0, end = 0;
    require(buffer.selection_position(&start, &end) != 0,
        L"Journal must retain an active selection");
    require(start == expectedStart && end == expectedEnd,
        L"Journal must retain selection byte positions");
}

void requireUtf8(Fl_Text_Buffer& buffer) {
    std::unique_ptr<char, decltype(&std::free)> text(buffer.text(), &std::free);
    const auto* cursor = text.get();
    const auto* end = cursor + buffer.length();
    while (cursor < end) {
        int length = 0;
        const auto codePoint = fl_utf8decode(cursor, end, &length);
        require(length > 0 && !(length == 1 && static_cast<unsigned char>(*cursor) >= 128)
                && codePoint != 0xfffd,
            L"Journal truncation must retain complete UTF-8 characters");
        cursor += length;
    }
    require(cursor == end, L"Journal buffer must end at a complete UTF-8 character");
}

void checkGeometry(cb::Panel& panel, cb::AppState& state) {
    auto& journal = panel.journal();
    require(!panel.shown(), L"Journal test must never show the panel");
    if (!state.logVisible) panel.button(cb::Control::Log).do_callback();
    panel.resize(panel.x(), panel.y(), 620, cb::defaultLogHeight);
    const int originalHeight = journal.h();
    panel.resize(panel.x(), panel.y(), 620, 500);
    require(journal.h() > originalHeight, L"Journal must grow with panel height");
    require(state.logHeight == 500, L"Expanded logical height must be remembered");
    int minimumWidth = 0, minimumHeight = 0, maximumWidth = 0, maximumHeight = 0;
    panel.get_size_range(&minimumWidth, &minimumHeight, &maximumWidth, &maximumHeight);
    require(minimumHeight == cb::minimumLogHeight, L"Expanded minimum height is wrong");
    require(maximumHeight == 0 || maximumHeight > minimumHeight,
        L"Expanded panel must permit vertical resizing");
    panel.button(cb::Control::Log).do_callback();
    require(!state.logVisible && !journal.visible(), L"Log callback must hide the FLTK journal");
    require(panel.h() == cb::compactHeight, L"Collapsed panel must stay compact");
    panel.get_size_range(&minimumWidth, &minimumHeight, &maximumWidth, &maximumHeight);
    require(minimumHeight == cb::compactHeight && maximumHeight == cb::compactHeight,
        L"Collapsed height must be locked");
    panel.resize(panel.x(), panel.y(), 620, 700);
    require(panel.h() == cb::compactHeight, L"Programmatic resizing must preserve compact height");
    panel.button(cb::Control::Log).do_callback();
    require(state.logVisible && journal.visible() && state.logHeight == 500 && panel.h() == 500,
        L"Collapsing and reopening must retain journal height");
    panel.resize(panel.x(), panel.y(), 620, cb::defaultLogHeight);
    require(journal.h() < 500 - cb::compactHeight, L"Journal must shrink with panel height");
}

void checkScroll(cb::Panel& panel) {
    auto& journal = panel.journal();
    auto& buffer = panel.journalBuffer();
    std::wstring output;
    for (int i = 0; i < 300; ++i)
        output += L"line " + std::to_wstring(i) + L" " + std::wstring(160, L'x') + L"\n";
    panel.appendLog(output);
    require(journal.atBottom(), L"Initial output must follow the end of the journal");
    buffer.select(5, 25);
    journal.scroll(13, 100);
    const int firstLine = journal.firstLine();
    const int horizontal = journal.horizontalPosition();
    require(!journal.atBottom() && firstLine > 1 && horizontal > 0,
        L"Scroll fixture must be above the bottom with horizontal scrolling");
    panel.appendLog(L"new output while reading\n");
    require(journal.firstLine() == firstLine, L"New output must preserve the first visible line");
    require(journal.horizontalPosition() == horizontal, L"New output must preserve horizontal scroll");
    requireSelection(buffer, 5, 25);
    journal.scroll(1000000, 0);
    require(journal.atBottom(), L"Manual scrolling must reach the bottom");
    panel.appendLog(L"output after returning to bottom\n");
    require(journal.atBottom(), L"Autoscroll must resume after returning to the bottom");
    requireSelection(buffer, 5, 25);

    journal.scroll(25, 80);
    const int beforeResize = journal.firstLine();
    panel.resize(panel.x(), panel.y(), 710, 460);
    require(journal.firstLine() == beforeResize,
        L"Resizing while reading must preserve the first visible line");
    requireSelection(buffer, 5, 25);
    journal.scroll(1000000, 0);
    panel.resize(panel.x(), panel.y(), 620, cb::defaultLogHeight);
    require(journal.atBottom(), L"Resizing at the bottom must keep following the journal");

    journal.wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
    journal.recalc_display();
    journal.scroll(40, 0);
    const int wrappedLine = journal.firstLine();
    panel.appendLog(L"Русский вывод после переноса строк: Привет, мир! 😀\n");
    if (journal.firstLine() != wrappedLine)
        std::cerr << "Wrapped viewport moved from " << wrappedLine << " to " << journal.firstLine()
            << " with " << buffer.length() << " UTF-8 bytes\n";
    require(journal.firstLine() == wrappedLine,
        L"Appending wrapped output must preserve the visible wrapped line");
    requireSelection(buffer, 5, 25);
    journal.scroll(1000000, 0);
    panel.appendLog(L"Ещё строка\n");
    require(journal.atBottom(), L"Wrapped output must resume autoscroll at the bottom");
    journal.wrap_mode(Fl_Text_Display::WRAP_NONE, 0);
    requireUtf8(buffer);
}

void checkTheme(cb::Panel& panel) {
    auto& journal = panel.journal();
    auto& buffer = panel.journalBuffer();
    buffer.select(5, 25);
    journal.scroll(15, 75);
    const int firstLine = journal.firstLine(), horizontal = journal.horizontalPosition();
    const int x = panel.x(), y = panel.y(), width = panel.w(), height = panel.h();
    const auto font = journal.textfont();
    const auto textSize = journal.textsize();
    Fl::focus(&panel.button(cb::Control::Run));
    auto* focused = Fl::focus();
    auto* actualBuffer = journal.buffer();
    panel.setTheme(false);
    const auto lightColor = journal.color();
    panel.setTheme(true);
    require(journal.color() != lightColor, L"Light and dark themes must use different journal colors");
    panel.setTheme(false);
    require(panel.x() == x && panel.y() == y && panel.w() == width && panel.h() == height,
        L"Theme changes must preserve panel geometry");
    require(journal.textfont() == font && journal.textsize() == textSize,
        L"Theme changes must preserve fonts");
    require(Fl::focus() == focused && journal.buffer() == actualBuffer,
        L"Theme changes must preserve focus and the journal widget/buffer");
    require(journal.firstLine() == firstLine && journal.horizontalPosition() == horizontal,
        L"Theme changes must preserve journal scrolling");
    requireSelection(buffer, 5, 25);
    Fl::focus(nullptr);
}

void checkTruncation(cb::Panel& panel) {
    auto& journal = panel.journal();
    auto& buffer = panel.journalBuffer();
    journal.clear();
    const std::wstring line = L"Журнал 😀 UTF-8 " + std::wstring(150, L'я') + L"\n";
    for (int batch = 0; batch < 7; ++batch) {
        std::wstring output;
        for (int row = 0; row < 600; ++row) output += line;
        panel.appendLog(output);
    }
    require(buffer.length() < 1000000, L"Large output must not leave an unbounded journal");
    requireUtf8(buffer);
    require(journal.atBottom(), L"Truncating old output must retain autoscroll at the bottom");

    // Select retained tail text; removing old text must shift its offsets,
    // rather than dropping the user's selection.
    const int start = buffer.length() - 100;
    const int selectionStart = buffer.utf8_align(start);
    const int selectionEnd = buffer.utf8_align(start + 30);
    buffer.select(selectionStart, selectionEnd);
    std::unique_ptr<char, decltype(&std::free)> selected(buffer.selection_text(), &std::free);
    journal.scroll(std::max(1, journal.firstLine() - 15), 60);
    for (int batch = 0; batch < 8; ++batch) {
        std::wstring output;
        for (int row = 0; row < 150; ++row) output += line;
        panel.appendLog(output);
    }
    std::unique_ptr<char, decltype(&std::free)> after(buffer.selection_text(), &std::free);
    require(buffer.selected(), L"Truncation must retain a selection whose text is still in the retained tail");
    require(std::string(after.get()) == selected.get(),
        L"Truncation must preserve the selected retained UTF-8 text");
    requireUtf8(buffer);
}

struct SurfaceGuard {
    Fl_Window& window;
    bool visible = window.visible() != 0;
    explicit SurfaceGuard(Fl_Window& target, Fl_Image_Surface& surface) : window(target) {
        require(!window.shown(), L"Offscreen rendering must not create a native window");
        // Set only the flag required by Fl_Widget_Surface, without show().
        window.set_visible();
        Fl_Surface_Device::push_current(&surface);
    }
    ~SurfaceGuard() {
        Fl_Surface_Device::pop_current();
        if (!visible) window.clear_visible();
    }
};

std::unique_ptr<Fl_Image_Surface> scaledSurface(int width, int height, float scale) {
    // FLTK creates high-resolution image surfaces from the current graphics
    // driver's scale. This is scoped drawing state, not a monitor/OS setting.
    struct DriverScaleGuard {
        Fl_Graphics_Driver* driver;
        float previous;
        explicit DriverScaleGuard(float value) {
            Fl_Surface_Device::push_current(Fl_Display_Device::display_device());
            driver = Fl_Surface_Device::surface()->driver();
            previous = driver->scale();
            driver->scale(value);
        }
        ~DriverScaleGuard() {
            driver->scale(previous);
            Fl_Surface_Device::pop_current();
        }
    } scaleGuard(scale);
    return std::make_unique<Fl_Image_Surface>(width, height, 1);
}

struct Raster {
    int width{}, height{};
    std::vector<unsigned char> pixels;
    const unsigned char* pixel(int x, int y) const {
        return pixels.data() + (static_cast<size_t>(y) * width + x) * 3;
    }
};

Raster readRaster(Fl_Image_Surface& surface) {
    std::unique_ptr<Fl_RGB_Image> rgb(surface.image());
    require(rgb && rgb->array && rgb->d() >= 3, L"Cannot read scaled FLTK drawing");
    Raster result{rgb->data_w(), rgb->data_h()};
    const int depth = rgb->d(), stride = rgb->ld() ? rgb->ld() : result.width * depth;
    result.pixels.resize(static_cast<size_t>(result.width) * result.height * 3);
    for (int y = 0; y < result.height; ++y)
        for (int x = 0; x < result.width; ++x)
            std::copy_n(rgb->array + static_cast<std::ptrdiff_t>(y) * stride + x * depth, 3,
                result.pixels.data() + (static_cast<size_t>(y) * result.width + x) * 3);
    return result;
}

Raster renderWidget(Fl_Widget& widget, float scale) {
    auto surface = scaledSurface(widget.w(), widget.h(), scale);
    auto* window = widget.as_window() ? widget.as_window() : widget.window();
    require(window != nullptr, L"Offscreen widgets must belong to a fixture window");
    SurfaceGuard guard(*window, *surface);
    // A native child is always drawn over its window background. A fresh GDI
    // bitmap has no such parent frame, and fractional translated clipping may
    // leave edge pixels untouched. Initialize that frame before drawing the
    // actual widget so unused edge pixels cannot masquerade as icon ink.
    fl_color(window->color());
    fl_rectf(0, 0, widget.w(), widget.h());
    widget.clear_damage(FL_DAMAGE_ALL);
    surface->draw(&widget);
    auto result = readRaster(*surface);
    require(result.width == static_cast<int>(widget.w() * scale)
        && result.height == static_cast<int>(widget.h() * scale),
        L"Scaled FLTK drawing must contain the requested device-pixel resolution");
    return result;
}

void checkCompleteCaption(Fl_Button& button, float scale) {
    const auto actual = renderWidget(button, scale);
    const int x = button.x(), y = button.y(), width = button.w(), height = button.h();
    struct GeometryGuard {
        Fl_Button& button;
        int x, y, width, height;
        ~GeometryGuard() { button.resize(x, y, width, height); }
    } geometry{button, x, y, width, height};
    // Render the same real widget with generous width as a visual reference.
    // The text region must be identical at minimum panel width: no ellipsis or
    // missing glyphs are acceptable for an action caption.
    button.resize(x, y, width + 120, height);
    const auto reference = renderWidget(button, scale);
    for (int py = static_cast<int>(6 * scale); py < static_cast<int>((height - 6) * scale); ++py)
        for (int px = static_cast<int>(31 * scale); px < static_cast<int>((width - 4) * scale); ++px) {
            const auto* sample = actual.pixel(px, py);
            const auto* expected = reference.pixel(px, py);
            if (!std::equal(sample, sample + 3, expected)) {
                std::cerr << "Caption reference mismatch: label=" << (button.label() ? button.label() : "")
                    << " scale=" << scale << " geometry=" << x << ',' << y << ',' << width << ',' << height
                    << " enabled=" << button.active() << " value=" << button.value()
                    << " pixel=" << px << ',' << py << " actualRGB=" << static_cast<int>(sample[0]) << ','
                    << static_cast<int>(sample[1]) << ',' << static_cast<int>(sample[2])
                    << " referenceRGB=" << static_cast<int>(expected[0]) << ','
                    << static_cast<int>(expected[1]) << ',' << static_cast<int>(expected[2]) << '\n';
                require(false, L"Minimum-width action captions must match their complete, unclipped reference drawing");
            }
        }
    const auto* background = reference.pixel(reference.width - static_cast<int>(20 * scale), reference.height / 2);
    for (int py = static_cast<int>(6 * scale); py < static_cast<int>((height - 6) * scale); ++py)
        for (int px = static_cast<int>((width - 4) * scale); px < static_cast<int>((width + 80) * scale); ++px)
            require(std::equal(background, background + 3, reference.pixel(px, py)),
                L"The full caption must fit inside the actual button rather than continue past its edge");
}

void checkGearSymmetry(Fl_Button& button, float scale, bool dark) {
    const auto raster = renderWidget(button, scale);
    const auto* background = raster.pixel(0, 0);
    const int cx = button.w() / 2, cy = button.h() / 2;
    const int left = std::max(0, static_cast<int>((cx - 10) * scale));
    const int right = std::min(raster.width, static_cast<int>((cx + 10) * scale) + 1);
    const int top = std::max(0, static_cast<int>((cy - 10) * scale));
    const int bottom = std::min(raster.height, static_cast<int>((cy + 10) * scale) + 1);
    const auto ink = [&](int x, int y) {
        if (x < left || x >= right || y < top || y >= bottom) return 0.;
        const auto* pixel = raster.pixel(x, y);
        return static_cast<double>(std::abs(pixel[0] - background[0])
            + std::abs(pixel[1] - background[1]) + std::abs(pixel[2] - background[2]));
    };
    double total = 0.;
    for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x) total += ink(x, y);
    require(total > 1000., L"The Settings icon must contain a visible gear outline");
    const auto mirrorError = [&](bool horizontal) {
        double best = total * 2;
        // GDI+ may place a symmetric path between pixels at fractional DPI.
        // Choose the best reflection phase within one physical pixel.
        const int axis = static_cast<int>(std::lround(2 * (horizontal ? cx : cy) * scale));
        for (int phase = -2; phase <= 2; ++phase) {
            double error = 0.;
            for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x)
                error += std::abs(ink(x, y) - ink(horizontal ? axis + phase - x : x,
                    horizontal ? y : axis + phase - y));
            best = std::min(best, error);
        }
        return best / total;
    };
    const double horizontal = mirrorError(true), vertical = mirrorError(false);
    if (horizontal >= .28 || vertical >= .28)
        std::cerr << "Gear reflection error in " << (dark ? "dark" : "light") << " theme at scale "
            << scale << ": " << horizontal << ", " << vertical << '\n';
    require(horizontal < .28 && vertical < .28,
        L"The Settings gear must have mirror-symmetric teeth at every tested DPI scale");
    const int middleX = static_cast<int>(cx * scale), middleY = static_cast<int>(cy * scale);
    if (ink(middleX, middleY) >= 10.) {
        const auto* middle = raster.pixel(middleX, middleY);
        std::cerr << "Gear center failure in " << (dark ? "dark" : "light") << " theme at scale " << scale << " background RGB "
            << static_cast<int>(background[0]) << ',' << static_cast<int>(background[1]) << ',' << static_cast<int>(background[2])
            << " center RGB " << static_cast<int>(middle[0]) << ',' << static_cast<int>(middle[1]) << ',' << static_cast<int>(middle[2])
            << " logical position " << button.x() << ',' << button.y() << '\n';
        const auto directory = std::filesystem::absolute(L"build/engine-tests/ui-preview-debug");
        std::filesystem::create_directories(directory);
        const auto path = directory / (std::wstring(dark ? L"gear-dark-" : L"gear-light-")
            + std::to_wstring(static_cast<int>(scale * 100)) + L".ppm");
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "P6\n" << raster.width << ' ' << raster.height << "\n255\n";
        file.write(reinterpret_cast<const char*>(raster.pixels.data()), static_cast<std::streamsize>(raster.pixels.size()));
        std::wcerr << L"Gear failure drawing: " << path.wstring() << L'\n';
    }
    require(ink(middleX, middleY) < 10., L"The gear's central hole must remain open and centered");
}

void checkBuildModeDrawing(cb::Panel& panel, float scale) {
    auto& main = panel.button(cb::Control::Build);
    auto& arrow = panel.button(cb::Control::BuildMenu);
    const bool originalMode = panel.state().buildAndRun;
    const bool mainEnabled = main.active() != 0, arrowEnabled = arrow.active() != 0;
    const std::array geometry{main.x(), main.y(), main.w(), main.h(), arrow.x(), arrow.y(), arrow.w(), arrow.h()};
    for (const bool combined : {false, true}) {
        panel.state().buildAndRun = combined;
        panel.updateControls();
        main.activate();
        arrow.activate();
        main.handle(FL_LEAVE);
        arrow.handle(FL_LEAVE);
        main.value(0);
        arrow.value(0);
        require(std::string_view(main.label()) == (combined ? "Собрать и запустить" : "Собрать")
            && std::array{main.x(), main.y(), main.w(), main.h(), arrow.x(), arrow.y(), arrow.w(), arrow.h()} == geometry,
            L"Changing build mode must update its caption while preserving the entire split-button geometry");
        const std::array<unsigned char, 3> expected = combined
            ? std::array<unsigned char, 3>{196, 255, 210} : std::array<unsigned char, 3>{188, 197, 171};
        for (auto* button : {&main, &arrow}) {
            const auto raster = renderWidget(*button, scale);
            const auto* fill = raster.pixel(raster.width / 2, static_cast<int>((button->h() - 6) * scale));
            require(std::equal(expected.begin(), expected.end(), fill),
                L"Both normal split-button halves must render the requested gray or green mode color in either theme");
        }
        const auto raster = renderWidget(main, scale);
        int darkTextPixels = 0;
        for (int y = static_cast<int>(6 * scale); y < static_cast<int>((main.h() - 6) * scale); ++y)
            for (int x = static_cast<int>(31 * scale); x < static_cast<int>((main.w() - 4) * scale); ++x) {
                const auto* pixel = raster.pixel(x, y);
                if (pixel[0] < 90 && pixel[1] < 90 && pixel[2] < 90) ++darkTextPixels;
            }
        require(darkTextPixels > 30,
            L"The gray and green build modes must both draw readable dark caption text in either theme");
        checkCompleteCaption(main, scale);
    }
    panel.state().buildAndRun = originalMode;
    panel.updateControls();
    if (!mainEnabled) main.deactivate();
    if (!arrowEnabled) arrow.deactivate();
}

void checkMinimumWidthDrawing(cb::Panel& panel) {
    const int x = panel.x(), y = panel.y(), width = panel.w(), height = panel.h();
    int minimumWidth = 0;
    panel.get_size_range(&minimumWidth, nullptr, nullptr, nullptr);
    panel.resize(x, y, minimumWidth, height);
    Fl::focus(nullptr);
    for (const bool dark : {false, true}) {
        panel.setTheme(dark);
        for (const float scale : {1.f, 1.25f, 1.5f, 2.f}) {
            checkBuildModeDrawing(panel, scale);
            checkGearSymmetry(panel.button(cb::Control::Settings), scale, dark);
            for (const auto control : {cb::Control::Build, cb::Control::Run, cb::Control::Pin}) {
                auto& button = panel.button(control);
                const std::string original = button.label() ? button.label() : "";
                const auto checkLabels = [&](std::initializer_list<const char*> labels) {
                    for (const auto* caption : labels) {
                        button.copy_label(caption);
                        checkCompleteCaption(button, scale);
                    }
                };
                if (control == cb::Control::Build) checkLabels({"Собрать", "Собрать и запустить", "Отменить"});
                else if (control == cb::Control::Run) checkLabels({"Запустить", "Остановить", "Ожидание"});
                else checkLabels({"Закрепить", "Поверх окон"});
                button.copy_label(original.c_str());
            }
            for (const auto control : {cb::Control::Settings, cb::Control::Build, cb::Control::Run}) {
                auto& button = panel.button(control);
                const bool enabled = button.active() != 0;
                button.activate();
                button.handle(FL_LEAVE);
                const auto normal = renderWidget(button, scale);
                button.value(1);
                const auto pressed = renderWidget(button, scale);
                require(normal.pixels != pressed.pixels, L"Pressed buttons must have visible feedback at every scale");
                button.value(0);
                button.deactivate();
                const auto disabled = renderWidget(button, scale);
                require(normal.pixels != disabled.pixels, L"Disabled controls must be visually distinguishable at every scale");
                if (enabled) button.activate();
            }
        }
    }
    panel.resize(x, y, width, height);
    panel.updateControls();
}

std::vector<unsigned char> imagePixels(Fl_Image_Surface& surface) {
    std::unique_ptr<Fl_RGB_Image> rgb(surface.image());
    require(rgb && rgb->array && rgb->d() >= 3, L"Cannot read the actual FLTK offscreen RGB drawing");
    const int width = rgb->data_w(), height = rgb->data_h(), depth = rgb->d();
    const int stride = rgb->ld() ? rgb->ld() : width * depth;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        const auto* row = rgb->array + static_cast<std::ptrdiff_t>(y) * stride;
        for (int x = 0; x < width; ++x)
            std::copy_n(row + static_cast<std::ptrdiff_t>(x) * depth, 3,
                pixels.data() + (static_cast<size_t>(y) * width + x) * 3);
    }
    return pixels;
}

void checkPartialDraw(Fl_Window& window, Fl_Widget& child, const std::function<void()>& update) {
    Fl_Image_Surface surface(window.w(), window.h(), 0);
    SurfaceGuard guard(window, surface);
    // damage(ALL) is ignored by unmapped windows. clear_damage(bits) sets the
    // raw flag, letting this real draw exercise full/child damage faithfully.
    window.clear_damage(FL_DAMAGE_ALL);
    surface.draw(&window);
    const auto before = imagePixels(surface);
    require(before.size() == static_cast<size_t>(window.w()) * window.h() * 3,
        L"The unscaled offscreen frame must match its window dimensions");
    update();
    window.clear_damage(FL_DAMAGE_CHILD);
    for (auto* parent = child.parent(); parent && parent != &window; parent = parent->parent())
        parent->clear_damage(FL_DAMAGE_CHILD);
    child.clear_damage(FL_DAMAGE_ALL);
    surface.draw(&window);
    const auto after = imagePixels(surface);
    require(before.size() == after.size(), L"Partial updates must preserve rendered image geometry");
    bool changedChild = false;
    for (int y = 0; y < window.h(); ++y) for (int x = 0; x < window.w(); ++x) {
        const size_t index = (static_cast<size_t>(y) * window.w() + x) * 3;
        const bool differs = !std::equal(before.begin() + static_cast<std::ptrdiff_t>(index),
            before.begin() + static_cast<std::ptrdiff_t>(index + 3), after.begin() + static_cast<std::ptrdiff_t>(index));
        const bool inside = x >= child.x() && x < child.x() + child.w() && y >= child.y() && y < child.y() + child.h();
        if (!inside && differs) {
            std::cerr << "Partial draw changed an unrelated pixel at " << x << ',' << y << '\n';
            require(false, L"Updating one child must preserve the surrounding labels and widgets");
        }
        if (inside && differs) changedChild = true;
    }
    require(changedChild, L"Partial draw must update the changed child, not merely preserve the previous frame");
    require(!window.shown(), L"Partial draw regression must never show a native window");
}

void checkPartialUpdates(cb::Panel& panel) {
    auto& button = panel.button(cb::Control::Build);
    const int originalValue = button.value();
    checkPartialDraw(panel, button, [&] { button.value(!originalValue); });
    button.value(originalValue);
    cb::SettingsDialog settings(panel);
    auto& input = settings.configurationInput();
    checkPartialDraw(settings, input, [&] { input.value("Partial redraw changed this input"); });
}

void writeRaster(const Raster& raster, const std::filesystem::path& file) {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output << "P6\n" << raster.width << ' ' << raster.height << "\n255\n";
    output.write(reinterpret_cast<const char*>(raster.pixels.data()),
        static_cast<std::streamsize>(raster.pixels.size()));
    require(output.good(), L"Cannot write the FLTK preview image");
}

void writePreview(Fl_Window& window, const std::filesystem::path& file, float scale = 1.f) {
    require(!window.shown(), L"Offscreen rendering must not create a native window");
    writeRaster(renderWidget(window, scale), file);
    require(!window.shown(), L"Drawing must not show the native panel or settings");
}

struct ProgressFixture {
    TempDirectory temporary;
    cb::AppState state{(temporary.root / L"settings.ini").wstring()};
    cb::Panel panel{state};

    ProgressFixture() {
        const auto project = temporary.root / L"CMakeLists.txt";
        std::ofstream source(project, std::ios::binary | std::ios::trunc);
        source << "cmake_minimum_required(VERSION 3.24)\nproject(ProgressPreview LANGUAGES NONE)\n"
            "file(WRITE \"${CMAKE_CURRENT_SOURCE_DIR}/configure-started\" \"ready\")\n"
            "execute_process(COMMAND \"${CMAKE_COMMAND}\" -E sleep 30)\n";
        source.close();
        require(source.good(), L"Cannot write the isolated progress preview project");
        state.pinned = false;
        state.settings.cmakeFile = project.wstring();
        state.settings.buildDirectory = (temporary.root / L"build").wstring();
        state.settings.compiler = cb::CompilerMode::Environment;
        panel.resize(panel.x(), panel.y(), 500, cb::compactHeight);
        panel.build();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
        while (!std::filesystem::exists(temporary.root / L"configure-started")
                && panel.operation() == cb::Operation::Building && std::chrono::steady_clock::now() < deadline) {
            Fl::check();
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        panel.drainEvents();
        require(std::filesystem::exists(temporary.root / L"configure-started")
                && panel.operation() == cb::Operation::Building && !panel.shown() && !panel.buildProgress(),
            L"Progress drawings require a real hidden active CMake configuration");
    }
    ~ProgressFixture() { panel.closePanel(); }

    void animate() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{350};
        while (std::chrono::steady_clock::now() < deadline) {
            Fl::wait(.02);
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        require(panel.operation() == cb::Operation::Building && !panel.buildProgress(),
            L"Unknown build progress must remain animated during configuration");
    }
};

std::array<unsigned char, 3> colorBytes(Fl_Color color) {
    std::array<unsigned char, 3> result{};
    Fl::get_color(color, result[0], result[1], result[2]);
    return result;
}

bool matchesColor(const unsigned char* pixel, const std::array<unsigned char, 3>& color) {
    for (size_t channel = 0; channel < color.size(); ++channel)
        if (std::abs(static_cast<int>(pixel[channel]) - color[channel]) > 2) return false;
    return true;
}

int progressBandRow(const Raster& raster, cb::Panel& panel, float scale, const cb::Palette& palette) {
    const auto accent = colorBytes(palette.accent), track = colorBytes(palette.soft);
    const int buttonBottom = panel.button(cb::Control::Build).y() + panel.button(cb::Control::Build).h();
    std::vector<int> rows;
    // Find the rendered bar beneath the actions; do not assume its exact Y,
    // thickness or inset. A visible bar spans most of this minimum-width panel.
    for (int y = static_cast<int>((buttonBottom + 1) * scale);
            y < static_cast<int>((buttonBottom + 19) * scale); ++y) {
        int colored = 0;
        for (int x = 0; x < raster.width; ++x)
            if (matchesColor(raster.pixel(x, y), accent) || matchesColor(raster.pixel(x, y), track)) ++colored;
        if (colored > raster.width * .8) rows.push_back(y);
    }
    require(!rows.empty(), L"Active build must draw a clearly visible progress bar below its action buttons");
    return rows[rows.size() / 2];
}

void checkProgressDrawing(const std::filesystem::path& outputDirectory = {}) {
    ProgressFixture fixture;
    auto& panel = fixture.panel;
    constexpr std::array scales{1.f, 1.25f, 1.5f, 2.f};
    std::array<Raster, 8> unknownBefore;
    std::array<Raster, 8> unknownAfter;
    fixture.animate();
    for (int frame = 0; frame < 2; ++frame) {
        size_t index = 0;
        for (const bool dark : {false, true}) {
            panel.setTheme(dark);
            for (const float scale : scales) {
                const auto raster = renderWidget(panel, scale);
                (frame ? unknownAfter : unknownBefore)[index++] = raster;
                if (!outputDirectory.empty()) {
                    const auto name = std::wstring(L"panel-") + (dark ? L"dark" : L"light")
                        + L"-progress-indeterminate-" + std::to_wstring(frame + 1) + L"-"
                        + std::to_wstring(static_cast<int>(scale * 100)) + L".ppm";
                    writeRaster(raster, outputDirectory / name);
                }
            }
        }
        if (!frame) fixture.animate();
    }

    panel.onEvent({cb::EventKind::Progress, {}, {}, 0, {}, cb::BuildProgress{5, 10}});
    require(panel.buildProgress() && panel.buildProgress()->completed == 5 && panel.buildProgress()->total == 10,
        L"Determinate preview must exercise a real active panel with measured progress");
    size_t index = 0;
    for (const bool dark : {false, true}) {
        panel.setTheme(dark);
        const auto palette = cb::themePalette(dark);
        const auto accent = colorBytes(palette.accent), track = colorBytes(palette.soft);
        const auto background = colorBytes(palette.surface);
        for (const float scale : scales) {
            const auto raster = renderWidget(panel, scale);
            const int row = progressBandRow(raster, panel, scale, palette);
            int filled = 0, unfilled = 0, first = raster.width, last = -1;
            bool animated = false;
            for (int x = 0; x < raster.width; ++x) {
                if (matchesColor(raster.pixel(x, row), accent)) ++filled;
                else if (matchesColor(raster.pixel(x, row), track)) ++unfilled;
                else continue;
                first = std::min(first, x); last = x;
                if (!std::equal(unknownBefore[index].pixel(x, row), unknownBefore[index].pixel(x, row) + 3,
                        unknownAfter[index].pixel(x, row))) animated = true;
            }
            require(std::abs(filled - unfilled) <= 3 && matchesColor(raster.pixel(first + (last - first) / 4, row), accent)
                    && matchesColor(raster.pixel(first + 3 * (last - first) / 4, row), track),
                L"50 percent must visibly fill the left half of the bar while retaining the right half's track");
            require(animated, L"Indeterminate build progress must move between actual FLTK animation snapshots");

            // The right-aligned percentage must match the same panel drawn with
            // generous width, and must contain visible text at minimum width.
            panel.resize(panel.x(), panel.y(), 700, cb::compactHeight);
            const auto reference = renderWidget(panel, scale);
            panel.resize(panel.x(), panel.y(), 500, cb::compactHeight);
            const int cropLeft = static_cast<int>((panel.w() - 45) * scale);
            const int cropRight = static_cast<int>((panel.w() - 14) * scale);
            int percentageInk = 0;
            for (int y = static_cast<int>(116 * scale); y < static_cast<int>(147 * scale); ++y)
                for (int x = cropLeft; x < cropRight; ++x) {
                    const auto* actual = raster.pixel(x, y);
                    require(std::equal(actual, actual + 3, reference.pixel(x + reference.width - raster.width, y)),
                        L"Minimum-width build percentage must match its complete wider reference drawing");
                    if (!matchesColor(actual, background)) ++percentageInk;
                }
            require(percentageInk > 20 * scale * scale,
                L"Measured build progress must show a readable percentage in the right side of the status row");
            if (!outputDirectory.empty()) {
                const auto name = std::wstring(L"panel-") + (dark ? L"dark" : L"light") + L"-progress-50-"
                    + std::to_wstring(static_cast<int>(scale * 100)) + L".ppm";
                writeRaster(raster, outputDirectory / name);
            }
            ++index;
        }
    }
    require(!panel.shown(), L"Progress preview validation must never show a native window");
}

void checkConfigureDurationDrawing() {
    TempDirectory temporary;
    cb::AppState state((temporary.root / L"settings.ini").wstring());
    state.load();
    state.pinned = false;
    cb::Panel panel(state);
    panel.resize(panel.x(), panel.y(), 500, cb::compactHeight);
    constexpr auto duration = std::chrono::milliseconds{12345};
    for (const bool dark : {false, true}) {
        panel.setTheme(dark);
        const auto palette = cb::themePalette(dark);
        const auto background = colorBytes(palette.surface);
        for (const float scale : {1.f, 1.25f, 1.5f, 2.f}) {
            panel.journal().clear();
            panel.onEvent({cb::EventKind::ConfigureSucceeded, L"OK", {}, 0, duration});
            require(panel.lastBuildDuration() == duration && !panel.buildProgress() && !panel.failed(),
                L"Configure result must clear progress and retain its measured duration before drawing");
            std::unique_ptr<char, decltype(&std::free)> text(panel.journalBuffer().text(), &std::free);
            require(std::string_view(text.get()).find("CMake: 12,35 с") != std::string_view::npos
                && std::string_view(text.get()).find("Время сборки:") == std::string_view::npos,
                L"Configure-only journal must clearly name CMake instead of build time");
            const auto narrow = renderWidget(panel, scale);
            panel.resize(panel.x(), panel.y(), 700, cb::compactHeight);
            const auto wide = renderWidget(panel, scale);
            panel.resize(panel.x(), panel.y(), 500, cb::compactHeight);
            panel.onEvent({cb::EventKind::BuildSucceeded, L"OK", {}, 0, duration});
            const auto build = renderWidget(panel, scale);
            int ink = 0, changedCaptionPixels = 0;
            for (int y = static_cast<int>(116 * scale); y < static_cast<int>(147 * scale); ++y)
                for (int x = static_cast<int>((panel.w() - 180) * scale);
                        x < static_cast<int>((panel.w() - 14) * scale); ++x) {
                    const auto* pixel = narrow.pixel(x, y);
                    require(std::equal(pixel, pixel + 3, wide.pixel(x + wide.width - narrow.width, y)),
                        L"CMake duration at minimum width must match its complete wider reference drawing");
                    if (!matchesColor(pixel, background)) ++ink;
                    if (!std::equal(pixel, pixel + 3, build.pixel(x, y))) ++changedCaptionPixels;
                }
            require(ink > 40 * scale * scale && changedCaptionPixels > 10 * scale * scale,
                L"Configure-only status row must draw a readable caption distinct from the build caption");
        }
    }
    require(!panel.shown(), L"Configure duration raster checks must not open a native panel");
    panel.closePanel();
}

void renderPreviews(const std::filesystem::path& outputDirectory) {
    std::filesystem::create_directories(outputDirectory);
    TempDirectory temporary;
    cb::AppState state((temporary.root / L"settings.ini").wstring());
    state.load();
    state.pinned = true;
    state.settings.cmakeFile = L"C:\\GitRepos\\Демонстрационный проект\\CMakeLists.txt";
    state.settings.buildDirectory = L"build-release";
    state.settings.configuration = L"Release";
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    require(length != 0 && length < executable.size(), L"Cannot locate the preview fixture executable");
    const std::wstring path(executable.data(), length);
    state.targets = {{L"ExampleApp", path}, {L"ExampleTool", path}};
    state.chosenTarget = state.targets.front().name;
    state.chosenExecutable = path;
    cb::Panel panel(state);
    checkMinimumWidthDrawing(panel);
    {
        cb::SettingsDialog settings(panel);
        int minimumWidth = 0;
        panel.get_size_range(&minimumWidth, nullptr);
        for (const bool dark : {false, true}) {
            const std::wstring theme = dark ? L"dark" : L"light";
            panel.setTheme(dark);
            settings.setTheme(dark);
            for (const float scale : {1.f, 1.25f, 1.5f, 2.f}) {
                const auto suffix = L"-" + std::to_wstring(static_cast<int>(scale * 100)) + L".ppm";
                if (state.logVisible) panel.button(cb::Control::Log).do_callback();
                panel.resize(panel.x(), panel.y(), minimumWidth, cb::compactHeight);
                writePreview(panel, outputDirectory / (L"panel-" + theme + suffix), scale);
                writePreview(settings, outputDirectory / (L"settings-" + theme + suffix), scale);
                if (scale == 1.f) {
                    writePreview(panel, outputDirectory / (L"panel-" + theme + L".ppm"));
                    writePreview(settings, outputDirectory / (L"settings-" + theme + L".ppm"));
                }
                state.buildAndRun = true;
                panel.updateControls();
                writePreview(panel, outputDirectory / (L"panel-" + theme + L"-build-and-run" + suffix), scale);
                state.buildAndRun = false;
                panel.updateControls();
                panel.button(cb::Control::Settings).handle(FL_ENTER);
                panel.button(cb::Control::Build).value(1);
                panel.button(cb::Control::Run).deactivate();
                panel.button(cb::Control::RunMenu).deactivate();
                Fl::focus(&panel.button(cb::Control::Close));
                writePreview(panel, outputDirectory / (L"panel-" + theme + L"-states" + suffix), scale);
                panel.button(cb::Control::Settings).handle(FL_LEAVE);
                panel.button(cb::Control::Build).value(0);
                Fl::focus(nullptr);
                panel.updateControls();
                panel.button(cb::Control::Log).do_callback();
                panel.resize(panel.x(), panel.y(), minimumWidth, cb::defaultLogHeight);
                panel.journal().clear();
                panel.appendLog(L"CMake: конфигурация Release\n"
                    L"Собраны ExampleApp.exe и ExampleTool.exe\nРусский UTF-8: Привет, мир! 😀\n");
                panel.onEvent({cb::EventKind::BuildSucceeded, L"Сборка завершена.", {}, 0, std::chrono::milliseconds{12345}});
                writePreview(panel, outputDirectory / (L"panel-" + theme + L"-journal" + suffix), scale);
            }
        }
        require(!settings.shown(), L"Settings previews must remain entirely offscreen");
    }
    require(!panel.shown(), L"Panel previews must remain entirely offscreen");
    checkPartialUpdates(panel);
    panel.closePanel();
    checkProgressDrawing(outputDirectory);
    std::wcout << L"Actual FLTK offscreen previews: " << outputDirectory.wstring() << L'\n';
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        Fl::lock();
        if (argc == 3 && std::wstring_view(argv[1]) == L"--render-preview") {
            renderPreviews(std::filesystem::absolute(argv[2]));
            return 0;
        }
        require(argc == 1, L"Usage: journal_ui [--render-preview <folder>]");
        TempDirectory temporary;
        cb::AppState state((temporary.root / L"settings.ini").wstring());
        state.load();
        cb::Panel panel(state);
        state.pinned = false;
        checkGeometry(panel, state);
        checkScroll(panel);
        checkTheme(panel);
        checkTruncation(panel);
        checkMinimumWidthDrawing(panel);
        checkPartialUpdates(panel);
        checkProgressDrawing();
        checkConfigureDurationDrawing();
        panel.button(cb::Control::Log).do_callback();
        require(!state.logVisible, L"Failure fixture must start with a collapsed journal");
        panel.onEvent({cb::EventKind::BuildFailed, L"Проверочная ошибка"});
        require(state.logVisible && panel.journal().visible(), L"Failure must automatically open the journal");
        require(!panel.shown(), L"Test panel must remain hidden throughout validation");
        panel.save();
        cb::AppState restored(state.configFile);
        restored.load();
        require(restored.logVisible && restored.logHeight == state.logHeight,
            L"Journal visibility and height must survive an isolated settings restart");
        panel.closePanel();
        Fl::check();
        std::wcout << L"FLTK journal geometry, scrolling, Unicode and theme checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
