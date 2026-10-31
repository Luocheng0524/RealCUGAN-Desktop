#pragma once
#include <algorithm>
#include <cstddef>
#include <numeric>
#include <vector>

// Batch-list-only gesture model. Geometry is frozen for the gesture; the
// authoritative item vector is untouched until the owner commits on release.
namespace BatchReorder {
struct Grid {
    // Neutral defaults only; press() receives the current responsive grid.
    float x=0,y=0,width=1,height=1,gapX=0,gapY=0;
    size_t columns=1;
    float left(size_t slot) const {return x+float(slot%columns)*(width+gapX);}
    float top(size_t slot) const {return y+float(slot/columns)*(height+gapY);}
    float center(size_t slot) const {return left(slot)+width/2;}
};
struct Gesture {
    bool pending=false,active=false;
    size_t item=0,slot=0;
    float x=0,y=0,grabX=0,grabY=0;
    Grid grid;
    std::vector<size_t> order;

    void press(size_t count,size_t index,float px,float py,Grid geometry) {
        grid=geometry;item=slot=index;x=px;y=py;
        grabX=x-grid.left(slot);grabY=y-grid.top(slot);
        order.resize(count);std::iota(order.begin(),order.end(),size_t(0));
        pending=true;active=false;
    }
    void activate() {if(pending){pending=false;active=true;}}
    void reset() {pending=active=false;order.clear();}
    void insert(size_t target) {
        const auto moving=order[slot];
        if(target>slot)std::move(order.begin()+slot+1,order.begin()+target+1,order.begin()+slot);
        else std::move_backward(order.begin()+target,order.begin()+slot,order.begin()+slot+1);
        order[target]=moving;slot=target;
    }
    // Only an actual side-to-side crossing can generate an event. Repeated
    // samples, dwelling and animation ticks cannot generate reorder events.
    // Consume a fast mouse segment in geometric order so skipped OS samples
    // still account for each distinct line once, without deferred swaps.
    size_t move(float px,float py) {
        if(!active){x=px;y=py;return 0;}
        const float startX=x,startY=y;
        double consumed=0;size_t events=0;
        for(size_t guard=0;guard<order.size()*2;++guard) {
            double first=2;size_t target=slot;
            auto crossing=[&](float start,float end,float line,int direction,size_t next) {
                const double current=start+(end-start)*consumed;
                if((direction>0&&current<=line&&end>line)||
                   (direction<0&&current>=line&&end<line)) {
                    const double t=(double(line)-start)/(double(end)-start);
                    if(t>=consumed&&t<first){first=t;target=next;}
                }
            };
            const size_t row=slot/grid.columns,col=slot%grid.columns;
            // Entering half of the adjacent row is enough: the leading edge
            // crosses that row's horizontal center. Preserve the grab offset.
            if(row>0)crossing(startY,py,grid.top(slot-grid.columns)+grid.height/2+grabY,-1,slot-grid.columns);
            if((row+1)*grid.columns<order.size()) {
                const size_t next=std::min((row+1)*grid.columns+col,order.size()-1);
                crossing(startY,py,grid.top(next)+grid.height/2+grabY-grid.height,1,next);
            }
            // Never wrap horizontally into another row. After insertion,
            // the next iteration uses the NEW adjacent slot's centerline.
            if(col>0)crossing(startX,px,grid.center(slot-1),-1,slot-1);
            if(col+1<grid.columns&&slot+1<order.size())crossing(startX,px,grid.center(slot+1),1,slot+1);
            if(target==slot)break;
            insert(target);++events;consumed=first;
        }
        x=px;y=py;return events;
    }
};

// Finite animation, independent of crossing decisions. A retarget starts
// from the currently visible position and never queues behind an older move.
struct Motion {
    static constexpr double duration=.080;
    float x=0,y=0,fromX=0,fromY=0,toX=0,toY=0;
    double started=0;
    bool active=false;
    void reset(float px,float py) {x=fromX=toX=px;y=fromY=toY=py;active=false;}
    bool sample(double now) {
        if(!active)return false;
        const float oldX=x,oldY=y;
        double t=std::clamp((now-started)/duration,0.0,1.0);
        if(t>=1){x=toX;y=toY;active=false;}
        else {float ease=float(1-(1-t)*(1-t)*(1-t));x=fromX+(toX-fromX)*ease;y=fromY+(toY-fromY)*ease;}
        return x!=oldX||y!=oldY;
    }
    void retarget(float px,float py,double now) {
        sample(now);
        if(px==toX&&py==toY)return;
        fromX=x;fromY=y;toX=px;toY=py;started=now;active=x!=toX||y!=toY;
    }
};
}
