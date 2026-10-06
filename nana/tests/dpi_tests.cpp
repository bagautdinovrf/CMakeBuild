#include "ui.hpp"
#include "platform.hpp"
#include <nana/gui.hpp>
#include <nana/gui/detail/bedrock.hpp>
#include <nana/gui/detail/window_manager.hpp>
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct EventConnection {
    nana::detail::event_interface& event;
    nana::event_handle handle;
    ~EventConnection() { event.remove(handle); }
};

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string fileBytes(const std::filesystem::path& file) {
    std::ifstream input{file, std::ios::binary};
    expect(input.is_open(), "Could not read the test-owned INI");
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

int pixels(int logical, unsigned dpi) {
    return static_cast<int>(logical * (static_cast<double>(dpi) / 96.0));
}

nana::rectangle rectangle(const nana::widget& widget) {
    return nana::rectangle{widget.pos(), widget.size()};
}

nana::rectangle scaled(const nana::rectangle& logical, unsigned dpi) {
    const int left = pixels(logical.x, dpi), top = pixels(logical.y, dpi);
    return {left, top,
        static_cast<unsigned>(pixels(logical.x + static_cast<int>(logical.width), dpi) - left),
        static_cast<unsigned>(pixels(logical.y + static_cast<int>(logical.height), dpi) - top)};
}

void checkRectangle(const nana::widget& widget, const nana::rectangle& expected,
    const std::string& context) {
    const auto actual = rectangle(widget);
    expect(actual.x == expected.x && actual.y == expected.y
        && actual.width == expected.width && actual.height == expected.height,
        context + ": widget rectangle differs from logical geometry");
}

void checkFont(const nana::widget& widget, const char* family, double points,
    unsigned dpi, const std::string& context) {
    const auto font = widget.typeface();
    expect(font.name() == family && font.size() == points, context + ": font identity changed");
    LOGFONTW native{};
    expect(GetObjectW(reinterpret_cast<HFONT>(font.handle()), sizeof(native), &native) == sizeof(native),
        context + ": could not read the native font");
    const auto expectedHeight = -static_cast<LONG>(points * dpi / 72.0);
    expect(native.lfHeight == expectedHeight, context + ": font did not follow message DPI");
}

void checkWindow(cb::na::Panel& panel, int width, int height, unsigned dpi,
    const std::string& context, const RECT* suggested = nullptr) {
    const auto window = reinterpret_cast<HWND>(panel.form().native_handle());
    RECT client{}, outer{};
    expect(GetClientRect(window, &client) && GetWindowRect(window, &outer),
        context + ": could not read HWND geometry");
    const auto size = panel.form().size();
    expect(client.right - client.left == static_cast<LONG>(size.width)
        && client.bottom - client.top == static_cast<LONG>(size.height),
        context + ": HWND client and Nana size differ");
    expect(outer.right - outer.left == static_cast<LONG>(size.width)
        && outer.bottom - outer.top == static_cast<LONG>(size.height),
        context + ": HWND outer and Nana size differ");
    expect(size.width == static_cast<unsigned>(pixels(width, dpi))
        && size.height == static_cast<unsigned>(pixels(height, dpi)),
        context + ": DPI transition accumulated size drift");
    if (suggested) {
        expect(outer.left == suggested->left && outer.top == suggested->top,
            context + ": WM_DPICHANGED expected position was not applied; actual=("
                + std::to_string(outer.left) + ',' + std::to_string(outer.top) + ") expected=("
                + std::to_string(suggested->left) + ',' + std::to_string(suggested->top) + ") DPI=" + std::to_string(dpi));
    }
}

void sendDpi(cb::na::Panel& panel, unsigned dpi, const RECT& suggested) {
    const auto window = reinterpret_cast<HWND>(panel.form().native_handle());
    SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
    cb::platform::drainPreviewMessages();
    panel.refresh();
    nana::api::refresh_window_tree(panel.form());
    cb::platform::drainPreviewMessages();
}

void checkSaved(const cb::AppState& state, int width, int logHeight, const std::string& context) {
    expect(state.width == width && state.logHeight == logHeight,
        context + ": physical dimensions leaked into logical state");
    expect(cb::platform::readSetting(state.configFile, L"Width") == std::to_wstring(width)
        && cb::platform::readSetting(state.configFile, L"LogHeight") == std::to_wstring(logHeight),
        context + ": saved dimensions are not logical pixels");
}

void checkNativeResize(cb::na::Panel& panel, bool logVisible, int width, int logHeight,
    unsigned dpi, const std::string& context) {
    const auto window = reinterpret_cast<HWND>(panel.form().native_handle());
    const int resizedWidth = width + 32, resizedHeight = logVisible ? logHeight + 28 : 150;
    unsigned resizeEvents{};
    const auto event = panel.form().events().resized([&resizeEvents] { ++resizeEvents; });
    const EventConnection connection{panel.form().events().resized, event};
    expect(SetWindowPos(window, nullptr, 0, 0, pixels(resizedWidth, dpi), pixels(resizedHeight, dpi),
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, context + ": SetWindowPos failed");
    cb::platform::drainPreviewMessages();
    panel.refresh();
    cb::platform::drainPreviewMessages();
    checkWindow(panel, resizedWidth, resizedHeight, dpi, context + " native WM_SIZE");
    expect(resizeEvents > 0, context + ": native resize did not reach Nana resized callback");
    // Hidden fixtures intentionally do not persist user geometry. Restore the
    // fixture size before the next transition, using the same native WM_SIZE path.
    expect(SetWindowPos(window, nullptr, 0, 0, pixels(width, dpi), pixels(logVisible ? logHeight : 150, dpi),
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, context + ": restore resize failed");
    cb::platform::drainPreviewMessages();
    panel.refresh();
    checkWindow(panel, width, logVisible ? logHeight : 150, dpi, context + " restored WM_SIZE");
}

void testSynthetic(const std::filesystem::path& root, bool logVisible, int width, int logHeight) {
    const auto context = std::string(logVisible ? "journal" : "compact") + '-' + std::to_string(width);
    cb::AppState state{(root / (context + ".ini")).wstring()};
    state.width = width; state.logHeight = logHeight; state.logVisible = logVisible; state.pinned = false;
    state.save();
    cb::na::Panel panel{state, true};
    cb::na::preparePreview(panel.form());
    const auto window = reinterpret_cast<HWND>(panel.form().native_handle());
    expect((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0,
        context + ": test-owned preview window can activate");
    for (int line = 0; line < 80; ++line)
        panel.controller().journal().append(L"Строка DPI " + std::to_wstring(line) + L" — Unicode\n");
    panel.refresh();
    panel.journal().caret_pos({2, 12});
    panel.journal().select_points({2, 12}, {7, 15});
    const auto selection = panel.journal().selection();
    const auto caption = panel.journal().caption_wstring();
    nana::widget& focused = logVisible ? static_cast<nana::widget&>(panel.journal())
        : static_cast<nana::widget&>(panel.buildButton());
    bool hasFocus{};
    unsigned focusLosses{};
    const auto focusEvent = focused.events().focus([&](const nana::arg_focus& arg) {
        hasFocus = arg.getting;
        if (!arg.getting) ++focusLosses;
    });
    const EventConnection focusConnection{focused.events().focus, focusEvent};
    // Nana's public focus() calls Win32 SetFocus. Set the same internal widget
    // focus with root_has_been_focused=true so the offscreen test never activates.
    nana::detail::bedrock::instance().wd_manager().set_focus(
        focused.handle(), true, nana::arg_focus::reason::general);
    expect(hasFocus, context + ": could not set test-owned Nana focus");

    const RECT baseline{-30000, -30000, -30000 + width, -30000 + (logVisible ? logHeight : 150)};
    sendDpi(panel, 96, baseline);
    checkWindow(panel, width, logVisible ? logHeight : 150, 96, context + " baseline", &baseline);
    const auto runRectangle = rectangle(panel.runButton());
    const auto buildRectangle = rectangle(panel.buildButton());
    const auto journalRectangle = rectangle(panel.journal());
    constexpr std::array<unsigned, 5> sequence{96, 120, 144, 192, 96};
    for (int cycle = 0; cycle < 8; ++cycle) {
        for (const unsigned dpi : sequence) {
            const auto step = context + " cycle=" + std::to_string(cycle) + " DPI=" + std::to_string(dpi);
            const int x = -30000 + cycle * 13 + static_cast<int>(dpi), y = -30000 + cycle * 7;
            const RECT suggested{x, y, x + pixels(width, dpi), y + pixels(logVisible ? logHeight : 150, dpi)};
            sendDpi(panel, dpi, suggested);
            checkWindow(panel, width, logVisible ? logHeight : 150, dpi, step, &suggested);
            checkRectangle(panel.runButton(), scaled(runRectangle, dpi), step + " Run");
            checkRectangle(panel.buildButton(), scaled(buildRectangle, dpi), step + " Build");
            if (logVisible) checkRectangle(panel.journal(), scaled(journalRectangle, dpi), step + " Journal");
            checkFont(panel.runButton(), "Segoe UI", 9.75, dpi, step + " Run");
            checkFont(panel.buildButton(), "Segoe UI", 9.75, dpi, step + " Build");
            checkFont(panel.journal(), "Consolas", 9, dpi, step + " Journal");
            expect(panel.journal().selection() == selection, step + ": journal selection changed");
            expect(panel.journal().caption_wstring() == caption, step + ": journal text changed");
            expect(hasFocus && focusLosses == 0, step + ": widget focus changed");
            panel.controller().save();
            checkSaved(state, width, logHeight, step);
            if (cycle == 0 && (dpi == 96 || dpi == 192))
                checkNativeResize(panel, logVisible, width, logHeight, dpi, step);
        }
    }
    // Exercise the existing button's bound production callback after all moves.
    panel.controller().selectProject(L"not-a-CMake-file.txt");
    expect(panel.controller().status() == L"Нужен файл CMakeLists.txt", context + ": callback setup failed");
    nana::arg_click click;
    click.window_handle = panel.buildButton().handle();
    panel.buildButton().events().click.emit(click, panel.buildButton().handle());
    panel.refresh();
    expect(panel.controller().operation() == cb::na::Operation::Idle
        && panel.controller().status() == L"Выберите CMakeLists.txt",
        context + ": Build callback stopped working after DPI transitions");
    expect(panel.journal().selection() == selection && hasFocus && focusLosses == 0,
        context + ": callback/refresh changed journal selection or focus");
    panel.form().close();
    cb::platform::drainPreviewMessages();
}

void mouseMessage(HWND window, UINT message, WPARAM buttons, nana::point point) {
    SendMessageW(window, message, buttons,
        MAKELPARAM(static_cast<WORD>(point.x), static_cast<WORD>(point.y)));
}

void testNestedDpiChanges() {
    // This fixture owns no controller/INI and never activates. Every DPI
    // callback deliberately causes another synchronous, alternating change;
    // an unbounded pending-message loop prevents SendMessage from returning
    // and is caught by the nana_dpi CTest timeout.
    nana::form form{nana::rectangle{-32000, -32000, 120, 80},
        nana::appearance(false, false, false, false, false, false, false)};
    cb::na::preparePreview(form);
    const auto window = reinterpret_cast<HWND>(form.native_handle());
    std::vector<unsigned> observed;
    unsigned callbackDepth{}, maximumDepth{};
    bool generateNested = true;
    unsigned lastPendingDpi{};
    cb::platform::DesktopRect lastPendingBounds{}, lastObservedBounds{};
    cb::platform::DpiChangeHandler handler;
    handler.bind(window, [&](double scale, cb::platform::DesktopRect bounds) {
        ++callbackDepth;
        maximumDepth = (std::max)(maximumDepth, callbackDepth);
        const auto dpi = static_cast<unsigned>(std::lround(scale * 96.0));
        observed.push_back(dpi);
        lastObservedBounds = bounds;
        if (generateNested) {
            lastPendingDpi = dpi == 96 ? 120u : 96u;
            lastPendingBounds = {bounds.x + 1, bounds.y + 1, bounds.width, bounds.height};
            const RECT nested{lastPendingBounds.x, lastPendingBounds.y,
                lastPendingBounds.x + lastPendingBounds.width,
                lastPendingBounds.y + lastPendingBounds.height};
            SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(lastPendingDpi, lastPendingDpi),
                reinterpret_cast<LPARAM>(&nested));
        }
        --callbackDepth;
    });
    const RECT suggested{-31000, -31000, -30880, -30920};
    SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(120, 120),
        reinterpret_cast<LPARAM>(&suggested));
    expect(!observed.empty() && observed.front() == 120 && observed.size() <= 4,
        "Nested alternating DPI changes were not processed within a bounded transaction");
    expect(callbackDepth == 0 && maximumDepth == 1,
        "Nested DPI changes recursively invoked the owner callback");

    // Stop the deliberately infinite producer before pumping the deferred
    // change. The most recent nested message must survive the work limit.
    generateNested = false;
    const auto synchronousCount = observed.size();
    cb::platform::drainPreviewMessages();
    expect(observed.size() == synchronousCount + 1 && observed.back() == lastPendingDpi
        && lastObservedBounds.x == lastPendingBounds.x && lastObservedBounds.y == lastPendingBounds.y
        && lastObservedBounds.width == lastPendingBounds.width
        && lastObservedBounds.height == lastPendingBounds.height,
        "Deferred DPI processing lost the most recent nested scale or bounds");

    // Verify that the UI can dispatch the next queued native message after
    // the alternating callbacks, and a fresh DPI transaction still works.
    expect(PostMessageW(window, WM_NULL, 0, 0) != FALSE,
        "Could not queue a native message after nested DPI changes");
    MSG message{};
    expect(PeekMessageW(&message, window, WM_NULL, WM_NULL, PM_REMOVE) != FALSE,
        "UI did not return to native message dispatch after nested DPI changes");
    DispatchMessageW(&message);
    const auto previous = observed.size();
    SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(96, 96),
        reinterpret_cast<LPARAM>(&suggested));
    expect(observed.size() == previous + 1 && observed[previous] == 96,
        "A fresh DPI transaction stopped working after nested changes");
    form.close();
    cb::platform::drainPreviewMessages();
}

RECT windowRectangle(HWND window, const std::string& context) {
    RECT result{};
    expect(GetWindowRect(window, &result) != FALSE, context + ": GetWindowRect failed");
    return result;
}

void checkSameWindow(HWND window, const RECT& expected, const std::string& context) {
    const auto actual = windowRectangle(window, context);
    expect(actual.left == expected.left && actual.top == expected.top
        && actual.right == expected.right && actual.bottom == expected.bottom,
        context + ": an ended drag still moved the test window");
}

void testDragAnchorGeometry() {
    const DWORD messagePosition = GetMessagePos();
    const int pointerX = static_cast<short>(LOWORD(messagePosition));
    const int pointerY = static_cast<short>(HIWORD(messagePosition));
    cb::platform::WindowDrag drag;
    drag.anchorX = 187.2;
    drag.anchorY = 21.6;
    // Test both panel dimensions and a settings dialog whose height is clamped
    // by its new monitor. The clicked logical point must not depend on height.
    for (const int width : {733, 505}) for (const int height : {417, 660, 240}) {
        for (unsigned dpi : {96u, 120u, 144u, 192u, 120u, 96u}) {
            const double scale = dpi / 96.0;
            const cb::platform::DesktopRect suggested{-31000, -32000, pixels(width, dpi), pixels(height, dpi)};
            const auto actual = cb::platform::windowDragDpiBounds(drag, suggested, scale);
            expect(actual.x == static_cast<int>(std::round(pointerX - drag.anchorX * scale))
                && actual.y == static_cast<int>(std::round(pointerY - drag.anchorY * scale))
                && actual.width == suggested.width && actual.height == suggested.height,
                "DPI drag anchor depended on suggested position or window dimensions");
            expect(std::abs((pointerX - actual.x) / scale - drag.anchorX) <= .5 / scale + 1e-9
                && std::abs((pointerY - actual.y) / scale - drag.anchorY) <= .5 / scale + 1e-9,
                "DPI drag changed the originally grabbed logical point");
        }
    }
    drag.edges = 1;
    const auto unchanged = cb::platform::windowDragDpiBounds(drag, {-31000, -32000, 900, 400}, 1.25);
    expect(unchanged.x == -31000 && unchanged.y == -32000,
        "Moving-window anchor adjustment changed edge-resize bounds");
}

void testDragCapture(const std::filesystem::path& root) {
    // GetCapture is thread-local. A non-null baseline would belong to something
    // outside this fixture; never release or replace it just to run the test.
    expect(GetCapture() == nullptr && nana::api::capture_window() == nullptr,
        "Drag fixture started with an existing capture");
    cb::AppState state{(root / L"drag-capture.ini").wstring()};
    state.width = 733; state.logHeight = 417; state.logVisible = true; state.pinned = false;
    state.save();
    const auto initialIni = fileBytes(state.configFile);
    cb::na::Panel panel{state, true};
    cb::na::preparePreview(panel.form());
    nana::form captureOwner{nana::rectangle{-32000, -32000, 120, 80},
        nana::appearance(false, false, false, false, false, false, false)};
    cb::na::preparePreview(captureOwner);
    const auto window = reinterpret_cast<HWND>(panel.form().native_handle());
    const auto otherWindow = reinterpret_cast<HWND>(captureOwner.native_handle());
    struct OwnedCaptureCleanup {
        cb::na::Panel& panel;
        HWND window, other;
        ~OwnedCaptureCleanup() {
            if (nana::api::capture_window() == panel.form().handle()) panel.form().release_capture();
            const auto current = GetCapture();
            if (current == window || current == other) ReleaseCapture();
        }
    } cleanup{panel, window, otherWindow};
    unsigned downEvents{}, upEvents{};
    const auto down = panel.form().events().mouse_down([&] { ++downEvents; });
    const EventConnection downConnection{panel.form().events().mouse_down, down};
    const auto up = panel.form().events().mouse_up([&] { ++upEvents; });
    const EventConnection upConnection{panel.form().events().mouse_up, up};
    unsigned dpi = 96;
    double logicalAnchorX{}, logicalAnchorY{};
    const auto titlePoint = [&] { return nana::point{pixels(25, dpi), pixels(20, dpi)}; };
    const auto captureState = [&] {
        std::ostringstream description;
        description << " [native=" << GetCapture() << ", panel=" << window
            << ", other=" << otherWindow << ", Nana=" << nana::api::capture_window()
            << ", form=" << panel.form().handle() << ']';
        return description.str();
    };
    const auto checkCaptured = [&](const std::string& context) {
        expect(GetCapture() == window && nana::api::capture_window() == panel.form().handle(),
            context + ": title drag did not retain native and Nana capture" + captureState());
    };
    const auto checkReleased = [&](const std::string& context) {
        expect(GetCapture() == nullptr && nana::api::capture_window() == nullptr,
            context + ": drag capture was not released" + captureState());
    };
    const auto changeDpi = [&](unsigned nextDpi) {
        const bool captured = GetCapture() == window;
        dpi = nextDpi;
        const int x = -30000 + static_cast<int>(dpi), y = -30000;
        const RECT suggested{x, y, x + pixels(state.width, dpi), y + pixels(state.logHeight, dpi)};
        RECT expected = suggested;
        if (captured) {
            const DWORD pointer = GetMessagePos();
            expected.left = static_cast<LONG>(std::round(static_cast<short>(LOWORD(pointer)) - logicalAnchorX * dpi / 96.0));
            expected.top = static_cast<LONG>(std::round(static_cast<short>(HIWORD(pointer)) - logicalAnchorY * dpi / 96.0));
            expected.right = expected.left + pixels(state.width, dpi);
            expected.bottom = expected.top + pixels(state.logHeight, dpi);
        }
        // Do not pump unrelated physical mouse messages while the synthetic
        // button is held. Dispatch and refresh the actual native DPI message.
        SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
        panel.refresh();
        checkWindow(panel, state.width, state.logHeight, dpi, "captured DPI transition", &expected);
        if (captured) {
            mouseMessage(window, WM_MOUSEMOVE, MK_LBUTTON, titlePoint());
            checkSameWindow(window, expected, "first unchanged-pointer move after DPI");
        }
    };
    const auto begin = [&] {
        const auto previous = downEvents;
        // GetMessagePos is a real dispatched desktop point, unrelated to the
        // synthetic lParam. Keep this native fixture transparent, then put a
        // real title-sized offset under that point instead of a 30K offset.
        expect(cb::platform::preparePreviewDragWindow(window, dpi / 96.0, 25, 20),
            "Could not prepare the transparent native drag fixture");
        // A move onto a physical monitor can deliver its DPI before capture.
        dpi = static_cast<unsigned>(std::lround(panel.form().size().width * 96.0 / state.width));
        const auto bounds = windowRectangle(window, "drag logical anchor setup");
        const DWORD pointer = GetMessagePos();
        logicalAnchorX = (static_cast<double>(static_cast<short>(LOWORD(pointer))) - bounds.left) * 96.0 / dpi;
        logicalAnchorY = (static_cast<double>(static_cast<short>(HIWORD(pointer))) - bounds.top) * 96.0 / dpi;
        expect(std::abs(logicalAnchorX) < 100 && std::abs(logicalAnchorY) < 50,
            "Synthetic drag fixture did not grab a realistic title point");
        mouseMessage(window, WM_LBUTTONDOWN, MK_LBUTTON, titlePoint());
        expect(downEvents == previous + 1, "Native title WM_LBUTTONDOWN did not reach the form");
        checkCaptured("begin title drag");
    };
    const auto checkNoMovement = [&](const std::string& context, HWND expectedCapture = nullptr) {
        const auto previous = windowRectangle(window, context);
        // Moving only our HWND creates a real difference from the old drag
        // origin even when the physical cursor/GetMessagePos does not change.
        expect(SetWindowPos(window, nullptr, previous.left + 37, previous.top + 23, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, context + ": fixture move failed");
        const auto moved = windowRectangle(window, context);
        mouseMessage(window, WM_MOUSEMOVE, 0, titlePoint());
        panel.refresh();
        checkSameWindow(window, moved, context);
        expect(GetCapture() == expectedCapture && nana::api::capture_window() == nullptr,
            context + ": ended drag changed another capture");
    };

    changeDpi(96);
    begin();
    for (int cycle = 0; cycle < 12; ++cycle) {
        for (unsigned nextDpi : {120u, 192u, 144u, 96u}) {
            changeDpi(nextDpi);
            checkCaptured("repeated proportional drag anchor transition");
        }
    }
    mouseMessage(window, WM_LBUTTONUP, 0, titlePoint());
    checkReleased("repeated drag anchor transitions completion");
    for (const bool overJournal : {false, true}) {
        begin();
        changeDpi(192); checkCaptured("title drag at 192 DPI");
        changeDpi(96); checkCaptured("title drag returned to 96 DPI");
        if (!overJournal) { changeDpi(192); checkCaptured("title drag returned to 192 DPI"); }
        const nana::widget& child = overJournal ? static_cast<const nana::widget&>(panel.journal())
            : static_cast<const nana::widget&>(panel.buildButton());
        const auto bounds = rectangle(child);
        const auto previousUp = upEvents;
        mouseMessage(window, WM_LBUTTONUP, 0,
            {bounds.x + static_cast<int>(bounds.width / 2), bounds.y + static_cast<int>(bounds.height / 2)});
        expect(upEvents == previousUp + 1, "WM_LBUTTONUP over a child bypassed the capturing form");
        checkReleased(overJournal ? "release over journal" : "release over button");
        checkNoMovement(overJournal ? "move after journal release" : "move after button release");
    }

    begin();
    changeDpi(192); checkCaptured("lost mouse-up setup");
    mouseMessage(window, WM_MOUSEMOVE, 0, titlePoint());
    checkReleased("mouse move without the left button");
    checkNoMovement("move after lost mouse-up");

    begin();
    changeDpi(96); checkCaptured("WM_CANCELMODE setup");
    SendMessageW(window, WM_CANCELMODE, 0, 0);
    checkReleased("WM_CANCELMODE");
    checkNoMovement("move after WM_CANCELMODE");

    begin();
    changeDpi(192); checkCaptured("stolen capture setup");
    const auto previousCapture = SetCapture(otherWindow); // A second test-owned HWND, without activation.
    expect(GetCapture() == otherWindow && nana::api::capture_window() == nullptr,
        "WM_CAPTURECHANGED did not clear Nana drag while preserving the new capture"
            + captureState() + " SetCapture(previous="
            + std::to_string(reinterpret_cast<std::uintptr_t>(previousCapture)) + ')');
    mouseMessage(window, WM_LBUTTONUP, 0, titlePoint());
    checkNoMovement("move after another HWND took capture", otherWindow);
    expect(GetCapture() == otherWindow, "Old drag released another test-owned HWND's capture");
    ReleaseCapture(); // Release only the capture explicitly acquired above.
    checkReleased("release test-owned alternate capture");

    begin(); // A cancelled/stolen drag must not prevent the next normal drag.
    changeDpi(96); checkCaptured("fresh drag after capture loss");
    mouseMessage(window, WM_LBUTTONUP, 0, titlePoint());
    checkReleased("fresh drag completion");
    checkNoMovement("move after fresh drag completion");
    checkSaved(state, 733, 417, "drag capture INI");
    expect(fileBytes(state.configFile) == initialIni, "Drag/capture scenarios changed the isolated INI bytes");
    begin();
    changeDpi(192); checkCaptured("close during an active drag");
    panel.form().close();
    cb::platform::drainPreviewMessages();
    checkReleased("close during an active drag");
    expect(IsWindow(window) == FALSE, "Closing the active drag left its native HWND alive");
    checkSaved(state, 733, 417, "closed drag capture INI");
    expect(fileBytes(state.configFile) == initialIni, "Closing the active drag changed the isolated INI bytes");
    captureOwner.close();
    cb::platform::drainPreviewMessages();
}

struct Monitor { HMONITOR handle; RECT work; RECT bounds; };

BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return FALSE;
    reinterpret_cast<std::vector<Monitor>*>(data)->push_back({monitor, info.rcWork, info.rcMonitor});
    return TRUE;
}

void testPhysicalMonitorSeams() {
    std::vector<Monitor> monitors;
    expect(EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&monitors)) != FALSE,
        "Could not enumerate monitors for the DPI seam regression");
    struct OwnedWindow {
        HWND handle{};
        ~OwnedWindow() { if (handle) DestroyWindow(handle); }
    } fixture{CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"STATIC", L"CMakeBuild test-owned DPI seam", WS_POPUP,
        -32000, -32000, 200, 80, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)};
    expect(fixture.handle != nullptr && SetLayeredWindowAttributes(fixture.handle, 0, 0, LWA_ALPHA) != FALSE,
        "Could not create a transparent test-owned monitor seam window");
    expect(IsWindowVisible(fixture.handle) == FALSE, "Monitor seam fixture unexpectedly became visible");
    unsigned cases{};
    for (const auto& left : monitors) for (const auto& right : monitors) {
        if (left.bounds.right != right.bounds.left) continue;
        const LONG overlapTop = (std::max)(left.bounds.top, right.bounds.top);
        const LONG overlapBottom = (std::min)(left.bounds.bottom, right.bounds.bottom);
        if (overlapBottom - overlapTop < 150) continue;
        const int seam = left.bounds.right;
        const int y = static_cast<int>(overlapTop + (overlapBottom - overlapTop - 150) / 2);
        for (const bool targetOnRight : {false, true}) {
            const auto& target = targetOnRight ? right : left;
            const auto& neighbor = targetOnRight ? left : right;
            const int direction = targetOnRight ? 1 : -1;
            // Check both 100% and 125%-sized panels. Their centers straddle
            // the real seam by 20 pixels, as a grabbed point away from the
            // center can do when a DPI resize changes the majority monitor.
            for (const int width : {733, 916}) {
                if (target.bounds.right - target.bounds.left < width
                    || neighbor.bounds.right - neighbor.bounds.left < width) continue;
                const cb::platform::DesktopRect valid{seam - width / 2 + direction * 20, y, width, 150};
                expect(SetWindowPos(fixture.handle, nullptr, valid.x, valid.y, valid.width, valid.height,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER) != FALSE,
                    "Could not place the transparent HWND at a real monitor seam");
                expect(MonitorFromWindow(fixture.handle, MONITOR_DEFAULTTONEAREST) == target.handle,
                    "Monitor seam fixture did not start on its target monitor");
                const auto unchanged = cb::platform::monitorStableDpiBounds(fixture.handle, valid);
                expect(unchanged.x == valid.x && unchanged.y == valid.y
                    && unchanged.width == valid.width && unchanged.height == valid.height,
                    "Already stable DPI bounds moved at a real monitor seam");

                const cb::platform::DesktopRect candidate{seam - width / 2 - direction * 20, y, width, 150};
                const RECT proposed{candidate.x, candidate.y,
                    candidate.x + candidate.width, candidate.y + candidate.height};
                expect(MonitorFromRect(&proposed, MONITOR_DEFAULTTONEAREST) == neighbor.handle,
                    "Monitor seam candidate did not reproduce a majority-monitor reversal");
                const auto stable = cb::platform::monitorStableDpiBounds(fixture.handle, candidate);
                const RECT corrected{stable.x, stable.y, stable.x + stable.width, stable.y + stable.height};
                expect(MonitorFromRect(&corrected, MONITOR_DEFAULTTONEAREST) == target.handle,
                    "DPI bounds correction retained the neighboring monitor at the real seam");
                expect(stable.width == candidate.width && stable.height == candidate.height
                    && stable.y == candidate.y,
                    "Monitor seam correction changed DPI size or the unrelated axis");
                const int correction = direction * (stable.x - candidate.x);
                expect(correction >= 20 && correction <= 22,
                    "DPI monitor correction moved farther than the nearest boundary plus its pixel margin");
                expect(SetWindowPos(fixture.handle, nullptr, stable.x, stable.y, stable.width, stable.height,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER) != FALSE
                    && MonitorFromWindow(fixture.handle, MONITOR_DEFAULTTONEAREST) == target.handle,
                    "Corrected native bounds still reassigned the transparent HWND's monitor");
                expect(IsWindowVisible(fixture.handle) == FALSE, "Monitor seam fixture unexpectedly became visible");
                ++cases;
            }
        }
    }
    std::cout << "Physical monitor seam corrections: " << cases << '\n';
}

