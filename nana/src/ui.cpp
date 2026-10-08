#include "ui.hpp"
#include "platform.hpp"
#include "version.hpp"
#include "../tests/visual_fixture.hpp"
#include <nana/gui.hpp>
#include <nana/gui/element.hpp>
#include <nana/gui/widgets/checkbox.hpp>
#include <nana/gui/widgets/scroll.hpp>
#include <nana/paint/pixel_buffer.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unordered_set>
#include <map>
#include <thread>
#include <utility>

namespace cb::na {
namespace {
class FlatButton final : public nana::element::element_interface {
public:
    bool draw(graph_reference g, const nana::color& bg, const nana::color& fg,
        const nana::rectangle& r, nana::element_state state) override {
        auto fill = bg;
        if (state == nana::element_state::pressed) fill = bg.blend(fg, .20);
        else if (state == nana::element_state::hovered || state == nana::element_state::focus_hovered)
            fill = bg.blend(fg, .10);
        g.round_rectangle(r, 4, 4, bg.blend(fg, .18), true, fill);
        return true;
    }
};
class MenuRenderer final : public nana::menu::renderer_interface {
public:
    explicit MenuRenderer(Palette p,double scale=1.0,bool journal=false) : p_(std::move(p)),scale_(scale),journal_(journal) {}
    void background(graph_reference g, nana::window) override {
        g.typeface(nana::paint::font{journal_ ? "Microsoft Sans Serif" : "Segoe UI",journal_ ? 10.5 : 9.75,{},static_cast<unsigned>(std::lround(96*scale_))});
        g.rectangle(true, p_.surface); g.rectangle(false, p_.border);
    }
    void item(graph_reference g, const nana::rectangle& r, const attr& a) override {
        current_=r;
        if (a.item_state == state::active && a.enabled) g.rectangle(r, true, p_.accent);
    }
    void item_image(graph_reference, const nana::point&, unsigned, const nana::paint::image&) override {}
    void item_text(graph_reference g, const nana::point&, const std::string& t, unsigned, const attr& a) override {
        const auto fg=!a.enabled ? p_.text.blend(p_.soft,.67) : a.item_state==state::active ? (p_.surface.r()>128 ? nana::color{255,255,255} : nana::color{0,0,0}) : p_.text;
        auto value=platform::utf16(t);
        auto caption=value,shortcut=std::wstring{};
        if(const auto tab=value.find(L'\t');tab!=std::wstring::npos) {caption=value.substr(0,tab);shortcut=value.substr(tab+1);}
        const int inset=static_cast<int>(std::lround((journal_ ? 5 : 22)*scale_));
        const auto extent=g.text_extent_size(caption);
        g.string({current_.x+inset,current_.y+(static_cast<int>(current_.height)-static_cast<int>(extent.height))/2},caption,fg);
        if(!shortcut.empty()) { const auto size=g.text_extent_size(shortcut);g.string({static_cast<int>(g.width())-static_cast<int>(size.width)-static_cast<int>(std::lround(8*scale_)),current_.y+(static_cast<int>(current_.height)-static_cast<int>(size.height))/2},shortcut,fg); }
        if (a.check_style!=nana::menu::checks::none) {
            const int cx=current_.x+static_cast<int>(std::lround(9*scale_)),cy=current_.y+static_cast<int>(current_.height)/2;
            if(a.check_style==nana::menu::checks::option) {
                const int radius=static_cast<int>(std::lround(4*scale_));
                for(int y=-radius;y<=radius;++y) for(int x=-radius;x<=radius;++x) if(std::abs(std::hypot(x,y)-radius)<=.65) g.set_pixel(cx+x,cy+y,fg);
                if(a.checked) for(int y=-2;y<=2;++y) for(int x=-2;x<=2;++x) if(x*x+y*y<=4) g.set_pixel(cx+x,cy+y,p_.accent);
            } else {
                const int radius=static_cast<int>(std::lround(5*scale_));g.rectangle({cx-radius,cy-radius,static_cast<unsigned>(2*radius),static_cast<unsigned>(2*radius)},false,fg);
                if(a.checked) {g.line({cx-radius+2,cy},{cx-1,cy+2},p_.accent);g.line({cx-1,cy+2},{cx+radius-2,cy-3},p_.accent);}
            }
        }
    }
    void item_text(graph_reference g, const nana::point& p, std::u8string_view t, unsigned size, const attr& a) override {
        item_text(g, p, std::string{reinterpret_cast<const char*>(t.data()), t.size()}, size, a);
    }
    void sub_arrow(graph_reference g, const nana::point& p, unsigned, const attr&) override { g.string(p, L"›", p_.foreground); }
private:
    Palette p_;
    double scale_;
    bool journal_;
    nana::rectangle current_;
};
std::string menuText(std::wstring text) {
    if (text.size() > 100) {
        std::size_t left = 46, right = text.size() - 48;
        const auto high = [](wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; };
        const auto low = [](wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; };
        if (left && high(text[left - 1]) && low(text[left])) --left;
        if (right && high(text[right - 1]) && low(text[right])) ++right;
        text = text.substr(0, left) + L"…" + text.substr(right);
    }
    auto result = platform::utf8(text);
    std::string escaped;
    for (char c : result) { escaped += c; if (c == '&') escaped += '&'; }
    return escaped;
}
void setCaption(nana::widget& w, std::wstring text) {
    if (w.caption_wstring() != text) w.caption(std::move(text));
}
void click(nana::button& b) {
    nana::arg_click arg; arg.window_handle = b.handle(); b.events().click.emit(arg, b.handle());
}
void text(nana::paint::graphics& g, std::wstring value, const nana::rectangle& box,
          const nana::color& color, int pixels, double scale, bool right = false, int originY = 0) {
    if (!box.width || !box.height) return;
    g.typeface(nana::paint::font{"Segoe UI", pixels * .75, {}, static_cast<unsigned>(std::lround(96 * scale))});
    if (g.text_extent_size(value).width > box.width) {
        const auto ellipsis = g.text_extent_size(L"…").width;
        std::size_t lo{}, hi = value.size();
        while (lo < hi) {
            const auto middle = lo + (hi - lo + 1) / 2;
            if (g.text_extent_size(std::wstring_view{value}.substr(0, middle)).width + ellipsis <= box.width) lo = middle;
            else hi = middle - 1;
        }
        if (lo && lo < value.size() && value[lo - 1] >= 0xD800 && value[lo - 1] <= 0xDBFF && value[lo] >= 0xDC00 && value[lo] <= 0xDFFF) --lo;
        value.resize(lo); value += L"…";
    }
    const auto extent = g.text_extent_size(value);
    unsigned ascent{},descent{},leading{};
    g.text_metrics(ascent,descent,leading);
    const int logicalY=static_cast<int>(std::ceil((box.y+originY)/scale-1e-6));
    const int logicalHeight=static_cast<int>(std::ceil(box.height/scale-1e-6));
    const int fontHeight=static_cast<int>((ascent+descent)/scale),fontDescent=static_cast<int>(descent/scale);
    const int baseline=logicalY+(logicalHeight-fontHeight)/2+fontHeight-fontDescent;
    const int top=static_cast<int>((baseline-(scale==1 ? 0 : 1))*scale)-originY-static_cast<int>(ascent);
    g.string({box.x + (right ? static_cast<int>(box.width) - static_cast<int>(extent.width) : 0),
              top}, value, color);
}
void icon(nana::paint::graphics& g, unsigned kind, nana::point center, double scale,
          const nana::color& color, int originX=0, int originY=0) {
    const unsigned rgb=color.px_color().value & 0xffffff;
    const unsigned stroke=std::max(1u,static_cast<unsigned>(scale));
    const auto px=[scale](int value) {return static_cast<int>(value*scale);};
    const auto point=[&](int x,int y) {return platform::PaintPoint{center.x+x,center.y+y};};
    const auto line=[&](int x1,int y1,int x2,int y2) {
        platform::paintScaledLines(g.context(),{point(x1,y1),point(x2,y2)},rgb,scale,originX,originY);
    };
    const auto rectangle=[&](int x,int y,int width,int height,bool fill=false) {
        const int left=px(center.x+x)-originX,top=px(center.y+y)-originY;
        const int right=px(center.x+x+width)-originX,bottom=px(center.y+y+height)-originY;
        if(fill) g.rectangle({left,top,static_cast<unsigned>(right-left),static_cast<unsigned>(bottom-top)},true,color);
        else {
            const int inset=static_cast<int>(stroke)/2;
            platform::paintPolygon(g.context(),{{left+inset,top+inset},{right-static_cast<int>(stroke)+inset,top+inset},
                {right-static_cast<int>(stroke)+inset,bottom-static_cast<int>(stroke)+inset},{left+inset,bottom-static_cast<int>(stroke)+inset}},rgb,0,stroke,false);
        }
    };
    const auto polygon=[&](std::vector<platform::PaintPoint> points,bool fill=false) {
        for(auto& p:points) {p.x+=center.x;p.y+=center.y;}
        platform::paintScaledPolygon(g.context(),points,rgb,rgb,scale,originX,originY,fill);
    };
    const auto circle=[&](int x,int y,int radius,bool fill=false) {
        platform::paintScaledEllipse(g.context(),center.x+x-radius,center.y+y-radius,2*radius,2*radius,rgb,scale,originX,originY,fill);
    };
    switch(kind) {
    case 0: rectangle(-5,-8,11,16);line(2,3,5,3);line(5,3,5,7);line(2,3,2,7);break;
    case 1:
        polygon({{6,-8},{2,-7},{-1,-4},{-1,-1},{-8,6},{-8,8},{-6,9},{-4,8},{3,1},{6,1},{9,-2},{9,-6},{5,-2},{2,-5}});
        circle(-6,7,1);break;
    case 2: polygon({{-4,-6},{5,0},{-4,6}},true);break;
    case 3: case 9: case 10: line(-4,-2,0,2);line(0,2,4,-2);break;
    case 4: rectangle(-4,-6,8,7);line(-6,1,6,1);line(0,1,0,7);break;
    case 5:
        polygon({{-2,-8},{2,-8},{2,-6},{3,-5},{5,-7},{7,-5},{5,-3},{6,-2},
            {8,-2},{8,2},{6,2},{5,3},{7,5},{5,7},{3,5},{2,6},
            {2,8},{-2,8},{-2,6},{-3,5},{-5,7},{-7,5},{-5,3},{-6,2},
            {-8,2},{-8,-2},{-6,-2},{-5,-3},{-7,-5},{-5,-7},{-3,-5},{-2,-6}});
        circle(0,0,3);break;
    case 6: rectangle(-6,-8,13,17);line(-3,-4,3,-4);line(-3,0,3,0);line(-3,4,1,4);break;
    case 7: line(-5,3,5,3);break;
    case 8: case 11: line(-5,-5,5,5);line(5,-5,-5,5);break;
    case 12: rectangle(-4,-4,8,8,true);break;
    case 13: circle(0,0,3,true);break;
    }
}
template<bool Vertical> void paintScroll(nana::scroll<Vertical>& scroll, const Palette& p, double scale) {
    struct ScrollStyle { Palette palette; double scale; };
    static std::map<nana::window,std::shared_ptr<ScrollStyle>> drawings;
    const auto handle=scroll.handle();
    if(const auto old=drawings.find(handle);old!=drawings.end()) {
        auto& style=*old->second;
        if(style.scale==scale && style.palette.scrollTrack==p.scrollTrack
            && style.palette.scrollThumb==p.scrollThumb && style.palette.muted==p.muted) return;
        style={p,scale};
        scroll.scheme().button_size=static_cast<unsigned>(std::lround(16*scale));
        nana::api::refresh_window(scroll);
        return;
    }
    scroll.scheme().button_size=static_cast<unsigned>(std::lround(16*scale));
    const auto style=std::make_shared<ScrollStyle>(ScrollStyle{p,scale});
    drawings.emplace(handle,style);
    scroll.events().destroy([handle] { drawings.erase(handle); });
    scroll.drawing([&scroll,style](nana::paint::graphics& g) {
        const auto& p=style->palette;
        const auto scale=style->scale;
        g.rectangle(true,p.scrollTrack);
        const int button=static_cast<int>(scroll.scheme().button_size);
        const int extent=static_cast<int>(Vertical ? g.height() : g.width());
        const int thickness=static_cast<int>(Vertical ? g.width() : g.height());
        const auto arrowExtent=static_cast<unsigned>(std::min(button,extent/2));
        if constexpr(Vertical) {
            g.rectangle({0,0,g.width(),arrowExtent},true,p.scrollThumb);
            g.rectangle({0,extent-static_cast<int>(arrowExtent),g.width(),arrowExtent},true,p.scrollThumb);
        } else {
            g.rectangle({0,0,arrowExtent,g.height()},true,p.scrollThumb);
            g.rectangle({extent-static_cast<int>(arrowExtent),0,arrowExtent,g.height()},true,p.scrollThumb);
        }
        if(extent>2*button && scroll.range() && scroll.amount()>scroll.range()) {
            const auto available=static_cast<unsigned>(extent-2*button);
            const auto length=std::max(static_cast<unsigned>(button),static_cast<unsigned>(available*scroll.range()/scroll.amount()));
            const int position=static_cast<int>(scroll.value()*(available-length)/(scroll.amount()-scroll.range()));
            const int start=button+position;
            const auto thumb=Vertical ? nana::rectangle{0,start,static_cast<unsigned>(thickness),length}
                : nana::rectangle{start,0,length,static_cast<unsigned>(thickness)};
            g.rectangle(thumb,true,p.scrollThumb);
        }
        for (bool forward : {false,true}) {
            const int major=forward ? extent-button/2 : button/2, minor=thickness/2;
            const int radius=std::max(2,static_cast<int>(std::lround(4*scale)));
            for(int row=0;row<radius;++row) {
                const int coordinate=major+(forward ? radius/2-row : row-radius/2);
                if(Vertical) g.line({minor-row,coordinate},{minor+row,coordinate},p.muted);
                else g.line({coordinate,minor-row},{coordinate,minor+row},p.muted);
            }
        }
    });
    nana::api::refresh_window(scroll);
}
}

Palette Palette::system(bool dark) {
    Palette p = dark ? Palette{nana::color(32,35,41),nana::color(43,48,56),nana::color(238,241,245),nana::color(169,180,195),nana::color(60,67,78),nana::color(118,173,255),nana::color(20,36,61),nana::color(116,214,163),nana::color(255,149,149),nana::color(43,49,57),nana::color(62,72,84)}
        : Palette{nana::color(255,255,255),nana::color(244,246,248),nana::color(36,41,51),nana::color(104,115,130),nana::color(223,228,235),nana::color(18,101,211),nana::color(255,255,255),nana::color(34,123,83),nana::color(182,49,54),nana::color(244,246,249),nana::color(218,225,236)};
    p.background=p.surface; p.foreground=p.text; p.border=p.line; return p;
}
void colorWidget(nana::widget& widget, const Palette& p, bool field) {
    widget.bgcolor(field ? p.surface : p.background);
    widget.fgcolor(p.foreground);
}
void styleButton(nana::button& button) {
    button.set_bground(nana::pat::cloneable<nana::element::element_interface>(FlatButton{}));
    button.edge_effects(false); button.enable_focus_color(false);
}
void styleScrollbars(nana::widget& widget, const Palette& p, double scale) {
    nana::api::enum_widgets<nana::widget>(widget,true,[&](nana::widget& child) {
        if(auto* vertical=dynamic_cast<nana::scroll<true>*>(&child)) paintScroll(*vertical,p,scale);
        else if(auto* horizontal=dynamic_cast<nana::scroll<false>*>(&child)) paintScroll(*horizontal,p,scale);
    });
    if(auto* vertical=dynamic_cast<nana::scroll<true>*>(&widget)) paintScroll(*vertical,p,scale);
    else if(auto* horizontal=dynamic_cast<nana::scroll<false>*>(&widget)) paintScroll(*horizontal,p,scale);
}
void preparePreview(nana::form& form) {
    nana::api::take_active(form, false, nullptr);
    platform::preparePreviewWindow(form.native_handle());
    form.show();
    platform::drainPreviewMessages();
}
std::uint64_t savePreview(nana::form& form, const std::wstring& file) {
    nana::api::refresh_window_tree(form);
    nana::paint::graphics image;
    if (!nana::api::window_graphics(form, image)) throw std::runtime_error("could not capture Nana window");
    nana::paint::pixel_buffer pixels{image.handle(), nana::rectangle{image.size()}};
    std::unordered_set<unsigned> colors;
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned row = 0; row < pixels.size().height; ++row) {
        const auto data = pixels.raw_ptr(row);
        for (unsigned column = 0; column < pixels.size().width; ++column) {
            const auto value = data[column].value & 0x00ffffff;
            if (colors.size() < 32) colors.insert(value);
            hash = (hash ^ value) * 1099511628211ull;
        }
    }
    if (colors.size() < 8) throw std::runtime_error("Nana preview is blank or has no rendered controls");
    image.save_as_file(platform::utf8(file).c_str());
    if (!std::filesystem::is_regular_file(file)) throw std::runtime_error("Nana preview was not saved");
    return hash;
}

