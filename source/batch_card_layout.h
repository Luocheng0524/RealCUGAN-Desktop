#pragma once
#include "batch_reorder.h"
#include <algorithm>

namespace BatchCards {
constexpr size_t Columns=5,Rows=4;
constexpr float GapX=8,GapY=10;
constexpr unsigned ThumbnailWidth=512,ThumbnailHeight=288;
inline BatchReorder::Grid grid(float x,float y,float width,float height){
    return {x+16,y+18,std::max(1.f,(width-32-(Columns-1)*GapX)/Columns),
            std::max(1.f,(height-30-(Rows-1)*GapY)/Rows),GapX,GapY,Columns};
}
struct Box {float x,y,w,h;};
struct Content {Box thumbnail,name,detail,selection,selectionHit,removeHit;};
inline Content content(float width,float height){
    return {{12,10,std::max(1.f,width-24),std::max(1.f,height-54)},
            {8,height-42,std::max(1.f,width-16),17},
            {8,height-24,std::max(1.f,width-46),16},
            {width-26,height-26,18,18},
            {width-30,height-32,30,30},{width-28,0,28,24}};
}
inline Box fitImage(Box bounds,float width,float height){
    if(width<=0||height<=0)return {bounds.x,bounds.y,0,0};
    float scale=std::min(bounds.w/width,bounds.h/height);
    float w=width*scale,h=height*scale;
    return {bounds.x+(bounds.w-w)/2,bounds.y+(bounds.h-h)/2,w,h};
}
}
