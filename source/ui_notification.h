#pragma once
#include <windows.h>
#include <atomic>

namespace Batch {
// No payload crosses threads. The UI drains the owning object's synchronized
// results after acknowledging this coalesced wake-up; timers remain a fallback.
class UiNotification {
    std::atomic<HWND> window_{nullptr};
    std::atomic<bool> pending_{false};
    const UINT message_;
public:
    explicit UiNotification(UINT message):message_(message){}
    void bind(HWND window) noexcept {window_.store(window,std::memory_order_release);}
    void acknowledge() noexcept {pending_.store(false,std::memory_order_release);}
    void notify() noexcept {
        HWND window=window_.load(std::memory_order_acquire);
        if(window&&!pending_.exchange(true,std::memory_order_acq_rel))
            if(!PostMessageW(window,message_,0,0))pending_.store(false,std::memory_order_release);
    }
};
inline void notifyUi(UiNotification* notification) noexcept {if(notification)notification->notify();}
}