void JournalBox::restoreView(nana::point p) { get_drawer_trigger().editor()->restore_content_origin(p); }
void JournalBox::scrollSpace(unsigned pixels) { get_drawer_trigger().editor()->scroll_space(pixels); }
void JournalBox::scrollCorner(const nana::color& color) { get_drawer_trigger().editor()->scroll_corner_color(color); }
void JournalBox::padding(unsigned top,unsigned right,unsigned bottom,unsigned left) {get_drawer_trigger().editor()->padding(top,right,bottom,left);}
unsigned JournalBox::linePitch() const { return std::max(1u, get_drawer_trigger().editor()->line_height()); }
void JournalBox::textGeometry(double scale,int logicalTextY,int physicalWindowY) {
    auto* editor=get_drawer_trigger().editor();
    const auto oldCaret=caret_pos();
    const auto oldSelection=selection();const bool hadSelection=selected();
    const auto oldOrigin=content_origin();
    const unsigned oldPitch=std::max(1u,editor->line_height());
    nana::paint::graphics metrics{{1,1}};metrics.typeface(typeface());
    unsigned ascent{},descent{},leading{};metrics.text_metrics(ascent,descent,leading);
    const int logicalHeight=static_cast<int>((ascent+descent)/scale),logicalDescent=static_cast<int>(descent/scale);
    const unsigned pitch=std::max(1u,static_cast<unsigned>(logicalHeight*scale));
    const int top=static_cast<int>((logicalTextY+logicalHeight-logicalDescent-(scale==1 ? 0 : 1))*scale)-static_cast<int>(ascent)-physicalWindowY;
    editor->line_height(pitch);
    editor->text_y_offset(top-editor->text_area(false).y);
    caret_pos(oldCaret,false);if(hadSelection) select_points(oldSelection.first,oldSelection.second);
    auto origin=oldOrigin;
    origin.y=oldOrigin.y/static_cast<int>(oldPitch)*static_cast<int>(pitch)+std::min(oldOrigin.y%static_cast<int>(oldPitch),static_cast<int>(pitch)-1);
    restoreView(origin);
}
void JournalBox::selectWordAt(nana::point point) {
    const auto oldCaret=caret_pos(); const auto oldOrigin=content_origin();
    const auto oldSelection=selection(); const bool hadSelection=selected();
    const auto click=get_drawer_trigger().editor()->mouse_caret(point,false);
    auto content=caption_wstring(); std::erase(content,L'\r');
    const auto offset=[&](nana::upoint p) {
        std::size_t start{};
        for(unsigned row=0;row<p.y && start<content.size();++row) {
            const auto newline=content.find(L'\n',start);
            if(newline==std::wstring::npos) return content.size();
            start=newline+1;
        }
        const auto newline=content.find(L'\n',start);
        return start+std::min<std::size_t>(p.x,(newline==std::wstring::npos ? content.size() : newline)-start);
    };
    auto pos=offset(click); const auto first=offset(oldSelection.first),last=offset(oldSelection.second);
    if(hadSelection && ((first<pos && pos<last) || (last<pos && pos<first))) {
        caret_pos(oldCaret,false); select_points(oldSelection.first,oldSelection.second); restoreView(oldOrigin); return;
    }
    const auto high=[](wchar_t c) {return c>=0xD800 && c<=0xDBFF;};
    const auto low=[](wchar_t c) {return c>=0xDC00 && c<=0xDFFF;};
    if(pos && pos<content.size() && low(content[pos]) && high(content[pos-1])) --pos;
    const auto previous=[&](std::size_t p) {if(p) --p;if(p && low(content[p]) && high(content[p-1])) --p;return p;};
    const auto next=[&](std::size_t p) {if(p<content.size()) {if(high(content[p]) && p+1<content.size() && low(content[p+1])) ++p;++p;}return p;};
    // FLTK treats ASCII alphanumerics/underscore as words; all other Unicode
    // stays within a word except NBSP and ideographic punctuation U+3000–301F.
    const auto separator=[&](std::size_t p) {
        if(p>=content.size()) return true;
        const auto c=content[p];
        if(c<128) return !((c>=L'0' && c<=L'9') || (c>=L'A' && c<=L'Z') || (c>=L'a' && c<=L'z') || c==L'_');
        return c==0xA0 || (c>=0x3000 && c<=0x301F);
    };
    std::size_t begin=pos,end=pos;
    if(pos<content.size() && content[pos]!=L'\n' && content[pos]!=0) {
        while(begin && !separator(begin)) begin=previous(begin);
        if(separator(begin)) begin=next(begin);
        while(end<content.size() && !separator(end)) end=next(end);
    }
    if(begin>end) std::swap(begin,end);
    const auto anchor=[&](std::size_t p) {
        unsigned row{}; std::size_t line{};
        for(std::size_t i=0;i<p;++i) if(content[i]==L'\n') {++row;line=i+1;}
        return nana::upoint{static_cast<unsigned>(p-line),row};
    };
    select_points(anchor(begin),anchor(end)); caret_pos(oldCaret,false); restoreView(oldOrigin);
}
void JournalBox::scrollToEnd() {
    const auto lines = text_line_count();
    const auto last = lines ? lines - 1 : 0;
    const auto& text = get_drawer_trigger().editor()->textbase().getline(last);
    caret_pos({static_cast<unsigned>(text.size()), static_cast<unsigned>(last)});
}
void JournalBox::update(const Journal& journal, bool autoScroll) {
    if (journal.revision() == revision_) return;
    // Append/pruning may temporarily move the caret and scrollbar values.
    // Publish the widget tree only after the complete reading state is restored.
    nana::api::batch_updates(*this, [&] {
        const auto oldOrigin = content_origin();
        const auto oldCaret = caret_pos();
        const auto oldSelection = selection();
        const auto anchor = get_drawer_trigger().editor()->content_anchor();
        const auto anchorCoordinates = get_drawer_trigger().editor()->content_coordinates(anchor);
        const bool hadSelection = selected();
        const auto positions = text_position();
        const auto lastLine = text_line_count() ? text_line_count() - 1 : 0;
        const bool atEnd = positions.empty() || positions.back().y >= lastLine;
        const bool follow = autoScroll && atEnd && !hadSelection;
        const auto removedNow = journal.totalRemovedBytes() - removed_;
        std::size_t removedUnits{};
        std::wstring droppedWide, previousWide, currentWide;
        if (removedNow) {
            const auto dropped = previous_.substr(0, static_cast<std::size_t>(std::min<std::uint64_t>(removedNow, previous_.size())));
            droppedWide = platform::utf16(dropped);
            removedUnits = droppedWide.size();
            if (!follow) {
                previousWide = platform::utf16(previous_); currentWide = platform::utf16(journal.text());
            }
        }
        if (!removedNow && journal.text().starts_with(previous_)) {
            // Appending with a selection would replace it. Temporarily release the
            // selection, then restore caret/selection and both viewport coordinates.
            select(false);
            append(platform::utf16(journal.text().substr(previous_.size())), false);
        } else if (removedNow && removedNow <= previous_.size()
            && journal.text().starts_with(std::string_view{previous_}.substr(static_cast<std::size_t>(removedNow)))) {
            // Retained rows already have measured extents. Erase only the old prefix,
            // then append new rows instead of replacing and measuring the whole log.
            const auto row = static_cast<unsigned>(std::count(droppedWide.begin(), droppedWide.end(), L'\n'));
            const auto newline = droppedWide.rfind(L'\n');
            const nana::upoint end{static_cast<unsigned>(newline == std::wstring::npos
                ? droppedWide.size() : droppedWide.size() - newline - 1), row};
            auto* editor = get_drawer_trigger().editor();
            editor->select_points({0, 0}, end);
            editor->backspace(false, false);
            const auto retainedBytes = previous_.size() - static_cast<std::size_t>(removedNow);
            append(platform::utf16(journal.text().substr(retainedBytes)), false);
        } else get_drawer_trigger().editor()->text(platform::utf16(journal.text()),false);
        if (follow) scrollToEnd();
        else {
            const auto adjust = [&](nana::upoint p) {
                if (!removedNow) return p;
                std::size_t line{}, offset{};
                while (line < p.y && offset < previousWide.size()) {
                    const auto newline = previousWide.find(L'\n', offset);
                    if (newline == std::wstring::npos) { offset = previousWide.size(); break; }
                    offset = newline + 1; ++line;
                }
                const auto lineEnd = previousWide.find(L'\n', offset);
                offset += std::min<std::size_t>(p.x, (lineEnd == std::wstring::npos ? previousWide.size() : lineEnd) - offset);
                offset = offset > removedUnits ? offset - removedUnits : 0;
                offset = std::min(offset, currentWide.size());
                const auto row = static_cast<unsigned>(std::count(currentWide.begin(), currentWide.begin() + static_cast<std::ptrdiff_t>(offset), L'\n'));
                const auto start = offset ? currentWide.rfind(L'\n', offset - 1) : std::wstring::npos;
                return nana::upoint{static_cast<unsigned>(start == std::wstring::npos ? offset : offset - start - 1), row};
            };
            caret_pos(adjust(oldCaret), false);
            if (hadSelection) select_points(adjust(oldSelection.first), adjust(oldSelection.second));
            if (removedNow) {
                const auto coordinates = get_drawer_trigger().editor()->content_coordinates(adjust(anchor));
                restoreView({std::max(0, oldOrigin.x + coordinates.x - anchorCoordinates.x), std::max(0, oldOrigin.y + coordinates.y - anchorCoordinates.y)});
            } else restoreView(oldOrigin);
        }
        previous_ = journal.text(); revision_ = journal.revision(); removed_ = journal.totalRemovedBytes();
        nana::api::refresh_window(*this);
    });
}

