#include "ui.hpp"
#include "platform.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Scroll.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>

#include <algorithm>
#include <deque>
#include <string>

namespace cb {
namespace {
constexpr int settingsHeaderHeight = 43;

bool activationKey() {
    return (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter)
        && !(Fl::event_state() & (FL_SHIFT | FL_CTRL | FL_ALT | FL_META));
}

void revealFocused(Fl_Widget& widget) {
    for (auto* parent = widget.parent(); parent; parent = parent->parent()) {
        if (auto* scroll = dynamic_cast<Fl_Scroll*>(parent)) {
            int delta = 0;
            if (widget.y() < scroll->y()) delta = widget.y() - scroll->y();
            else if (widget.y() + widget.h() > scroll->y() + scroll->h())
                delta = widget.y() + widget.h() - scroll->y() - scroll->h();
            if (delta) scroll->scroll_to(scroll->xposition(), std::max(0, scroll->yposition() + delta));
            break;
        }
    }
}

void drawFocus(const Fl_Widget& widget, Fl_Color color) {
    if (Fl::focus() != &widget || !Fl::visible_focus()) return;
    fl_color(color);
    fl_line_style(FL_DOT);
    fl_rect(widget.x() + 4, widget.y() + 4, widget.w() - 8, widget.h() - 8);
    fl_line_style(FL_SOLID);
}

std::string ellipsized(std::string text, int width, Fl_Font font, int size) {
    fl_font(font, size);
    if (fl_width(text.c_str()) <= width) return text;
    const double suffix = fl_width("…");
    std::size_t lower = 0, upper = text.size(), best = 0;
    while (lower < upper) {
        const auto middle = lower + (upper - lower + 1) / 2;
        auto cut = middle;
        while (cut && cut < text.size() && (static_cast<unsigned char>(text[cut]) & 0xc0) == 0x80) --cut;
        if (fl_width(text.c_str(), static_cast<int>(cut)) + suffix <= width) {
            best = std::max(best, cut);
            lower = middle;
        } else upper = middle - 1;
    }
    text.resize(best);
    return text + "…";
}

std::string tooltipText(std::string_view text) {
    // Tooltip drawing always enables FLTK's @symbol syntax.
    std::string escaped;
    for (const char character : text) {
        if (character == '@') escaped += '@';
        escaped += character;
    }
    return escaped;
}

class SettingsInput final : public Fl_Input {
    const Palette& colors_;
    std::string hint_;
public:
    SettingsInput(int x, int y, int w, int h, const Palette& colors, const char* hint)
        : Fl_Input(x, y, w, h), colors_(colors), hint_(hint) {
        box(FL_BORDER_BOX);
        tooltip(hint_.c_str());
    }

    int handle(int event) override {
        if (event == FL_FOCUS) revealFocused(*this);
        // Editing keeps its normal horizontal scrolling; hovering reveals the
        // entire path or target without moving its cursor or selection.
        if (event == FL_ENTER) {
            const auto tip = *value() ? hint_ + "\n" + value() : hint_;
            copy_tooltip(tooltipText(tip).c_str());
        }
        return Fl_Input::handle(event);
    }

    void draw() override {
        Fl_Input::draw();
        fl_color(colors_.line);
        fl_rect(x(), y(), w(), h());
    }
};

class SettingsChoice final : public Fl_Choice {
    const Palette& colors_;
    std::deque<std::string> labels_;
    std::deque<std::string> popupLabels_;
    std::vector<Fl_Menu_Item> items_{Fl_Menu_Item{}};

    std::string currentLabel() const {
        const int selected = value();
        return selected >= 0 && static_cast<std::size_t>(selected) < labels_.size()
            ? labels_[static_cast<std::size_t>(selected)] : std::string{};
    }
public:
    SettingsChoice(int x, int y, int w, int h, const Palette& colors)
        : Fl_Choice(x, y, w, h), colors_(colors) {
        box(FL_BORDER_BOX);
        down_box(FL_BORDER_BOX);
    }

