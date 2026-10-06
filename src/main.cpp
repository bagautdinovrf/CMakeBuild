#include "ui.hpp"
#include "platform.hpp"
#include <FL/Fl.H>
#include <FL/Fl_Tooltip.H>
#include <FL/fl_ask.H>
#include <exception>

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    cb::platform::initialize();
    int result=1;
    try {
        Fl::lock();
        Fl::scheme("gtk+");
        Fl::visible_focus(1);
        Fl_Tooltip::delay(.5f);
        cb::AppState state;
        state.load();
        cb::Panel panel(state);
        Fl_Tooltip::font(cb::appFont);
        Fl_Tooltip::size(12);
        panel.show();
        result=Fl::run();
        panel.closePanel();
    } catch(const std::exception&) {
        fl_alert("Не удалось запустить панель CMakeBuild.");
    }
    cb::platform::shutdown();
    return result;
}
