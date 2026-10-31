#pragma once
#include <stdexcept>
namespace TileBorder {
// Reflect without repeating the edge (the existing ncnn type=2 convention).
// A one-pixel axis has only one possible source value.
inline int reflect(int coordinate,int length){
    if(length<=0)throw std::runtime_error("Empty reflection axis");
    if(length==1)return 0;
    const int period=2*(length-1);
    int value=coordinate%period;if(value<0)value+=period;
    return value<length?value:period-value;
}
inline bool needsRepeatedReflection(int w,int h,int top,int bottom,int left,int right){
    return top>=h||bottom>=h||left>=w||right>=w;
}
}