Panel::Panel(AppState& state, bool hidden)
    : state_(state), controller_(state),
      form_(nana::rectangle{30, 30, static_cast<unsigned>(std::max(500,state.width)), 150},
        nana::appearance(false, true, false, false, false, false, false)),
      project_(form_), recent_(form_), build_(form_, L"Собрать"), buildMenu_(form_),
      run_(form_, L"Запустить"), runMenu_(form_), settings_(form_), pin_(form_, L"Закрепить"),
      log_(form_), minimize_(form_), close_(form_), journal_(form_),
      timer_(std::chrono::milliseconds{65}), hidden_(hidden) {
    form_.caption(appTitle);
    const std::array<nana::button*,11> buttons{&project_, &build_, &run_, &runMenu_, &pin_, &settings_, &log_, &minimize_, &close_, &buildMenu_, &recent_};
    for (unsigned index=0;index<buttons.size();++index) {
        auto* b=buttons[index]; styleButton(*b); bindKeys(*b);
        b->drawing([this,b,index](nana::paint::graphics& g) { drawButton(g,*b,index); });
        b->events().mouse_enter([this,b,index] { buttonStates_[index].hover=true; nana::api::refresh_window(*b); });
        b->events().mouse_leave([this,b,index] { buttonStates_[index].hover=false; nana::api::refresh_window(*b); });
        b->events().mouse_down([this,b,index](const nana::arg_mouse& a) { if(a.button==nana::mouse::left_button) buttonStates_[index].pressed=true; nana::api::refresh_window(*b); });
        b->events().mouse_up([this,b,index] { buttonStates_[index].pressed=false; nana::api::refresh_window(*b); });
    }
    form_.drawing([this](nana::paint::graphics& g) { drawPanel(g); });
    journal_.multi_lines(true).editable(false).enable_caret().line_wrapped(false);
    journal_.set_undo_queue_length(0);
    journal_.enable_border_focused(false); nana::api::widget_borderless(journal_,true);
    // Textbox's outer focus/hover effect is independent of its editor border.
    nana::api::effects_edge_nimbus(journal_, nana::effects::edge_nimbus::none);
    bindKeys(form_); bindKeys(journal_);
    platform::configurePanelWindow(form_.native_handle());
    platform::setApplicationIcon(form_.native_handle());
    platform::setTopmost(form_.native_handle(), state_.pinned && !hidden_);
    scale_ = platform::windowScale(form_.native_handle());
    theme(platform::darkTheme()); layout(true);
    project_.events().click([this] { selectProject(); });
    recent_.events().click([this] { showProjectMenu(); });
    build_.events().click([this] { if (state_.settings.cmakeFile.empty() && !hidden_) selectProject(); controller_.build(state_.buildAndRun ? BuildAction::BuildAndRun : BuildAction::Build); refresh(); });
    buildMenu_.events().click([this] { showBuildMenu(); });
    run_.events().click([this] { if (!controller_.run()) showRunMenu(); refresh(); });
    runMenu_.events().click([this] { showRunMenu(); });
    settings_.events().click([this] { showSettings(form_, controller_, palette_); refresh(); });
    pin_.events().click([this] { state_.pinned = !state_.pinned; platform::setTopmost(form_.native_handle(), state_.pinned); controller_.save(); refresh(); });
    log_.events().click([this] { controller_.toggleLog(); layout(true); refresh(); });
    minimize_.events().click([this] { platform::minimizeWindow(form_.native_handle()); });
    close_.events().click([this] { form_.close(); });
    journal_.events().mouse_down([this](const nana::arg_mouse& a) {
        if (a.button != nana::mouse::right_button) return;
        journal_.selectWordAt(a.pos);
        prepareMenu(journalMenu_); journalMenu_.clear();
        journalMenu_.append("Cut", [](nana::menu::item_proxy&) {}).enabled(false);
        journalMenu_.append("Copy", [this](nana::menu::item_proxy&) { journal_.copy(); });
        journalMenu_.append("Paste", [](nana::menu::item_proxy&) {}).enabled(false);
        journalMenu_.popup(journal_, a.pos.x, a.pos.y);
    });
    form_.events().resizing([this](const nana::arg_resizing& a) {
        a.width = std::max(a.width, static_cast<unsigned>(500 * scale_));
        a.height = state_.logVisible ? std::max(a.height, static_cast<unsigned>(240 * scale_))
            : static_cast<unsigned>(150 * scale_);
    });
    form_.events().resized([this] { if (!layoutActive_) { layout(); saveGeometry(); } });
    form_.events().move([this] { if (!layoutActive_) saveGeometry(); });
    auto edgeDrag = std::make_shared<platform::WindowDrag>();
    auto edgeDragging = std::make_shared<bool>(false);
    const auto finishDrag = [this, edgeDragging] {
        if (!std::exchange(*edgeDragging, false)) return;
        if (form_.empty()) return;
        form_.release_capture();
        form_.cursor(nana::cursor::arrow);
        saveGeometry();
    };
    dragCancelHandler_.bind(form_.native_handle(), finishDrag);
    form_.events().unload([this, finishDrag] { finishDrag(); saveGeometry(); controller_.close(); });
    const auto edgesAt = [this](nana::point point) {
        const int margin = std::max(4, static_cast<int>(std::lround(6 * scale_)));
        unsigned edges{};
        if (point.x < margin) edges |= 1;
        else if (point.x >= static_cast<int>(form_.size().width) - margin) edges |= 2;
        if (state_.logVisible) { if(point.y<margin) edges|=4; else if(point.y >= static_cast<int>(form_.size().height) - margin) edges |= 8; }
        return edges;
    };
    form_.events().mouse_down([this, edgeDrag, edgeDragging, edgesAt](const nana::arg_mouse& a) {
        if (a.button != nana::mouse::left_button) return;
        const auto edges = edgesAt(a.pos);
        if (edges || (a.pos.y<static_cast<int>(std::lround(43*scale_)) && a.pos.x<pin_.pos().x-static_cast<int>(std::lround(8*scale_)))) {
            *edgeDragging = platform::beginWindowDrag(form_.native_handle(), edges, *edgeDrag, scale_);
            // Route mouse-up to the form even when DPI/layout puts a child
            // widget underneath the cursor during this drag.
            if (*edgeDragging) form_.set_capture(true);
        }
    });
    form_.events().mouse_move([this, edgeDrag, edgeDragging, edgesAt, finishDrag](const nana::arg_mouse& a) {
        if (*edgeDragging) {
            if (!a.left_button || !platform::updateWindowDrag(form_.native_handle(), *edgeDrag, 500,
                state_.logVisible ? 240 : 150, state_.logVisible ? 0 : 150, scale_)) finishDrag();
            return;
        }
        const auto edges = edgesAt(a.pos);
        form_.cursor(((edges & 8) && (edges & 1)) || ((edges & 4) && (edges & 2)) ? nana::cursor::size_bottom_left
            : ((edges & 8) && (edges & 2)) || ((edges & 4) && (edges & 1)) ? nana::cursor::size_bottom_right
            : (edges & 12) ? nana::cursor::size_ns : (edges & 3) ? nana::cursor::size_we : nana::cursor::arrow);
    });
    form_.events().mouse_up([finishDrag](const nana::arg_mouse& a) {
        if (a.button == nana::mouse::left_button) finishDrag();
    });
    dpiHandler_.bind(form_.native_handle(), [this, edgeDrag, edgeDragging, finishDrag](double scale, platform::DesktopRect bounds) {
        dpiChanged(scale, bounds, *edgeDragging ? edgeDrag.get() : nullptr);
        if (*edgeDragging && !platform::rebaseWindowDrag(form_.native_handle(), *edgeDrag, scale)) finishDrag();
    });
    if (state_.x != std::numeric_limits<int>::min() && !hidden_)
        platform::restoreNativePosition(form_.native_handle(), state_.x, state_.y);
    timer_.elapse([this] {
        controller_.drainEvents();
        const bool dark = platform::darkTheme(); if (dark != dark_) theme(dark);
        refreshPanel(true);
        if (controller_.runSelectionRequested() && !runMenuOpen_) showRunMenu();
    });
    if(!hidden_) timer_.start(); refresh();
    if (!hidden_) form_.show();
}
Panel::~Panel() { timer_.stop(); controller_.close(); }
int Panel::run() { nana::exec(); return 0; }
void Panel::drawButton(nana::paint::graphics& g, nana::button& button, unsigned control) {
    const auto& state=buttonStates_[control];
    const bool primary=control==2 || control==3;
    const bool selected=(control==4 && state_.pinned) || (control==6 && state_.logVisible);
    const bool enabled=button.enabled();
    const bool buildMode=(control==1 || control==9) && controller_.operation()==Operation::Idle && enabled;
    const auto modeText=nana::color{36,41,51};
    auto bg=buildMode ? state_.buildAndRun ? nana::color{196,255,210} : nana::color{188,197,171}
        : primary && enabled ? palette_.accent : enabled && (state.hover || state.pressed || selected) ? palette_.soft : palette_.surface;
    if(enabled && (state.pressed || ((primary || buildMode) && state.hover))) bg=bg.blend(buildMode ? modeText : palette_.text,state.pressed ? .14 : .06);
    const auto fg=!enabled ? palette_.muted.blend(palette_.surface,.32) : buildMode ? modeText : primary ? palette_.accentText : selected ? palette_.accent : palette_.text;
    g.rectangle(true,palette_.surface);
    const auto r=buttonRectangles_[control];
    int bx=r.x+1,bw=static_cast<int>(r.width)-2;
    const auto join=[&](unsigned main,unsigned arrow,bool isArrow) {
        if(isArrow) { bx-=static_cast<int>(buttonRectangles_[main].width); bw+=static_cast<int>(buttonRectangles_[main].width); }
        else bw+=static_cast<int>(buttonRectangles_[arrow].width);
    };
    if(control==2 || control==3) join(2,3,control==3);
    else if(control==1 || control==9) join(1,9,control==9);
    else if(control==0 || control==10) join(0,10,control==10);
    const bool outline=primary || control==0 || control==10 || control==1 || control==9;
    std::vector<platform::PaintPoint> contour;
    constexpr double lut[]{0.0,.07612,.29289,.61732,1.0};
    const double top=r.y+1,bottom=r.y+r.height-2,right=bx+bw-1;
    const auto add=[&](double x,double y) {contour.push_back({static_cast<int>(x),static_cast<int>(y)});};
    contour.reserve(20);
    for(unsigned i=0;i<5;++i) add(bx+lut[i]*4,top+lut[4-i]*4);
    for(unsigned i=0;i<5;++i) add(right-lut[4-i]*4,top+lut[i]*4);
    for(unsigned i=0;i<5;++i) add(right-lut[i]*4,bottom-lut[4-i]*4);
    for(unsigned i=0;i<5;++i) add(bx+lut[4-i]*4,bottom-lut[i]*4);
    platform::paintScaledPolygon(g.context(),contour,bg.px_color().value & 0xffffff,bg.px_color().value & 0xffffff,scale_,button.pos().x,button.pos().y,true);
    if(outline) platform::paintScaledPolygon(g.context(),contour,palette_.line.px_color().value & 0xffffff,0,scale_,button.pos().x,button.pos().y,false);
    if(control==3 || control==9 || control==10) {
        platform::paintScaledLines(g.context(),{{r.x,r.y+8},{r.x,r.y+static_cast<int>(r.height)-8}},palette_.line.px_color().value & 0xffffff,scale_,button.pos().x,button.pos().y);
    }
    const bool label=control==1 || control==2 || control==4;
    const nana::point center{r.x+static_cast<int>(label ? 17 : r.width/2),r.y+static_cast<int>(r.height/2)+(state.pressed ? 1 : 0)};
    const unsigned kind=control==1 && controller_.operation()==Operation::Building ? 11 : control==2 && controller_.operation()==Operation::Running ? 12 : control;
    icon(g,kind,center,scale_,fg,button.pos().x,button.pos().y);
    if(label) text(g,button.caption_wstring(),{static_cast<int>((r.x+31)*scale_)-button.pos().x,0,static_cast<unsigned>((r.width-36)*scale_),static_cast<unsigned>(r.height*scale_)},fg,control==4 ? 12 : 13,scale_,false,button.pos().y);
    if(previewFocus_==static_cast<int>(control) || (!hidden_ && nana::api::focus_window()==button.handle())) {
        const int inset=control==6 ? 2 : 4;
        platform::paintFocusRectangle(g.context(),r.x+inset,r.y+inset,static_cast<int>(r.width)-2*inset,static_cast<int>(r.height)-2*inset,fg.px_color().value & 0xffffff,scale_,button.pos().x,button.pos().y);
    }
}
void Panel::drawPanel(nana::paint::graphics& g) {
    const auto px=[this](int v) {return static_cast<int>(v*scale_);};
    const auto box=[&](int x,int y,int width,int height) {return nana::rectangle{px(x),px(y),static_cast<unsigned>(std::max(0,px(x+width)-px(x))),static_cast<unsigned>(std::max(0,px(y+height)-px(y)))};};
    const int width=static_cast<int>(g.width()),logicalWidth=static_cast<int>(std::lround(width/scale_));
    g.rectangle(true,palette_.surface);
    for(unsigned inset=0;inset<std::max(1u,static_cast<unsigned>(scale_));++inset) g.rectangle({static_cast<int>(inset),static_cast<int>(inset),g.width()-2*inset,g.height()-2*inset},false,palette_.line);
    for(const auto position:{nana::point{16,23},nana::point{23,16},nana::point{23,23}}) g.rectangle(box(position.x,position.y,5,5),true,palette_.accent);
    text(g,std::wstring{appTitle},{px(39),px(9),static_cast<unsigned>(std::max(0,pin_.pos().x-px(47))),static_cast<unsigned>(px(28))},palette_.text,13,scale_);
    auto project=state_.settings.cmakeFile.empty() ? L"Выберите проект" : std::filesystem::path(state_.settings.cmakeFile).parent_path().filename().wstring();
    if(project.empty()) project=L"CMake-проект";
    const int projectX=px(84);
    const int projectWidth=std::max(0,px(buttonRectangles_[1].x-10)-projectX);
    text(g,project,{projectX,px(55),static_cast<unsigned>(projectWidth),static_cast<unsigned>(px(21))},palette_.text,13,scale_);
    text(g,state_.settings.cmakeFile.empty() ? L"CMakeLists.txt" : state_.settings.cmakeFile,{projectX,px(77),static_cast<unsigned>(projectWidth),static_cast<unsigned>(px(21))},palette_.muted,12,scale_);
    if(controller_.operation()==Operation::Building) {
        const int barWidth=width-px(30); g.rectangle({px(15),px(104),static_cast<unsigned>(barWidth),static_cast<unsigned>(px(5))},true,palette_.soft);
        if(const auto progress=controller_.buildProgress()) g.rectangle({px(15),px(104),static_cast<unsigned>(barWidth*static_cast<long double>(progress->completed)/progress->total),static_cast<unsigned>(px(5))},true,palette_.accent);
        else {const int segment=std::max(px(30),barWidth/5),offset=(barWidth+segment)*animation_/50-segment;const int start=std::max(0,offset),end=std::min(barWidth,offset+segment);if(end>start) g.rectangle({px(15)+start,px(104),static_cast<unsigned>(end-start),static_cast<unsigned>(px(5))},true,palette_.accent);}
    }
    const int divider=static_cast<int>(113*scale_-(scale_-1));
    g.rectangle({0,divider,static_cast<unsigned>(width),std::max(1u,static_cast<unsigned>(scale_))},true,palette_.line);
    const auto dot=controller_.failed() ? palette_.error : controller_.operation()==Operation::Idle ? palette_.success : palette_.accent;
    icon(g,13,{19,131},scale_,dot);
    auto duration=controller_.durationText();
    if(controller_.operation()==Operation::Building) if(const auto progress=controller_.buildProgress()) duration=std::to_wstring(progress->completed==progress->total ? 100 : std::min(99,static_cast<int>(100.0L*progress->completed/progress->total)))+L"%";
    g.typeface(nana::paint::font{"Segoe UI",9,{},static_cast<unsigned>(std::lround(96*scale_))});
    const int durationWidth=duration.empty() ? 0 : static_cast<int>(g.text_extent_size(duration).width);
    auto status=controller_.status().empty() ? L"Выберите CMakeLists.txt" : controller_.status();
    if(controller_.canQueueRun() && controller_.runQueued()) status=L"Запуск после сборки · "+status;
    text(g,status,{px(30),px(116),static_cast<unsigned>(std::max(0,width-px(44)-(durationWidth ? durationWidth+px(16) : 0))),static_cast<unsigned>(px(31))},controller_.failed() ? palette_.error : palette_.muted,12,scale_);
    if(durationWidth) text(g,duration,{width-px(14)-durationWidth,px(116),static_cast<unsigned>(durationWidth),static_cast<unsigned>(px(31))},palette_.muted,12,scale_,true);
    if(state_.logVisible) g.line({px(15),px(151)},{px(logicalWidth-15),px(151)},palette_.line);
}
void Panel::bindKeys(nana::widget& w) {
    w.events().key_press([this, &w](const nana::arg_keyboard& a) {
        if(!a.ctrl && !a.alt && !a.shift && a.key==nana::keyboard::enter) {
            if(auto* button=dynamic_cast<nana::button*>(&w);button && button->enabled() && nana::api::focus_window()==button->handle()) {
                click(*button); a.stop_propagation(); return;
            }
        }
        if (!a.ctrl && !a.alt && !a.shift && (a.key == VK_DOWN || a.key == VK_F4)) {
            if(!w.enabled()) return;
            if (&w == &project_ || &w == &recent_) { showProjectMenu(); a.stop_propagation(); return; }
            if (&w == &build_ || &w == &buildMenu_) { showBuildMenu(); a.stop_propagation(); return; }
            if (&w == &run_ || &w == &runMenu_) { showRunMenu(); a.stop_propagation(); return; }
        }
        shortcut(a.key, a.ctrl, a.alt, a.shift);
    });
}
void Panel::shortcut(unsigned key, bool ctrl, bool alt, bool shift) {
    if (alt || shift) return;
    if (key == VK_F5 && ctrl) { if (!controller_.run()) showRunMenu(); }
    else if (key == VK_F5 && !ctrl && controller_.operation() == Operation::Idle) { if (state_.settings.cmakeFile.empty() && !hidden_) selectProject(); controller_.build(BuildAction::BuildAndRun); }
    else if (key == VK_F6 && !ctrl && controller_.operation() != Operation::Running) { if (state_.settings.cmakeFile.empty() && !hidden_) selectProject(); controller_.build(); }
    else if ((key == 'O' || key == 'o') && ctrl && controller_.operation() == Operation::Idle) selectProject();
    refresh();
}
void Panel::saveGeometry() {
    if (hidden_ || layoutActive_ || form_.empty()) return;
    state_.width = static_cast<int>(std::lround(form_.size().width / scale_));
    if (state_.logVisible) state_.logHeight = std::max(240, static_cast<int>(std::lround(form_.size().height / scale_)));
    platform::captureNativePosition(form_.native_handle(), state_.x, state_.y);
    controller_.save();
}
void Panel::dpiChanged(double scale, platform::DesktopRect bounds, const platform::WindowDrag* drag) {
    if (previewScale_ || scale <= 0) return;
    const auto oldOrigin = journal_.content_origin();
    const auto oldScale = scale_;
    const auto oldPitch = journal_.linePitch();
    // WM_SIZE/WM_MOVE arrive inside SetWindowPos. They must see the new scale,
    // and must not save intermediate dimensions over the stable logical size.
    layoutActive_ = true;
    scale_ = scale;
    bounds.width = static_cast<int>(std::max(500, state_.width) * scale_);
    bounds.height = static_cast<int>((state_.logVisible ? std::max(240, state_.logHeight) : 150) * scale_);
    if (drag) bounds = platform::windowDragDpiBounds(*drag, bounds, scale_);
    platform::setWindowBounds(form_.native_handle(), bounds);
    layoutActive_ = false;
    layout();
    const auto pitch = journal_.linePitch();
    journal_.restoreView({static_cast<int>(oldOrigin.x * scale_ / oldScale),
        oldOrigin.y / static_cast<int>(oldPitch) * static_cast<int>(pitch)
            + std::min(static_cast<int>(oldOrigin.y % static_cast<int>(oldPitch) * scale_ / oldScale), static_cast<int>(pitch) - 1)});
    if (!drag) saveGeometry();
}
void Panel::layout(bool fitHeight) {
    if (layoutActive_) return;
    layoutActive_ = true;
    const auto px = [this](int v) { return static_cast<int>(v * scale_); };
    if (fitHeight) form_.size({static_cast<unsigned>(px(std::max(500, state_.width))), static_cast<unsigned>(px(state_.logVisible ? std::max(240, state_.logHeight) : 150))});
    observedLogVisible_ = state_.logVisible;
    const auto width = static_cast<int>(form_.size().width);
    const std::array<nana::button*,11> buttons{&project_,&build_,&run_,&runMenu_,&pin_,&settings_,&log_,&minimize_,&close_,&buildMenu_,&recent_};
    const auto box = [&](nana::widget& w, int x, int y, int ww, int hh) {
        const auto button=std::find(buttons.begin(),buttons.end(),&w);
        if(button!=buttons.end()) buttonRectangles_[static_cast<std::size_t>(button-buttons.begin())]={x,y,static_cast<unsigned>(ww),static_cast<unsigned>(hh)};
        w.move(nana::rectangle{px(x), px(y), static_cast<unsigned>(px(x+ww)-px(x)), static_cast<unsigned>(px(y+hh)-px(y))});
    };
    const int logicalWidth = static_cast<int>(std::lround(width / scale_));
    nana::paint::graphics measuring{{1,1}};
    const auto captionWidth=[&](int pixels,std::initializer_list<const wchar_t*> captions) {
        measuring.typeface(nana::paint::font{"Segoe UI",pixels*.75,{},96});
        unsigned extent{};
        for(const auto* caption:captions) extent=std::max(extent,measuring.text_extent_size(caption).width);
        return static_cast<int>(extent)+38;
    };
    const int pinWidth=std::max(109,captionWidth(12,{L"Закрепить",L"Поверх окон"}));
    const int buildWidth=std::max(96,captionWidth(13,{L"Собрать",L"Собрать и запустить",L"Отменить"}));
    const int runWidth=std::max(107,captionWidth(13,{L"Запустить",L"Остановить",L"Ожидание"}));
    box(close_,logicalWidth-40,9,27,27); box(minimize_,logicalWidth-71,9,27,27);
    box(log_,logicalWidth-103,9,27,27); box(settings_,logicalWidth-135,9,27,27);
    box(pin_,logicalWidth-143-pinWidth,9,pinWidth,27);
    box(project_,15,57,36,37); box(recent_,51,57,23,37);
    box(run_,logicalWidth-43-runWidth,57,runWidth,37); box(runMenu_,logicalWidth-43,57,28,37);
    const int buildX=logicalWidth-43-runWidth-36-buildWidth;
    box(build_,buildX,57,buildWidth,37); box(buildMenu_,buildX+buildWidth,57,28,37);
    if (state_.logVisible) {
        const int logicalHeight=static_cast<int>(std::lround(form_.size().height/scale_));
        journal_.move(nana::rectangle{px(15), px(153), static_cast<unsigned>(px(logicalWidth-15)-px(15)),
            static_cast<unsigned>(std::max(1,px(logicalHeight-18)-px(153)))});
        journal_.show();
    } else journal_.hide();
    const unsigned dpi = static_cast<unsigned>(std::lround(scale_ * 96));
    const nana::paint::font font{"Segoe UI", 9.75, {}, dpi};
    for (auto* w : std::initializer_list<nana::widget*>{&project_, &recent_, &build_, &buildMenu_, &run_, &runMenu_, &settings_, &pin_, &log_, &minimize_, &close_}) w->typeface(font);
    pin_.typeface(nana::paint::font{"Segoe UI",9,{},dpi});
    journal_.typeface(nana::paint::font{"Consolas", 9, {}, dpi});
    const int logicalHeight=static_cast<int>(std::lround(form_.size().height/scale_));
    journal_.padding(static_cast<unsigned>(px(154)-px(153)),static_cast<unsigned>(px(logicalWidth-15)-px(logicalWidth-18)),
        static_cast<unsigned>(px(logicalHeight-18)-px(logicalHeight-19)),static_cast<unsigned>(px(18)-px(15)));
    journal_.scrollSpace(static_cast<unsigned>(px(16)));
    journal_.textGeometry(scale_,154,px(153));
    styleScrollbars(journal_,palette_,scale_);
    refreshValid_ = false;
    layoutActive_ = false;
    nana::api::refresh_window(form_);
}
void Panel::previewScale(double scale) { previewScale_=true; scale_=scale; layout(true); refresh(); }
void Panel::theme(bool dark) {
    nana::api::batch_updates(form_, [&] {
        refreshValid_ = false;
        dark_ = dark; palette_ = Palette::system(dark);
        for (auto* w : std::initializer_list<nana::widget*>{static_cast<nana::widget*>(&form_), &project_, &recent_, &build_, &buildMenu_, &run_, &runMenu_, &settings_, &pin_, &log_, &minimize_, &close_}) colorWidget(*w, palette_);
        colorWidget(journal_, palette_, true);
        journal_.scrollCorner(palette_.scrollTrack);
        journal_.scheme().selection = palette_.accent;
        journal_.scheme().selection_unfocused = palette_.accent.blend(palette_.surface, .40);
        journal_.scheme().selection_text = dark ? nana::color{0,0,0} : nana::color{255,255,255};
        styleScrollbars(journal_,palette_,scale_);
        platform::setWindowTheme(form_.native_handle(), dark);
        for (auto* menu : {&projects_, &builds_, &targets_, &journalMenu_}) prepareMenu(*menu);
    });
}
void Panel::prepareMenu(nana::menu& menu) {
    const bool journal=&menu==&journalMenu_;
    menu.renderer(nana::pat::cloneable<nana::menu::renderer_interface>(MenuRenderer{palette_,scale_,journal}));
    menu.item_pixels(static_cast<unsigned>(21*scale_)-1);
    menu.max_pixels(static_cast<unsigned>(std::lround(760 * scale_)));
}
void Panel::refresh() { refreshPanel(false); }
void Panel::refreshPanel(bool animate) {
    if (observedLogVisible_ != state_.logVisible) layout(true);
    const auto operation = controller_.operation();
    const auto progress = controller_.buildProgress();
    const auto duration = controller_.lastBuildDuration();
    const auto revision = controller_.journal().revision();
    const bool journalChanged = revision != observedJournalRevision_;
    const bool idle = operation == Operation::Idle;
    const bool building = operation == Operation::Building;
    const bool running = operation == Operation::Running;
    const bool pulse = animate && building && !progress;
    if (pulse) animation_ = (animation_ + 1) % 50;
    // Compare borrowed strings on idle ticks; copy the snapshot only when
    // observable chrome changes, rather than allocating or touching widgets.
    const auto& previous = observedRefresh_;
    const bool chromeChanged = !refreshValid_
        || previous.operation != operation || previous.action != controller_.buildAction()
        || previous.failed != controller_.failed() || previous.cancelling != controller_.cancellationRequested()
        || previous.canQueue != controller_.canQueueRun() || previous.runQueued != controller_.runQueued()
        || previous.hasTargets != !state_.targets.empty() || previous.pinned != state_.pinned
        || previous.buildAndRun != state_.buildAndRun
        || previous.logVisible != state_.logVisible || previous.progress != progress || previous.duration != duration
        || previous.project != state_.settings.cmakeFile || previous.status != controller_.status()
        || (journalChanged && duration && previous.durationText != controller_.durationText());
    if (!journalChanged && !chromeChanged && !pulse) return;
    nana::api::batch_updates(form_, [&] {
        if (journalChanged) {
            journal_.update(controller_.journal(), autoScroll_);
            // Nana creates/closes the two scrollbars as content starts/stops
            // overflowing. Newly created bars need the current custom drawing.
            styleScrollbars(journal_, palette_, scale_);
            observedJournalRevision_ = revision;
        }
        if (!chromeChanged) {
            if (pulse) nana::api::refresh_window(form_);
            return;
        }
        project_.enabled(idle); recent_.enabled(idle); settings_.enabled(idle); buildMenu_.enabled(idle);
        build_.enabled(!running && !controller_.cancellationRequested());
        run_.enabled(!controller_.cancellationRequested()
            && (running || (idle && !state_.targets.empty()) || (building && controller_.canQueueRun() && !controller_.runQueued())));
        runMenu_.enabled(idle && !state_.targets.empty());
        const std::array<nana::button*,11> buttons{&project_,&build_,&run_,&runMenu_,&pin_,&settings_,&log_,&minimize_,&close_,&buildMenu_,&recent_};
        for(unsigned index=0;index<buttons.size();++index) if(!buttons[index]->enabled()) buttonStates_[index]={};
        setCaption(build_, building ? L"Отменить" : state_.buildAndRun ? L"Собрать и запустить" : L"Собрать");
        setCaption(run_, running ? L"Остановить" : controller_.runQueued() ? L"Ожидание" : L"Запустить");
        setCaption(pin_,state_.pinned ? L"Поверх окон" : L"Закрепить");
        project_.tooltip(state_.settings.cmakeFile.empty() ? "Выбрать CMakeLists.txt проекта (Ctrl+O)" : platform::utf8(state_.settings.cmakeFile));
        run_.tooltip(controller_.runQueued() ? "Запуск после успешной сборки" : running ? "Остановить приложение и дочерние процессы (Ctrl+F5)" : "Запустить или поставить запуск в очередь (Ctrl+F5)");
        build_.tooltip(building ? "Отменить текущую операцию (F6)" : state_.buildAndRun
            ? "Собрать и запустить (F5). Режим кнопки выбирается в меню" : "Собрать (F6). Режим кнопки выбирается в меню");
        settings_.tooltip("Настройки проекта и запуска"); pin_.tooltip(state_.pinned ? "Открепить панель" : "Поверх остальных окон"); log_.tooltip(state_.logVisible ? "Скрыть журнал" : "Показать журнал");
        observedRefresh_ = {
            .operation = operation, .action = controller_.buildAction(),
            .failed = controller_.failed(), .cancelling = controller_.cancellationRequested(),
            .canQueue = controller_.canQueueRun(), .runQueued = controller_.runQueued(),
            .hasTargets = !state_.targets.empty(), .pinned = state_.pinned, .logVisible = state_.logVisible,
            .buildAndRun = state_.buildAndRun,
            .progress = progress, .duration = duration,
            .project = state_.settings.cmakeFile, .status = controller_.status(), .durationText = controller_.durationText()
        };
        refreshValid_ = true;
        nana::api::refresh_window(form_);
        for(auto* b:{&project_,&recent_,&build_,&buildMenu_,&run_,&runMenu_,&settings_,&pin_,&log_,&minimize_,&close_}) nana::api::refresh_window(*b);
    });
}
void Panel::selectProject() {
    if (controller_.operation() != Operation::Idle) return;
    const auto path = platform::selectPath(form_.native_handle(), false, L"Выбрать CMakeLists.txt");
    if (!path.empty()) controller_.selectProject(path);
    refresh();
}
void Panel::showProjectMenu() {
    if (controller_.operation() != Operation::Idle) return;
    projects_.clear(); prepareMenu(projects_);
    projects_.append("Открыть проект…\tCtrl+O", [this](nana::menu::item_proxy&) { selectProject(); });
    projects_.append_splitter();
    for (const auto& path : state_.recentProjects) projects_.append(menuText(path), [this, path](nana::menu::item_proxy&) { controller_.selectProject(path); refresh(); })
        .check_style(nana::menu::checks::highlight).checked(platform::projectSection(path)==platform::projectSection(state_.settings.cmakeFile));
    projects_.popup(project_, 0, static_cast<int>(project_.size().height)+1);
}
void Panel::showBuildMenu() {
    if (controller_.operation() != Operation::Idle) return;
    builds_.clear(); prepareMenu(builds_);
    const std::pair<const char*, BuildAction> actions[] = {{"Собрать\tF6", BuildAction::Build}, {"Собрать и запустить\tF5", BuildAction::BuildAndRun}, {"Очистить", BuildAction::Clean}, {"Пересобрать", BuildAction::Rebuild}, {"CMake", BuildAction::Configure}};
    for (const auto& [name, action] : actions) {
        auto item=builds_.append(name, [this, action](nana::menu::item_proxy&) {
            if (action==BuildAction::Build || action==BuildAction::BuildAndRun) {
                const bool buildAndRun=action==BuildAction::BuildAndRun;
                if (state_.buildAndRun!=buildAndRun) { state_.buildAndRun=buildAndRun; controller_.save(); }
            }
            if (state_.settings.cmakeFile.empty() && !hidden_) selectProject();
            controller_.build(action); refresh();
        });
        if (action==BuildAction::Build || action==BuildAction::BuildAndRun)
            item.check_style(nana::menu::checks::option).checked(state_.buildAndRun==(action==BuildAction::BuildAndRun));
        if (action==BuildAction::BuildAndRun) builds_.append_splitter();
    }
    builds_.popup(build_, 0, static_cast<int>(build_.size().height)+1);
}
void Panel::showRunMenu() {
    if (controller_.operation() != Operation::Idle || runMenuOpen_) return;
    state_.restoreTargets(); refresh();
    targets_.clear(); prepareMenu(targets_);
    for (std::size_t index = 0; index < state_.targets.size(); ++index) {
        const auto target = state_.targets[index];
        targets_.append(menuText(target.name), [this, index](nana::menu::item_proxy&) { controller_.chooseRunTarget(index); refresh(); })
            .check_style(nana::menu::checks::option).checked(state_.chosenTarget == target.name);
    }
    if (state_.targets.empty()) targets_.append("Сначала соберите исполняемую цель").enabled(false);
    const bool awaited = controller_.runSelectionRequested();
    runMenuOpen_ = true;
    if (awaited) {
        targets_.popup_await(run_, 0, static_cast<int>(run_.size().height)+1);
        controller_.dismissRunSelectionRequest();
        runMenuOpen_ = false;
    } else {
        targets_.destroy_answer([this] { runMenuOpen_ = false; });
        targets_.popup(run_, 0, static_cast<int>(run_.size().height)+1);
    }
}