void testActualMonitors(const std::filesystem::path& root) {
    std::vector<Monitor> monitors;
    expect(EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&monitors)) != FALSE,
        "Could not enumerate physical monitors");
    expect(!monitors.empty(), "No desktop monitors are available");
    cb::AppState state{(root / L"actual-monitors.ini").wstring()};
    state.width = 733; state.logHeight = 417; state.logVisible = true; state.pinned = false;
    state.save();
    cb::na::Panel panel{state, true};
    cb::na::preparePreview(panel.form());
    panel.form().hide();
    const auto window = reinterpret_cast<HWND>(panel.form().native_handle());
    expect(IsWindowVisible(window) == FALSE, "Physical monitor fixture is visible");
    std::cout << "Physical monitors: " << monitors.size() << "; observed window DPI:";
    for (int cycle = 0; cycle < 3; ++cycle) {
        for (std::size_t index = 0; index < monitors.size(); ++index) {
            const auto& monitor = monitors[index];
            const int x = monitor.work.left + 8;
            const int y = monitor.work.top + 8;
            expect(SetWindowPos(window, nullptr, x, y, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
                "Could not move hidden HWND onto a physical monitor");
            cb::platform::drainPreviewMessages();
            panel.refresh();
            cb::platform::drainPreviewMessages();
            const auto dpi = GetDpiForWindow(window);
            expect(dpi != 0, "GetDpiForWindow failed on a physical monitor");
            if (cycle == 0) std::cout << ' ' << dpi;
            const auto context = "physical monitor=" + std::to_string(index) + " DPI=" + std::to_string(dpi);
            expect(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) == monitor.handle,
                context + ": test HWND did not move onto the requested monitor");
            checkWindow(panel, state.width, state.logHeight, dpi, context);
            checkFont(panel.runButton(), "Segoe UI", 9.75, dpi, context + " Run");
            checkFont(panel.journal(), "Consolas", 9, dpi, context + " Journal");
            panel.controller().save();
            checkSaved(state, 733, 417, context);
            expect(IsWindowVisible(window) == FALSE, context + ": hidden fixture became visible");
        }
    }
    std::cout << '\n';
    panel.form().close();
    cb::platform::drainPreviewMessages();
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { std::cerr << "Usage: NanaDpiTests <isolated-directory>\n"; return 2; }
    cb::platform::initialize();
    try {
        const auto root = std::filesystem::absolute(argv[1]);
        std::filesystem::create_directories(root);
        for (const bool logVisible : {false, true}) {
            testSynthetic(root, logVisible, 620, 365);
            testSynthetic(root, logVisible, 733, 417);
        }
        testDragAnchorGeometry();
        testNestedDpiChanges();
        testDragCapture(root);
        testPhysicalMonitorSeams();
        // A fresh Panel restores real monitor DPI after the synthetic messages.
        testActualMonitors(root);
        cb::platform::shutdown();
        std::cout << "Nana native WM_DPICHANGED/WM_SIZE regression checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        cb::platform::shutdown();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
