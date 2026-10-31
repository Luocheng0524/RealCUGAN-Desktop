// 预览几何计算独立于 Windows：同一屏幕区域映射到原图与结果的对应像素。
#pragma once
#include <algorithm>
#include <cstdint>
#include <cmath>

namespace Preview {
// 仅纹理上传时预乘透明度，源像素和保存数据不作修改。
inline void premultiplyRow(const unsigned char* source,unsigned char* destination,uint32_t count) {
    for(uint32_t x=0;x<count;++x){
        unsigned a=source[x*4+3];
        for(int c=0;c<3;++c)destination[x*4+c]=(source[x*4+c]*a+127)/255;
        destination[x*4+3]=a;
    }
}

// 分界线两侧 8 个逻辑像素可抓取；中间圆形把手按实际圆形范围命中。
inline bool hitDivider(float x,float y,float divider,float top,float height) {
    if(y<top||y>top+height)return false;
    float dx=x-divider,dy=y-(top+height/2);
    return std::abs(dx)<=8.f||dx*dx+dy*dy<=17.f*17.f;
}
// 对数倍率上的临界阻尼运动：收到新目标直接续接当前位置和速度，无动画队列。
// 使用解析积分，不依赖帧率；没有输入等待时间或固定播放时长。
struct SmoothZoom {
    double position=0,target=0,velocity=0;
    bool active=false;
    void reset(double scale){position=target=std::log(std::clamp(scale,.005,20.0));velocity=0;active=false;}
    double scale() const{return std::exp(position);}
    void wheel(int steps){target=std::clamp(target+steps*std::log(1.15),std::log(.005),std::log(20.0));active=true;}
    void advance(double seconds){
        if(!active||seconds<=0)return;
        constexpr double omega=80;
        double error=position-target,coefficient=velocity+omega*error,decay=std::exp(-omega*seconds);
        position=target+(error+coefficient*seconds)*decay;
        velocity=(velocity-omega*coefficient*seconds)*decay;
        if(position<std::log(.005)){position=std::log(.005);velocity=0;}
        if(position>std::log(20.0)){position=std::log(20.0);velocity=0;}
        if(std::abs(position-target)<1e-6&&std::abs(velocity)<1e-4){position=target;velocity=0;active=false;}
    }
};

inline float anchoredPan(float cursor,float viewportStart,float viewportSize,float imageSize,float scale,float fraction) {
    return cursor-viewportStart-viewportSize/2+imageSize*scale/2-fraction*imageSize*scale;
}
struct Rect { float x, y, w, h; };
inline bool intersects(Rect a,Rect b) {
    return a.x+a.w>b.x&&a.x<b.x+b.w&&a.y+a.h>b.y&&a.y<b.y+b.h;
}
struct TileRegion {uint32_t x,y,w,h,uploadX,uploadY,uploadW,uploadH;};
inline TileRegion tileRegion(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t imageW,uint32_t imageH) {
    uint32_t left=x?x-1:0,top=y?y-1:0;
    uint32_t right=std::min(imageW,x+w+1),bottom=std::min(imageH,y+h+1);
    return {x,y,w,h,left,top,right-left,bottom-top};
}
}