int runUiSmoke(const std::wstring& isolatedIni, const std::wstring& imageDirectory) {
    std::filesystem::create_directories(imageDirectory);
    AppState state{isolatedIni}; state.load();
    state.settings = BuildSettings{}; state.chosenTarget.clear(); state.chosenExecutable.clear(); state.recentProjects.clear();
    state.logVisible = true; state.logHeight = 365; state.width = 620; state.pinned = false; state.buildAndRun=false;
    const auto fixture = std::filesystem::path(isolatedIni).parent_path();
    state.settings.cmakeFile = (fixture / L"CMakeLists.txt").wstring();
    { std::ofstream source{std::filesystem::path(state.settings.cmakeFile)}; source << "cmake_minimum_required(VERSION 3.24)\nproject(NanaSmoke NONE)\n"; }
    for (const auto& name : {L"alpha & one", L"beta"}) {
        const auto exe = fixture / (std::wstring(name) + L".exe");
        { std::ofstream file{exe, std::ios::binary}; file << "UI fixture; never executed"; }
        state.targets.push_back({name, exe.wstring()});
    }
    state.save();
    Panel panel{state, true};
    auto expect = [](bool condition, const char* message) { if (!condition) throw std::runtime_error(message); };
    expect(panel.runButton().caption_wstring() == L"Запустить", "run caption");
    const auto geometry = panel.runButton().size();
    auto& journal = panel.controller().journal();
    const auto journalScrollbar = [&](bool vertical) -> nana::scroll_interface& {
        nana::scroll_interface* result{};
        nana::api::enum_widgets<nana::widget>(panel.journal(), true, [&](nana::widget& child) {
            if (vertical) {
                if (auto* bar = dynamic_cast<nana::scroll<true>*>(&child)) result = bar;
            } else if (auto* bar = dynamic_cast<nana::scroll<false>*>(&child)) result = bar;
        });
        expect(result != nullptr, "journal regression fixture is missing a scrollbar");
        return *result;
    };
    const auto expectJournalScrollbars = [&](const char* context) {
        const auto origin = panel.journal().content_origin();
        expect(journalScrollbar(false).value() == static_cast<std::size_t>(origin.x)
            && journalScrollbar(true).value() == static_cast<std::size_t>(origin.y), context);
    };
    const auto continueJournalScrolling = [&](const char* context) {
        // Exercise the real content-view wheel callback, which starts from the
        // scrollbar value. Preserving painted rows alone misses a stale thumb.
        for (const bool vertical : {true, false}) for (const bool upwards : {true, false}) {
            const auto before = panel.journal().content_origin();
            auto expected = before;
            auto& coordinate = vertical ? expected.y : expected.x;
            auto& scrollbar = journalScrollbar(vertical);
            const auto step = static_cast<int>(scrollbar.step());
            const auto remainder = coordinate % step;
            coordinate = upwards ? std::max(0, coordinate - (remainder ? remainder : step))
                : std::min(static_cast<int>(scrollbar.amount() - scrollbar.range()), coordinate + step - remainder);
            nana::arg_wheel wheel{};
            wheel.evt_code = nana::event_code::mouse_wheel;
            wheel.window_handle = panel.journal().handle();
            wheel.which = vertical ? nana::arg_wheel::wheel::vertical : nana::arg_wheel::wheel::horizontal;
            wheel.upwards = upwards; wheel.distance = 120; wheel.pos = {20, 20};
            panel.journal().events().mouse_wheel.emit(wheel, panel.journal().handle());
            expect(expected != before && panel.journal().content_origin() == expected, context);
            expectJournalScrollbars(context);
        }
    };
    const auto rootFrameHash = [&]() -> std::optional<std::uint64_t> {
        nana::paint::graphics root;
        if (!nana::api::root_graphics(panel.form(), root) || root.empty()) return {};
        nana::paint::pixel_buffer pixels{root.handle(), nana::rectangle{root.size()}};
        std::uint64_t hash = 1469598103934665603ull;
        for (unsigned row = 0; row < pixels.size().height; ++row)
            for (unsigned column = 0; column < pixels.size().width; ++column)
                hash = (hash ^ (pixels.raw_ptr(row)[column].value & 0x00ffffff)) * 1099511628211ull;
        return hash;
    };
    const auto updateJournalBatch = [&](const char* context) {
        const auto originalFrame = rootFrameHash();
        expect(originalFrame.has_value(), "journal batch could not read the composed root frame");
        const auto originalOrigin = panel.journal().content_origin();
        unsigned journalDraws{}, scrollbarDraws{}, intermediateViews{};
        bool stable = true;
        {
            struct DrawingProbes {
                std::array<std::pair<nana::window, nana::drawing_handle>, 3> handles{};
                ~DrawingProbes() {
                    for (const auto& [window, drawing] : handles)
                        if (drawing) nana::api::remove_drawing(window, drawing);
                }
            } probes;
            const std::array windows{panel.journal().handle(), journalScrollbar(true).window_handle(),
                journalScrollbar(false).window_handle()};
            for (std::size_t index = 0; index < windows.size(); ++index) {
                const auto window = windows[index];
                probes.handles[index] = {window, nana::api::drawing(window, [&, index](nana::paint::graphics&) {
                    if (index == 0) ++journalDraws; else ++scrollbarDraws;
                    intermediateViews += panel.journal().content_origin() != originalOrigin;
                    stable &= rootFrameHash() == originalFrame;
                })};
            }
            // Observe the real JournalBox transaction alone. Panel::refresh
            // also refreshes scrollbar styling after the final frame is ready.
            panel.journal().update(journal, panel.autoScroll_);
        }
        expect(journalDraws > 0 && scrollbarDraws > 0 && intermediateViews > 0,
            "journal batch fixture did not exercise intermediate drawing");
        expect(stable, context);
        const auto finalFrame = rootFrameHash();
        expect(finalFrame && finalFrame != originalFrame, "journal batch did not publish its final composed frame");
        panel.refresh();
    };
    const auto expectJournalFollowing = [&](const char* context) {
        const auto positions = panel.journal().text_position();
        expect(!positions.empty() && positions.back().y + 1 >= panel.journal().text_line_count()
            && panel.journal().caret_pos().y + 1 == panel.journal().text_line_count(), context);
        expectJournalScrollbars(context);
    };
    for (int i = 0; i < 250; ++i) journal.append(L"Строка журнала " + std::to_wstring(i) + L" — Unicode и диагностика\n");
    panel.refresh();
    // Observe real drawer callbacks instead of asserting unstable wall times.
    // Idle polls and journal-only updates must not repaint unrelated controls.
    preparePreview(panel.form());
    const auto buildGeometry=panel.buildButton().size();
    const auto buildPosition=panel.buildButton().pos();
    const auto runPosition=panel.runButton().pos();
    for (bool dark : {false,true}) for (bool buildAndRun : {false,true}) {
        state.buildAndRun=buildAndRun; panel.theme(dark); panel.refresh();
        expect(panel.buildButton().caption_wstring()==(buildAndRun ? L"Собрать и запустить" : L"Собрать"), "saved build mode caption");
        expect(panel.buildButton().size()==buildGeometry && panel.buildButton().pos()==buildPosition
            && panel.runButton().pos()==runPosition, "build mode changed button geometry");
        for (const auto control : {1u,9u}) {
            auto& button=control==1 ? panel.build_ : panel.buildMenu_;
            nana::paint::graphics graphic{button.size()}; panel.drawButton(graphic,button,control);
            nana::paint::pixel_buffer pixels{graphic.handle(),nana::rectangle{graphic.size()}};
            const auto fill=pixels.pixel(static_cast<int>(graphic.width()/2),static_cast<int>(5*panel.scale_)).value & 0xffffff;
            expect(fill==(buildAndRun ? 0xc4ffd2u : 0xbcc5abu), "idle build split button does not use requested mode color");
            // Fractional DPI antialiases the icon's thin strokes; even the
            // darkest arrow pixel may blend with its fill. Inspect only the
            // glyph interior so the theme's dark outline cannot satisfy this.
            const auto modeText=0x242933u;
            bool darkText=false;
            for (unsigned row=static_cast<unsigned>(10*panel.scale_); row<static_cast<unsigned>(27*panel.scale_); ++row)
                for (unsigned column=static_cast<unsigned>((control==9 ? 6 : 7)*panel.scale_);
                    column<static_cast<unsigned>((control==9 ? 22 : 26)*panel.scale_); ++column) {
                    const auto pixel=pixels.raw_ptr(row)[column].value;
                    bool darkPixel=true;
                    for (const auto shift : {0u,8u,16u})
                        darkPixel&=((pixel>>shift)&255u)<=(((fill>>shift)&255u)+((modeText>>shift)&255u))/2;
                    darkText|=darkPixel;
                }
            expect(darkText, "build mode foreground must stay dark in both themes");
        }
        const auto name=std::wstring{buildAndRun ? L"panel-build-and-run-" : L"panel-build-only-"}+(dark ? L"dark.bmp" : L"light.bmp");
        savePreview(panel.form(),(std::filesystem::path(imageDirectory)/name).wstring());
    }
    state.buildAndRun=false; panel.theme(false); panel.refresh();
    struct PaintCounts { unsigned form{}, buttons{}; };
    const auto paints = std::make_shared<PaintCounts>();
    const auto formProbe = panel.form().drawing([paints](nana::paint::graphics&) { ++paints->form; });
    const auto runProbe = panel.runButton().drawing([paints](nana::paint::graphics&) { ++paints->buttons; });
    const auto buildProbe = panel.buildButton().drawing([paints](nana::paint::graphics&) { ++paints->buttons; });
    platform::drainPreviewMessages();
    *paints = {};
    for (int i = 0; i < 100; ++i) panel.refresh();
    platform::drainPreviewMessages();
    expect(paints->form == 0 && paints->buttons == 0, "idle refresh repainted unchanged panel controls");
    journal.append(L"Обновление только журнала 😀\n"); panel.refresh();
    platform::drainPreviewMessages();
    expect(paints->form == 0 && paints->buttons == 0, "journal-only refresh repainted panel controls");
    expect(panel.journal().caption_wstring().find(L"Обновление только журнала 😀") != std::wstring::npos,
        "journal-only refresh skipped its new text");
    state.pinned = true; panel.refresh();
    expect(paints->form > 0 && paints->buttons > 0, "changed panel state was not repainted");
    state.pinned = false; panel.refresh();
    nana::api::remove_drawing(panel.form(), formProbe);
    nana::api::remove_drawing(panel.runButton(), runProbe);
    nana::api::remove_drawing(panel.buildButton(), buildProbe);
    // Scrolling without selecting leaves the caret at the end. An append
    // temporarily follows that caret, then must restore both scrollbar values.
    journal.append(std::wstring(275, L'x') + L" horizontal scrolling\n"); panel.refresh();
    {
        const auto originalScale = panel.scale_;
        const auto originalPreview = panel.previewScale_;
        const auto originalTheme = panel.dark_;
        const auto originalSize = panel.form().size();
        const auto originalWidth = state.width;
        const auto originalHeight = state.logHeight;
        // Exercise the application's real padding, scrollbar thickness and
        // painting at each supported preview scale, including fractional DPI.
        for (bool dark : {false, true}) for (double scale : {1.0, 1.25, 1.5, 2.0}) {
            state.width = originalWidth; state.logHeight = originalHeight;
            panel.previewScale(scale); panel.theme(dark); panel.refresh();
            nana::api::refresh_window_tree(panel.form());
            const auto vertical = journalScrollbar(true).window_handle();
            const auto horizontal = journalScrollbar(false).window_handle();
            const nana::rectangle v{nana::api::window_position(vertical), nana::api::window_size(vertical)};
            const nana::rectangle h{nana::api::window_position(horizontal), nana::api::window_size(horizontal)};
            const nana::rectangle corner{v.x, h.y, v.width, h.height};
            nana::paint::graphics graphic;
            expect(nana::api::window_graphics(panel.journal(), graphic), "could not capture journal scrollbar junction");
            const auto expectJunction = [&](bool condition, const char* message) {
                if (condition) return;
                const auto describe = [](const nana::rectangle& r) {
                    return std::to_string(r.x) + "," + std::to_string(r.y) + ","
                        + std::to_string(r.width) + "x" + std::to_string(r.height);
                };
                throw std::runtime_error(std::string{message} + " (scale=" + std::to_string(scale)
                    + ", theme=" + (dark ? "dark" : "light") + ", v=" + describe(v) + ", h=" + describe(h)
                    + ", graph=" + std::to_string(graphic.width()) + "x" + std::to_string(graphic.height()) + ")");
            };
            const auto thickness = static_cast<unsigned>(16 * scale);
            expectJunction(v.width == thickness && h.height == thickness,
                "journal scrollbar thickness does not follow application scale");
            expectJunction(v.bottom() == h.y && h.right() == v.x,
                "journal scrollbars leave a gap or overlap at their intersection");
            // The borderless editor's horizontal skew is 1 + padding_bottom:
            // its bar and corner extend one pixel below the outer editor area.
            // Nana clips child composition to that area; inspect every visible
            // pixel while asserting that exact, pre-existing one-pixel overhang.
            expectJunction(corner.x > 0 && corner.y > 0 && corner.right() <= static_cast<int>(graphic.width())
                && corner.bottom() == static_cast<int>(graphic.height()) + 1 && corner.height > 1,
                "journal scrollbar corner has unexpected clipping at the editor boundary");
            const nana::rectangle visibleCorner{corner.x, corner.y, corner.width, corner.height - 1};
            nana::paint::pixel_buffer pixels{graphic.handle(), nana::rectangle{graphic.size()}};
            const auto track = panel.palette_.scrollTrack.px_color().value & 0x00ffffff;
            const auto button = panel.palette_.scrollThumb.px_color().value & 0x00ffffff;
            for (int y = visibleCorner.y; y < visibleCorner.bottom(); ++y)
                for (int x = visibleCorner.x; x < visibleCorner.right(); ++x)
                    expectJunction((pixels.pixel(x, y).value & 0x00ffffff) == track,
                        "journal scrollbar corner does not match its theme's track color");
            // The last row/column of each arrow button must directly touch the
            // corner; a strip of editor background here reproduces the gap.
            for (int x = visibleCorner.x; x < visibleCorner.right(); ++x)
                expectJunction((pixels.pixel(x, visibleCorner.y - 1).value & 0x00ffffff) == button,
                    "journal vertical scrollbar leaves an unpainted seam above its corner");
            for (int y = visibleCorner.y; y < visibleCorner.bottom(); ++y)
                expectJunction((pixels.pixel(visibleCorner.x - 1, y).value & 0x00ffffff) == button,
                    "journal horizontal scrollbar leaves an unpainted seam beside its corner");
            const auto name = std::wstring{L"journal-junction-"} + (dark ? L"dark-" : L"light-")
                + std::to_wstring(static_cast<int>(scale * 100)) + L".bmp";
            graphic.save_as_file(platform::utf8((std::filesystem::path(imageDirectory) / name).wstring()).c_str());
        }
        state.width = originalWidth; state.logHeight = originalHeight;
        panel.previewScale(originalScale); panel.theme(originalTheme);
        panel.form().size(originalSize); panel.refresh();
        panel.previewScale_ = originalPreview;
    }
    unsigned unchangedStylePaints{};
    std::vector<std::pair<nana::window, nana::drawing_handle>> styleProbes;
    nana::api::enum_widgets<nana::widget>(panel.journal(), true, [&](nana::widget& child) {
        if (dynamic_cast<nana::scroll_interface*>(&child))
            styleProbes.emplace_back(child.handle(), child.drawing([&](nana::paint::graphics&) { ++unchangedStylePaints; }));
    });
    for (int repeat = 0; repeat < 20; ++repeat) styleScrollbars(panel.journal(), panel.palette_, panel.scale_);
    for (const auto& [window, drawing] : styleProbes) nana::api::remove_drawing(window, drawing);
    expect(styleProbes.size() == 2 && unchangedStylePaints == 0, "unchanged journal styling repainted its scrollbars");
    panel.journal().select(false); panel.journal().scrollToEnd();
    journal.append(L"Автопрокрутка у конца журнала\n"); panel.refresh();
    expectJournalFollowing("journal did not follow new messages while already at the end");
    const auto readingCaret = panel.journal().caret_pos();
    journalScrollbar(false).value(72);
    journalScrollbar(true).value(40 * panel.journal().linePitch() + 3);
    expect(panel.journal().content_origin().x > 0 && panel.journal().content_origin().y > 0,
        "journal reading fixture did not scroll away from the origin");
    for (int packet = 0; packet < 3; ++packet) {
        const auto readingOrigin = panel.journal().content_origin();
        const auto message = L"Новая порция во время чтения " + std::to_wstring(packet) + L"\n";
        // A large first packet visibly moves the thumb even after rounding,
        // allowing the composed-frame assertion to verify the final flush.
        for (int line = 0; line < (packet == 0 ? 80 : 1); ++line) journal.append(message);
        if (packet == 0) updateJournalBatch("journal append exposed an intermediate scroll position");
        else panel.refresh();
        expect(!panel.journal().selected() && panel.journal().caret_pos() == readingCaret,
            "journal append changed the inactive caret while reading");
        expect(panel.journal().content_origin() == readingOrigin, "journal reading viewport moved on append");
        expectJournalScrollbars("journal scrollbar moved away from the reading viewport on append");
        continueJournalScrolling("journal wheel resumed from the wrong position after append");
    }
    journalScrollbar(true).value(journalScrollbar(true).amount() - journalScrollbar(true).range());
    journal.append(L"Автопрокрутка после возврата вниз\n"); panel.refresh();
    expectJournalFollowing("journal did not resume following after scrolling back to the end");
    panel.journal().caret_pos({2, 12}); panel.journal().select_points({2, 12}, {7, 15});
    journalScrollbar(false).value(72);
    journalScrollbar(true).value(8 * panel.journal().linePitch() + 3);
    const auto selection = panel.journal().selection(); const auto origin = panel.journal().content_origin();
    journal.append(L"Новая строка\n"); panel.refresh();
    expect(panel.journal().selection() == selection, "journal selection moved on append");
    expect(panel.journal().content_origin() == origin, "journal viewport moved on append");
    expectJournalScrollbars("journal scrollbar moved away from the selected viewport on append");
    continueJournalScrolling("journal wheel resumed from the wrong position with a selection");
    expect(panel.journal().selection() == selection, "journal wheel changed the selection");
    journal.append(L"😀 После non-BMP Unicode текст сохраняется\n"); panel.refresh();
    expect(panel.journal().caption_wstring().find(L"После non-BMP Unicode текст сохраняется")!=std::wstring::npos,"journal lost text following Unicode emoji");
    preparePreview(panel.form());
    const auto initialSize = panel.form().size();
    for (unsigned delta : {20u, 0u}) {
        panel.form().size({initialSize.width + delta, initialSize.height + delta});
        platform::drainPreviewMessages();
        const auto native = platform::clientArea(panel.form().native_handle());
        expect(native.width == static_cast<int>(panel.form().size().width) && native.height == static_cast<int>(panel.form().size().height), "Nana/native geometry mismatch after resize");
        expect(panel.form().size().width == initialSize.width + delta && panel.form().size().height == initialSize.height + delta, "Nana geometry drifted after resize");
    }
    std::uint64_t lightHash{};
    for (bool dark : {false, true}) {
        panel.theme(dark); panel.refresh();
        expect(panel.journal().selection() == selection, "theme changed selection");
        expect(panel.runButton().size() == geometry, "theme changed geometry");
        const auto imagePath = std::filesystem::path(imageDirectory) / (dark ? L"panel-dark.bmp" : L"panel-light.bmp");
        const auto hash = savePreview(panel.form(), imagePath.wstring());
        if (!dark) lightHash = hash;
        else expect(hash != lightHash, "light and dark panel renders are identical");
    }
    const int settingsResult = runSettingsSmoke(panel.form(), panel.controller(), Palette::system(false));
    expect(settingsResult == 0, "settings smoke");
    journal.clear(); journal.append(L"first_word second_word\n"); panel.refresh(); panel.journal().restoreView({0,0});
    panel.journal().selectWordAt({20,5});
    expect(panel.journal().selection()==std::pair<nana::upoint,nana::upoint>{{0,0},{10,0}},"journal context word selection");
    const auto contextSelection=panel.journal().selection();
    panel.journal().selectWordAt({30,5});
    expect(panel.journal().selection()==contextSelection,"journal context changed an existing selection");
    panel.journal().selectWordAt({450,5});
    expect(!panel.journal().selected(),"journal context beyond line did not clear selection");
    journal.clear(); panel.refresh();
    journal.append(std::wstring(800000, L'a')); panel.refresh();
    panel.journal().select_points({600000, 0}, {600010, 0});
    journal.append(std::wstring(200000, L'b')); panel.refresh();
    expect(panel.journal().selection() == std::pair<nana::upoint, nana::upoint>{{50000, 0}, {50010, 0}}, "partial-line pruning changed selected text");
    // Prefix pruning must retain UTF-16 selection/caret coordinates and the
    // same visible rows, including surrogate pairs and horizontal scrolling.
    const std::wstring trimLine = L"😀 " + std::wstring(275, L'x') + L" tail\n";
    std::wstring multiline;
    for (int row = 0; row < 3000; ++row) multiline += trimLine;
    journal.clear(); journal.append(multiline); panel.refresh();
    panel.journal().caret_pos({4, 2802}, false);
    panel.journal().select_points({0, 2800}, {2, 2802});
    panel.journal().restoreView({40, static_cast<int>(panel.journal().linePitch() * 2800 + 3)});
    const auto trimOrigin = panel.journal().content_origin();
    const auto trimCaret = panel.journal().caret_pos();
    const auto trimRemoved = journal.totalRemovedBytes();
    const auto trimPrevious = journal.text();
    std::wstring moreLines;
    for (int row = 0; row < 600; ++row) moreLines += trimLine;
    journal.append(moreLines);
    updateJournalBatch("journal pruning exposed an intermediate scroll position");
    const auto removedBytes = static_cast<std::size_t>(journal.totalRemovedBytes() - trimRemoved);
    expect(removedBytes < trimPrevious.size(), "multiline trim fixture has no retained old rows");
    const auto droppedRows = static_cast<unsigned>(std::count(trimPrevious.begin(), trimPrevious.begin() + static_cast<std::ptrdiff_t>(removedBytes), '\n'));
    expect(panel.journal().selection() == std::pair<nana::upoint, nana::upoint>{{0, 2800 - droppedRows}, {2, 2802 - droppedRows}}, "multiline pruning changed Unicode selection");
    expect(panel.journal().caret_pos() == nana::upoint{trimCaret.x, trimCaret.y - droppedRows}, "multiline pruning changed caret");
    expect(panel.journal().content_origin() == nana::point{trimOrigin.x, trimOrigin.y - static_cast<int>(droppedRows * panel.journal().linePitch())}, "multiline pruning moved the retained viewport");
    expectJournalScrollbars("multiline pruning left scrollbar values outside the retained viewport");
    continueJournalScrolling("journal wheel resumed from the wrong position after multiline pruning");
    const auto expectJournalText = [&] {
        auto actual = panel.journal().caption_wstring();
        std::erase(actual, L'\r');
        expect(actual == platform::utf16(journal.text()), "pruning changed retained Unicode journal text");
    };
    expectJournalText();
    panel.journal().select(false); panel.journal().scrollToEnd();
    nana::api::refresh_window(panel.journal());
    journal.append(moreLines + moreLines + moreLines); panel.refresh();
    expectJournalText();
    expect(panel.journal().caret_pos().y + 1 == panel.journal().text_line_count(), "pruning stopped following new messages");
    // One unseen packet can remove all previously displayed rows. That path
    // still replaces the text because it has no retained metrics to reuse.
    journal.append(multiline + multiline); panel.refresh();
    expectJournalText();
    state.settings.cmakeFile.clear(); state.targets.clear(); state.chosenTarget.clear(); state.chosenExecutable.clear();
    click(panel.buildButton()); // Empty project is reported through the real button callback.
    panel.refresh(); expect(panel.controller().operation() == Operation::Idle, "empty project started build");
    panel.showBuildMenu();
    expect(panel.builds_.size()==6 && panel.builds_.text(0)=="Собрать\tF6" && panel.builds_.text(1)=="Собрать и запустить\tF5"
        && panel.builds_.text(3)=="Очистить" && panel.builds_.text(4)=="Пересобрать" && panel.builds_.text(5)=="CMake", "build menu order");
    expect(panel.builds_.checked(0) && !panel.builds_.checked(1), "build menu must indicate its saved mode");
    panel.builds_.close();
    const auto chooseBuild=[&](std::size_t item) {
        panel.showBuildMenu();
        for (std::size_t index=0; index<panel.builds_.size(); ++index) panel.builds_.enabled(index,index==item);
        panel.builds_.goto_next(true); panel.builds_.pick(); platform::drainPreviewMessages();
    };
    chooseBuild(1);
    expect(state.buildAndRun && panel.buildButton().caption_wstring()==L"Собрать и запустить", "menu must select persistent build-and-run mode");
    AppState restoredMode{isolatedIni}; restoredMode.load(); expect(restoredMode.buildAndRun, "menu mode not saved to isolated INI");
    for (const auto item : {3u,4u,5u}) { chooseBuild(item); expect(state.buildAndRun, "one-shot build command changed persistent mode"); }
    chooseBuild(0);
    restoredMode.load(); expect(!state.buildAndRun && !restoredMode.buildAndRun, "menu must restore and save ordinary build mode");
    // An absent test-owned CMake path rejects operations quickly, while the
    // actual callbacks still expose the requested command and run queue.
    state.settings.cmakeFile=(fixture/L"CMakeLists.txt").wstring();
    state.settings.cmakeExecutable=(fixture/L"absent-mode-cmake.exe").wstring();
    state.settings.compiler=CompilerMode::Environment;
    const auto finishBuild=[&] {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{5};
        while (panel.controller().operation()!=Operation::Idle && std::chrono::steady_clock::now()<deadline) {
            panel.controller().drainEvents(); platform::drainPreviewMessages(); std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        panel.refresh(); expect(panel.controller().operation()==Operation::Idle, "isolated build mode callback did not finish");
        expect(!panel.controller().runQueued(), "failed build must discard queued launch");
    };
    chooseBuild(1);
    expect(panel.controller().buildAction()==BuildAction::BuildAndRun && panel.controller().runQueued(), "build-and-run menu did not execute its selected action");
    finishBuild();
    click(panel.buildButton());
    expect(panel.controller().buildAction()==BuildAction::BuildAndRun && panel.controller().runQueued(), "main button did not retain build-and-run mode");
    finishBuild();
    panel.shortcut(VK_F6);
    expect(panel.controller().buildAction()==BuildAction::Build && state.buildAndRun && !panel.controller().runQueued(), "F6 must keep its explicit command without changing mode");
    finishBuild();
    chooseBuild(4);
    expect(panel.controller().buildAction()==BuildAction::Rebuild && state.buildAndRun && !panel.controller().runQueued(), "rebuild must remain one-shot without queued launch");
    finishBuild();
    chooseBuild(0);
    expect(panel.controller().buildAction()==BuildAction::Build && !state.buildAndRun && !panel.controller().runQueued(), "build menu did not execute ordinary build");
    finishBuild();
    panel.shortcut(VK_F5);
    expect(panel.controller().buildAction()==BuildAction::BuildAndRun && !state.buildAndRun && panel.controller().runQueued(), "F5 must keep its explicit command without changing mode");
    finishBuild();
    click(panel.buildButton());
    expect(panel.controller().buildAction()==BuildAction::Build && !panel.controller().runQueued(), "main button did not retain ordinary build mode");
    finishBuild();
    panel.form().close();
    return 0;
}
int runVisualPreviews(const std::wstring& isolatedIni, const std::wstring& imageDirectory) {
    namespace visual=harness::visual;
    std::filesystem::create_directories(imageDirectory);
    const auto fixture=std::filesystem::path(isolatedIni).parent_path();
    AppState state{isolatedIni};
    state.width=visual::panelWidth; state.logHeight=visual::journalHeight; state.logVisible=false; state.pinned=true;
    state.settings.cmakeFile=visual::projectFile;
    state.settings.buildDirectory=visual::buildDirectory; state.settings.configuration=visual::configuration;
    for(const auto* name:visual::targetNames) {
        const auto executable=fixture/(std::wstring{name}+L".exe");
        {std::ofstream file{executable,std::ios::binary};file<<"Preview fixture; never executed";}
        state.targets.push_back({name,executable.wstring()});
    }
    state.chosenTarget=state.targets.front().name; state.chosenExecutable=state.targets.front().executable;
    Panel panel{state,true}; preparePreview(panel.form_);
    const auto capture=[&](visual::View view,bool dark,double scale,int percent,int height) {
        panel.form_.size({static_cast<unsigned>(visual::panelWidth*scale),static_cast<unsigned>(height*scale)});
        platform::drainPreviewMessages();
        const auto stem=visual::imageStem(view,dark,percent);
        savePreview(panel.form_,(std::filesystem::path(imageDirectory)/(stem+L".bmp")).wstring());
    };
    for(bool dark:{false,true}) for(std::size_t index=0;index<visual::scales.size();++index) {
        const double scale=visual::scales[index]; const int percent=visual::percentages[index];
        state.logVisible=false; panel.previewScale(scale); panel.theme(dark); panel.refresh();
        capture(visual::View::Panel,dark,scale,percent,visual::compactHeight);
        state.buildAndRun=true; panel.refresh();
        capture(visual::View::BuildAndRun,dark,scale,percent,visual::compactHeight);
        state.buildAndRun=false; panel.refresh();
        if(runSettingsPreviews(panel.form_,panel.controller_,Palette::system(dark),imageDirectory,scale)) throw std::runtime_error("Nana settings preview failed");
        panel.buttonStates_[5].hover=true; panel.buttonStates_[1].pressed=true; panel.run_.enabled(false); panel.runMenu_.enabled(false); panel.previewFocus_=8;
        capture(visual::View::States,dark,scale,percent,visual::compactHeight);
        panel.buttonStates_[5].hover=false; panel.buttonStates_[1].pressed=false; panel.previewFocus_=-1;
        state.logVisible=true; panel.controller_.journal().clear(); panel.controller_.journal().append(visual::journalText);
        panel.controller_.onEvent({EventKind::BuildSucceeded,L"Сборка завершена.",{},0,std::chrono::milliseconds{12345}});
        panel.layout(true); panel.refresh();
        capture(visual::View::Journal,dark,scale,percent,visual::journalHeight);
    }
    panel.form_.close(); return 0;
}
}

