#pragma once
#include "app_state.hpp"
#include "platform.hpp"
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Buffer.H>
#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace cb {
inline constexpr int compactHeight = 150, minimumLogHeight = 240, defaultLogHeight = 365;
inline constexpr Fl_Font appFont = FL_FREE_FONT, journalFont = FL_FREE_FONT+1;
struct Palette {
    Fl_Color surface{}, soft{}, text{}, muted{}, line{}, accent{}, accentText{}, success{}, error{};
    Fl_Color scrollTrack{}, scrollThumb{};
};
Palette themePalette(bool dark);
void styleScrollbars(Fl_Group&, const Palette&);
enum class Control { Pick, Build, Run, RunMenu, Pin, Settings, Log, Minimize, Close, BuildMenu, PickMenu };
enum class Operation { Idle, Building, Running };

class Journal : public Fl_Text_Display {
public:
    Journal(int x, int y, int w, int h);
    ~Journal() override;
    void append(std::wstring_view);
    void clear();
    bool atBottom();
    int firstLine() { recalc_display(); return scroll_row(); }
    int horizontalPosition() { recalc_display(); return scroll_col(); }
    void resize(int x, int y, int w, int h) override;
private:
    Fl_Text_Buffer text_;
};

class Panel : public Fl_Double_Window {
public:
    explicit Panel(AppState&);
    ~Panel() override;
    AppState& state() { return state_; }
    Operation operation() const { return operation_; }
    bool failed() const { return failed_; }
    std::optional<std::chrono::milliseconds> lastBuildDuration() const { return lastBuildDuration_; }
    std::optional<BuildProgress> buildProgress() const { return buildProgress_; }
    Fl_Button& button(Control c) { return *buttons_[static_cast<size_t>(c)]; }
    Journal& journal() { return *journal_; }
    Fl_Text_Buffer& journalBuffer() { return *journal_->buffer(); }
    Fl_Menu_Button& runMenu() { return *menu_; }
    Fl_Menu_Button& buildMenu() { return *buildMenu_; }
    Fl_Menu_Button& projectMenu() { return *projectMenu_; }
    std::function<const Fl_Menu_Item*(Fl_Menu_Button&)> popupMenu;
    std::function<const Fl_Menu_Item*(Fl_Menu_Button&)> popupBuildMenu;
    std::function<const Fl_Menu_Item*(Fl_Menu_Button&)> popupProjectMenu;
    void show();
    void resize(int x, int y, int w, int h) override;
    int handle(int) override;
    void appendLog(std::wstring_view value) { journal_->append(value); }
    void updateControls();
    void updateTheme();
    void setTheme(bool dark);
    void onEvent(Event);
    void drainEvents();
    void selectProject(std::wstring);
    void showRecentProjects();
    bool chooseRecentProject(size_t);
    void build(bool cleanFirst = false, bool runAfter = false);
    void buildAndRun() { build(false, true); }
    void clean();
    void configure();
    void showBuildActions();
    void run();
    bool chooseRunTarget(size_t);
    void showRunTargets();
    void toggleLog();
    void closePanel();
    void save();
    void applySettings(BuildSettings, std::wstring chosen, bool selectionChanged);
private:
    struct EventQueue;
    AppState& state_;
    Palette colors_;
    std::array<Fl_Button*,11> buttons_{};
    Journal* journal_{};
    Fl_Menu_Button* menu_{};
    Fl_Menu_Button* buildMenu_{};
    Fl_Menu_Button* projectMenu_{};
    std::array<Fl_Menu_Item,6> buildMenuItems_{};
    std::vector<std::string> projectMenuLabels_;
    std::vector<Fl_Menu_Item> projectMenuItems_;
    std::vector<std::string> menuLabels_;
    std::vector<Fl_Menu_Item> menuItems_;
    std::shared_ptr<EventQueue> events_;
    std::unique_ptr<Engine> engine_;
    Operation operation_ = Operation::Idle;
    bool failed_ = false, closing_ = false;
    std::wstring status_;
    std::optional<std::chrono::milliseconds> lastBuildDuration_;
    std::optional<BuildProgress> buildProgress_;
    unsigned progressPulse_ = 0;
    bool cleaning_ = false, lastDurationIsClean_ = false;
    bool configuring_ = false, lastDurationIsConfigure_ = false;
    bool runAfterBuild_ = false, cancelling_ = false;
    int drag_ = 0, startX_ = 0, startY_ = 0, startW_ = 0, startH_ = 0, mouseX_ = 0, mouseY_ = 0;
    platform::WindowDrag nativeDrag_{};
    bool nativeDragging_ = false;
    void layout();
    void pickProject();
    void editSettings();
    void activate(Control);
    void showBuildDuration(const Event&);
    void resetBuildProgress();
    void drawBuildProgress();
    bool canQueueRun() const { return !closing_ && operation_==Operation::Building && !cleaning_ && !configuring_ && !cancelling_; }
    void draw() override;
    static void checkEvents(void*);
    static void animateProgress(void*);
    static int systemEvent(void*,void*);
};

class SettingsDialog : public Fl_Double_Window {
public:
    explicit SettingsDialog(Panel&);
    ~SettingsDialog() override;
    void show() override;
    Fl_Input& configurationInput() { return *configuration_; }
    Fl_Choice& runChoice() { return *runTarget_; }
    Fl_Input& argumentsInput() { return *arguments_; }
    Fl_Input& workingDirectoryInput() { return *workingDirectory_; }
    Fl_Input& environmentInput() { return *environment_; }
    void apply();
    void cancel();
    void setTheme(bool dark);
    bool accepted() const { return accepted_; }
    const BuildSettings& settingsValue() const { return settings_; }
    const std::wstring& chosenTarget() const { return chosen_; }
    bool selectionChanged() const { return runChanged_; }
    const std::vector<std::pair<std::wstring, RunSettings>>& launchSettings() const { return launchSettings_; }
    int handle(int) override;
private:
    Panel& panel_;
    Palette colors_;
    BuildSettings settings_;
    std::wstring chosen_;
    std::vector<Target> targets_;
    struct RunDraft {
        std::wstring target, arguments, directory, environment;
    };
    std::vector<RunDraft> runDrafts_;
    std::vector<std::pair<std::wstring, RunSettings>> launchSettings_;
    std::wstring launchTarget_, placeholderTarget_;
    bool accepted_ = false, runChanged_ = false;
    bool dragging_ = false;
    platform::WindowDrag nativeDrag_{};
    bool nativeDragging_ = false;
    int dragX_ = 0, dragY_ = 0, mouseX_ = 0, mouseY_ = 0;
    Fl_Button* closeButton_{};
    Fl_Input *cmake_{}, *directory_{}, *configuration_{}, *buildTarget_{};
    Fl_Input *arguments_{}, *workingDirectory_{}, *environment_{};
    Fl_Choice *compiler_{}, *runTarget_{};
    Fl_Check_Button* buildTests_{};
    Fl_Text_Display* tools_{};
    Fl_Text_Buffer toolsText_;
    void captureRunDraft();
    void loadRunDraft();
    void changeRunTarget();
    bool validateRunDrafts();
    void draw() override;
};
}
