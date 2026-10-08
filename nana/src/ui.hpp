#pragma once
#include "controller.hpp"
#include "platform.hpp"
#include <nana/gui/widgets/form.hpp>
#include <nana/gui/widgets/button.hpp>
#include <nana/gui/widgets/label.hpp>
#include <nana/gui/widgets/textbox.hpp>
#include <nana/gui/widgets/menu.hpp>
#include <nana/gui/widgets/progress.hpp>
#include <nana/gui/widgets/skeletons/text_editor.hpp>
#include <nana/gui/timer.hpp>
#include <memory>
#include <array>

namespace cb::na {
struct Palette {
    nana::color surface, soft, text, muted, line, accent, accentText, success, error, scrollTrack, scrollThumb;
    // Shared aliases keep dialog controls on the same palette as the panel.
    nana::color background, foreground, border;
    static Palette system(bool dark);
};
void colorWidget(nana::widget&, const Palette&, bool field = false);
void styleButton(nana::button&);
void styleScrollbars(nana::widget&, const Palette&, double scale);
void preparePreview(nana::form&);
std::uint64_t savePreview(nana::form&, const std::wstring& file);

class JournalBox final : public nana::textbox {
public:
    using nana::textbox::textbox;
    void update(const Journal&, bool autoScroll);
    void scrollToEnd();
    void restoreView(nana::point);
    void scrollSpace(unsigned);
    void scrollCorner(const nana::color&);
    void padding(unsigned top, unsigned right, unsigned bottom, unsigned left);
    void textGeometry(double scale, int logicalTextY, int physicalWindowY);
    unsigned linePitch() const;
    void selectWordAt(nana::point);
private:
    std::uint64_t revision_ = ~std::uint64_t{}, removed_{};
    std::string previous_;
};

class Panel {
public:
    explicit Panel(AppState&, bool hidden = false);
    ~Panel();
    int run();
    void refresh();
    void theme(bool dark);
    void shortcut(unsigned key, bool ctrl = false, bool alt = false, bool shift = false);
    Controller& controller() noexcept { return controller_; }
    nana::form& form() noexcept { return form_; }
    nana::button& runButton() noexcept { return run_; }
    nana::button& buildButton() noexcept { return build_; }
    JournalBox& journal() noexcept { return journal_; }
    void showRunMenu();
    void showBuildMenu();
    void showProjectMenu();
    void previewScale(double);
private:
    AppState& state_;
    Controller controller_;
    nana::form form_;
    nana::button project_, recent_, build_, buildMenu_, run_, runMenu_, settings_, pin_, log_, minimize_, close_;
    JournalBox journal_;
    nana::timer timer_;
    platform::DpiChangeHandler dpiHandler_;
    platform::WindowDragCancelHandler dragCancelHandler_;
    nana::menu projects_, builds_, targets_, journalMenu_;
    Palette palette_;
    bool dark_{}, hidden_{}, autoScroll_{true}, wrap_{false}, layoutActive_{}, observedLogVisible_{}, runMenuOpen_{}, previewScale_{};
    double scale_{1.0};
    unsigned animation_{};
    int previewFocus_{-1};
    struct ButtonState { bool hover{}, pressed{}; };
    std::array<ButtonState, 11> buttonStates_{};
    std::array<nana::rectangle, 11> buttonRectangles_{};
    struct RefreshState {
        Operation operation = Operation::Idle;
        BuildAction action = BuildAction::Build;
        bool failed{}, cancelling{}, canQueue{}, runQueued{}, hasTargets{}, pinned{}, logVisible{}, buildAndRun{};
        std::optional<BuildProgress> progress;
        std::optional<std::chrono::milliseconds> duration;
        std::wstring project, status, durationText;
    };
    RefreshState observedRefresh_;
    bool refreshValid_{};
    std::uint64_t observedJournalRevision_ = ~std::uint64_t{};
    void refreshPanel(bool animate);
    void drawPanel(nana::paint::graphics&);
    void drawButton(nana::paint::graphics&, nana::button&, unsigned);
    void layout(bool fitHeight = false);
    void dpiChanged(double scale, platform::DesktopRect bounds, const platform::WindowDrag* drag = nullptr);
    void selectProject();
    void bindKeys(nana::widget&);
    void saveGeometry();
    void prepareMenu(nana::menu&);
    friend int runUiSmoke(const std::wstring&, const std::wstring&);
    friend int runVisualPreviews(const std::wstring&, const std::wstring&);
};
void showSettings(nana::form&, Controller&, const Palette&);
int runSettingsPreviews(nana::form&, Controller&, const Palette&, const std::wstring& imageDirectory, double scale);
int runSettingsSmoke(nana::form&, Controller&, const Palette&);
int runUiSmoke(const std::wstring& isolatedIni, const std::wstring& imageDirectory);
int runVisualPreviews(const std::wstring& isolatedIni, const std::wstring& imageDirectory);
}
