#pragma once
#include <windows.h>
#include <gdiplus.h>
#include <algorithm>

namespace Ui {
using Gdiplus::RectF;
enum class Screen { Single, Batch, Results };
struct ImportPanel {
    RectF title,description,button,note;
    bool hit(float x,float y) const {return button.Contains(x,y);}
};
constexpr float ImportPanelHeight=188;
inline ImportPanel importPanel(RectF canvas) {
    const float cx=canvas.X+canvas.Width/2,top=canvas.Y+(canvas.Height-ImportPanelHeight)/2;
    const float width=std::max(1.f,std::min(480.f,canvas.Width-48));
    return {RectF(cx-width/2,top,width,32),RectF(cx-width/2,top+42,width,40),
            RectF(cx-88,top+100,176,40),RectF(cx-width/2,top+152,width,36)};
}
struct TextHeights {float help=32,detail=0,name=22,interaction=32,lock=32;};
struct Sidebar {
    RectF headingScale,scale2,scale3,headingMode,conservative,clean,denoise,help,hint;
    RectF run,cancel,clear,save,exportResults,preview,previous,next,back;
    RectF resultHeading,summary,details,filename,dimensions,interaction,lock,progress,previewOrdinal;
};
inline Sidebar sidebar(Screen screen,float height,TextHeights text) {
    Sidebar s;constexpr float x=20,w=248;float y=108;
    auto row=[&](float h,float gap=10.f){RectF r(x,y,w,h);y+=h+gap;return r;};
    if(screen==Screen::Results) {
        s.resultHeading=row(24,12);s.summary=row(24,text.detail>0?8.f:0.f);
        if(text.detail>0)s.details=row(text.detail,10);
        else s.details=RectF(x,y,w,0);
        y+=10;s.filename=row(text.name,6);s.dimensions=row(22,12);
        s.interaction=row(text.interaction,8);s.lock=row(text.lock,16);
        s.previewOrdinal=row(22,6);s.previous=RectF(x,y,119,36);s.next=RectF(x+129,y,119,36);y+=46;
        s.back=row(36);s.exportResults=row(36);
        return s;
    }
    s.headingScale=row(22);s.scale2=RectF(x,y,119,36);s.scale3=RectF(x+129,y,119,36);y+=56;
    s.headingMode=row(22);s.conservative=RectF(x,y,76,36);s.clean=RectF(x+84,y,76,36);s.denoise=RectF(x+168,y,80,36);y+=46;
    s.help=row(text.help,12);
    if(screen==Screen::Single) {
        s.run=row(40,8);s.cancel=row(28,8);s.clear=row(28,18);
        s.progress=RectF(x,y,w,88);
        s.save=RectF(x,std::max(y+104,height-112),w,44);
    } else {
        s.hint=row(32,12);s.run=row(40,8);s.exportResults=row(36,8);s.preview=row(36,10);
        s.clear=RectF(x,y,119,28);s.cancel=RectF(x+129,y,119,28);y+=46;
        s.progress=RectF(x,y,w,104);
    }
    return s;
}
}