    ~SettingsChoice() override { menu(nullptr); }

    void refreshTooltip() { copy_tooltip(tooltipText(currentLabel()).c_str()); }

    void addLiteral(std::wstring_view text) {
        // FLTK add() parses '/', '_' and '&', merges labels, and uses a
        // fixed-size parsing buffer. Owned items preserve long literal names
        // and the one-to-one correspondence between menu indexes and targets.
        labels_.push_back(platform::utf8(text));
        int workX = 0, workY = 0, workWidth = 0, workHeight = 0;
        Fl::screen_work_area(workX, workY, workWidth, workHeight, window() ? window()->screen_num() : 0);
        const auto caption = ellipsized(labels_.back(), std::max(80, workWidth - 80), textfont(), textsize());
        std::string escaped;
        for (const char character : caption) {
            if (character == '&' || character == '@') escaped += character;
            escaped += character;
        }
        popupLabels_.push_back(std::move(escaped));
        Fl_Menu_Item item{};
        item.label(popupLabels_.back().c_str());
        items_.insert(items_.end() - 1, item);
        menu(items_.data());
    }

    int handle(int event) override {
        if (event == FL_FOCUS) revealFocused(*this);
        if (event == FL_ENTER) refreshTooltip();
        if (event == FL_KEYDOWN && Fl::focus() == this && active_r()) {
            const auto modifiers = Fl::event_state() & (FL_SHIFT | FL_CTRL | FL_ALT | FL_META);
            const bool open = activationKey() || (Fl::event_key() == FL_Down
                && (modifiers == 0 || modifiers == FL_ALT))
                || (Fl::event_key() == FL_F + 4 && modifiers == 0);
            if (open) {
                // Do not create a native popup for offscreen render/tests.
                if (window() && window()->shown()) Fl_Choice::handle(FL_PUSH);
                refreshTooltip();
                return 1;
            }
        }
        const int handled = Fl_Choice::handle(event);
        if (event == FL_PUSH) refreshTooltip();
        return handled;
    }

    void draw() override {
        fl_push_clip(x(), y(), w(), h());
        fl_color(colors_.surface);
        fl_rectf(x(), y(), w(), h());
        fl_color(colors_.line);
        fl_rect(x(), y(), w(), h());
        const auto foreground = active_r() ? colors_.text : colors_.muted;
        const auto text = ellipsized(currentLabel(), std::max(0, w() - 36), textfont(), textsize());
        fl_color(foreground);
        fl_draw(text.c_str(), x() + 7, y(), w() - 36, h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP, nullptr, 0);
        fl_color(colors_.line);
        fl_line(x() + w() - 27, y() + 6, x() + w() - 27, y() + h() - 7);
        fl_color(foreground);
        const int cx = x() + w() - 14, cy = y() + h() / 2;
        fl_line(cx - 4, cy - 2, cx, cy + 2, cx + 4, cy - 2);
        drawFocus(*this, foreground);
        fl_pop_clip();
    }
};

class SettingsCheckButton final : public Fl_Check_Button {
    const Palette& colors_;
public:
    SettingsCheckButton(int x, int y, int w, int h, const char* caption, const Palette& colors)
        : Fl_Check_Button(x, y, w, h, caption), colors_(colors) { box(FL_FLAT_BOX); }

    int handle(int event) override {
        if (event == FL_FOCUS) revealFocused(*this);
        if (event == FL_KEYDOWN && Fl::focus() == this && active_r() && activationKey()) {
            value(!value());
            set_changed();
            do_callback(FL_REASON_RELEASED);
            return 1;
        }
        return Fl_Check_Button::handle(event);
    }

