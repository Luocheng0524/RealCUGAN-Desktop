#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

namespace Batch {
enum class Stage { Pending, Preparing, Processing, WaitingEncode, Encoding, Completed, Failed, Cancelled };
enum class Phase { Stopped, Running, ErrorAwaitCancel, Cancelling };
inline bool terminal(Stage s) { return s==Stage::Completed||s==Stage::Failed||s==Stage::Cancelled; }
inline bool active(Stage s) { return s==Stage::Preparing||s==Stage::Processing||s==Stage::WaitingEncode||s==Stage::Encoding; }
// The scheduler and UI use the same monotone transition rules. A committed PNG
// is immutable, including when completion and cancellation happen together.
inline bool canTransition(Stage from,Stage to) {
    if(terminal(from)||from==to)return false;
    if(to==Stage::Cancelled||to==Stage::Failed)return true;
    return (from==Stage::Pending&&to==Stage::Preparing)||
        (from==Stage::Preparing&&to==Stage::Processing)||
        (from==Stage::Processing&&to==Stage::WaitingEncode)||
        (from==Stage::WaitingEncode&&to==Stage::Encoding)||
        (from==Stage::Encoding&&to==Stage::Completed);
}
constexpr uint64_t MiB=1024ull*1024;
inline uint64_t memoryReservation(uint32_t w,uint32_t h,int scale) {
    if(!w||!h||w>5000||h>5000||(scale!=2&&scale!=3)||uint64_t(w)*h*scale*scale>64000000)
        return std::numeric_limits<uint64_t>::max();
    // Input decode + worker RGB/BGRA + output, encoder, and allocator margin.
    // Kept until PNG commit, so four simultaneous completions are pre-funded.
    return uint64_t(w)*h*(24ull+uint64_t(scale)*scale*16ull)+384*MiB;
}
inline bool admit(uint64_t used,uint64_t reserved,uint64_t need,uint64_t capacity,uint64_t available) {
    if(!capacity||!available||need>capacity||used>capacity)return false;
    return reserved<=capacity-used&&need<=capacity-used-reserved&&reserved<=available&&need<=available-reserved;
}
inline int lanes(int gpu,int requested) {return gpu<0?1:(requested==2||requested==4?requested:1);}
}
