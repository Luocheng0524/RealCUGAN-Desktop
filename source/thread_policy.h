// 自动选择属于平衡启发式，不冒充实测最快配置；手动允许用满可用逻辑处理器。
#pragma once
#include <algorithm>
namespace Hardware {
inline int chooseThreads(int logical,int physical,int requested){
    logical=std::max(1,logical);
    if(requested>0)return std::clamp(requested,1,logical);
    physical=std::clamp(physical,1,logical);
    return std::min(physical,std::max(1,logical-1));
}
}
