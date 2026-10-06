#include "ui.hpp"
#include "platform.hpp"
#include "version.hpp"
#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>
#include <algorithm>
#include <climits>
#include <cmath>
#include <deque>
#include <filesystem>
#include <format>
#include <mutex>
#include <utility>

namespace cb {
namespace {
Fl_Color rgb(unsigned char r,unsigned char g,unsigned char b) { return fl_rgb_color(r,g,b); }
std::wstring durationText(std::chrono::milliseconds elapsed) {
    const auto hundredths=(std::max(elapsed.count(),std::chrono::milliseconds::rep{0})+5)/10;
    const auto seconds=hundredths/100, fraction=hundredths%100;
    if(seconds>=3600) return std::format(L"{} ч {:02} мин {:02},{:02} с",seconds/3600,seconds/60%60,seconds%60,fraction);
    if(seconds>=60) return std::format(L"{} мин {:02},{:02} с",seconds/60,seconds%60,fraction);
    return std::format(L"{},{:02} с",seconds,fraction);
}
std::string fitText(std::string value,int width,Fl_Font font,int size) {
    fl_font(font,size);
    if(fl_width(value.c_str())<=width) return value;
    const double available=width-fl_width("…");
    // Binary search keeps even unusually long Unicode target names inexpensive.
    size_t left=0,right=value.size();
    while(left<right) {
        const size_t middle=left+(right-left+1)/2;
        if(fl_width(value.c_str(),static_cast<int>(middle))<=available) left=middle;else right=middle-1;
    }
    while(left && (static_cast<unsigned char>(value[left])&0xc0)==0x80) --left;
    value.resize(left);value+="…";return value;
}
void ellipsis(std::string value,int x,int y,int w,int h,Fl_Font font,int size,Fl_Color color) {
    if(w<=0) return;
    value=fitText(std::move(value),w,font,size);
    fl_color(color);fl_draw(value.c_str(),x,y,w,h,FL_ALIGN_LEFT|FL_ALIGN_INSIDE|FL_ALIGN_CLIP,nullptr,0);
}
class PanelButton final : public Fl_Button {
    Panel& panel_;
    const Palette& colors_;
    Control control_;
    bool hover_=false;
public:
    PanelButton(Panel& p,const Palette& colors,Control c) : Fl_Button(0,0,1,1),panel_(p),colors_(colors),control_(c) { labelfont(appFont);labelsize(13);box(FL_FLAT_BOX); }
    int handle(int event) override {
        if((control_==Control::Run || control_==Control::RunMenu) && active_r() && panel_.operation()==Operation::Idle && event==FL_KEYDOWN
            && (Fl::event_key()==FL_Down || (Fl::event_key()==FL_F+4 && !Fl::event_alt()))) { panel_.showRunTargets();return 1; }
        if((control_==Control::Build || control_==Control::BuildMenu) && active_r() && event==FL_KEYDOWN
            && (Fl::event_key()==FL_Down || (Fl::event_key()==FL_F+4 && !Fl::event_alt()))) { panel_.showBuildActions();return 1; }
        if((control_==Control::Pick || control_==Control::PickMenu) && active_r() && event==FL_KEYDOWN
            && (Fl::event_key()==FL_Down || (Fl::event_key()==FL_F+4 && !Fl::event_alt()))) { panel_.showRecentProjects();return 1; }
        if(event==FL_ENTER || event==FL_LEAVE) {hover_=event==FL_ENTER;redraw();}
        if(event==FL_DEACTIVATE || event==FL_HIDE) hover_=false;
        if(event==FL_KEYDOWN && active_r() && Fl::focus()==this
            && (Fl::event_key()==FL_Enter || Fl::event_key()==FL_KP_Enter)
            && !(Fl::event_state()&(FL_CTRL|FL_ALT|FL_META|FL_SHIFT))) {
            do_callback();return 1;
        }
        return Fl_Button::handle(event);
    }
    void draw() override {
        const bool primary=control_==Control::Run || control_==Control::RunMenu;
        const bool selected=(control_==Control::Pin && panel_.state().pinned) || (control_==Control::Log && panel_.state().logVisible);
        const bool enabled=active_r()!=0;
        auto bg=primary && enabled ? colors_.accent : enabled && (hover_ || value() || selected) ? colors_.soft : colors_.surface;
        if(enabled && (value() || (primary && hover_))) bg=fl_color_average(bg,colors_.text,value() ? .86f : .94f);
        const auto fg=!enabled ? fl_color_average(colors_.muted,colors_.surface,.68f) : primary ? colors_.accentText : selected ? colors_.accent : colors_.text;
        fl_push_clip(x(),y(),w(),h());fl_color(colors_.surface);fl_rectf(x(),y(),w(),h());
        int bx=x()+1,bw=w()-2;
        if(control_==Control::Run) bw+=panel_.button(Control::RunMenu).w();
        if(control_==Control::RunMenu) {bx-=panel_.button(Control::Run).w();bw+=panel_.button(Control::Run).w();}
        if(control_==Control::Build) bw+=panel_.button(Control::BuildMenu).w();
        if(control_==Control::BuildMenu) {bx-=panel_.button(Control::Build).w();bw+=panel_.button(Control::Build).w();}
        if(control_==Control::Pick) bw+=panel_.button(Control::PickMenu).w();
        if(control_==Control::PickMenu) {bx-=panel_.button(Control::Pick).w();bw+=panel_.button(Control::Pick).w();}
        fl_color(bg);fl_rounded_rectf(bx,y()+1,bw,h()-2,4);
        if(primary || control_==Control::Pick || control_==Control::PickMenu || control_==Control::Build || control_==Control::BuildMenu) {fl_color(colors_.line);fl_rounded_rect(bx,y()+1,bw,h()-2,4);}
        if(control_==Control::RunMenu || control_==Control::BuildMenu || control_==Control::PickMenu) {fl_color(colors_.line);fl_line(x(),y()+8,x(),y()+h()-8);}
        const bool hasLabel=control_==Control::Build || control_==Control::Run || control_==Control::Pin;
        const int cx=hasLabel ? x()+17 : x()+w()/2,cy=y()+h()/2+(value() ? 1 : 0);
        fl_color(fg);fl_line_style(FL_SOLID,1);
        const int antialias=fl_antialias();fl_antialias(1);
        switch(control_) {
        case Control::Pick:
            fl_rect(cx-5,cy-8,11,16);fl_line(cx+2,cy+3,cx+5,cy+3,cx+5,cy+7);fl_line(cx+2,cy+3,cx+2,cy+7);break;
        case Control::Build:
            if(panel_.operation()==Operation::Building) {fl_line(cx-5,cy-5,cx+5,cy+5);fl_line(cx+5,cy-5,cx-5,cy+5);}
            else {
                constexpr std::array<std::array<int,2>,14> wrench{{{6,-8},{2,-7},{-1,-4},{-1,-1},{-8,6},{-8,8},{-6,9},{-4,8},{3,1},{6,1},{9,-2},{9,-6},{5,-2},{2,-5}}};
                fl_begin_loop();for(const auto& p:wrench) fl_vertex(cx+p[0],cy+p[1]);fl_end_loop();
                fl_circle(cx-6,cy+7,1);
            }
            break;
        case Control::Run:
            if(panel_.operation()==Operation::Running) fl_rectf(cx-4,cy-4,8,8);else fl_polygon(cx-4,cy-6,cx+5,cy,cx-4,cy+6);break;
        case Control::RunMenu: case Control::BuildMenu: case Control::PickMenu: fl_line(cx-4,cy-2,cx,cy+2,cx+4,cy-2);break;
        case Control::Pin: fl_rect(cx-4,cy-6,8,7);fl_line(cx-6,cy+1,cx+6,cy+1);fl_line(cx,cy+1,cx,cy+7);break;
        case Control::Settings: {
            // Exact reflections keep all eight teeth symmetric even after GDI
            // rounds logical vertices. Polar alternating radii twist the teeth.
            constexpr std::array<std::array<int,2>,32> gear{{
                {-2,-8},{2,-8},{2,-6},{3,-5},{5,-7},{7,-5},{5,-3},{6,-2},
                {8,-2},{8,2},{6,2},{5,3},{7,5},{5,7},{3,5},{2,6},
                {2,8},{-2,8},{-2,6},{-3,5},{-5,7},{-7,5},{-5,3},{-6,2},
                {-8,2},{-8,-2},{-6,-2},{-5,-3},{-7,-5},{-5,-7},{-3,-5},{-2,-6}
            }};
            fl_begin_loop();
            for(const auto& point:gear) fl_vertex(cx+point[0],cy+point[1]);
            fl_end_loop();fl_circle(cx,cy,3);
            break;
        }
        case Control::Log: fl_rect(cx-6,cy-8,13,17);fl_line(cx-3,cy-4,cx+3,cy-4);fl_line(cx-3,cy,cx+3,cy);fl_line(cx-3,cy+4,cx+1,cy+4);break;
        case Control::Minimize: fl_line(cx-5,cy+3,cx+5,cy+3);break;
        case Control::Close: fl_line(cx-5,cy-5,cx+5,cy+5);fl_line(cx+5,cy-5,cx-5,cy+5);break;
        }
        fl_antialias(antialias);
        if(hasLabel) ellipsis(label() ? label() : "",x()+31,y(),w()-36,h(),appFont,control_==Control::Pin ? 12 : 13,fg);
        if(Fl::focus()==this && Fl::visible_focus()) {
            const int inset=control_==Control::Log ? 2 : 4;
            fl_color(fg);fl_line_style(FL_DOT);fl_rect(x()+inset,y()+inset,w()-2*inset,h()-2*inset);
        }
        fl_line_style(FL_SOLID);fl_pop_clip();
    }
};
std::string menuLabel(std::string_view value) {
    std::string result;
    for(char c:value) {
        if(c=='&' || c=='@') result+=c;
        result+=c;
    }
    return result;
}
std::string tooltipLabel(std::wstring_view value) {
    std::string result;
    for(const char character:platform::utf8(value)) {
        if(character=='@') result+=character;
        result+=character;
    }
    return result;
}
}

Journal::Journal(int x,int y,int w,int h) : Fl_Text_Display(x,y,w,h) {
    buffer(&text_);textfont(journalFont);textsize(12);box(FL_FLAT_BOX);wrap_mode(WRAP_NONE,0);
}
Journal::~Journal() {buffer(nullptr);}
bool Journal::atBottom() {recalc_display();return text_.length()==0 || mLastChar>=text_.length();}
void Journal::clear() {text_.text("");scroll(1,0);}
void Journal::append(std::wstring_view value) {
    if(value.empty()) return;
    const bool follow=atBottom();
    int first=mFirstChar,row=scroll_row(),horizontal=scroll_col(),begin=0,end=0;
    bool trimmed=false;
    const bool selected=text_.selection_position(&begin,&end)!=0;
    auto bytes=platform::utf8(value);std::erase(bytes,'\r');text_.append(bytes.c_str());
    // Cut at a line boundary (or a code point for a huge single line), never in UTF-8.
    if(text_.length()>900000) {
        int cut=text_.length()-450000;
        const int lineEnd=text_.line_end(cut);
        cut=lineEnd<text_.length() ? lineEnd+1 : text_.utf8_align(cut);
        text_.remove(0,cut);first=std::max(0,first-cut);begin=std::max(0,begin-cut);end=std::max(0,end-cut);trimmed=true;
    }
    if(selected && end>begin) text_.select(begin,end);else text_.unselect();
    recalc_display();scroll(follow ? INT_MAX : trimmed ? count_lines(0,first,true)+1 : row,horizontal);recalc_display();
}
void Journal::resize(int x,int y,int w,int h) {
    const bool follow=buffer() && atBottom();const int first=mFirstChar,horizontal=mHorizOffset;
    Fl_Text_Display::resize(x,y,w,h);
    if(buffer()) {recalc_display();scroll(follow ? INT_MAX : count_lines(0,first,true)+1,horizontal);recalc_display();}
}
struct Panel::EventQueue {
    std::mutex mutex;std::deque<Event> pending;bool accepting=true,wakePosted=false;
};
Panel::Panel(AppState& state) : Fl_Double_Window(std::max(500,state.width),state.logVisible ? state.logHeight : compactHeight),state_(state) {
    static const bool fonts=[] {Fl::set_font(appFont,"Segoe UI");Fl::set_font(journalFont,"Consolas");return true;}();
    (void)fonts;
    border(0);copy_label(platform::utf8(appTitle).c_str());position(100,100);begin();
    for(size_t i=0;i<buttons_.size();++i) {
        buttons_[i]=new PanelButton(*this,colors_,static_cast<Control>(i));
        buttons_[i]->callback([](Fl_Widget* widget,void* data) {
            auto& p=*static_cast<Panel*>(data);
            for(size_t j=0;j<p.buttons_.size();++j) if(widget==p.buttons_[j]) {p.activate(static_cast<Control>(j));return;}
        },this);
    }
    journal_=new Journal(15,153,w()-30,std::max(1,h()-171));menu_=new Fl_Menu_Button(0,0,1,1);menu_->textfont(appFont);menu_->textsize(13);menu_->hide();
    buildMenu_=new Fl_Menu_Button(0,0,1,1);buildMenu_->textfont(appFont);buildMenu_->textsize(13);buildMenu_->hide();
    projectMenu_=new Fl_Menu_Button(0,0,1,1);projectMenu_->textfont(appFont);projectMenu_->textsize(13);projectMenu_->hide();
    buildMenuItems_[0].text="Собрать";buildMenuItems_[1].text="Пересборка";buildMenuItems_[2].text="Очистить";buildMenuItems_[3].text="Собрать и запустить";buildMenuItems_[4].text="CMake";
    for(size_t i=0;i<5;++i) {buildMenuItems_[i].labelfont_=appFont;buildMenuItems_[i].labelsize_=13;}
    buildMenuItems_[0].shortcut_=FL_F+6;buildMenuItems_[3].shortcut_=FL_F+5;
    buildMenu_->menu(buildMenuItems_.data());end();
    resizable(journal_);callback([](Fl_Widget*,void* data){static_cast<Panel*>(data)->closePanel();},this);
    const std::array<const char*,11> tips{"Выбрать CMakeLists.txt проекта (Ctrl+O)","Собрать проект (F6)","Запустить выбранную цель (Ctrl+F5)","Выбрать цель запуска (↓ / F4)","Закрепить поверх окон","Настройки","Показать журнал","Свернуть","Закрыть","Сборка, пересборка, очистка, сборка и запуск или CMake (↓ / F4)","Недавние проекты (↓ / F4)"};
    for(size_t i=0;i<tips.size();++i) buttons_[i]->copy_tooltip(tips[i]);
    status_=state_.settings.cmakeFile.empty() ? L"Выберите CMakeLists.txt" : L"Готов к сборке · "+state_.settings.configuration;
    updateTheme();layout();updateControls();events_=std::make_shared<EventQueue>();
    engine_=std::make_unique<Engine>([queue=events_](Event event) {
        bool wake=false;
        {std::lock_guard lock(queue->mutex);if(!queue->accepting) return;
            if(event.kind==EventKind::Log && !queue->pending.empty() && queue->pending.back().kind==EventKind::Log
                && queue->pending.back().text.size()+event.text.size()<32768) queue->pending.back().text+=event.text;
            else queue->pending.push_back(std::move(event));
            if(!queue->wakePosted) {queue->wakePosted=true;wake=true;}
        }
        // No window pointers or per-line callbacks cross the thread boundary.
        if(wake) Fl::awake();
    });
    Fl::add_check(checkEvents,this);Fl::add_system_handler(systemEvent,this);
}
Panel::~Panel() {closePanel();menu_->menu(nullptr);buildMenu_->menu(nullptr);projectMenu_->menu(nullptr);}
void Panel::checkEvents(void* data) {static_cast<Panel*>(data)->drainEvents();}
int Panel::systemEvent(void* event,void* data) {if(platform::themeChanged(event)) static_cast<Panel*>(data)->updateTheme();return 0;}
void Panel::show() {
    Fl_Double_Window::show();auto native=reinterpret_cast<void*>(fl_xid(this));
    platform::configurePanelWindow(native);
    platform::restoreNativePosition(native,state_.x,state_.y);platform::setTopmost(native,state_.pinned);updateTheme();
}
void Panel::drainEvents() {
    if(closing_ || !events_) return;
    std::deque<Event> ready;
    {std::lock_guard lock(events_->mutex);ready.swap(events_->pending);events_->wakePosted=false;}
    std::wstring log;
    std::optional<Event> progress;
    for(auto& event:ready) {
        if(event.kind==EventKind::Progress) {progress=std::move(event);continue;}
        if(event.kind==EventKind::Log) {
            log+=event.text;
            if(log.size()>=65536) {appendLog(log);log.clear();}
        }
        else {
            if(!log.empty()) {appendLog(log);log.clear();}
            if(progress) {onEvent(std::move(*progress));progress.reset();}
            onEvent(std::move(event));
        }
    }
    if(!log.empty()) appendLog(log);
    if(progress) onEvent(std::move(*progress));
}
void Panel::closePanel() {
    runAfterBuild_=false;
    if(closing_) return;closing_=true;Fl::remove_check(checkEvents,this);Fl::remove_system_handler(systemEvent);
    resetBuildProgress();
    if(events_) {std::lock_guard lock(events_->mutex);events_->accepting=false;}
    // The worker only owns the queue mutex and never waits for the FLTK UI lock.
    if(engine_) {engine_->cancel();engine_.reset();}
    if(events_) {std::lock_guard lock(events_->mutex);events_->pending.clear();}
    save();hide();
}
void Panel::save() {
    state_.width=w();if(state_.logVisible) state_.logHeight=h();
    if(shown()) platform::captureNativePosition(reinterpret_cast<void*>(fl_xid(this)),state_.x,state_.y);
    state_.save();
}
void Panel::layout() {
    auto place=[&](Control c,int x,int y,int width,int height){button(c).resize(x,y,width,height);};
    const auto captionWidth=[](int size,std::initializer_list<const char*> captions) {
        fl_font(appFont,size);double width=0;
        for(const auto* caption:captions) width=std::max(width,fl_width(caption));
        return static_cast<int>(std::ceil(width))+38;
    };
    const int pinWidth=std::max(109,captionWidth(12,{"Закрепить","Поверх окон"}));
    const int buildWidth=std::max(96,captionWidth(13,{"Собрать","Отменить"}));
    const int runWidth=std::max(107,captionWidth(13,{"Запустить","Остановить","Ожидание"}));
    place(Control::Close,w()-40,9,27,27);place(Control::Minimize,w()-71,9,27,27);place(Control::Log,w()-103,9,27,27);place(Control::Settings,w()-135,9,27,27);
    place(Control::Pin,w()-143-pinWidth,9,pinWidth,27);place(Control::Pick,15,57,36,37);place(Control::PickMenu,51,57,23,37);
    place(Control::Run,w()-43-runWidth,57,runWidth,37);place(Control::RunMenu,w()-43,57,28,37);
    place(Control::Build,button(Control::Run).x()-36-buildWidth,57,buildWidth,37);
    place(Control::BuildMenu,button(Control::Build).x()+buildWidth,57,28,37);
    journal_->resize(15,153,w()-30,std::max(1,h()-171));if(state_.logVisible) journal_->show();else journal_->hide();
    size_range(500,state_.logVisible ? minimumLogHeight : compactHeight,0,state_.logVisible ? 0 : compactHeight);redraw();
}
void Panel::resize(int x,int y,int width,int height) {
    width=std::max(500,width);height=state_.logVisible ? std::max(minimumLogHeight,height) : compactHeight;
    Fl_Double_Window::resize(x,y,width,height);
    if(journal_) {if(state_.logVisible) state_.logHeight=height;layout();}
}
void Panel::toggleLog() {state_.logVisible=!state_.logVisible;resize(x(),y(),w(),state_.logVisible ? state_.logHeight : compactHeight);save();updateControls();}
int Panel::handle(int event) {
    if((event==FL_KEYDOWN || event==FL_SHORTCUT) && Fl::event_key()==FL_F+4 && Fl::event_alt()) {closePanel();return 1;}
    if(event==FL_KEYDOWN || event==FL_SHORTCUT) {
        const int modifiers=Fl::event_state()&(FL_CTRL|FL_ALT|FL_META|FL_SHIFT);
        if(modifiers==0 && Fl::event_key()==FL_F+5) {if(operation_==Operation::Idle) buildAndRun();return 1;}
        if(modifiers==0 && Fl::event_key()==FL_F+6) {build();return 1;}
        if(modifiers==FL_CTRL && Fl::event_key()==FL_F+5) {run();return 1;}
        if(modifiers==FL_CTRL && (Fl::event_key()=='o' || Fl::event_key()=='O')) {pickProject();return 1;}
    }
    const int mx=Fl::event_x(),my=Fl::event_y();
    if(event==FL_PUSH && Fl::event_button()==FL_LEFT_MOUSE) {
        int edges=0;
        if(mx<6) edges|=1;
        if(mx>=w()-6) edges|=2;
        if(state_.logVisible) {if(my<6) edges|=4;if(my>=h()-6) edges|=8;}
        if(edges || (my<43 && mx<button(Control::Pin).x()-8)) {
            drag_=edges ? edges : 16;
            nativeDragging_=shown() && platform::beginWindowDrag(reinterpret_cast<void*>(fl_xid(this)),static_cast<unsigned>(edges),nativeDrag_);
            startX_=x();startY_=y();startW_=w();startH_=h();mouseX_=Fl::event_x_root();mouseY_=Fl::event_y_root();return 1;
        }
    }
    if(event==FL_DRAG && drag_) {
        if(nativeDragging_) {
            platform::updateWindowDrag(reinterpret_cast<void*>(fl_xid(this)),nativeDrag_,500,state_.logVisible ? minimumLogHeight : compactHeight,
                state_.logVisible ? 0 : compactHeight,Fl::screen_scale(screen_num()));
            return 1;
        }
        const int dx=Fl::event_x_root()-mouseX_,dy=Fl::event_y_root()-mouseY_;
        if(drag_==16) position(startX_+dx,startY_+dy);
        else {
            const int width=std::max(500,startW_+(drag_&2 ? dx : drag_&1 ? -dx : 0));
            const int height=state_.logVisible ? std::max(minimumLogHeight,startH_+(drag_&8 ? dy : drag_&4 ? -dy : 0)) : compactHeight;
            resize(drag_&1 ? startX_+startW_-width : startX_,drag_&4 ? startY_+startH_-height : startY_,width,height);
        }
        return 1;
    }
    if(event==FL_RELEASE && drag_) {drag_=0;nativeDragging_=false;save();return 1;}
    if(event==FL_MOVE) {
        const bool horizontal=mx<6 || mx>=w()-6,vertical=state_.logVisible && (my<6 || my>=h()-6);
        cursor(horizontal && vertical ? ((mx<6)==(my<6) ? FL_CURSOR_NWSE : FL_CURSOR_NESW) : horizontal ? FL_CURSOR_WE : vertical ? FL_CURSOR_NS : FL_CURSOR_DEFAULT);
    }
    if(event==FL_LEAVE) cursor(FL_CURSOR_DEFAULT);
    return Fl_Double_Window::handle(event);
}
Palette themePalette(bool dark) {
    // Scrollbar colors match the ImGui panel's soft background and line color.
    return dark ? Palette{rgb(32,35,41),rgb(43,48,56),rgb(238,241,245),rgb(169,180,195),rgb(60,67,78),rgb(118,173,255),rgb(20,36,61),rgb(116,214,163),rgb(255,149,149),rgb(43,49,57),rgb(62,72,84)}
        : Palette{rgb(255,255,255),rgb(244,246,248),rgb(36,41,51),rgb(104,115,130),rgb(223,228,235),rgb(18,101,211),rgb(255,255,255),rgb(34,123,83),rgb(182,49,54),rgb(244,246,249),rgb(218,225,236)};
}
void styleScrollbars(Fl_Group& group,const Palette& colors) {
    for(int i=0;i<group.children();++i) {
        auto* widget=group.child(i);
        if(auto* scrollbar=dynamic_cast<Fl_Scrollbar*>(widget)) {
            scrollbar->color(colors.scrollTrack);scrollbar->selection_color(colors.scrollThumb);
            scrollbar->labelcolor(colors.muted);scrollbar->slider(FL_FLAT_BOX);scrollbar->redraw();
        }
        else if(auto* nested=dynamic_cast<Fl_Group*>(widget)) styleScrollbars(*nested,colors);
    }
}
void Panel::setTheme(bool dark) {
    colors_=themePalette(dark);
    // Update colors only: retain every widget, font, geometry, focus and buffer.
    Fl::set_color(FL_BACKGROUND_COLOR,colors_.soft);Fl::set_color(FL_BACKGROUND2_COLOR,colors_.surface);
    Fl::set_color(FL_FOREGROUND_COLOR,colors_.text);Fl::set_color(FL_SELECTION_COLOR,colors_.accent);color(colors_.surface);
    if(journal_) {journal_->color(colors_.surface);journal_->textcolor(colors_.text);journal_->selection_color(colors_.accent);styleScrollbars(*journal_,colors_);journal_->redraw();}
    if(menu_) {menu_->color(colors_.surface);menu_->textcolor(colors_.text);menu_->selection_color(colors_.accent);}
    if(buildMenu_) {buildMenu_->color(colors_.surface);buildMenu_->textcolor(colors_.text);buildMenu_->selection_color(colors_.accent);}
    if(projectMenu_) {projectMenu_->color(colors_.surface);projectMenu_->textcolor(colors_.text);projectMenu_->selection_color(colors_.accent);}
    if(shown()) platform::setWindowTheme(reinterpret_cast<void*>(fl_xid(this)),dark);
    for(auto* window=Fl::first_window();window;window=Fl::next_window(window))
        if(auto* dialog=dynamic_cast<SettingsDialog*>(window)) dialog->setTheme(dark);
    redraw();
}
void Panel::updateTheme() {setTheme(platform::darkTheme());}
void Panel::updateControls() {
    const bool busy=operation_!=Operation::Idle;
    const bool waitingForBuild=canQueueRun() && runAfterBuild_;
    auto enabled=[&](Control c,bool yes){if(yes) button(c).activate();else button(c).deactivate();};
    enabled(Control::Pick,!busy);enabled(Control::PickMenu,!busy);enabled(Control::Settings,!busy);enabled(Control::Build,operation_!=Operation::Running);
    enabled(Control::BuildMenu,!busy);
    enabled(Control::Run,operation_==Operation::Running || (canQueueRun() && !waitingForBuild) || (!busy && !state_.targets.empty()));enabled(Control::RunMenu,!busy && !state_.targets.empty());
    button(Control::Build).copy_label(operation_==Operation::Building ? "Отменить" : "Собрать");
    button(Control::Run).copy_label(operation_==Operation::Running ? "Остановить" : waitingForBuild ? "Ожидание" : "Запустить");button(Control::Pin).copy_label(state_.pinned ? "Поверх окон" : "Закрепить");
    if(!state_.settings.cmakeFile.empty()) button(Control::Pick).copy_tooltip(tooltipLabel(state_.settings.cmakeFile).c_str());
    button(Control::Build).copy_tooltip(tooltipLabel(L"Собрать (F6); собрать и запустить (F5)\n"+state_.settings.configuration+L" · "+(state_.settings.buildDirectory.empty() ? L"build-cmakebuild" : state_.settings.buildDirectory)).c_str());
    if(operation_==Operation::Building) button(Control::Build).copy_tooltip(configuring_ ? "Отменить конфигурирование CMake и завершить его процессы" : cleaning_ ? "Отменить очистку и завершить её процессы" : "Отменить сборку и завершить её процессы");
    const auto* target=state_.findChosenTarget();
    button(Control::Run).copy_tooltip(operation_==Operation::Running ? "Остановить приложение и его дочерние процессы"
        : canQueueRun() ? runAfterBuild_ ? "Запуск после успешной сборки уже запланирован" : "Запустить после успешной сборки (Ctrl+F5)"
        : operation_==Operation::Building && cancelling_ ? "Запуск недоступен: сборка отменяется"
        : target ? tooltipLabel(L"Запустить "+target->name+L" (Ctrl+F5)\n"+target->executable).c_str()
        : state_.targets.empty() ? "Исполняемые цели появятся после сборки" : "Выбрать цель запуска");
    button(Control::Log).copy_tooltip(state_.logVisible ? "Скрыть журнал" : "Показать журнал");
    button(Control::Pin).copy_tooltip(state_.pinned ? "Открепить от переднего плана" : "Закрепить поверх окон");
    for(auto* b:buttons_) b->redraw();
    redraw();
}
void Panel::activate(Control control) {
    switch(control) {
    case Control::Pick:pickProject();break;
    case Control::PickMenu:showRecentProjects();break;
    case Control::Build:build();break;
    case Control::BuildMenu:showBuildActions();break;
    case Control::Run:run();break;
    case Control::RunMenu:showRunTargets();break;
    case Control::Pin:state_.pinned=!state_.pinned;if(shown()) platform::setTopmost(reinterpret_cast<void*>(fl_xid(this)),state_.pinned);save();updateControls();break;
    case Control::Settings:editSettings();break;
    case Control::Log:toggleLog();break;
    case Control::Minimize:iconize();break;
    case Control::Close:closePanel();break;
    }
}
void Panel::pickProject() {
    if(operation_!=Operation::Idle) return;
    const auto path=platform::selectPath(shown() ? reinterpret_cast<void*>(fl_xid(this)) : nullptr,false,L"Выберите CMakeLists.txt проекта");
    if(!path.empty()) selectProject(path);
}
void Panel::selectProject(std::wstring path) {
    if(operation_!=Operation::Idle) return;
    if(_wcsicmp(std::filesystem::path(path).filename().c_str(),L"CMakeLists.txt")!=0) {failed_=true;status_=L"Нужен файл CMakeLists.txt";updateControls();return;}
    std::error_code error;
    if(!std::filesystem::is_regular_file(path,error)) {failed_=true;status_=L"Файл проекта не найден";appendLog(L"Файл проекта не найден: "+path+L"\n");updateControls();return;}
    if(state_.selectProject(path)) {journal_->clear();lastBuildDuration_.reset();resetBuildProgress();}
    status_=L"Готов к сборке · "+state_.settings.configuration;failed_=false;save();updateControls();
}
bool Panel::chooseRecentProject(size_t index) {
    if(operation_!=Operation::Idle || index>=state_.recentProjects.size()) return false;
    const auto path=state_.recentProjects[index];
    std::error_code error;
    if(!std::filesystem::is_regular_file(path,error)) {failed_=true;status_=L"Файл проекта не найден";appendLog(L"Файл проекта не найден: "+path+L"\n");updateControls();return false;}
    selectProject(path);return !failed_;
}
void Panel::showRecentProjects() {
    if(operation_!=Operation::Idle) return;
    projectMenu_->menu(nullptr);projectMenuLabels_.clear();projectMenuItems_.clear();
    projectMenuLabels_.reserve(state_.recentProjects.size()+1);projectMenuLabels_.emplace_back("Открыть проект…");
    int workX=0,workY=0,workWidth=0,workHeight=0;Fl::screen_work_area(workX,workY,workWidth,workHeight,screen_num());
    for(const auto& path:state_.recentProjects) projectMenuLabels_.push_back(menuLabel(fitText(platform::utf8(path),std::max(100,workWidth-80),appFont,13)));
    projectMenuItems_.resize(projectMenuLabels_.size()+1);
    for(size_t index=0;index<projectMenuLabels_.size();++index) {
        auto& item=projectMenuItems_[index];item.text=projectMenuLabels_[index].c_str();item.labelfont_=appFont;item.labelsize_=13;
        if(index==0) {item.flags=FL_MENU_DIVIDER;item.shortcut_=FL_CTRL+'o';}
        else if(platform::projectSection(state_.recentProjects[index-1])==platform::projectSection(state_.settings.cmakeFile)) item.flags=FL_MENU_TOGGLE|FL_MENU_VALUE;
    }
    projectMenu_->menu(projectMenuItems_.data());projectMenu_->resize(button(Control::Pick).x(),button(Control::Pick).y()+button(Control::Pick).h(),button(Control::Pick).w()+button(Control::PickMenu).w(),1);
    const auto* chosen=popupProjectMenu ? popupProjectMenu(*projectMenu_) : projectMenu_->popup();
    if(chosen==projectMenuItems_.data()) pickProject();
    else for(size_t index=1;index<projectMenuLabels_.size();++index) if(chosen==projectMenuItems_.data()+index) {chooseRecentProject(index-1);break;}
}
void Panel::editSettings() {
    if(operation_!=Operation::Idle) return;
    SettingsDialog dialog(*this);dialog.set_modal();dialog.show();
    while(dialog.shown() && !closing_) Fl::wait();
    if(dialog.accepted()) {
        applySettings(dialog.settingsValue(),dialog.chosenTarget(),dialog.selectionChanged());
        for(const auto& [target,settings]:dialog.launchSettings()) state_.setRunSettings(target,settings);
    }
}
void Panel::applySettings(BuildSettings settings,std::wstring chosen,bool selectionChanged) {
    const bool changed=state_.settings.buildDirectory!=settings.buildDirectory || state_.settings.configuration!=settings.configuration;
    state_.settings=std::move(settings);
    if(selectionChanged) {state_.chosenTarget=std::move(chosen);state_.chosenExecutable.clear();}
    if(changed) {state_.restoreTargets();lastBuildDuration_.reset();resetBuildProgress();}
    status_=state_.settings.cmakeFile.empty() ? L"Выберите CMakeLists.txt" : L"Готов к сборке · "+state_.settings.configuration;failed_=false;save();updateControls();
}
void Panel::build(bool cleanFirst,bool runAfter) {
    if(closing_) return;
    if(operation_==Operation::Building) {runAfterBuild_=false;cancelling_=true;engine_->cancel();status_=configuring_ ? L"Отмена CMake…" : cleaning_ ? L"Отмена очистки…" : L"Отмена сборки…";updateControls();return;}
    if(operation_!=Operation::Idle) return;
    if(state_.settings.cmakeFile.empty()) pickProject();
    if(state_.settings.cmakeFile.empty()) return;
    runAfterBuild_=runAfter;cancelling_=false;
    journal_->clear();lastBuildDuration_.reset();resetBuildProgress();cleaning_=false;configuring_=false;failed_=false;status_=cleanFirst ? L"Подготовка пересборки…" : L"Подготовка сборки…";operation_=Operation::Building;
    Fl::add_timeout(.08,animateProgress,this);updateControls();
    auto settings=state_.settings;settings.cleanFirst=cleanFirst;if(cleanFirst) settings.target.clear();
    if(!engine_->build(std::move(settings))) {runAfterBuild_=false;operation_=Operation::Idle;resetBuildProgress();failed_=true;status_=L"Не удалось начать сборку";updateControls();}
}
void Panel::clean() {
    if(closing_) return;
    if(operation_!=Operation::Idle) return;
    if(state_.settings.cmakeFile.empty()) pickProject();
    if(state_.settings.cmakeFile.empty()) return;
    runAfterBuild_=false;cancelling_=false;
    journal_->clear();lastBuildDuration_.reset();resetBuildProgress();cleaning_=true;configuring_=false;failed_=false;status_=L"Подготовка очистки…";operation_=Operation::Building;
    Fl::add_timeout(.08,animateProgress,this);updateControls();
    if(!engine_->clean(state_.settings)) {operation_=Operation::Idle;resetBuildProgress();failed_=true;status_=L"Не удалось начать очистку";updateControls();}
}
void Panel::configure() {
    if(closing_ || operation_!=Operation::Idle) return;
    if(state_.settings.cmakeFile.empty()) pickProject();
    if(state_.settings.cmakeFile.empty()) return;
    runAfterBuild_=false;cancelling_=false;
    journal_->clear();lastBuildDuration_.reset();resetBuildProgress();cleaning_=false;configuring_=true;failed_=false;status_=L"Подготовка CMake…";operation_=Operation::Building;
    Fl::add_timeout(.08,animateProgress,this);updateControls();
    if(!engine_->configure(state_.settings)) {operation_=Operation::Idle;resetBuildProgress();failed_=true;status_=L"Не удалось начать CMake";updateControls();}
}
void Panel::showBuildActions() {
    if(operation_!=Operation::Idle) return;
    buildMenu_->resize(button(Control::Build).x(),button(Control::Build).y()+button(Control::Build).h(),button(Control::Build).w()+button(Control::BuildMenu).w(),1);
    const auto* chosen=popupBuildMenu ? popupBuildMenu(*buildMenu_) : buildMenu_->popup();
    if(chosen==buildMenuItems_.data() || chosen==buildMenuItems_.data()+1) build(chosen==buildMenuItems_.data()+1);
    else if(chosen==buildMenuItems_.data()+2) clean();
    else if(chosen==buildMenuItems_.data()+3) buildAndRun();
    else if(chosen==buildMenuItems_.data()+4) configure();
}
void Panel::run() {
    if(closing_) return;
    if(operation_==Operation::Running) {engine_->cancel();status_=L"Остановка приложения…";updateControls();return;}
    if(canQueueRun()) {
        if(!std::exchange(runAfterBuild_,true)) appendLog(L"Запуск после успешной сборки запланирован.\n");
        updateControls();return;
    }
    if(operation_!=Operation::Idle) return;
    std::erase_if(state_.targets,[](const Target& t){std::error_code e;return !std::filesystem::is_regular_file(t.executable,e);});
    if(state_.chosenTarget.empty() && state_.targets.size()==1) state_.selectRunTarget(0);
    const auto* found=state_.findChosenTarget();
    if(!found) {showRunTargets();updateControls();return;}
    const auto target=*found;save();failed_=false;status_=L"Запуск "+target.name+L"…";operation_=Operation::Running;updateControls();
    const auto source=std::filesystem::absolute(state_.settings.cmakeFile).parent_path();
    const auto directory=state_.settings.buildDirectory.empty() ? source/L"build-cmakebuild" : std::filesystem::path(state_.settings.buildDirectory).is_absolute() ? std::filesystem::path(state_.settings.buildDirectory) : source/state_.settings.buildDirectory;
    auto settings=state_.runSettingsFor(target.name);
    if(!settings.workingDirectory.empty() && std::filesystem::path(settings.workingDirectory).is_relative()) settings.workingDirectory=(source/settings.workingDirectory).lexically_normal().wstring();
    if(!engine_->runConfigured(target,std::move(settings),directory.wstring())) {operation_=Operation::Idle;failed_=true;status_=L"Не удалось запустить приложение";updateControls();}
}
bool Panel::chooseRunTarget(size_t index) {
    if(operation_!=Operation::Idle || !state_.selectRunTarget(index)) return false;
    status_=L"Цель запуска: "+state_.chosenTarget;failed_=false;save();updateControls();return true;
}
void Panel::showRunTargets() {
    if(operation_!=Operation::Idle) return;
    std::erase_if(state_.targets,[](const Target& t){std::error_code e;return !std::filesystem::is_regular_file(t.executable,e);});updateControls();
    menu_->menu(nullptr);menuLabels_.clear();menuItems_.clear();
    // Keep one literal entry per target. Fl_Menu_::add parses paths, merges
    // mnemonic labels, and copies through a fixed-size internal buffer.
    menuLabels_.reserve(state_.targets.size());
    int workX=0,workY=0,workWidth=0,workHeight=0;
    Fl::screen_work_area(workX,workY,workWidth,workHeight,screen_num());
    for(const auto& target:state_.targets) {
        menuLabels_.push_back(menuLabel(fitText(platform::utf8(target.name+L" — "+std::filesystem::path(target.executable).filename().wstring()),
            std::max(100,workWidth-80),appFont,13)));
    }
    if(state_.targets.empty()) return;
    menuItems_.resize(state_.targets.size()+1);
    for(size_t index=0;index<state_.targets.size();++index) {
        auto& item=menuItems_[index];item.text=menuLabels_[index].c_str();
        item.flags=FL_MENU_RADIO|(state_.targets[index].name==state_.chosenTarget ? FL_MENU_VALUE : 0);
        item.labelfont_=appFont;item.labelsize_=13;
    }
    menu_->menu(menuItems_.data());
    menu_->resize(button(Control::Run).x(),button(Control::Run).y()+button(Control::Run).h(),button(Control::Run).w()+button(Control::RunMenu).w(),1);
    const auto* chosen=popupMenu ? popupMenu(*menu_) : menu_->popup();
    if(chosen && chosen>=menu_->menu() && chosen<menu_->menu()+state_.targets.size()) chooseRunTarget(static_cast<size_t>(chosen-menu_->menu()));
}
void Panel::onEvent(Event event) {
    switch(event.kind) {
    case EventKind::Log:appendLog(event.text);return;
    case EventKind::Status:status_=std::move(event.text);break;
    case EventKind::Targets:state_.targets=std::move(event.targets);break;
    case EventKind::Progress:
        if(operation_!=Operation::Building || configuring_ || !event.progress || !event.progress->total || event.progress->completed>event.progress->total) return;
        if(buildProgress_ && buildProgress_->total==event.progress->total && event.progress->completed<buildProgress_->completed) return;
        buildProgress_=event.progress;Fl::remove_timeout(animateProgress,this);redraw();return;
    case EventKind::BuildSucceeded:
        resetBuildProgress();
        operation_=Operation::Idle;if(!event.targets.empty()) state_.targets=std::move(event.targets);
        std::erase_if(state_.targets,[](const Target& t){std::error_code e;return !std::filesystem::is_regular_file(t.executable,e);});
        if(state_.chosenTarget.empty() && state_.targets.size()==1) state_.selectRunTarget(0);
        status_=event.text.empty() ? L"Сборка завершена" : event.text;if(state_.targets.empty()) status_+=L" · нет исполняемых целей";failed_=false;showBuildDuration(event);save();
        if(std::exchange(runAfterBuild_,false)) {
            if(!state_.findChosenTarget() && state_.chosenTarget.empty() && !state_.targets.empty()) {status_=L"Выберите цель для запуска после сборки";showRunTargets();}
            if(closing_) return;
            if(state_.findChosenTarget()) {run();return;}
            appendLog(state_.targets.empty() ? L"Запуск пропущен: нет собранных исполняемых целей.\n" : L"Запуск пропущен: выберите доступную цель запуска.\n");
        }
        break;
    case EventKind::CleanSucceeded:
        runAfterBuild_=false;
        resetBuildProgress();operation_=Operation::Idle;state_.targets.clear();failed_=false;
        status_=event.text.empty() ? L"Очистка завершена" : event.text;showBuildDuration(event);save();break;
    case EventKind::ConfigureSucceeded:
        runAfterBuild_=false;resetBuildProgress();operation_=Operation::Idle;failed_=false;
        state_.targets=std::move(event.targets);
        std::erase_if(state_.targets,[](const Target& t){std::error_code error;return !std::filesystem::is_regular_file(t.executable,error);});
        if(state_.chosenTarget.empty() && state_.targets.size()==1) state_.selectRunTarget(0);
        status_=event.text.empty() ? L"CMake завершён" : event.text;showBuildDuration(event);save();break;
    case EventKind::BuildFailed: case EventKind::CleanFailed: case EventKind::ConfigureFailed:
        runAfterBuild_=false;
        resetBuildProgress();
        operation_=Operation::Idle;status_=event.text.empty() ? event.kind==EventKind::ConfigureFailed ? L"CMake завершился с ошибкой" : event.kind==EventKind::CleanFailed ? L"Очистка завершилась с ошибкой" : L"Сборка завершилась с ошибкой" : event.text;failed_=event.exitCode!=ERROR_CANCELLED;
        showBuildDuration(event);
        if(failed_ && !state_.logVisible) toggleLog();
        break;
    case EventKind::Started:status_=event.text.empty() ? L"Приложение запущено" : event.text;break;
    case EventKind::Finished:
        if(operation_!=Operation::Running) return;
        operation_=Operation::Idle;failed_=event.exitCode!=0 && event.exitCode!=ERROR_CANCELLED;status_=event.text.empty() ? L"Приложение завершено · код "+std::to_wstring(event.exitCode) : event.text;
        if(failed_ && !state_.logVisible) toggleLog();
        break;
    }
    updateControls();
}
void Panel::showBuildDuration(const Event& event) {
    lastBuildDuration_=event.buildDuration;
    lastDurationIsClean_=event.kind==EventKind::CleanSucceeded || event.kind==EventKind::CleanFailed;
    lastDurationIsConfigure_=event.kind==EventKind::ConfigureSucceeded || event.kind==EventKind::ConfigureFailed;
    if(lastBuildDuration_) appendLog(std::wstring(lastDurationIsConfigure_ ? L"\nВремя CMake: " : lastDurationIsClean_ ? L"\nВремя очистки: " : L"\nВремя сборки: ")+durationText(*lastBuildDuration_)+L"\n");
}
void Panel::resetBuildProgress() {
    Fl::remove_timeout(animateProgress,this);buildProgress_.reset();progressPulse_=0;
}
void Panel::animateProgress(void* data) {
    auto& panel=*static_cast<Panel*>(data);
    if(panel.closing_ || panel.operation_!=Operation::Building || panel.buildProgress_) return;
    panel.progressPulse_=(panel.progressPulse_+1)%50;
    panel.damage(FL_DAMAGE_ALL,15,104,panel.w()-30,5);
    Fl::repeat_timeout(.08,animateProgress,data);
}
void Panel::drawBuildProgress() {
    if(operation_!=Operation::Building) return;
    const int width=w()-30;
    fl_push_clip(15,104,width,5);fl_color(colors_.soft);fl_rectf(15,104,width,5);fl_color(colors_.accent);
    if(buildProgress_) {
        const auto fraction=static_cast<long double>(buildProgress_->completed)/buildProgress_->total;
        fl_rectf(15,104,static_cast<int>(width*fraction),5);
    } else {
        const int segment=std::max(30,width/5);
        const int offset=static_cast<int>((width+segment)*progressPulse_/50)-segment;
        fl_rectf(15+offset,104,segment,5);
    }
    fl_pop_clip();
}
void Panel::draw() {
    // Child-only damage must retain the surrounding pixels. Clearing the whole
    // background here would erase siblings which FLTK does not redraw.
    if(damage() & ~FL_DAMAGE_CHILD) {
    fl_color(colors_.surface);fl_rectf(0,0,w(),h());fl_color(colors_.line);fl_rect(0,0,w(),h());
    fl_color(colors_.accent);fl_rectf(16,23,5,5);fl_rectf(23,16,5,5);fl_rectf(23,23,5,5);
    ellipsis(platform::utf8(appTitle)+" · FLTK",39,9,button(Control::Pin).x()-47,28,appFont,13,colors_.text);
    auto project=state_.settings.cmakeFile.empty() ? L"Выберите проект" : std::filesystem::path(state_.settings.cmakeFile).parent_path().filename().wstring();
    if(project.empty()) project=L"CMake-проект";
    const int projectX=button(Control::PickMenu).x()+button(Control::PickMenu).w()+10;
    ellipsis(platform::utf8(project),projectX,55,button(Control::Build).x()-projectX-10,21,appFont,13,colors_.text);
    ellipsis(platform::utf8(state_.settings.cmakeFile.empty() ? L"CMakeLists.txt" : state_.settings.cmakeFile),projectX,77,button(Control::Build).x()-projectX-10,21,appFont,12,colors_.muted);
    drawBuildProgress();
    fl_color(colors_.line);fl_line(0,113,w(),113);fl_color(failed_ ? colors_.error : operation_==Operation::Idle ? colors_.success : colors_.accent);fl_pie(16,128,6,6,0,360);
    const auto duration=operation_==Operation::Building && buildProgress_
        ? std::format("{}%",buildProgress_->completed==buildProgress_->total ? 100 : std::min(99,static_cast<int>(100.0L*buildProgress_->completed/buildProgress_->total)))
        : lastBuildDuration_ ? platform::utf8(std::wstring(lastDurationIsConfigure_ ? L"CMake: " : lastDurationIsClean_ ? L"Очистка: " : L"Сборка: ")+durationText(*lastBuildDuration_)) : std::string{};
    fl_font(appFont,12);
    const int durationWidth=duration.empty() ? 0 : static_cast<int>(std::ceil(fl_width(duration.c_str())));
    const int statusWidth=std::max(0,w()-44-(durationWidth ? durationWidth+16 : 0));
    const auto status=canQueueRun() && runAfterBuild_ ? L"Запуск после сборки · "+status_ : status_;
    ellipsis(platform::utf8(status),30,116,statusWidth,31,appFont,12,failed_ ? colors_.error : colors_.muted);
    if(durationWidth) {
        fl_color(colors_.muted);fl_draw(duration.c_str(),w()-14-durationWidth,116,durationWidth,31,FL_ALIGN_RIGHT|FL_ALIGN_INSIDE|FL_ALIGN_CLIP,nullptr,0);
    }
    if(state_.logVisible) {fl_color(colors_.line);fl_line(15,151,w()-15,151);}
    }
    draw_children();
}
}
