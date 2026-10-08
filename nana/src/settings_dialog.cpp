#include "ui.hpp"
#include "platform.hpp"
#include <nana/gui/widgets/panel.hpp>
#include <nana/gui/widgets/combox.hpp>
#include <nana/gui/widgets/float_listbox.hpp>
#include <nana/gui/widgets/checkbox.hpp>
#include <nana/gui/widgets/scroll.hpp>
#include <nana/gui/msgbox.hpp>
#include <nana/gui/element.hpp>
#include <nana/paint/pixel_buffer.hpp>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace cb::na {
namespace {
std::wstring trim(std::wstring text) {
    const auto first = text.find_first_not_of(L" \t\r\n\f\v");
    if (first == std::wstring::npos) return {};
    return text.substr(first, text.find_last_not_of(L" \t\r\n\f\v") - first + 1);
}
std::wstring environmentText(const RunSettings& settings) {
    std::wstring result;
    for (const auto& [key, value] : settings.environment) {
        if (!result.empty()) result += L'\n'; result += key + L"=" + value;
    }
    return result;
}
struct RunDraft { std::wstring target, arguments, workingDirectory, environment; };
constexpr int headerHeight = 43, footerHeight = 66, bodyHeight = 579;
constexpr auto smokeCmakeArguments = LR"(  -DDESIGNER_BUILD_TESTS=OFF "-DTEXT=Юникод и пробелы" -DEMPTY=  )";
class SettingsText final : public nana::textbox {
public:
    using nana::textbox::textbox;
    void scrollSpace(unsigned pixels) { get_drawer_trigger().editor()->scroll_space(pixels); }
    void scrollCorner(const nana::color& color) { get_drawer_trigger().editor()->scroll_corner_color(color); }
    void borderScrollbars() { get_drawer_trigger().editor()->keep_scrollbars_at_border(); }
    void rootRenderers(const Palette&, const double&);
    void contentArea(nana::rectangle area, unsigned pitch) {
        auto* editor = get_drawer_trigger().editor();
        const auto dimensions = size();
        const nana::rectangle bounds{2, 2, dimensions.width - 4, dimensions.height - 4};
        const unsigned left = static_cast<unsigned>(std::max(0, area.x - bounds.x));
        const unsigned right = static_cast<unsigned>(std::max(0, bounds.right() - area.right()));
        // editor_area accepts the outer rectangle and accounts for padding.
        // Do not remove and restore it: wrapped fields would reflow twice.
        editor->padding(0, right, 0, left);
        editor->editor_area(bounds);
        editor->line_height(pitch);
        const int naturalY = bounds.y + (multi_lines() ? 0 : std::max(0, (static_cast<int>(bounds.height) - static_cast<int>(pitch)) / 2));
        const int targetY = area.y + (multi_lines() ? 0 : std::max(0, (static_cast<int>(area.height) - static_cast<int>(pitch)) / 2));
        editor->text_y_offset(targetY - naturalY);
        editor->render(nana::api::focus_window() == handle());
    }
};
class SettingsChoice final : public nana::combox {
public:
    using nana::combox::combox;
    void filterKeys() { get_drawer_trigger().filter_event(nana::event_code::key_press, true); }
};
struct TextMetrics { int ascent{}, height{}, logicalHeight{}, logicalDescent{}; };
TextMetrics textMetrics(nana::paint::graphics& graph, double scale) {
    unsigned ascent{}, descent{}, leading{};
    graph.text_metrics(ascent, descent, leading);
    return {static_cast<int>(ascent), static_cast<int>(ascent + descent),
        static_cast<int>((ascent + descent) / scale), static_cast<int>(descent / scale)};
}
int textTop(nana::paint::graphics& graph, double scale, int y, int height) {
    const auto metrics = textMetrics(graph, scale);
    // FLTK centers in logical units and subtracts one logical unit from the
    // baseline when its scalable GDI driver is active (issue #1308).
    const int baseline = y + (height - metrics.logicalHeight) / 2 + metrics.logicalHeight - metrics.logicalDescent - (scale == 1 ? 0 : 1);
    return static_cast<int>(baseline * scale) - metrics.ascent;
}
void outline(nana::paint::graphics& graph, const nana::color& color, double scale) {
    const unsigned thickness = std::max(1u, static_cast<unsigned>(scale));
    for (unsigned inset = 0; inset < thickness && graph.width() > 2 * inset && graph.height() > 2 * inset; ++inset)
        graph.rectangle({static_cast<int>(inset), static_cast<int>(inset), graph.width() - 2 * inset, graph.height() - 2 * inset}, false, color);
}
void SettingsText::rootRenderers(const Palette& palette, const double& scale) {
    auto& renderers = get_drawer_trigger().editor()->customized_renderers();
    renderers.background = [this, &palette](nana::paint::graphics& canvas, const nana::rectangle&, const nana::color&) {
        canvas.rectangle(true, palette.surface);
        if (auto* graph = nana::api::dev::window_graphics(handle())) graph->rectangle(true, palette.surface);
    };
    renderers.border = [&palette, &scale](nana::paint::graphics& graph, const nana::color&) { outline(graph, palette.border, scale); };
}
void rounded(nana::paint::graphics& graph, const nana::rectangle& logical, int radius, double scale,
    int originX, int originY, const nana::color& fill, const nana::color& border, bool drawBorder) {
    constexpr double lut[]{0.0, .07612, .29289, .61732, 1.0};
    const double left = logical.x, top = logical.y, right = left + logical.width - 1, bottom = top + logical.height - 1;
    std::vector<platform::PaintPoint> points;
    points.reserve(20);
    // FLTK's Windows vertex buffer truncates to LONG before GDI+ scales the
    // path. Preserve that order so small rounded corners retain their shape.
    const auto point = [](double x, double y) { return platform::PaintPoint{static_cast<int>(x), static_cast<int>(y)}; };
    for (unsigned i = 0; i < 5; ++i) points.push_back(point(left + lut[i] * radius, top + lut[4 - i] * radius));
    for (unsigned i = 0; i < 5; ++i) points.push_back(point(right - lut[4 - i] * radius, top + lut[i] * radius));
    for (unsigned i = 0; i < 5; ++i) points.push_back(point(right - lut[i] * radius, bottom - lut[4 - i] * radius));
    for (unsigned i = 0; i < 5; ++i) points.push_back(point(left + lut[4 - i] * radius, bottom - lut[i] * radius));
    const auto rgb = [](const nana::color& color) { return color.px_color().value & 0xffffff; };
    platform::paintScaledPolygon(graph.context(), points, rgb(fill), rgb(fill), scale, originX, originY, true);
    if (drawBorder) platform::paintScaledPolygon(graph.context(), points, rgb(border), 0, scale, originX, originY, false);
}
std::wstring ellipsized(nana::paint::graphics& graph, std::wstring text, unsigned width) {
    if (graph.text_extent_size(text).width <= width) return text;
    const auto suffix = graph.text_extent_size(L"…").width;
    std::size_t lower{}, upper = text.size(), best{};
    while (lower < upper) {
        const auto middle = lower + (upper - lower + 1) / 2;
        auto cut = middle;
        if (cut && cut < text.size() && text[cut - 1] >= 0xD800 && text[cut - 1] <= 0xDBFF && text[cut] >= 0xDC00 && text[cut] <= 0xDFFF) --cut;
        if (graph.text_extent_size(std::wstring_view{text}.substr(0, cut)).width + suffix <= width) {
            best = std::max(best, cut); lower = middle;
        } else upper = middle - 1;
    }
    text.resize(best);
    return text + L"…";
}
class DialogButton final : public nana::element::element_interface {
public:
    DialogButton(const Palette& palette, const double& scale, bool primary, bool close, nana::rectangle logical = {})
        : palette_(palette), scale_(scale), primary_(primary), close_(close), logical_(logical) {}
    bool draw(graph_reference graph, const nana::color&, const nana::color&, const nana::rectangle& r, nana::element_state state) override {
        const auto px = [this](int value) { return static_cast<int>(value * scale_); };
        if (r.width <= static_cast<unsigned>(px(2)) || r.height <= static_cast<unsigned>(px(2))) return true;
        const bool enabled = state != nana::element_state::disabled;
        auto fill = primary_ && enabled ? palette_.accent : palette_.surface;
        if (enabled && !primary_ && (state == nana::element_state::hovered || state == nana::element_state::focus_hovered || state == nana::element_state::pressed)) fill = palette_.soft;
        if (state == nana::element_state::pressed) fill = fill.blend(palette_.foreground, .10);
        graph.rectangle(r, true, palette_.surface);
        const auto localX = [this, &px](int value) { return px(logical_.x + value) - px(logical_.x); };
        const auto localY = [this, &px](int value) { return px(logical_.y + value) - px(logical_.y); };
        const nana::rectangle inset = logical_.width ? nana::rectangle{localX(1), localY(1),
            static_cast<unsigned>(localX(static_cast<int>(logical_.width) - 1) - localX(1)), static_cast<unsigned>(localY(static_cast<int>(logical_.height) - 1) - localY(1))}
            : nana::rectangle{r.x + px(1), r.y + px(1), r.width - static_cast<unsigned>(px(2)), r.height - static_cast<unsigned>(px(2))};
        if (logical_.width) rounded(graph, {logical_.x + 1, logical_.y + 1, logical_.width - 2, logical_.height - 2}, 4,
            scale_, px(logical_.x), px(logical_.y), fill, palette_.border, !close_);
        else graph.round_rectangle(inset, px(4), px(4), close_ ? fill : palette_.border, true, fill);
        if (close_) {
            const int shift = state == nana::element_state::pressed ? 1 : 0;
            const int cx = static_cast<int>(logical_.width) / 2 + shift, cy = static_cast<int>(logical_.height) / 2 + shift;
            if (logical_.width) {
                const auto rgb = (enabled ? palette_.foreground : palette_.muted).px_color().value & 0xffffff;
                platform::paintScaledLines(graph.context(), {{logical_.x + cx - 5, logical_.y + cy - 5}, {logical_.x + cx + 5, logical_.y + cy + 5}}, rgb, scale_, px(logical_.x), px(logical_.y));
                platform::paintScaledLines(graph.context(), {{logical_.x + cx + 5, logical_.y + cy - 5}, {logical_.x + cx - 5, logical_.y + cy + 5}}, rgb, scale_, px(logical_.x), px(logical_.y));
            }
        }
        return true;
    }
private:
    const Palette& palette_; const double& scale_; bool primary_{}, close_{}; nana::rectangle logical_;
};
class ChoiceRenderer final : public nana::combox::item_renderer {
public:
    ChoiceRenderer(const Palette& palette, const double& scale) : palette_(palette), scale_(scale) {}
    void image(bool, unsigned) override {}
    void background(widget_reference, graph_reference graph) override {
        // Nana calls background after drawing every item: only paint the rim,
        // otherwise the completed dropdown rows are erased.
        graph.rectangle(false, palette_.border);
        graph.rectangle(nana::rectangle{graph.size()}.pare_off(1), false, palette_.surface);
    }
    void item(widget_reference, graph_reference graph, const nana::rectangle& rectangle, const item_interface* choice, state_t state) override {
        font(graph);
        graph.rectangle(rectangle, true, state == StateHighlighted ? palette_.accent : palette_.surface);
        const auto height = graph.text_extent_size(choice->text()).height;
        graph.string({rectangle.x + static_cast<int>(8 * scale_), rectangle.y + std::max(0, (static_cast<int>(rectangle.height) - static_cast<int>(height)) / 2)}, choice->text(), state == StateHighlighted ? (palette_.surface.r() > 128 ? nana::color{255,255,255} : nana::color{0,0,0}) : palette_.foreground);
    }
    unsigned item_pixels(graph_reference graph) const override {
        font(graph);
        return graph.text_extent_size(L"Mg").height + static_cast<unsigned>(std::lround(4 * scale_));
    }
private:
    const Palette& palette_; const double& scale_;
    void font(graph_reference graph) const {
        graph.typeface(nana::paint::font{"Segoe UI", 9.75, {}, static_cast<unsigned>(std::lround(96 * scale_))});
    }
};
class SettingsDialog {
public:
    SettingsDialog(nana::form& owner, Controller& controller, Palette palette)
        : controller_(controller), original_(controller.state().settings), originalChosen_(controller.state().chosenTarget),
          scale_(platform::windowScale(owner.native_handle())), palette_(std::move(palette)),
          form_(owner.handle(), geometry(owner), nana::appearance(false, false, true, false, false, false, false)),
          title_(form_, L"Настройки CMakeBuild"), close_(form_), viewport_(form_), content_(viewport_.handle()), scrollbar_(form_.handle(), {}),
          cmakeLabel_(content_, L"CMake.exe"), cmake_(content_), cmakeBrowse_(content_, L"…"),
          cmakeArgsLabel_(content_, L"Параметры CMake"), cmakeArguments_(content_),
          directoryLabel_(content_, L"Каталог сборки"), directory_(content_), directoryBrowse_(content_, L"…"),
          configLabel_(content_, L"Конфигурация"), configuration_(content_),
          compilerLabel_(content_, L"Компилятор"), compiler_(content_),
          buildTargetLabel_(content_, L"Цель сборки"), buildTarget_(content_),
          runTargetLabel_(content_, L"Цель запуска"), runTarget_(content_), tests_(content_, L"Собирать тесты"),
          argsLabel_(content_, L"Аргументы"), arguments_(content_),
          workingLabel_(content_, L"Рабочая папка"), workingDirectory_(content_), workingBrowse_(content_, L"…"),
          envLabel_(content_, L"Окружение"), environment_(content_), tools_(content_),
          save_(form_, L"Сохранить"), cancel_(form_, L"Отмена"), themeTimer_(std::chrono::milliseconds{250}) {
        form_.caption(L"Настройки CMakeBuild");
        platform::configureDialogWindow(form_.native_handle(), owner.native_handle());
        const auto bounds = geometry(owner);
        form_.size(bounds.dimension());
        directory_.caption(original_.buildDirectory); configuration_.caption(original_.configuration);
        cmake_.caption(original_.cmakeExecutable); cmakeArguments_.caption(original_.cmakeArguments); buildTarget_.caption(original_.target);
        for (auto* field : {&directory_, &configuration_, &cmake_, &cmakeArguments_, &buildTarget_, &arguments_, &workingDirectory_}) field->multi_lines(false);
        hint(directory_, "Пусто — build-cmakebuild рядом с проектом");
        hint(cmake_, "Пусто — найти CMake автоматически"); hint(buildTarget_, "Пусто — собрать все цели");
        hint(cmakeArguments_, "Дополнительные параметры конфигурации, например -DDESIGNER_BUILD_TESTS=OFF; значения с пробелами заключайте в двойные кавычки");
        hint(configuration_, "Release, Debug, RelWithDebInfo или MinSizeRel");
        hint(arguments_, "Аргументы запуска; пути с пробелами заключайте в двойные кавычки");
        hint(workingDirectory_, "Пусто — папка EXE; относительный путь — от папки проекта");
        hint(environment_, "Переменные запуска: ИМЯ=значение, по одной строке. PATH заменяет автоматически найденные пути");
        directoryBrowse_.tooltip("Выбрать каталог сборки"); cmakeBrowse_.tooltip("Выбрать CMake.exe");
        workingBrowse_.tooltip("Выбрать рабочую папку запуска"); close_.tooltip("Закрыть настройки (Escape)");
        tests_.tooltip("Добавить тестовые цели в сборку; сами тесты не запускаются");
        compiler_.editable(false);
        choiceRenderer_ = std::make_unique<ChoiceRenderer>(palette_, scale_);
        compiler_.renderer(choiceRenderer_.get()); runTarget_.renderer(choiceRenderer_.get());
        for (const auto& mode : {"Автоматически", "MSVC", "MinGW", "Текущее окружение PATH"}) compiler_.push_back(mode);
        compiler_.option(static_cast<std::size_t>(original_.compiler)); tests_.check(original_.buildTests);
        const bool missingChosen = !originalChosen_.empty() && std::ranges::none_of(controller_.state().targets, [this](const Target& target) { return target.name == originalChosen_; });
        const auto placeholder = missingChosen ? originalChosen_ + L" (EXE пока не собран)"
            : controller_.state().targets.empty() ? std::wstring{L"Будет найдена после сборки"} : std::wstring{L"Выбрать стрелкой на кнопке"};
        runTarget_.editable(false); runTarget_.push_back(platform::utf8(placeholder)); targetNames_.push_back(missingChosen ? originalChosen_ : L"");
        for (const auto& target : controller_.state().targets) { targetNames_.push_back(target.name); runTarget_.push_back(platform::utf8(target.name)); }
        auto selected = std::ranges::find(targetNames_, originalChosen_);
        runTarget_.option(selected == targetNames_.end() ? 0 : static_cast<std::size_t>(selected - targetNames_.begin()));
        selectedTarget_ = targetNames_[runTarget_.option()]; loadDraft(); runTarget_.enabled(!controller_.state().targets.empty());
        environment_.multi_lines(true); environment_.set_undo_queue_length(50);
        tools_.borderScrollbars();
        tools_.multi_lines(true).editable(false).line_wrapped(true); tools_.caption(describeDetectedTools());
        tools_.tooltip("Найденные CMake, компиляторы и установленные наборы Qt");
        for (auto* b : {&directoryBrowse_, &cmakeBrowse_, &workingBrowse_, &save_, &cancel_, &close_}) {
            b->set_bground(nana::pat::cloneable<nana::element::element_interface>{DialogButton{palette_, scale_, b == &save_, b == &close_}});
            b->edge_effects(false); b->enable_focus_color(false);
            b->drawing([this, b](nana::paint::graphics& graph) {
                const auto state = nana::api::element_state(*b);
                DialogButton{palette_, scale_, b == &save_, b == &close_, buttonLogical(*b)}.draw(graph, palette_.surface, palette_.foreground, nana::rectangle{graph.size()}, state);
                const auto foreground = !b->enabled() ? palette_.muted : b == &save_ ? palette_.accentText : palette_.foreground;
                if (b != &close_) {
                    graph.typeface(b->typeface()); const auto caption = b->caption_wstring(); const auto extent = graph.text_extent_size(caption);
                    const auto logical = buttonLogical(*b); const int shift = state == nana::element_state::pressed ? 1 : 0;
                    const int textWidth = static_cast<int>(std::lround(extent.width / scale_));
                    graph.string({px(logical.x + 4 + (static_cast<int>(logical.width) - 8 - textWidth) / 2 + shift) - px(logical.x),
                        textTop(graph, scale_, logical.y + shift, static_cast<int>(logical.height)) - px(logical.y)}, caption, foreground);
                }
                drawFocus(*b, graph, foreground);
            });
            b->events().key_press([b](const nana::arg_keyboard& arg) {
                if (arg.key == nana::keyboard::enter && !arg.alt && !arg.ctrl && !arg.shift && b->enabled()) {
                    nana::arg_click click; click.window_handle = b->handle(); b->events().click.emit(click, b->handle());
                }
            });
        }
        close_.events().click([this] { form_.close(); });
        form_.drawing([this](nana::paint::graphics& graph) {
            outline(graph, palette_.border, scale_);
            const int logicalWidth = static_cast<int>(std::lround(graph.width() / scale_));
            platform::paintScaledLines(graph.context(), {{0, headerHeight}, {logicalWidth, headerHeight}}, palette_.border.px_color().value & 0xffffff, scale_, 0, 0);
            const int footerY = static_cast<int>(std::lround(graph.height() / scale_)) - footerHeight;
            platform::paintScaledLines(graph.context(), {{0, footerY}, {logicalWidth, footerY}}, palette_.border.px_color().value & 0xffffff, scale_, 0, 0);
            for (const auto& point : {nana::point{16, 23}, nana::point{23, 16}, nana::point{23, 23}})
                graph.rectangle(rect(point.x, point.y, 5, 5), true, palette_.accent);
        });
        for (const auto& [label, y] : std::initializer_list<std::pair<nana::label*, int>>{
            {&title_, 9}, {&cmakeLabel_, 61}, {&cmakeArgsLabel_, 100}, {&directoryLabel_, 139}, {&configLabel_, 178}, {&compilerLabel_, 217},
            {&buildTargetLabel_, 256}, {&runTargetLabel_, 295}, {&argsLabel_, 376}, {&workingLabel_, 415}, {&envLabel_, 454}}) {
            label->drawing([this, label, y](nana::paint::graphics& graph) {
                graph.rectangle(true, palette_.surface); graph.typeface(label->typeface());
                graph.string({0, textTop(graph, scale_, y, label == &title_ ? 28 : 26) - px(y)}, label->caption_wstring(), palette_.foreground);
            });
        }
        for (auto* field : {&cmake_, &cmakeArguments_, &directory_, &configuration_, &buildTarget_, &arguments_, &workingDirectory_, &environment_, &tools_}) {
            field->rootRenderers(palette_, scale_);
            field->enable_border_focused(false);
            field->drawing([this, field](nana::paint::graphics& graph) {
                if (!field->enabled()) {
                    // Nana hardcodes an inactive gray surface. Retain the FLTK
                    // palette while leaving editor selection and scroll data intact.
                    graph.rectangle(true, palette_.surface); graph.typeface(field->typeface());
                    const auto caption = field->caption_wstring(); const auto foreground = palette_.foreground.blend(palette_.soft, .67);
                    const int lineHeight = static_cast<int>(field->line_pixels());
                    const auto area = field->text_area(); int y = area.y;
                    std::size_t start{};
                    while (start < caption.size() && y < static_cast<int>(graph.height())) {
                        const auto end = caption.find(L'\n', start);
                        graph.string({area.x, y}, std::wstring_view{caption}.substr(start, end == std::wstring::npos ? end : end - start), foreground);
                        if (end == std::wstring::npos) break;
                        start = end + 1; y += lineHeight;
                    }
                }
                outline(graph, palette_.border, scale_);
            });
        }
        for (auto* choice : {&compiler_, &runTarget_}) {
            choice->drawing([this, choice](nana::paint::graphics& graph) { drawChoice(*choice, graph); });
            choice->events().mouse_enter([choice] { choice->tooltip(platform::utf8(choice->caption_wstring())); });
            choice->filterKeys();
            choice->events().key_press([this, choice](const nana::arg_keyboard& arg) {
                const bool open = !arg.ctrl && !arg.shift && (((arg.key == nana::keyboard::enter || arg.key == 0x73) && !arg.alt) || arg.key == nana::keyboard::os_arrow_down);
                if (open && choice->enabled()) {
                    if (smoking_) return;
                    nana::arg_mouse mouse{}; mouse.evt_code = nana::event_code::mouse_down; mouse.window_handle = choice->handle();
                    mouse.pos = {static_cast<int>(choice->size().width) - px(14), static_cast<int>(choice->size().height) / 2};
                    mouse.button = nana::mouse::left_button; mouse.left_button = true;
                    nana::api::emit_internal_event(nana::event_code::mouse_down, choice->handle(), mouse);
                } else nana::api::emit_internal_event(nana::event_code::key_press, choice->handle(), arg);
            });
        }
        tests_.drawing([this](nana::paint::graphics& graph) { drawCheck(graph); });
        tests_.events().key_press([this](const nana::arg_keyboard& arg) {
            if (arg.key == nana::keyboard::enter && !arg.alt && !arg.ctrl && !arg.shift && tests_.enabled()) tests_.check(!tests_.checked());
        });
        directoryBrowse_.events().click([this] { browse(directory_, true, L"Выберите каталог сборки"); });
        cmakeBrowse_.events().click([this] { browse(cmake_, false, L"Выберите CMake.exe", L"CMake", L"cmake.exe"); });
        workingBrowse_.events().click([this] { if (!selectedTarget_.empty()) browse(workingDirectory_, true, L"Выберите рабочую папку запуска"); });
        runTarget_.events().selected([this] { storeDraft(); selectedTarget_ = targetNames_[runTarget_.option()]; loadDraft(); });
        save_.events().click([this] { save(); }); cancel_.events().click([this] { form_.close(); });
        scrollbar_.events().value_changed([this] { content_.move(0, -static_cast<int>(scrollbar_.value())); });
        auto wheel = [this](const nana::arg_wheel& a) {
            const auto step = static_cast<std::size_t>(px(58));
            const auto current = scrollbar_.value();
            scrollbar_.value(a.upwards ? (current > step ? current - step : 0) : current + step);
        };
        viewport_.events().mouse_wheel(wheel); content_.events().mouse_wheel(wheel);
        for (auto* w : std::initializer_list<nana::widget*>{&directory_, &configuration_, &cmake_, &cmakeArguments_, &compiler_, &buildTarget_, &tests_, &runTarget_, &arguments_, &workingDirectory_, &environment_, &directoryBrowse_, &cmakeBrowse_, &workingBrowse_}) {
            w->events().focus([this, w](const nana::arg_focus& a) { if (a.getting) reveal(*w); });
            w->events().key_press([this, w](const nana::arg_keyboard& a) {
                if (a.alt || a.ctrl || a.shift) return;
                if (a.key == nana::keyboard::escape) { form_.close(); return; }
                if (a.key == nana::keyboard::enter && w != &environment_ && w != &compiler_ && w != &runTarget_ && w != &tests_
                    && w != &directoryBrowse_ && w != &cmakeBrowse_ && w != &workingBrowse_) save();
            });
        }
        form_.events().key_press([this](const nana::arg_keyboard& a) { if (a.key == nana::keyboard::escape) form_.close(); });
        auto drag = std::make_shared<platform::WindowDrag>(); auto dragging = std::make_shared<bool>(false);
        const auto finishDrag = [this, dragging] {
            if (std::exchange(*dragging, false) && !title_.empty()) title_.release_capture();
        };
        dragCancelHandler_.bind(form_.native_handle(), finishDrag);
        form_.events().unload([finishDrag] { finishDrag(); });
        title_.events().mouse_down([this, drag, dragging](const nana::arg_mouse& a) {
            if (a.button == nana::mouse::left_button) { *dragging = platform::beginWindowDrag(form_.native_handle(), 0, *drag, scale_); if (*dragging) title_.set_capture(true); }
        });
        title_.events().mouse_move([this, drag, dragging, finishDrag](const nana::arg_mouse& a) {
            if (*dragging && (!a.left_button || !platform::updateWindowDrag(form_.native_handle(), *drag, 240, 200, 0, scale_))) finishDrag();
        });
        title_.events().mouse_up([finishDrag](const nana::arg_mouse& a) {
            if (a.button == nana::mouse::left_button) finishDrag();
        });
        form_.events().resized([this] { fitToWorkArea(); });
        dpiHandler_.bind(form_.native_handle(), [this, drag, dragging, finishDrag](double scale, platform::DesktopRect bounds) {
            if (previewScale_ > 0) return;
            fitting_ = true;
            const auto previousScale = scale_;
            scale_ = scale;
            const auto scrollPosition = static_cast<std::size_t>(scrollbar_.value() * scale_ / previousScale);
            const auto dimensions = fittedSize();
            bounds.width = static_cast<int>(dimensions.width);
            bounds.height = static_cast<int>(dimensions.height);
            if (*dragging) bounds = platform::windowDragDpiBounds(*drag, bounds, scale_);
            platform::setWindowBounds(form_.native_handle(), bounds);
            scrollbar_.value(scrollPosition);
            layout();
            fitting_ = false;
            nana::api::refresh_window_tree(form_);
            if (*dragging && !platform::rebaseWindowDrag(form_.native_handle(), *drag, scale_)) finishDrag();
        });
        platform::restoreNativePosition(form_.native_handle(), bounds.x, bounds.y);
        dark_ = platform::darkTheme(); fitToWorkArea(); applyTheme(palette_); cmake_.focus();
        themeTimer_.elapse([this] { const bool dark = platform::darkTheme(); if (dark != dark_) { dark_ = dark; applyTheme(Palette::system(dark)); } });
        themeTimer_.start();
    }
    ~SettingsDialog() { themeTimer_.stop(); if (!compiler_.empty()) compiler_.renderer(nullptr); if (!runTarget_.empty()) runTarget_.renderer(nullptr); }
    void show() { form_.show(); fitToWorkArea(); cmake_.focus(); form_.modality(); }
    nana::form& form() { return form_; }
    void applyTheme(Palette p) {
        nana::api::batch_updates(form_, [&] {
            palette_ = std::move(p);
            for (auto* w : std::initializer_list<nana::widget*>{&form_, &viewport_, &content_, &title_, &directoryLabel_, &configLabel_, &cmakeLabel_, &cmakeArgsLabel_, &compilerLabel_, &buildTargetLabel_, &runTargetLabel_, &argsLabel_, &workingLabel_, &envLabel_, &tests_, &directoryBrowse_, &cmakeBrowse_, &workingBrowse_, &save_, &cancel_, &close_}) colorWidget(*w, palette_, true);
            for (auto* w : std::initializer_list<nana::widget*>{&directory_, &configuration_, &cmake_, &cmakeArguments_, &buildTarget_, &arguments_, &workingDirectory_, &environment_, &tools_, &compiler_, &runTarget_}) colorWidget(*w, palette_, true);
            for (auto* field : {&directory_, &configuration_, &cmake_, &cmakeArguments_, &buildTarget_, &arguments_, &workingDirectory_, &environment_, &tools_}) {
                field->scrollCorner(palette_.scrollTrack);
                field->scheme().selection = palette_.accent;
                field->scheme().selection_unfocused = field == &tools_ ? palette_.accent.blend(palette_.surface, .4) : palette_.surface;
                field->scheme().selection_text = palette_.surface.r() > 128 ? nana::color{255,255,255} : nana::color{0,0,0};
            }
            tests_.scheme().square_border_color = palette_.border; tests_.scheme().square_bgcolor = palette_.surface;
            save_.fgcolor(palette_.accentText);
            for (auto* field : {&environment_, &tools_}) styleScrollbars(*field, palette_, scale_);
            styleScrollbars(scrollbar_, palette_, scale_);
            platform::setWindowTheme(form_.native_handle(), dark_);
            nana::api::refresh_window_tree(form_);
            nana::api::update_window(form_);
        });
    }
    void popupSmoke(const std::wstring& directory) {
        smoking_ = true; themeTimer_.stop();
        if (compiler_.the_number_of_options() != 4 || targetNames_.size() < 3)
            throw std::runtime_error("settings dropdown fixtures are incomplete");
        for (double scale : {1.0, 1.25, 1.5, 2.0}) {
            previewScale_ = scale; fitToWorkArea(); preparePreview(form_);
            for (bool dark : {false, true}) {
                dark_ = dark; applyTheme(Palette::system(dark));
                for (auto* choice : {&compiler_, &runTarget_}) {
                    for (std::size_t selection = 0; selection < choice->the_number_of_options(); ++selection) {
                        nana::arg_mouse mouse{};
                        mouse.window_handle = choice->handle(); mouse.button = nana::mouse::left_button;
                        mouse.left_button = true;
                        mouse.pos = {static_cast<int>(choice->size().width) - px(14), static_cast<int>(choice->size().height) / 2};
                        mouse.evt_code = nana::event_code::mouse_down;
                        nana::api::emit_internal_event(mouse.evt_code, choice->handle(), mouse);
                        const auto popupHandle = nana::api::capture_window();
                        auto* popup = dynamic_cast<nana::float_listbox*>(nana::api::get_widget(popupHandle));
                        if (!popup) throw std::runtime_error("settings dropdown did not open");
                        try {
                            if (popup->length() != choice->the_number_of_options())
                                throw std::runtime_error("settings dropdown lost options");
                            nana::paint::graphics metrics{nana::size{1, 1}}; metrics.typeface(choice->typeface());
                            const auto pitch = metrics.text_extent_size(L"Mg").height + static_cast<unsigned>(px(4));
                            if (popup->size().height != popup->length() * pitch + 4)
                                throw std::runtime_error("settings dropdown initial height does not match its DPI font");
                            nana::paint::graphics image;
                            if (!nana::api::window_graphics(popupHandle, image))
                                throw std::runtime_error("settings dropdown could not be captured");
                            nana::paint::pixel_buffer pixels{image.handle(), nana::rectangle{image.size()}};
                            const auto surface = palette_.surface.px_color().value & 0xffffff;
                            for (std::size_t row = 0; row < popup->length(); ++row) {
                                if (popup->text(row) != choice->text(row))
                                    throw std::runtime_error("settings dropdown caption differs from its option");
                                bool ink = false;
                                // Inspect row interiors, excluding borders and the scrollbar.
                                for (unsigned y = 3 + static_cast<unsigned>(row) * pitch; y < 1 + (row + 1) * pitch; ++y)
                                    for (unsigned x = 2 + static_cast<unsigned>(px(8)); x + 3 < image.width(); ++x)
                                        ink |= (pixels.raw_ptr(y)[x].value & 0xffffff) != surface;
                                if (!ink) throw std::runtime_error("settings dropdown row text was erased");
                            }
                            if (selection == 0) {
                                const auto name = std::wstring{choice == &compiler_ ? L"compiler-" : L"run-target-"}
                                    + (dark ? L"dark-" : L"light-") + std::to_wstring(static_cast<int>(scale * 100)) + L".bmp";
                                image.save_as_file(platform::utf8((std::filesystem::path(directory) / name).wstring()).c_str());
                            }
                            mouse.window_handle = popupHandle;
                            mouse.pos = {px(12), 2 + static_cast<int>(selection * pitch + pitch / 2)};
                            for (auto event : {nana::event_code::mouse_move, nana::event_code::mouse_down, nana::event_code::mouse_up}) {
                                mouse.evt_code = event; mouse.left_button = event != nana::event_code::mouse_up;
                                nana::api::emit_internal_event(event, popupHandle, mouse);
                            }
                            platform::drainPreviewMessages();
                            if (choice->option() != selection || platform::utf8(choice->caption_wstring()) != choice->text(selection))
                                throw std::runtime_error("settings dropdown selected the wrong row");
                            if (choice == &runTarget_ && selectedTarget_ != targetNames_[selection])
                                throw std::runtime_error("settings dropdown did not switch the run draft");
                        } catch (...) {
                            if (nana::api::is_window(popupHandle)) nana::api::close_window(popupHandle);
                            throw;
                        }
                    }
                }
            }
        }
        form_.close();
    }
    int smoke(const std::wstring& directory, bool commit = false) {
        smoking_ = true; themeTimer_.stop();
        if (cmakeArguments_.multi_lines() || cmakeArguments_.caption_wstring() != original_.cmakeArguments)
            throw std::runtime_error("CMake argument field did not restore the saved single-line value");
        cmakeArguments_.caption(smokeCmakeArguments);
        const auto snapshot = controller_.state().settings.configuration;
        configuration_.caption(L"   ");
        if (save() || controller_.state().settings.configuration != snapshot
            || controller_.state().settings.cmakeArguments != original_.cmakeArguments)
            throw std::runtime_error("invalid settings changed the saved build configuration");
        configuration_.caption(original_.configuration);
        if (targetNames_.size() > 1) {
            runTarget_.option(1); selectedTarget_ = targetNames_[1]; loadDraft();
            arguments_.caption(L"\"Юникод и пробелы\" --flag"); environment_.caption(L"EMPTY=\nCustom=значение");
            storeDraft();
            if (targetNames_.size() > 2) { runTarget_.option(2); arguments_.caption(L"--beta"); environment_.caption(L"Beta=2"); storeDraft(); }
            runTarget_.option(1); selectedTarget_ = targetNames_[1]; loadDraft();
            if (arguments_.caption_wstring() != L"\"Юникод и пробелы\" --flag") throw std::runtime_error("run draft lost");
        }
        preparePreview(form_);
        fitToWorkArea();
        const auto expected = fittedSize();
        const auto actual = platform::clientArea(form_.native_handle());
        if (form_.size() != expected || actual.width != static_cast<int>(expected.width) || actual.height != static_cast<int>(expected.height))
            throw std::runtime_error("settings/native geometry mismatch");
        const auto work = platform::monitorWorkArea(form_.native_handle());
        if (actual.width > work.width || actual.height > work.height) throw std::runtime_error("settings exceeds monitor work area");
        const auto originalDpi = static_cast<unsigned>(std::lround(scale_ * 96));
        const auto input = arguments_.caption_wstring();
        const auto environment = environment_.caption_wstring();
        const auto focus = nana::api::focus_window();
        for (unsigned dpi : {120u, 192u, 144u, 96u, originalDpi}) {
            platform::dispatchPreviewDpiChange(form_.native_handle(), dpi, {-31000, -32000, 505, 699});
            platform::drainPreviewMessages();
            const auto dimensions = fittedSize();
            const auto native = platform::clientArea(form_.native_handle());
            if (form_.size() != dimensions || native.width != static_cast<int>(dimensions.width)
                || native.height != static_cast<int>(dimensions.height))
                throw std::runtime_error("settings DPI transition desynchronized native/Nana geometry");
            if (arguments_.caption_wstring() != input || environment_.caption_wstring() != environment
                || cmakeArguments_.caption_wstring() != smokeCmakeArguments
                || nana::api::focus_window() != focus)
                throw std::runtime_error("settings DPI transition changed input/focus");
            int x{}, y{};
            platform::captureNativePosition(form_.native_handle(), x, y);
            if (x != -31000 || y != -32000)
                throw std::runtime_error("settings DPI position incorrectly includes owner coordinates");
        }
        // Exercise the dialog's bound title callbacks without activating its
        // offscreen HWND or changing the user's cursor/button state.
        nana::arg_mouse dragMouse{};
        dragMouse.window_handle = title_.handle();
        dragMouse.button = nana::mouse::left_button;
        dragMouse.pos = {20, 20};
        for (const bool missedMouseUp : {false, true}) {
            if (!platform::preparePreviewDragWindow(form_.native_handle(), scale_, 60, 20))
                throw std::runtime_error("settings transparent drag fixture could not be prepared");
            dragMouse.evt_code = nana::event_code::mouse_down;
            dragMouse.left_button = true;
            title_.events().mouse_down.emit(dragMouse, title_.handle());
            if (nana::api::capture_window() != title_.handle())
                throw std::runtime_error("settings title did not capture a drag");
            platform::WindowDrag originalAnchor;
            if (!platform::beginWindowDrag(form_.native_handle(), 0, originalAnchor, scale_))
                throw std::runtime_error("settings title drag anchor could not be captured");
            for (unsigned dpi : {120u, 192u, 144u, 96u, originalDpi}) {
                platform::dispatchPreviewDpiChange(form_.native_handle(), dpi, {-31000, -32000, 505, 699});
                const auto dimensions = platform::clientArea(form_.native_handle());
                const auto anchoredBounds = platform::windowDragDpiBounds(originalAnchor, dimensions, dpi / 96.0);
                int x{}, y{};
                platform::captureNativePosition(form_.native_handle(), x, y);
                if (x != anchoredBounds.x || y != anchoredBounds.y)
                    throw std::runtime_error("settings DPI transition changed the originally grabbed title point; actual=("
                        + std::to_string(x) + ',' + std::to_string(y) + ") expected=("
                        + std::to_string(anchoredBounds.x) + ',' + std::to_string(anchoredBounds.y)
                        + ") DPI=" + std::to_string(dpi));
                dragMouse.evt_code = nana::event_code::mouse_move;
                title_.events().mouse_move.emit(dragMouse, title_.handle());
                platform::captureNativePosition(form_.native_handle(), x, y);
                if (x != anchoredBounds.x || y != anchoredBounds.y)
                    throw std::runtime_error("settings drag jumped on the first unchanged-pointer move after DPI");
            }
            if (nana::api::capture_window() != title_.handle())
                throw std::runtime_error("settings title lost drag capture at a DPI transition");
            dragMouse.left_button = false;
            dragMouse.evt_code = missedMouseUp ? nana::event_code::mouse_move : nana::event_code::mouse_up;
            if (missedMouseUp) title_.events().mouse_move.emit(dragMouse, title_.handle());
            else title_.events().mouse_up.emit(dragMouse, title_.handle());
            if (nana::api::capture_window() == title_.handle())
                throw std::runtime_error("settings title drag remained captured after button release");
            int beforeX{}, beforeY{}, afterX{}, afterY{};
            platform::captureNativePosition(form_.native_handle(), beforeX, beforeY);
            dragMouse.evt_code = nana::event_code::mouse_move;
            title_.events().mouse_move.emit(dragMouse, title_.handle());
            platform::captureNativePosition(form_.native_handle(), afterX, afterY);
            if (beforeX != afterX || beforeY != afterY || arguments_.caption_wstring() != input
                || cmakeArguments_.caption_wstring() != smokeCmakeArguments
                || environment_.caption_wstring() != environment)
                throw std::runtime_error("settings moved or changed input after ending a title drag");
        }
        std::uint64_t lightHash{};
        for (bool dark : {false, true}) {
            const auto editedArguments = arguments_.caption_wstring();
            const auto editedEnvironment = environment_.caption_wstring();
            dark_ = dark; applyTheme(Palette::system(dark));
            if (arguments_.caption_wstring() != editedArguments || environment_.caption_wstring() != editedEnvironment
                || cmakeArguments_.caption_wstring() != smokeCmakeArguments)
                throw std::runtime_error("theme changed settings input");
            const auto imagePath = std::filesystem::path(directory) / (dark ? L"settings-dark.bmp" : L"settings-light.bmp");
            const auto hash = savePreview(form_, imagePath.wstring());
            if (!dark) lightHash = hash;
            else if (hash == lightHash) throw std::runtime_error("light and dark settings renders are identical");
        }
        if (commit && !save()) throw std::runtime_error("valid settings could not be saved");
        return 0;
    }
    int preview(const std::wstring& directory, double scale) {
        smoking_ = true; themeTimer_.stop(); previewScale_ = scale;
        fitToWorkArea(); preparePreview(form_); fitToWorkArea(); cmake_.focus();
        const auto expected = fittedSize();
        const auto native = platform::clientArea(form_.native_handle());
        if (form_.size() != expected || native.width != static_cast<int>(expected.width) || native.height != static_cast<int>(expected.height))
            throw std::runtime_error("settings synthetic DPI/native geometry mismatch");
        for (bool dark : {false, true}) {
            dark_ = dark; applyTheme(Palette::system(dark));
            const auto suffix = std::to_wstring(static_cast<int>(std::lround(scale * 100)));
            savePreview(form_, (std::filesystem::path(directory) / ((dark ? L"settings-dark-" : L"settings-light-") + suffix + L".bmp")).wstring());
        }
        form_.close(); return 0;
    }
private:
    Controller& controller_;
    BuildSettings original_;
    std::wstring originalChosen_;
    double scale_{}, previewScale_{};
    Palette palette_;
    nana::form form_;
    nana::label title_;
    nana::button close_;
    nana::panel<true> viewport_, content_;
    nana::scroll<true> scrollbar_;
    nana::label cmakeLabel_; SettingsText cmake_; nana::button cmakeBrowse_;
    nana::label cmakeArgsLabel_; SettingsText cmakeArguments_;
    nana::label directoryLabel_; SettingsText directory_; nana::button directoryBrowse_;
    nana::label configLabel_; SettingsText configuration_;
    nana::label compilerLabel_; SettingsChoice compiler_;
    nana::label buildTargetLabel_; SettingsText buildTarget_;
    nana::label runTargetLabel_; SettingsChoice runTarget_; nana::checkbox tests_;
    nana::label argsLabel_; SettingsText arguments_;
    nana::label workingLabel_; SettingsText workingDirectory_; nana::button workingBrowse_;
    nana::label envLabel_; SettingsText environment_;
    SettingsText tools_;
    nana::button save_, cancel_;
    nana::timer themeTimer_;
    platform::DpiChangeHandler dpiHandler_;
    platform::WindowDragCancelHandler dragCancelHandler_;
    std::unique_ptr<ChoiceRenderer> choiceRenderer_;
    bool dark_{}, fitting_{}, smoking_{};
    std::vector<std::wstring> targetNames_;
    std::wstring selectedTarget_;
    std::vector<RunDraft> drafts_;
    static void hint(SettingsText& field, std::string text) {
        field.tooltip(text);
        field.events().mouse_enter([&field, text = std::move(text)] {
            const auto value = field.caption_wstring();
            field.tooltip(value.empty() ? text : text + "\n" + platform::utf8(value));
        });
    }
    static nana::rectangle geometry(nana::form& owner) {
        const auto work = platform::monitorWorkArea(owner.native_handle());
        const auto scale = platform::windowScale(owner.native_handle());
        const auto px = [scale](int value) { return static_cast<int>(value * scale); };
        const int width = std::min(px(505), std::max(px(300), work.width - px(16)));
        const int height = std::min(px(699), std::max(px(180), work.height - px(16)));
        int ownerX{}, ownerY{}; platform::captureNativePosition(owner.native_handle(), ownerX, ownerY);
        const auto ownerSize = platform::clientArea(owner.native_handle());
        const int x = std::clamp(ownerX + (ownerSize.width - width) / 2, work.x, std::max(work.x, work.x + work.width - width));
        const int y = std::clamp(ownerY + px(20), work.y, std::max(work.y, work.y + work.height - height));
        return {x, y, static_cast<unsigned>(width), static_cast<unsigned>(height)};
    }
    int px(int value) const { return static_cast<int>(value * scale_); }
    nana::rectangle rect(int x, int y, int width, int height) const {
        return {px(x), px(y), static_cast<unsigned>(px(x + width) - px(x)), static_cast<unsigned>(px(y + height) - px(y))};
    }
    nana::rectangle buttonLogical(const nana::button& button) const {
        const int width = static_cast<int>(std::lround(form_.size().width / scale_)), height = static_cast<int>(std::lround(form_.size().height / scale_));
        if (&button == &save_) return {width - 238, height - 48, 115, 30};
        if (&button == &cancel_) return {width - 113, height - 48, 95, 30};
        if (&button == &close_) return {width - 40, 9, 27, 27};
        return {width - 48, (&button == &cmakeBrowse_ ? 18 : &button == &directoryBrowse_ ? 96 : 372) + headerHeight, 30, 26};
    }
    nana::rectangle inputLogical(const SettingsText& field) const {
        const int width = static_cast<int>(std::lround(form_.size().width / scale_));
        if (&field == &tools_) return {18, 503 + headerHeight, static_cast<unsigned>(width - 36), 76};
        const int x = width < 420 ? 130 : 158;
        const int y = &field == &cmake_ ? 18 : &field == &cmakeArguments_ ? 57 : &field == &directory_ ? 96 : &field == &configuration_ ? 135
            : &field == &buildTarget_ ? 213 : &field == &arguments_ ? 333 : &field == &workingDirectory_ ? 372 : 411;
        return {x, y + headerHeight, static_cast<unsigned>(width - x - 18 - ((&field == &cmake_ || &field == &directory_ || &field == &workingDirectory_) ? 37 : 0)),
            &field == &environment_ ? 78u : 26u};
    }
    void configureTextAreas(const nana::paint::font& font) {
        nana::paint::graphics metricsGraph{nana::size{1, 1}}; metricsGraph.typeface(font);
        const auto metrics = textMetrics(metricsGraph, scale_);
        const auto pitch = static_cast<unsigned>(std::max(1, px(metrics.logicalHeight)));
        for (auto* field : {&cmake_, &cmakeArguments_, &directory_, &configuration_, &buildTarget_, &arguments_, &workingDirectory_, &environment_, &tools_}) {
            const auto logical = inputLogical(*field);
            const bool tools = field == &tools_, multiLine = tools || field == &environment_;
            const int left = px(logical.x + (tools ? 4 : 2)) - px(logical.x);
            const int right = px(logical.x + static_cast<int>(logical.width) - (tools ? 4 : 1)) - px(logical.x);
            const int textY = textTop(metricsGraph, scale_, logical.y + (tools ? 2 : 1),
                multiLine ? metrics.logicalHeight : static_cast<int>(logical.height) - 2) - px(logical.y);
            const int areaHeight = multiLine ? px(logical.y + static_cast<int>(logical.height) - (tools ? 2 : 1)) - px(logical.y) - textY
                : std::max(static_cast<int>(pitch), static_cast<int>(field->size().height) - 4);
            const int top = multiLine ? textY : textY - (areaHeight - static_cast<int>(pitch)) / 2;
            field->contentArea({left, top, static_cast<unsigned>(std::max(1, right - left)), static_cast<unsigned>(std::max(1, areaHeight))}, pitch);
        }
    }
    nana::size fittedSize() const {
        if (previewScale_ > 0) return {static_cast<unsigned>(505 * previewScale_), static_cast<unsigned>(699 * previewScale_)};
        const auto work = platform::monitorWorkArea(form_.native_handle());
        return {static_cast<unsigned>(std::min(px(505), std::max(px(300), work.width - px(16)))),
            static_cast<unsigned>(std::min(px(699), std::max(px(180), work.height - px(16))))};
    }
    void fitToWorkArea() {
        if (fitting_) return;
        fitting_ = true;
        if (previewScale_ > 0) scale_ = previewScale_;
        const auto dimensions = fittedSize();
        if (form_.size() != dimensions) form_.size(dimensions);
        layout(); fitting_ = false;
    }
    void layout() {
        const int width = static_cast<int>(form_.size().width), height = static_cast<int>(form_.size().height);
        const int logicalWidth = static_cast<int>(std::lround(width / scale_)), logicalHeight = static_cast<int>(std::lround(height / scale_));
        const int inputX = logicalWidth < 420 ? 130 : 158, fieldWidth = logicalWidth - inputX - 18;
        const auto viewportBounds = rect(1, headerHeight + 1, logicalWidth - 2, std::max(1, logicalHeight - headerHeight - 67));
        const int viewHeight = static_cast<int>(viewportBounds.height);
        const int contentHeight = px(headerHeight + 1 + bodyHeight) - px(headerHeight + 1);
        const bool scrollable = viewHeight < contentHeight;
        const auto box = [&](nana::widget& widget, int x, int y, int w, int h) {
            auto bounds = rect(x, y + headerHeight, w, h);
            bounds.x -= viewport_.pos().x; bounds.y -= viewport_.pos().y;
            widget.move(bounds);
        };
        title_.move(rect(39, 9, logicalWidth - 92, 28));
        title_.text_align(nana::align::left, nana::align_v::center);
        close_.move(rect(logicalWidth - 40, 9, 27, 27));
        viewport_.move(rect(1, headerHeight + 1, logicalWidth - 2 - (scrollable ? 16 : 0), std::max(1, logicalHeight - headerHeight - 67)));
        content_.move({0, -static_cast<int>(scrollbar_.value()), viewportBounds.width, static_cast<unsigned>(contentHeight)});
        scrollbar_.move(rect(logicalWidth - 17, headerHeight + 1, 16, std::max(1, logicalHeight - headerHeight - 67)));
        scrollbar_.amount(static_cast<std::size_t>(contentHeight)); scrollbar_.range(viewport_.size().height); scrollbar_.step(px(28));
        if (scrollable) scrollbar_.show(); else scrollbar_.hide();
        if (!scrollable) { scrollbar_.value(0); content_.move(0, 0); }
        const std::pair<nana::label*, int> labels[] = {{&cmakeLabel_, 18}, {&cmakeArgsLabel_, 57}, {&directoryLabel_, 96}, {&configLabel_, 135}, {&compilerLabel_, 174}, {&buildTargetLabel_, 213}, {&runTargetLabel_, 252}, {&argsLabel_, 333}, {&workingLabel_, 372}, {&envLabel_, 411}};
        for (const auto& [label, y] : labels) { box(*label, 21, y, inputX - 23, 26); label->text_align(nana::align::left, nana::align_v::center); }
        const std::pair<SettingsText*, int> fields[] = {{&cmake_, 18}, {&cmakeArguments_, 57}, {&directory_, 96}, {&configuration_, 135}, {&buildTarget_, 213}, {&arguments_, 333}, {&workingDirectory_, 372}};
        for (const auto& [field, y] : fields) box(*field, inputX, y, fieldWidth - ((field == &directory_ || field == &cmake_ || field == &workingDirectory_) ? 37 : 0), 26);
        box(compiler_, inputX, 174, fieldWidth, 26); box(runTarget_, inputX, 252, fieldWidth, 26);
        box(tests_, inputX, 291, fieldWidth, 26);
        box(environment_, inputX, 411, fieldWidth, 78); box(tools_, 18, 503, logicalWidth - 36, 76);
        for (const auto& [button, y] : std::initializer_list<std::pair<nana::button*, int>>{{&cmakeBrowse_, 18}, {&directoryBrowse_, 96}, {&workingBrowse_, 372}})
            box(*button, logicalWidth - 48, y, 30, 26);
        save_.move(rect(logicalWidth - 238, logicalHeight - 48, 115, 30));
        cancel_.move(rect(logicalWidth - 113, logicalHeight - 48, 95, 30));
        const nana::paint::font font{"Segoe UI", 9.75, {}, static_cast<std::size_t>(std::lround(scale_ * 96))};
        for (auto* w : std::initializer_list<nana::widget*>{&title_, &directoryLabel_, &configLabel_, &cmakeLabel_, &cmakeArgsLabel_, &compilerLabel_, &buildTargetLabel_, &runTargetLabel_, &argsLabel_, &workingLabel_, &envLabel_, &directory_, &configuration_, &cmake_, &cmakeArguments_, &compiler_, &buildTarget_, &tests_, &runTarget_, &arguments_, &workingDirectory_, &environment_, &tools_, &directoryBrowse_, &cmakeBrowse_, &workingBrowse_, &save_, &cancel_, &close_}) w->typeface(font);
        configureTextAreas(font);
        for (auto* field : {&environment_, &tools_}) { field->scrollSpace(px(16)); styleScrollbars(*field, palette_, scale_); }
        styleScrollbars(scrollbar_, palette_, scale_);
    }
    void drawFocus(nana::widget& widget, nana::paint::graphics& graph, const nana::color& color) const {
        if (nana::api::focus_window() != widget.handle()) return;
        nana::rectangle logical;
        if (const auto* button = dynamic_cast<const nana::button*>(&widget)) logical = buttonLogical(*button);
        else {
            const int width = static_cast<int>(std::lround(form_.size().width / scale_));
            const int x = width < 420 ? 130 : 158;
            logical = {x, (&widget == &compiler_ ? 174 : &widget == &runTarget_ ? 252 : 291) + headerHeight, static_cast<unsigned>(width - x - 18), 26};
        }
        platform::paintFocusRectangle(graph.context(), logical.x + 4, logical.y + 4,
            static_cast<int>(logical.width) - 8, static_cast<int>(logical.height) - 8, color.px_color().value & 0xffffff,
            scale_, px(logical.x), px(logical.y));
    }
    void drawChoice(nana::combox& choice, nana::paint::graphics& graph) {
        graph.typeface(choice.typeface()); graph.rectangle(true, palette_.surface); outline(graph, palette_.border, scale_);
        const auto foreground = choice.enabled() ? palette_.foreground : palette_.muted;
        const auto caption = ellipsized(graph, choice.caption_wstring(), graph.width() > static_cast<unsigned>(px(36)) ? graph.width() - px(36) : 0);
        const int dialogWidth = static_cast<int>(std::lround(form_.size().width / scale_));
        const int x = dialogWidth < 420 ? 130 : 158, y = (&choice == &compiler_ ? 174 : 252) + headerHeight;
        const int width = dialogWidth - x - 18;
        const auto point = [this, x, y](int localX, int localY) { return nana::point{px(x + localX) - px(x), px(y + localY) - px(y)}; };
        graph.string({point(7, 0).x, textTop(graph, scale_, y, 26) - px(y)}, caption, foreground);
        platform::paintScaledLines(graph.context(), {{x + width - 27, y + 6}, {x + width - 27, y + 19}}, palette_.border.px_color().value & 0xffffff, scale_, px(x), px(y));
        const int cx = width - 14, cy = 13;
        platform::paintScaledLines(graph.context(), {{x + cx - 4, y + cy - 2}, {x + cx, y + cy + 2}, {x + cx + 4, y + cy - 2}}, foreground.px_color().value & 0xffffff, scale_, px(x), px(y));
        drawFocus(choice, graph, foreground);
    }
    void drawCheck(nana::paint::graphics& graph) {
        graph.typeface(tests_.typeface()); graph.rectangle(true, palette_.surface);
        const bool enabled = tests_.enabled(), checked = tests_.checked();
        const int dialogWidth = static_cast<int>(std::lround(form_.size().width / scale_));
        const int widgetX = dialogWidth < 420 ? 130 : 158, widgetY = 291 + headerHeight;
        const int boxX = widgetX + 2, boxY = widgetY + 5;
        const auto foreground = enabled ? palette_.foreground : palette_.muted;
        const auto fill = enabled && checked ? palette_.accent : palette_.surface;
        rounded(graph, {boxX, boxY, 16, 16}, 3, scale_, px(widgetX), px(widgetY), fill, enabled && checked ? palette_.accent : palette_.border, true);
        if (checked) {
            const auto checkColor = enabled ? palette_.accentText : palette_.muted;
            platform::paintScaledPolygon(graph.context(), std::vector<platform::PaintPoint>{{boxX + 3, boxY + 6}, {boxX + 6, boxY + 9}, {boxX + 12, boxY + 3},
                {boxX + 12, boxY + 6}, {boxX + 6, boxY + 12}, {boxX + 3, boxY + 9}}, 0, checkColor.px_color().value & 0xffffff, scale_, px(widgetX), px(widgetY), true);
        }
        const auto caption = tests_.caption_wstring();
        graph.string({px(widgetX + 25) - px(widgetX), textTop(graph, scale_, widgetY, 26) - px(widgetY)}, caption, foreground);
        drawFocus(tests_, graph, foreground);
    }
    void reveal(nana::widget& w) {
        const auto y = static_cast<std::size_t>(std::max(0, w.pos().y));
        const auto end = y + w.size().height;
        if (y < scrollbar_.value()) scrollbar_.value(y);
        else if (end > scrollbar_.value() + scrollbar_.range()) scrollbar_.value(end - scrollbar_.range());
    }
    void browse(nana::textbox& field, bool folder, const std::wstring& title, const std::wstring& filterName = L"", const std::wstring& filter = L"") {
        const auto file = platform::selectPath(form_.native_handle(), folder, title, filterName, filter);
        if (!file.empty()) field.caption(file);
    }
    void storeDraft() {
        if (selectedTarget_.empty()) return;
        auto draft = std::ranges::find(drafts_, selectedTarget_, &RunDraft::target);
        if (draft != drafts_.end()) { draft->arguments = arguments_.caption_wstring(); draft->workingDirectory = workingDirectory_.caption_wstring(); draft->environment = environment_.caption_wstring(); }
    }
    void loadDraft() {
        auto draft = std::ranges::find(drafts_, selectedTarget_, &RunDraft::target);
        if (!selectedTarget_.empty() && draft == drafts_.end()) {
            const auto run = controller_.state().runSettingsFor(selectedTarget_);
            drafts_.push_back({selectedTarget_, run.arguments, run.workingDirectory, environmentText(run)}); draft = drafts_.end() - 1;
        }
        arguments_.caption(draft == drafts_.end() ? L"" : draft->arguments);
        workingDirectory_.caption(draft == drafts_.end() ? L"" : draft->workingDirectory);
        environment_.caption(draft == drafts_.end() ? L"" : draft->environment);
        for (auto* w : {&arguments_, &workingDirectory_, &environment_}) w->enabled(!selectedTarget_.empty());
    }
    bool invalid(const std::wstring& message, nana::widget& field) {
        if (!smoking_) { nana::msgbox alert{form_.handle(), "CMakeBuild"}; alert << platform::utf8(message); alert.show(); }
        field.focus(); reveal(field); return false;
    }
    bool save() {
        auto settings = original_;
        settings.configuration = trim(configuration_.caption_wstring());
        if (settings.configuration.empty()) return invalid(L"Укажите конфигурацию сборки.", configuration_);
        settings.buildDirectory = directory_.caption_wstring(); settings.cmakeExecutable = cmake_.caption_wstring();
        settings.cmakeArguments = cmakeArguments_.caption_wstring();
        if (settings.cmakeArguments.find_first_of(L"\r\n\0", 0, 3) != std::wstring::npos)
            return invalid(L"Параметры CMake должны занимать одну строку.", cmakeArguments_);
        settings.target = trim(buildTarget_.caption_wstring()); settings.compiler = static_cast<CompilerMode>(compiler_.option()); settings.buildTests = tests_.checked();
        storeDraft(); std::vector<std::pair<std::wstring, RunSettings>> runs;
        for (const auto& draft : drafts_) {
            const auto invalidRun = [&](const std::wstring& message, nana::widget& field) {
                storeDraft();
                const auto target = std::ranges::find(targetNames_, draft.target);
                if (target != targetNames_.end()) {
                    const auto index = static_cast<std::size_t>(target - targetNames_.begin());
                    if (runTarget_.option() != index) runTarget_.option(index);
                    else selectedTarget_ = draft.target;
                }
                loadDraft(); return invalid(message, field);
            };
            if (draft.arguments.find_first_of(L"\r\n\0", 0, 3) != std::wstring::npos)
                return invalidRun(L"Аргументы запуска должны занимать одну строку.", arguments_);
            if (draft.workingDirectory.find_first_of(L"\r\n\0", 0, 3) != std::wstring::npos)
                return invalidRun(L"Рабочая папка должна занимать одну строку.", workingDirectory_);
            RunSettings run{draft.arguments, trim(draft.workingDirectory), {}};
            std::size_t offset{};
            while (offset < draft.environment.size()) {
                const auto end = draft.environment.find(L'\n', offset);
                auto line = draft.environment.substr(offset, end == std::wstring::npos ? end : end - offset);
                offset = end == std::wstring::npos ? draft.environment.size() : end + 1;
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                if (trim(line).empty()) continue;
                const auto equal = line.find(L'=');
                if (equal == std::wstring::npos) return invalidRun(L"Введите окружение в формате ИМЯ=значение, по одной переменной в строке.", environment_);
                auto name = trim(line.substr(0, equal)); auto value = line.substr(equal + 1);
                if (trim(name).empty() || name.find_first_of(L"\r\n\0", 0, 3) != std::wstring::npos || value.find(L'\0') != std::wstring::npos)
                    return invalidRun(L"Укажите непустое имя переменной окружения без переводов строк.", environment_);
                if (value.find_first_of(L"\r\n\0", 0, 3) != std::wstring::npos)
                    return invalidRun(L"Укажите непустое имя переменной окружения без переводов строк.", environment_);
                if (std::ranges::any_of(run.environment, [&](const auto& entry) { return _wcsicmp(entry.first.c_str(), name.c_str()) == 0; }))
                    return invalidRun(L"Имена переменных окружения не должны повторяться.", environment_);
                run.environment.emplace_back(std::move(name), std::move(value));
                if (run.environment.size() > 4096) return invalidRun(L"Можно сохранить не более 4096 переменных окружения для одной цели.", environment_);
            }
            runs.emplace_back(draft.target, std::move(run));
        }
        controller_.applySettings(std::move(settings), selectedTarget_, selectedTarget_ != originalChosen_, std::move(runs));
        form_.close(); return true;
    }
};
}
void showSettings(nana::form& owner, Controller& controller, const Palette& palette) {
    if (controller.operation() != Operation::Idle) return;
    SettingsDialog dialog{owner, controller, palette}; dialog.show();
}
int runSettingsPreviews(nana::form& owner, Controller& controller, const Palette& palette, const std::wstring& imageDirectory, double scale) {
    SettingsDialog dialog{owner, controller, palette};
    return dialog.preview(imageDirectory, scale);
}
int runSettingsSmoke(nana::form& owner, Controller& controller, const Palette& palette) {
    const auto directory = std::filesystem::path(controller.state().configFile).parent_path() / L"images";
    std::filesystem::create_directories(directory);
    auto readIni = [&] {
        std::ifstream file{std::filesystem::path(controller.state().configFile), std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{file}, {}};
    };
    const auto before = readIni();
    const auto originalCmakeArguments = controller.state().settings.cmakeArguments;
    {
        SettingsDialog dialog{owner, controller, palette}; dialog.popupSmoke(directory.wstring());
    }
    if (readIni() != before) throw std::runtime_error("dropdown selection changed INI before saving");
    {
        SettingsDialog dialog{owner, controller, palette};
        dialog.smoke(directory.wstring()); dialog.form().close();
    }
    if (readIni() != before || controller.state().settings.cmakeArguments != originalCmakeArguments)
        throw std::runtime_error("cancelled settings changed CMake arguments or INI");
    {
        SettingsDialog dialog{owner, controller, palette}; dialog.smoke(directory.wstring(), true);
    }
    const auto settings = controller.state().runSettingsFor(L"alpha & one");
    if (settings.arguments != L"\"Юникод и пробелы\" --flag" || settings.environment.size() != 2 || settings.environment[0].first != L"EMPTY" || !settings.environment[0].second.empty())
        throw std::runtime_error("launch settings did not save from Nana controls");
    if (controller.state().runSettingsFor(L"beta").arguments != L"--beta")
        throw std::runtime_error("second target draft was not saved");
    AppState restarted{controller.state().configFile}; restarted.load();
    if (controller.state().settings.cmakeArguments != smokeCmakeArguments
        || restarted.settings.cmakeArguments != smokeCmakeArguments)
        throw std::runtime_error("CMake arguments did not save exactly from Nana controls");
    return 0;
}
}
