#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace Batch {
// Utilization targets are measured feedback objectives; duty is only pacing.
struct GpuLoadPolicy {
    int target=95,duty=95;unsigned high=0;
    uint64_t lastId=0,lastTick=0,firstHighId=0,firstHighTick=0,triggerId=0,triggerTick=0;double firstHighUtil=0,triggerUtil=0;bool latched=false;
    void invalidate(){high=0;}
    bool update(double utilization,uint64_t id,uint64_t tick){
        if(!id||!tick||!std::isfinite(utilization)||utilization<0){invalidate();return false;}
        if(id<=lastId||tick<=lastTick)return false;
        if(lastId&&id>lastId+1)invalidate();
        lastId=id;lastTick=tick;
        if(!latched){
            high=utilization>=95?high+1:0;
            if(high==1){firstHighId=id;firstHighTick=tick;firstHighUtil=utilization;}
            if(high<2)return false;
            latched=true;triggerId=id;triggerTick=tick;triggerUtil=utilization;target=80;duty=80;return true;
        }
        if(utilization>target+2)duty=std::max(10,duty-3);
        else if(utilization<target-4)duty=std::min(95,duty+2);
        return false;
    }
};
}