    void draw() override {
        fl_push_clip(x(), y(), w(), h());
        fl_color(colors_.surface);
        fl_rectf(x(), y(), w(), h());
        const int boxX = x() + 2, boxY = y() + (h() - 16) / 2;
        const bool enabled = active_r() != 0;
        fl_color(value() && enabled ? colors_.accent : colors_.surface);
        fl_rounded_rectf(boxX, boxY, 16, 16, 3);
        fl_color(enabled && value() ? colors_.accent : colors_.line);
        fl_rounded_rect(boxX, boxY, 16, 16, 3);
        if (value()) fl_draw_check(Fl_Rect(boxX + 2, boxY + 2, 12, 12), enabled ? colors_.accentText : colors_.muted);
        fl_color(enabled ? colors_.text : colors_.muted);
        fl_font(labelfont(), labelsize());
        fl_draw(label(), x() + 25, y(), w() - 25, h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
        drawFocus(*this, enabled ? colors_.text : colors_.muted);
        fl_pop_clip();
    }
};

class SettingsToolsDisplay final : public Fl_Text_Display {
    const Palette& colors_;
public:
    SettingsToolsDisplay(int x, int y, int w, int h, const Palette& colors)
        : Fl_Text_Display(x, y, w, h), colors_(colors) {}

    int handle(int event) override {
        if (event == FL_FOCUS) revealFocused(*this);
        return Fl_Text_Display::handle(event);
    }

    void draw() override {
        Fl_Text_Display::draw();
        fl_color(colors_.line);
        fl_rect(x(), y(), w(), h());
    }
};

enum class SettingsButtonKind { Plain, Primary, Close };

class RoundedSettingsButton final : public Fl_Button {
    const Palette& colors_;
    SettingsButtonKind kind_;
    bool hover_ = false;
public:
    RoundedSettingsButton(int x, int y, int w, int h, const char* caption,
        const Palette& colors, SettingsButtonKind kind = SettingsButtonKind::Plain)
        : Fl_Button(x, y, w, h, caption), colors_(colors), kind_(kind) {
        box(FL_FLAT_BOX);
        labelfont(appFont);
        labelsize(13);
    }

    int handle(int event) override {
        if (event == FL_FOCUS) revealFocused(*this);
        if (event == FL_ENTER || event == FL_LEAVE) {
            hover_ = event == FL_ENTER;
            redraw();
        }
        if (event == FL_HIDE || event == FL_DEACTIVATE) hover_ = false;
        if (event == FL_KEYDOWN && Fl::focus() == this && active_r() && activationKey()) {
            simulate_key_action();
            do_callback(FL_REASON_RELEASED);
            return 1;
        }
        return Fl_Button::handle(event);
    }

