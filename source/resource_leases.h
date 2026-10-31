#pragma once
#include "resource_budget.h"
#include <condition_variable>
#include <thread>
#include <atomic>

namespace Batch {
class DiskLease {
    struct Entry {uint64_t id=0,peak=0;std::wstring volume;std::vector<std::wstring> files;};
    inline static std::mutex mutex_;
    inline static std::vector<Entry> entries_;
    inline static uint64_t next_=0;
    uint64_t id_=0;
    static uint64_t occupied(const Entry& entry) noexcept {
        uint64_t bytes=0;for(const auto& path:entry.files){DWORD high=0;SetLastError(NO_ERROR);DWORD low=GetCompressedFileSizeW(path.c_str(),&high);
            if(low!=INVALID_FILE_SIZE||GetLastError()==NO_ERROR)bytes+=(uint64_t(high)<<32)|low;}
        return bytes;
    }
public:
    ~DiskLease(){reset();}
    DiskLease()=default;DiskLease(const DiskLease&)=delete;DiskLease& operator=(const DiskLease&)=delete;
    bool acquire(const std::wstring& folder,uint64_t peak,std::vector<std::wstring> files) {
        reset();wchar_t mount[32768]{},volume[32768]{};
        if(!GetVolumePathNameW(folder.c_str(),mount,32768))throw std::runtime_error("Disk volume mapping failed");
        if(!GetVolumeNameForVolumeMountPointW(mount,volume,32768))wcsncpy(volume,mount,32767);
        std::wstring key=volume;std::transform(key.begin(),key.end(),key.begin(),[](wchar_t c){return (wchar_t)towlower(c);});
        std::lock_guard<std::mutex> lock(mutex_);ULARGE_INTEGER available{};
        if(!GetDiskFreeSpaceExW(folder.c_str(),&available,nullptr,nullptr))throw std::runtime_error("Disk availability query failed");
        uint64_t pending=0;for(const auto& entry:entries_)if(entry.volume==key){uint64_t actual=occupied(entry);pending+=entry.peak>actual?entry.peak-actual:0;}
        if(pending>available.QuadPart||peak>available.QuadPart-pending)return false;
        Entry entry;entry.id=++next_;entry.peak=peak;entry.volume=std::move(key);entry.files=std::move(files);
        entries_.push_back(std::move(entry));id_=next_;return true;
    }
    void reset() noexcept {if(!id_)return;try{std::lock_guard<std::mutex> lock(mutex_);entries_.erase(std::remove_if(entries_.begin(),entries_.end(),[&](const Entry& entry){return entry.id==id_;}),entries_.end());id_=0;}catch(...){emergency("Disk reservation release failed");}}
};

// One process-local coordinator owns promises for all child processes and the
// actual D3D preview adapter. Observed bytes and promises for the SAME owner
// are reconciled with max(), rather than adding the same allocation twice.
class GpuBudget {
    struct Claim {uint64_t id=0,bytes=0;DWORD pid=0;bool pending=true;uint64_t commitAfter=UINT64_MAX;};
    struct Node {Device device;std::unique_ptr<GpuMonitor> monitor;GpuSample sample;ULONGLONG sampled=0,readyAfter=0,lastTouch=GetTickCount64();uint64_t epoch=0;bool failed=false;std::vector<Claim> claims;};
    AppBudget& app_;
    std::mutex mutex_;std::condition_variable cv_;std::vector<std::shared_ptr<Node>> nodes_;
    std::thread thread_;std::atomic<bool> stop_{false},done_{true};uint64_t next_=0;
    static bool same(LUID a,LUID b){return a.HighPart==b.HighPart&&a.LowPart==b.LowPart;}
    std::shared_ptr<Node> find(const Device& device){for(const auto& node:nodes_)if(same(node->device.luid,device.luid)){node->lastTouch=GetTickCount64();return node;}return {};}
    static uint64_t projected(const Node& node,uint64_t extra=0) {
        std::map<DWORD,uint64_t> promises;
        uint64_t pending=extra;
        for(const auto& claim:node.claims){if(claim.pending)pending+=claim.bytes;else promises[claim.pid]+=claim.bytes;}
        uint64_t total=(node.sample.used>node.sample.application?node.sample.used-node.sample.application:0)+pending;
        for(const auto& actual:node.sample.perProcess){auto it=promises.find(actual.first);total+=std::max(actual.second,it==promises.end()?uint64_t(0):it->second);if(it!=promises.end())promises.erase(it);}
        for(const auto& promise:promises)total+=promise.second;
        return total;
    }
    void work() noexcept {
        try {
            while(!stop_){std::vector<std::shared_ptr<Node>> nodes;
                {std::unique_lock<std::mutex> lock(mutex_);cv_.wait_for(lock,std::chrono::milliseconds(500));nodes=nodes_;
                 nodes_.erase(std::remove_if(nodes_.begin(),nodes_.end(),[](const std::shared_ptr<Node>& node){return node->claims.empty()&&GetTickCount64()-node->lastTouch>60000;}),nodes_.end());}
                if(stop_)break;
                for(const auto& node:nodes){if(stop_)break;try{
                    if(!node->monitor){auto monitor=std::make_unique<GpuMonitor>();monitor->initialize(node->device);node->monitor=std::move(monitor);node->readyAfter=GetTickCount64()+1000;continue;}
                    if(GetTickCount64()<node->readyAfter)continue;
                    auto sample=node->monitor->sample(app_.processes());
                    std::lock_guard<std::mutex> lock(mutex_);if(!node->failed){sample.previousId=node->sample.sampleId;sample.previousTick=node->sample.sampleTick;sample.previousUtilization=node->sample.utilization;}sample.sampleId=node->epoch+1;sample.sampleTick=GetTickCount64();node->sample=std::move(sample);node->sampled=GetTickCount64();node->failed=false;++node->epoch;
                    for(auto& claim:node->claims)if(claim.pending&&claim.commitAfter<=node->epoch)claim.pending=false;
                }catch(const std::exception& error){emergency(error.what());std::lock_guard<std::mutex> lock(mutex_);node->failed=true;}
                 catch(...){emergency("GPU sampling exception");std::lock_guard<std::mutex> lock(mutex_);node->failed=true;}}
            }
        }catch(...){emergency("GPU coordinator thread exception");try{std::lock_guard<std::mutex> lock(mutex_);for(auto& node:nodes_)node->failed=true;}catch(...){}}
        done_.store(true,std::memory_order_release);
    }
public:
    explicit GpuBudget(AppBudget& app):app_(app){}
    ~GpuBudget(){stop();}
    void registerDevice(const Device& device){if(!device.luidValid||!device.capacity)throw std::runtime_error("GPU stable identity/capacity missing");
        std::lock_guard<std::mutex> lock(mutex_);if(stop_)throw std::runtime_error("GPU coordinator is shutting down");
        if(!find(device)){if(nodes_.size()>=32)throw std::runtime_error("GPU coordinator adapter limit reached; cancel before continuing");auto node=std::make_shared<Node>();node->device=device;nodes_.push_back(std::move(node));}
        if(!thread_.joinable()){done_=false;try{thread_=std::thread([this]{work();});}catch(...){done_=true;throw;}}cv_.notify_all();}
    bool snapshot(const Device& device,GpuSample& sample,bool& failed){std::lock_guard<std::mutex> lock(mutex_);auto node=find(device);failed=!node||node->failed;if(!node||!node->sampled||GetTickCount64()-node->sampled>2500)return false;sample=node->sample;sample.sampleId=node->epoch;sample.sampleTick=node->sampled;return !failed;}
    std::string diagnostics(const Device& device){
        std::lock_guard<std::mutex> lock(mutex_);auto node=find(device);if(!node)return "gpu_budget_node=missing";
        uint64_t pending=0,committed=0,unbound=0;for(const auto& claim:node->claims){if(claim.pending)pending+=claim.bytes;else committed+=claim.bytes;if(!claim.pid)unbound+=claim.bytes;}
        return "gpu_claims="+std::to_string(node->claims.size())+" pending_bytes="+std::to_string(pending)+" committed_promises="+std::to_string(committed)+" unbound_bytes="+std::to_string(unbound)+" projected_bytes="+std::to_string(projected(*node))+" sample_epoch="+std::to_string(node->epoch)+" sample_age_ms="+std::to_string(node->sampled?GetTickCount64()-node->sampled:UINT64_MAX);
    }
    bool canReserve(const Device& device,uint64_t bytes){std::lock_guard<std::mutex> lock(mutex_);auto node=find(device);
        return node&&!node->failed&&node->sampled&&GetTickCount64()-node->sampled<=2500&&projected(*node,bytes)<=node->sample.capacity;}
    uint64_t reserve(const Device& device,uint64_t bytes,DWORD pid){std::lock_guard<std::mutex> lock(mutex_);auto node=find(device);
        if(!node||node->failed||!node->sampled||GetTickCount64()-node->sampled>2500||projected(*node,bytes)>node->sample.capacity)return 0;
        uint64_t id=++next_;node->claims.push_back({id,bytes,pid});return id;}
    void bind(uint64_t id,DWORD pid){std::lock_guard<std::mutex> lock(mutex_);for(auto& node:nodes_)for(auto& claim:node->claims)if(claim.id==id){claim.pid=pid;return;}}
    void commit(uint64_t id){std::lock_guard<std::mutex> lock(mutex_);for(auto& node:nodes_)for(auto& claim:node->claims)if(claim.id==id){if(claim.commitAfter==UINT64_MAX)claim.commitAfter=node->epoch+1;return;}}
    void release(uint64_t id) noexcept {try{std::lock_guard<std::mutex> lock(mutex_);for(auto& node:nodes_)node->claims.erase(std::remove_if(node->claims.begin(),node->claims.end(),[&](const Claim& claim){return claim.id==id;}),node->claims.end());}catch(...){emergency("GPU reservation release failed");}}
    bool admitted(const Device& device){std::lock_guard<std::mutex> lock(mutex_);auto node=find(device);return node&&!node->failed&&node->sampled&&GetTickCount64()-node->sampled<=2500&&projected(*node)<=node->sample.capacity;}
    bool previewPermit(const Device& device) noexcept {try{std::lock_guard<std::mutex> lock(mutex_);auto node=find(device);return node&&!node->failed&&node->sampled&&GetTickCount64()-node->sampled<=2500&&node->sample.utilization<=75&&projected(*node)<=node->sample.capacity;}catch(...){return false;}}
    void requestStop() noexcept {stop_=true;cv_.notify_all();}
    bool done()const{return done_.load(std::memory_order_acquire);}
    void stop(){requestStop();if(thread_.joinable())thread_.join();}
};
class GpuLease {
    GpuBudget* budget_=nullptr;uint64_t id_=0;
public:
    GpuLease()=default;~GpuLease(){reset();}
    GpuLease(const GpuLease&)=delete;GpuLease& operator=(const GpuLease&)=delete;
    GpuLease(GpuLease&& other)noexcept:budget_(other.budget_),id_(other.id_){other.budget_=nullptr;other.id_=0;}
    GpuLease& operator=(GpuLease&& other)noexcept{if(this!=&other){reset();budget_=other.budget_;id_=other.id_;other.budget_=nullptr;other.id_=0;}return *this;}
    bool acquire(GpuBudget& budget,const Device& device,uint64_t bytes,DWORD pid=0){reset();uint64_t id=budget.reserve(device,bytes,pid);if(!id)return false;budget_=&budget;id_=id;return true;}
    void bind(DWORD pid){if(budget_)budget_->bind(id_,pid);}
    void commit(){if(budget_)budget_->commit(id_);}
    void reset()noexcept{if(budget_)budget_->release(id_);budget_=nullptr;id_=0;}
    explicit operator bool()const{return id_!=0;}
};
inline uint64_t gpuWorkspaceEstimate(int scale) {
    // Existing models: <=256 feature channels, padded tile <=236x236.
    // 24 simultaneously budgeted FP32 feature buffers plus output/weights,
    // allocator slack and driver margin. This is an ESTIMATE, not a Vulkan
    // allocation cap: upstream/driver allocations remain a documented boundary.
    uint64_t features=236ull*236*256*4*24;
    uint64_t transfer=236ull*236*scale*scale*4*8;
    return features+transfer+160*MiB;
}
}
