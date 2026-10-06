#include "ui.hpp"
#include "platform.hpp"
#include "version.hpp"
#include "bench_common.hpp"

#include <nana/gui/programming_interface.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
class NanaAdapter {
public:
    static constexpr auto name = "Nana";
    static constexpr auto appVersion = CMAKEBUILD_BENCH_APP_VERSION;
    static constexpr auto toolkitRevision = CMAKEBUILD_BENCH_TOOLKIT_REVISION;

    static void initialize() { cb::platform::initialize(); }
    static void shutdown() { cb::platform::shutdown(); }

    explicit NanaAdapter(const std::filesystem::path& isolatedIni)
        : state_(isolatedIni.wstring()) {
        state_.load();
        state_.settings = {};
        state_.targets.clear();
        state_.chosenTarget.clear();
        state_.chosenExecutable.clear();
        state_.recentProjects.clear();
        state_.pinned = false;
        state_.logVisible = true;
        state_.logHeight = 365;
        state_.width = 620;
        state_.x = 100;
        state_.y = 100;
        // Keep the real 65 ms production timer active, including during idle.
        panel_ = std::make_unique<cb::na::Panel>(state_);
    }

    void show() {
        panel_->form().caption(std::wstring{cb::appTitle} + L" · benchmark");
        panel_->form().show();
        // STARTUPINFO may hide the first ShowWindow as well as the console.
        // Nana already regards its form as shown, so force the native state.
        ShowWindow(hwnd(), SW_SHOWNOACTIVATE);
        panel_->refresh();
        flush();
    }

    void flush() {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT)
                throw std::runtime_error("Nana benchmark window was closed");
            // Nana's normal timer uses a Win32 TIMERPROC, so DispatchMessage
            // delivers its callbacks just as nana::exec's message pump does.
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!IsWindow(hwnd()))
            throw std::runtime_error("Nana benchmark window is unavailable");
        UpdateWindow(hwnd());
        GdiFlush();
    }

    void wait(std::chrono::milliseconds duration) {
        const auto deadline = std::chrono::steady_clock::now() + duration;
        for (;;) {
            flush();
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero()) break;
            const auto timeout = static_cast<DWORD>(std::clamp<long long>(
                std::chrono::ceil<std::chrono::milliseconds>(remaining).count(), 1, MAXDWORD - 1));
            if (MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_FAILED)
                throw std::runtime_error("Nana benchmark message wait failed");
        }
    }

    void append(std::wstring_view text) {
        panel_->controller().journal().append(text);
        panel_->refresh();
    }
    void clear() {
        panel_->controller().journal().clear();
        panel_->refresh();
    }
    void theme(bool dark) {
        panel_->theme(dark);
        panel_->refresh();
    }
    void redraw() {
        // WM_PAINT alone would only copy Nana's cached graphics. Regenerate
        // every visible child drawer first, including the journal/scrollbars.
        nana::api::refresh_window_tree(panel_->form());
        RedrawWindow(hwnd(), nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_NOERASE);
        flush();
    }

    HWND hwnd() const { return reinterpret_cast<HWND>(panel_->form().native_handle()); }
    std::size_t byteCount() const { return panel_->controller().journal().text().size(); }
    std::string text() const {
        // The actual Nana editor reconstructs line endings as CRLF. The shared
        // corpus uses LF; normalize the widget snapshot before validation.
        auto result = cb::platform::utf8(panel_->journal().caption_wstring());
        std::erase(result, '\r');
        return result;
    }

private:
    cb::AppState state_;
    std::unique_ptr<cb::na::Panel> panel_;
};
} // namespace

int wmain(int argc, wchar_t** argv) {
    return cbbench::run<NanaAdapter>(argc, argv);
}