    void draw() override {
        const bool enabled = active_r() != 0;
        const bool primary = kind_ == SettingsButtonKind::Primary;
        auto background = primary && enabled ? colors_.accent
            : enabled && (hover_ || value()) ? colors_.soft : colors_.surface;
        if (enabled && value()) background = fl_color_average(background, colors_.text, .9f);
        const auto foreground = !enabled ? colors_.muted : primary ? colors_.accentText : colors_.text;
        fl_push_clip(x(), y(), w(), h());
        fl_color(colors_.surface);
        fl_rectf(x(), y(), w(), h());
        fl_color(background);
        fl_rounded_rectf(x() + 1, y() + 1, w() - 2, h() - 2, 4);
        if (kind_ != SettingsButtonKind::Close) {
            fl_color(colors_.line);
            fl_rounded_rect(x() + 1, y() + 1, w() - 2, h() - 2, 4);
        }
        fl_color(foreground);
        fl_line_style(FL_SOLID, 1);
        const int shift = value() ? 1 : 0;
        if (kind_ == SettingsButtonKind::Close) {
            const int cx = x() + w() / 2 + shift, cy = y() + h() / 2 + shift;
            fl_line(cx - 5, cy - 5, cx + 5, cy + 5);
            fl_line(cx + 5, cy - 5, cx - 5, cy + 5);
        } else {
            fl_font(labelfont(), labelsize());
            fl_draw(label() ? label() : "", x() + 4 + shift, y() + shift, w() - 8, h(),
                FL_ALIGN_CENTER | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
        }
        drawFocus(*this, foreground);
        fl_line_style(FL_SOLID);
        fl_pop_clip();
    }
};

std::wstring trimmed(std::wstring text) {
    constexpr std::wstring_view whitespace = L" \t\r\n\f\v";
    const auto first = text.find_first_not_of(whitespace);
    if (first == std::wstring::npos) return {};
    const auto last = text.find_last_not_of(whitespace);
    return text.substr(first, last - first + 1);
}
}

SettingsDialog::SettingsDialog(Panel& panel)
    : Fl_Double_Window(505, 660, "Настройки CMakeBuild"), panel_(panel),
      settings_(panel.state().settings), chosen_(panel.state().chosenTarget),
      targets_(panel.state().targets) {
    border(0);
    set_modal();
    screen_num(panel.screen_num());
    int workX = 0, workY = 0, workWidth = 0, workHeight = 0;
    Fl::screen_work_area(workX, workY, workWidth, workHeight,
        panel.screen_num());
    // On a small or highly scaled display keep the header and action buttons
    // visible. The body scrolls without squeezing editors or their text.
    if (workWidth > 0 && workHeight > 0) {
        size(std::min(w(), std::max(300, workWidth - 16)), std::min(h(), std::max(180, workHeight - 16)));
    }
    size_range(w(), h(), w(), h());
    position(std::clamp(panel.x() + (panel.w() - w()) / 2, workX, std::max(workX, workX + workWidth - w())),
        std::clamp(panel.y() + 20, workY, std::max(workY, workY + workHeight - h())));
    callback([](Fl_Widget*, void* data) { static_cast<SettingsDialog*>(data)->cancel(); }, this);

    begin();
    const int inputX = w() < 420 ? 130 : 158;
    const int fieldWidth = w() - inputX - 18;
    const auto label = [inputX](const char* caption, int y) {
        auto* widget = new Fl_Box(18, y + settingsHeaderHeight, inputX - 20, 26, caption);
        widget->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        widget->labelfont(appFont);
        widget->labelsize(13);
    };
    const auto input = [this, inputX](int y, int width, const std::wstring& value, const char* tip) {
        auto* widget = new SettingsInput(inputX, y + settingsHeaderHeight, width, 26, colors_, tip);
        widget->textfont(appFont);
        widget->textsize(13);
        widget->value(platform::utf8(value).c_str());
        return widget;
    };

    closeButton_ = new RoundedSettingsButton(w() - 40, 9, 27, 27, nullptr, colors_, SettingsButtonKind::Close);
    closeButton_->tooltip("Закрыть настройки (Escape)");
    closeButton_->callback([](Fl_Widget*, void* data) { static_cast<SettingsDialog*>(data)->cancel(); }, this);

    auto* body = new Fl_Scroll(1, settingsHeaderHeight + 1, w() - 2, h() - settingsHeaderHeight - 67);
    body->box(FL_FLAT_BOX);
    body->type(Fl_Scroll::VERTICAL);
    body->begin();

    label("CMake.exe", 18);
    cmake_ = input(18, fieldWidth - 37, settings_.cmakeExecutable, "Пусто — найти CMake автоматически");
    auto* pickCmake = new RoundedSettingsButton(w() - 48, 18 + settingsHeaderHeight, 30, 26, "…", colors_);
    pickCmake->tooltip("Выбрать CMake.exe");
    pickCmake->callback([](Fl_Widget*, void* data) {
        auto& dialog = *static_cast<SettingsDialog*>(data);
        const auto chosen = platform::selectPath(fl_xid(&dialog), false,
            L"Выберите CMake.exe", L"CMake", L"cmake.exe");
        if (!chosen.empty()) dialog.cmake_->value(platform::utf8(chosen).c_str());
    }, this);

    label("Каталог сборки", 57);
    directory_ = input(57, fieldWidth - 37, settings_.buildDirectory, "Пусто — build-cmakebuild рядом с проектом");
    auto* pickDirectory = new RoundedSettingsButton(w() - 48, 57 + settingsHeaderHeight, 30, 26, "…", colors_);
    pickDirectory->tooltip("Выбрать каталог сборки");
    pickDirectory->callback([](Fl_Widget*, void* data) {
        auto& dialog = *static_cast<SettingsDialog*>(data);
        const auto chosen = platform::selectPath(fl_xid(&dialog), true, L"Выберите каталог сборки");
        if (!chosen.empty()) dialog.directory_->value(platform::utf8(chosen).c_str());
    }, this);

    label("Конфигурация", 96);
    configuration_ = input(96, fieldWidth, settings_.configuration, "Release, Debug, RelWithDebInfo или MinSizeRel");
    label("Компилятор", 135);
    auto* compilerChoice = new SettingsChoice(inputX, 135 + settingsHeaderHeight, fieldWidth, 26, colors_);
    compiler_ = compilerChoice;
    compiler_->textfont(appFont);
    compiler_->textsize(13);
    for (const auto caption : {L"Автоматически", L"MSVC", L"MinGW", L"Текущее окружение PATH"}) compilerChoice->addLiteral(caption);
    compiler_->value(std::clamp(static_cast<int>(settings_.compiler), 0, 3));
    compilerChoice->refreshTooltip();

    label("Цель сборки", 174);
    buildTarget_ = input(174, fieldWidth, settings_.target, "Пусто — собрать все цели");
    label("Цель запуска", 213);
    auto* runChoice = new SettingsChoice(inputX, 213 + settingsHeaderHeight, fieldWidth, 26, colors_);
    runTarget_ = runChoice;
    runTarget_->textfont(appFont);
    runTarget_->textsize(13);
    const bool missingChosen = !chosen_.empty()
        && std::ranges::none_of(targets_, [&](const auto& target) { return target.name == chosen_; });
    const auto placeholder = missingChosen ? chosen_ + L" (EXE пока не собран)"
        : targets_.empty() ? std::wstring(L"Будет найдена после сборки") : std::wstring(L"Выбрать стрелкой на кнопке");
    runChoice->addLiteral(placeholder);
    int selected = 0;
    for (std::size_t index = 0; index < targets_.size(); ++index) {
        runChoice->addLiteral(targets_[index].name);
        if (targets_[index].name == chosen_) selected = static_cast<int>(index) + 1;
    }
    runTarget_->value(selected);
    runChoice->refreshTooltip();
    placeholderTarget_=missingChosen ? chosen_ : L"";
    launchTarget_=chosen_;
    runTarget_->callback([](Fl_Widget*, void* data) { static_cast<SettingsDialog*>(data)->changeRunTarget(); }, this);
    if (targets_.empty()) runTarget_->deactivate();

    buildTests_ = new SettingsCheckButton(inputX, 252 + settingsHeaderHeight, fieldWidth, 26, "Собирать тесты", colors_);
    buildTests_->value(settings_.buildTests ? 1 : 0);
    buildTests_->tooltip("Добавить тестовые цели в сборку; сами тесты не запускаются");
    label("Аргументы", 294);
    arguments_=input(294,fieldWidth,L"","Аргументы запуска; пути с пробелами заключайте в двойные кавычки");
    label("Рабочая папка", 333);
    workingDirectory_=input(333,fieldWidth-37,L"","Пусто — папка EXE; относительный путь — от папки проекта");
    auto* pickWorkingDirectory=new RoundedSettingsButton(w()-48,333+settingsHeaderHeight,30,26,"…",colors_);
    pickWorkingDirectory->tooltip("Выбрать рабочую папку запуска");
    pickWorkingDirectory->callback([](Fl_Widget*,void* data) {
        auto& dialog=*static_cast<SettingsDialog*>(data);
        if(dialog.launchTarget_.empty()) return;
        const auto chosen=platform::selectPath(fl_xid(&dialog),true,L"Выберите рабочую папку запуска");
        if(!chosen.empty()) dialog.workingDirectory_->value(platform::utf8(chosen).c_str());
    },this);
    label("Окружение",372);
    environment_=input(372,fieldWidth,L"","Переменные запуска: ИМЯ=значение, по одной строке. PATH заменяет автоматически найденные пути");
    environment_->resize(inputX,372+settingsHeaderHeight,fieldWidth,78);
    environment_->type(FL_MULTILINE_INPUT);
    loadRunDraft();
    tools_ = new SettingsToolsDisplay(18, 464 + settingsHeaderHeight, w() - 36, 76, colors_);
    tools_->box(FL_BORDER_BOX);
    tools_->textsize(13);
    tools_->textfont(appFont);
    tools_->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
    auto toolsDescription = platform::utf8(describeDetectedTools());
    std::erase(toolsDescription, '\r');
    toolsText_.text(toolsDescription.c_str());
    tools_->buffer(toolsText_);
    tools_->tooltip("Найденные CMake, компиляторы и установленные наборы Qt");
    body->end();

    auto* applyButton = new RoundedSettingsButton(w() - 238, h() - 48, 115, 30,
        "Сохранить", colors_, SettingsButtonKind::Primary);
    applyButton->callback([](Fl_Widget*, void* data) { static_cast<SettingsDialog*>(data)->apply(); }, this);
    auto* cancelButton = new RoundedSettingsButton(w() - 113, h() - 48, 95, 30, "Отмена", colors_);
    cancelButton->callback([](Fl_Widget*, void* data) { static_cast<SettingsDialog*>(data)->cancel(); }, this);
    end();
    const auto fonts = [](auto&& self, Fl_Group& group) -> void {
        for (int index = 0; index < group.children(); ++index) {
            auto* widget = group.child(index);
            widget->labelsize(13);
            widget->labelfont(appFont);
            if (auto* nested = dynamic_cast<Fl_Group*>(widget)) self(self, *nested);
        }
    };
    fonts(fonts, *this);
    resizable(nullptr);
    setTheme(platform::darkTheme());
    cmake_->take_focus();
}

SettingsDialog::~SettingsDialog() {
    // Child widgets are deleted by the base window after member destruction.
    // Detach first so Text_Display cannot retain its already destroyed buffer.
    tools_->buffer(nullptr);
}

void SettingsDialog::show() {
    Fl_Double_Window::show();
    platform::configureDialogWindow(fl_xid(this), fl_xid(&panel_));
    setTheme(platform::darkTheme());
    cmake_->take_focus();
}

void SettingsDialog::captureRunDraft() {
    if(launchTarget_.empty()) return;
    auto draft=std::ranges::find_if(runDrafts_,[&](const RunDraft& value){return value.target==launchTarget_;});
    if(draft==runDrafts_.end()) return;
    draft->arguments=platform::utf16(arguments_->value());
    draft->directory=platform::utf16(workingDirectory_->value());
    draft->environment=platform::utf16(environment_->value());
}

void SettingsDialog::loadRunDraft() {
    if(!launchTarget_.empty() && std::ranges::none_of(runDrafts_,[&](const RunDraft& value){return value.target==launchTarget_;})) {
        const auto settings=panel_.state().runSettingsFor(launchTarget_);
        std::wstring environment;
        for(const auto& [name,value]:settings.environment) {
            if(!environment.empty()) environment+=L'\n';
            environment+=name+L"="+value;
        }
        runDrafts_.push_back({launchTarget_,settings.arguments,settings.workingDirectory,std::move(environment)});
    }
    const auto draft=std::ranges::find_if(runDrafts_,[&](const RunDraft& value){return value.target==launchTarget_;});
    arguments_->value(draft==runDrafts_.end() ? "" : platform::utf8(draft->arguments).c_str());
    workingDirectory_->value(draft==runDrafts_.end() ? "" : platform::utf8(draft->directory).c_str());
    environment_->value(draft==runDrafts_.end() ? "" : platform::utf8(draft->environment).c_str());
    for(auto* field:{arguments_,workingDirectory_,environment_}) {
        if(launchTarget_.empty()) field->deactivate();else field->activate();
    }
}

void SettingsDialog::changeRunTarget() {
    captureRunDraft();
    runChanged_=true;
    const int selected=runTarget_->value();
    launchTarget_=selected>0 && static_cast<std::size_t>(selected)<=targets_.size()
        ? targets_[static_cast<std::size_t>(selected-1)].name : placeholderTarget_;
    loadRunDraft();
}

bool SettingsDialog::validateRunDrafts() {
    launchSettings_.clear();
    for(const auto& draft:runDrafts_) {
        RunSettings settings;
        settings.arguments=draft.arguments;
        settings.workingDirectory=trimmed(draft.directory);
        const auto invalid=[&](const char* message,Fl_Input* field) {
            launchTarget_=draft.target;
            for(std::size_t index=0;index<targets_.size();++index)
                if(targets_[index].name==draft.target) {runTarget_->value(static_cast<int>(index)+1);runChanged_=true;break;}
            loadRunDraft();
            fl_alert("%s",message);
            field->take_focus();
            return false;
        };
        if(settings.arguments.find_first_of(L"\r\n")!=std::wstring::npos)
            return invalid("Аргументы запуска должны занимать одну строку.",arguments_);
        if(settings.workingDirectory.find_first_of(L"\r\n")!=std::wstring::npos)
            return invalid("Рабочая папка должна занимать одну строку.",workingDirectory_);
        std::size_t position=0;
        while(position<draft.environment.size()) {
            const auto end=draft.environment.find(L'\n',position);
            auto line=draft.environment.substr(position,end==std::wstring::npos ? end : end-position);
            position=end==std::wstring::npos ? draft.environment.size() : end+1;
            if(!line.empty() && line.back()==L'\r') line.pop_back();
            if(trimmed(line).empty()) continue;
            const auto separator=line.find(L'=');
            if(separator==std::wstring::npos) return invalid("Введите окружение в формате ИМЯ=значение, по одной переменной в строке.",environment_);
            auto name=trimmed(line.substr(0,separator));
            auto value=line.substr(separator+1);
            if(name.empty() || name.find(L'\r')!=std::wstring::npos || value.find(L'\r')!=std::wstring::npos)
                return invalid("Укажите непустое имя переменной окружения без переводов строк.",environment_);
            if(std::ranges::any_of(settings.environment,[&](const auto& entry){return _wcsicmp(entry.first.c_str(),name.c_str())==0;}))
                return invalid("Имена переменных окружения не должны повторяться.",environment_);
            settings.environment.emplace_back(std::move(name),std::move(value));
            if(settings.environment.size()>4096)
                return invalid("Можно сохранить не более 4096 переменных окружения для одной цели.",environment_);
        }
        launchSettings_.emplace_back(draft.target,std::move(settings));
    }
    return true;
}

void SettingsDialog::apply() {
    const auto configuration = trimmed(platform::utf16(configuration_->value()));
    if (configuration.empty()) {
        fl_alert("Укажите конфигурацию сборки.");
        configuration_->take_focus();
        configuration_->insert_position(configuration_->size(), 0);
        return;
    }
    captureRunDraft();
    if(!validateRunDrafts()) return;
    settings_.cmakeExecutable = platform::utf16(cmake_->value());
    settings_.buildDirectory = platform::utf16(directory_->value());
    settings_.configuration = configuration;
    settings_.compiler = static_cast<CompilerMode>(std::clamp(compiler_->value(), 0, 3));
    settings_.target = trimmed(platform::utf16(buildTarget_->value()));
    settings_.buildTests = buildTests_->value() != 0;
    if (runChanged_ && !targets_.empty()) {
        const int selected = runTarget_->value();
        chosen_ = selected > 0 && static_cast<std::size_t>(selected) <= targets_.size()
            ? targets_[static_cast<std::size_t>(selected - 1)].name : placeholderTarget_;
    }
    accepted_ = true;
    hide();
}

void SettingsDialog::cancel() {
    accepted_ = false;
    hide();
}

void SettingsDialog::setTheme(bool dark) {
    // The palette is shared with the panel. Keep the same widgets, input values,
    // focus and geometry through a system theme change.
    colors_ = themePalette(dark);
    color(colors_.surface);
    const auto palette = [this](auto&& self, Fl_Group& group) -> void {
        for (int index = 0; index < group.children(); ++index) {
            auto* widget = group.child(index);
            widget->labelcolor(colors_.text);
            widget->selection_color(colors_.accent);
            widget->color(colors_.surface);
            if (auto* nested = dynamic_cast<Fl_Group*>(widget)) self(self, *nested);
        }
    };
    palette(palette, *this);
    styleScrollbars(*this, colors_);
    for (auto* input : {cmake_, directory_, configuration_, buildTarget_, arguments_, workingDirectory_, environment_}) {
        input->color(colors_.surface);
        input->textcolor(colors_.text);
        input->cursor_color(colors_.text);
    }
    for (auto* choice : {compiler_, runTarget_}) {
        choice->color(colors_.surface);
        choice->textcolor(colors_.text);
    }
    tools_->color(colors_.surface);
    tools_->textcolor(colors_.text);
    tools_->cursor_color(colors_.text);
    if (shown()) platform::setWindowTheme(fl_xid(this), dark);
    redraw();
}

void SettingsDialog::draw() {
    // Keep the background when only a child changes, matching draw_children's
    // damage handling. Otherwise an input redraw would erase unchanged controls.
    if (damage() & ~FL_DAMAGE_CHILD) {
        fl_color(colors_.surface);
        fl_rectf(0, 0, w(), h());
        fl_color(colors_.line);
        fl_rect(0, 0, w(), h());
        fl_line(0, settingsHeaderHeight, w(), settingsHeaderHeight);
        fl_line(0, h() - 66, w(), h() - 66);
        fl_color(colors_.accent);
        fl_rectf(16, 23, 5, 5);
        fl_rectf(23, 16, 5, 5);
        fl_rectf(23, 23, 5, 5);
        fl_color(colors_.text);
        fl_font(appFont, 13);
        fl_draw("Настройки CMakeBuild", 39, 9, w() - 92, 28,
            FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    }
    draw_children();
}

int SettingsDialog::handle(int event) {
    if (event == FL_SHOW) {
        const int result = Fl_Double_Window::handle(event);
        setTheme(platform::darkTheme());
        return result;
    }
    if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE
        && Fl::event_y() >= 0 && Fl::event_y() < settingsHeaderHeight
        && Fl::event_x() >= 0 && Fl::event_x() < w() - 45) {
        dragging_ = true;
        nativeDragging_ = shown() && platform::beginWindowDrag(fl_xid(this), 0, nativeDrag_);
        dragX_ = x();
        dragY_ = y();
        mouseX_ = Fl::event_x_root();
        mouseY_ = Fl::event_y_root();
        return 1;
    }
    if (event == FL_DRAG && dragging_) {
        if (nativeDragging_) platform::updateWindowDrag(fl_xid(this), nativeDrag_, w(), h());
        else position(dragX_ + Fl::event_x_root() - mouseX_, dragY_ + Fl::event_y_root() - mouseY_);
        return 1;
    }
    if (event == FL_RELEASE && dragging_) {
        dragging_ = false;
        nativeDragging_ = false;
        return 1;
    }
    if (event == FL_KEYDOWN || event == FL_SHORTCUT) {
        if (Fl::event_key() == FL_Escape || (Fl::event_key() == FL_F + 4 && Fl::event_alt())) {
            cancel();
            return 1;
        }
        if (activationKey()) {
            auto* focused = Fl::focus();
            if(focused==environment_) return Fl_Double_Window::handle(event);
            if (focused && focused->window() == this
                && (dynamic_cast<RoundedSettingsButton*>(focused) || dynamic_cast<SettingsChoice*>(focused)
                    || dynamic_cast<SettingsCheckButton*>(focused)))
                return focused->handle(FL_KEYDOWN);
            apply();
            return 1;
        }
    }
    return Fl_Double_Window::handle(event);
}

} // namespace cb
