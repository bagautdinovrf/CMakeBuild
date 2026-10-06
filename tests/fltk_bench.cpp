#include "ui.hpp"
#include "bench_common.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Tooltip.H>
#include <FL/platform.H>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace {
class FltkAdapter {
public:
    static constexpr const char* name = "FLTK";
    static constexpr const char* appVersion = CMAKEBUILD_BENCH_APP_VERSION;
    static constexpr const char* toolkitRevision = CMAKEBUILD_BENCH_TOOLKIT_REVISION;

    static void initialize() {
        cb::platform::initialize();
        Fl::lock();
        Fl::scheme("gtk+");
        Fl::visible_focus(1);
        Fl_Tooltip::delay(.5f);
        Fl_Tooltip::font(cb::appFont);
        Fl_Tooltip::size(12);
    }

    static void shutdown() { cb::platform::shutdown(); }

    explicit FltkAdapter(const std::filesystem::path& isolatedIni)
        : state_(isolatedIni.wstring()) {
        state_.load();
        state_.settings = cb::BuildSettings{};
        state_.targets.clear();
        state_.chosenTarget.clear();
        state_.chosenExecutable.clear();
        state_.recentProjects.clear();
        state_.pinned = false;
        state_.logVisible = true;
        state_.logHeight = cb::defaultLogHeight;
        state_.width = 620;
        state_.x = 100;
        state_.y = 100;
        panel_ = std::make_unique<cb::Panel>(state_);
        panel_->copy_label("CMakeBuild FLTK benchmark");
    }

    void show() {
        panel_->show();
        // The runner hides the console through STARTUPINFO. Windows may also
        // apply that first-show hint to FLTK's initial ShowWindow call.
        ShowWindow(hwnd(), SW_SHOWNOACTIVATE);
    }

    void flush() {
        // Drain real Windows events and paint only the damage raised by the
        // production widgets. Append workloads must not force a full redraw.
        Fl::check();
        Fl::flush();
        GdiFlush();
    }

    void wait(std::chrono::milliseconds duration) {
        const auto deadline = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero()) break;
            Fl::wait(std::chrono::duration<double>(remaining).count());
            Fl::flush();
            GdiFlush();
        }
    }

    void append(std::wstring_view text) { panel_->appendLog(text); }
    void clear() { panel_->journal().clear(); }
    void theme(bool dark) { panel_->setTheme(dark); }
    void redraw() {
        panel_->redraw();
        flush();
    }
    HWND hwnd() { return reinterpret_cast<HWND>(fl_xid(panel_.get())); }
    std::size_t byteCount() {
        return static_cast<std::size_t>(panel_->journalBuffer().length());
    }
    std::string text() {
        const std::unique_ptr<char, decltype(&std::free)> snapshot(
            panel_->journalBuffer().text(), &std::free);
        return snapshot ? std::string(snapshot.get()) : std::string{};
    }

private:
    // The actual Panel saves on destruction; its AppState always targets the
    // benchmark's disposable INI, never the user's shared settings.ini.
    cb::AppState state_;
    std::unique_ptr<cb::Panel> panel_;
};
}

int wmain(int argc, wchar_t** argv) {
    return cbbench::run<FltkAdapter>(argc, argv);
}
