#pragma once
#include "resource_leases.h"
#include "gpu_load_policy.h"
#include "ui_notification.h"
#include <gdiplus.h>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <sstream>
#include <thread>
#include <atomic>
#include <io.h>

namespace Batch {
using Clock=std::chrono::steady_clock;
inline double seconds(Clock::time_point t){return std::chrono::duration<double>(Clock::now()-t).count();}
inline const CLSID& pngEncoder(){static const CLSID value={0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};return value;}
struct Image {uint32_t w=0,h=0;std::vector<unsigned char> data;};
inline Image decode(const std::wstring& path,uint32_t expectedW=0,uint32_t expectedH=0,const std::function<bool()>& cancelled={}) {
    std::unique_ptr<Gdiplus::Bitmap> bitmap(Gdiplus::Bitmap::FromFile(path.c_str()));
    if(!bitmap||bitmap->GetLastStatus()!=Gdiplus::Ok)throw std::runtime_error("Image decode failed");
    if(cancelled&&cancelled())throw std::runtime_error("CANCELLED");
    Image image;image.w=bitmap->GetWidth();image.h=bitmap->GetHeight();
    if(!image.w||!image.h||uint64_t(image.w)*image.h>64000000||(expectedW&&image.w!=expectedW)||(expectedH&&image.h!=expectedH))throw std::runtime_error("Image dimensions changed or exceed limit");
    image.data.resize(size_t(image.w)*image.h*4);Gdiplus::BitmapData data{};Gdiplus::Rect area(0,0,image.w,image.h);
    if(bitmap->LockBits(&area,Gdiplus::ImageLockModeRead,PixelFormat32bppARGB,&data)!=Gdiplus::Ok)throw std::runtime_error("Image pixel access failed");
    struct Unlock {Gdiplus::Bitmap* bitmap;Gdiplus::BitmapData* data;~Unlock(){bitmap->UnlockBits(data);}} unlock{bitmap.get(),&data};
    for(uint32_t y=0;y<image.h;++y){if(cancelled&&cancelled()){throw std::runtime_error("CANCELLED");}memcpy(image.data.data()+size_t(y)*image.w*4,static_cast<unsigned char*>(data.Scan0)+ptrdiff_t(y)*data.Stride,size_t(image.w)*4);}
    return image;
}
inline void writePixels(const std::wstring& path,const Image& image) {
    using File=std::unique_ptr<FILE,decltype(&fclose)>;File file(_wfopen(path.c_str(),L"wb"),fclose);
    PixelHeader header{{'R','C','G','1'},image.w,image.h};
    if(!file||fwrite(&header,sizeof(header),1,file.get())!=1||fwrite(image.data.data(),1,image.data.size(),file.get())!=image.data.size()||fflush(file.get()))throw std::runtime_error("Input cache write failed");
    FILE* closing=file.release();if(fclose(closing))throw std::runtime_error("Input cache close failed");
}
inline Image readPixels(const std::wstring& path,uint32_t width,uint32_t height) {
    using File=std::unique_ptr<FILE,decltype(&fclose)>;File file(_wfopen(path.c_str(),L"rb"),fclose);PixelHeader header{};
    if(!file||fread(&header,sizeof(header),1,file.get())!=1||memcmp(header.magic,"RCG1",4)||header.width!=width||header.height!=height||uint64_t(width)*height>64000000)throw std::runtime_error("Invalid worker output dimensions");
    Image image;image.w=width;image.h=height;image.data.resize(size_t(width)*height*4);
    if(fread(image.data.data(),1,image.data.size(),file.get())!=image.data.size()||fgetc(file.get())!=EOF)throw std::runtime_error("Invalid worker output byte length");
    return image;
}
inline void encodePart(const std::wstring& raw,const std::wstring& part,uint32_t width,uint32_t height,const std::function<bool()>& cancelled={}) {
    if(cancelled&&cancelled())throw std::runtime_error("CANCELLED");
    auto image=readPixels(raw,width,height);
    if(cancelled&&cancelled())throw std::runtime_error("CANCELLED");
    {Gdiplus::Bitmap bitmap(width,height,width*4,PixelFormat32bppARGB,image.data.data());
     if(bitmap.GetLastStatus()!=Gdiplus::Ok||bitmap.Save(part.c_str(),&pngEncoder(),nullptr)!=Gdiplus::Ok)throw std::runtime_error("PNG encode failed: disk full or cache not writable");}
    // Decode the complete saved PNG, checking more than its file header.
    {if(cancelled&&cancelled())throw std::runtime_error("CANCELLED");auto verified=decode(part,width,height,cancelled);if(verified.data!=image.data)throw std::runtime_error("PNG cache pixel validation failed");}
    Handle file(CreateFileW(part.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!file||!FlushFileBuffers(file.get()))throw std::runtime_error("PNG cache flush failed");
}
class Session {
    std::wstring root_,token_;
public:
    ~Session(){clear();}
    void create() {
        if(!root_.empty())return;
        auto cache=runtimeDirectory(L"caches");wchar_t id[64];GUID guid{};
        if(FAILED(CoCreateGuid(&guid))||!StringFromGUID2(guid,id,64))throw std::runtime_error("Session path creation failed");
        token_=L"Local\\RealCUGAN_"+std::to_wstring(GetCurrentProcessId())+id;
        auto candidate=cache+L"\\RealCUGAN_"+std::to_wstring(GetCurrentProcessId())+id;
        if(!CreateDirectoryW(candidate.c_str(),nullptr))throw std::runtime_error("Session directory creation failed");
        root_=std::move(candidate);
    }
    const std::wstring& root()const{return root_;}
    std::wstring token(uint64_t batch)const{return token_+L"_"+std::to_wstring(batch);}
    std::wstring batchDirectory(uint64_t batch)const {
        auto path=root_+L"\\batch_"+std::to_wstring(batch);
        if(!CreateDirectoryW(path.c_str(),nullptr))throw std::runtime_error("Batch cache directory creation failed");
        return path;
    }
    bool clear()noexcept {
        if(root_.empty())return true;
        try{std::error_code error;std::filesystem::remove_all(root_,error);if(!error)root_.clear();else emergency("Session cache deletion failed",(DWORD)error.value());return !error;}catch(...){emergency("Session cleanup exception");return false;}
    }
};
class Log {
    std::mutex mutex_;FILE* file_=nullptr;size_t bytes_=0;
    std::wstring path_,previous_;
    std::atomic<bool> failed_{false};
public:
    ~Log(){if(file_&&fclose(file_))emergency("batch log close failed",GetLastError());}
    bool failed()const{return failed_;}
    void open(const std::wstring& directory,uint64_t batch){
        std::lock_guard<std::mutex> lock(mutex_);if(file_)fclose(file_);file_=nullptr;bytes_=0;failed_=false;
        SYSTEMTIME t{};GetLocalTime(&t);wchar_t name[160];
        swprintf(name,160,L"\\batch-%04u%02u%02u-%02u%02u%02u-%lu-%llu.log",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,GetCurrentProcessId(),(unsigned long long)batch);
        path_=directory+name;previous_=path_+L".previous";
        file_=_wfopen(path_.c_str(),L"wb");if(!file_)throw std::runtime_error("Persistent batch log cannot be created");
    }
    void write(const char* message) noexcept {
        try{std::lock_guard<std::mutex> lock(mutex_);
            if(!file_){failed_=true;emergency(message);return;}
            if(bytes_>=8*MiB){if(fclose(file_))failed_=true;file_=nullptr;
                if(!MoveFileExW(path_.c_str(),previous_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){failed_=true;emergency("log rotation failed",GetLastError());}
                file_=_wfopen(path_.c_str(),L"wb");bytes_=0;if(!file_){failed_=true;emergency(message);return;}}
            SYSTEMTIME now{};GetLocalTime(&now);char prefix[64];int n=snprintf(prefix,sizeof(prefix),"%02u:%02u:%02u.%03u ",now.wHour,now.wMinute,now.wSecond,now.wMilliseconds);
            size_t length=std::min<size_t>(strlen(message),65536);
            bool ok=n>0&&fwrite(prefix,1,(size_t)n,file_)==(size_t)n&&fwrite(message,1,length,file_)==length&&fputs("\r\n",file_)>=0&&fflush(file_)==0;
            if(!ok){failed_=true;emergency("batch log write/flush failed",GetLastError());emergency(message);clearerr(file_);}bytes_+=length+32;
        }catch(...){failed_=true;emergency(message);}
    }
    void write(const std::string& message) noexcept {write(message.c_str());}
};
inline std::vector<Device> probeDevices(const std::wstring& exe,std::string& diagnostics) {
    auto job=cleanupJob();Child child;child.start(exe,{L"--probe"},job.get());auto start=Clock::now();std::vector<Device> devices;bool done=false;
    for(;;){auto output=child.lines();bool exited=child.exited();if(exited){auto tail=child.lines();output.insert(output.end(),tail.begin(),tail.end());}
    for(const auto& line:output) {
        if(diagnostics.size()<65536)diagnostics+=line+"\n";
        if(line=="PROBE_DONE"||line=="PROBE_DONE\r")done=true;
        if(line.rfind("GPU\t",0)!=0)continue;
        std::vector<std::string> fields;std::istringstream stream(line);std::string field;while(std::getline(stream,field,'\t'))fields.push_back(field);
        if(fields.size()<10)throw std::runtime_error("Invalid GPU probe protocol");
        Device device;device.index=std::stoi(fields[1]);device.capacity=std::stoull(fields[2]);device.luid.LowPart=std::stoul(fields[3]);device.luid.HighPart=std::stol(fields[4]);device.luidValid=fields[5]=="1";device.name=wide(fields[6]);
        device.vendor=(unsigned)std::stoul(fields[8]);device.unified=std::stoi(fields[9])==1;if(fields.size()>10){device.uuid=fields[10];while(!device.uuid.empty()&&(device.uuid.back()=='\r'||device.uuid.back()=='\n'))device.uuid.pop_back();}
        if(device.index>=0&&device.capacity&&device.luidValid)devices.push_back(std::move(device));
    }
    if(exited){if(child.exitCode()!=0||!done)throw std::runtime_error("GPU probe failed; inspect hardware diagnostics");break;}
    if(seconds(start)>15)throw std::runtime_error("GPU probe timeout");
    Sleep(25);}
    return devices;
}
struct Task {
    uint64_t id=0;size_t index=0;std::wstring source,input,raw,png;
    uint32_t w=0,h=0;uint64_t reserved=0;std::shared_ptr<DiskLease> disk;Stage stage=Stage::Pending;
    int progress=0;
};
struct Event {
    enum class Kind { State, Progress, Fault, Stopped }kind=Kind::State;
    uint64_t batch=0,task=0;size_t index=0;Stage stage=Stage::Pending;
    int progress=0;std::wstring path,error;bool cancelled=false;
};
struct Config {
    uint64_t batch=0;std::wstring exe,models,cache,token,logs,hardware;
    int scale=2,noise=-1,threads=1,concurrency=1;Device device;
};
class Engine {
    UiNotification* notification_=nullptr;
    AppBudget& budget_;
    GpuBudget* gpuBudget_=nullptr;
    std::mutex mutex_;std::condition_variable cv_;
    std::deque<Event> events_;
    std::vector<Task> tasks_;std::deque<size_t> encoding_;
    std::thread coordinator_;std::vector<std::thread> lanes_;
    Phase phase_=Phase::Stopped;
    Config config_;Handle job_;GpuGate gate_;Log log_;
    GpuSample gpu_;
    Clock::time_point batchStart_{};uint64_t reserved_=0;
    size_t admitted_=0;std::map<std::thread::id,std::string> waiting_;GpuLoadPolicy loadPolicy_;ULONGLONG protectionStarted_=0;std::string protectionReason_;int feedbackDuty_=95;bool feedbackPaused_=true;size_t tickets_=0;bool producerDone_=false,sampleReady_=false;
    std::atomic<bool> monitorStop_{false},finished_{true},emergencyFault_{false},cleanupPending_{false},stopNotice_{false},stopCancelled_{false};
    void push(Event event) {
        event.batch=config_.batch;
        if(event.kind==Event::Kind::Progress){for(auto it=events_.rbegin();it!=events_.rend();++it)if(it->task==event.task&&it->kind==Event::Kind::Progress){*it=std::move(event);notifyUi(notification_);return;}}
        events_.push_back(std::move(event));notifyUi(notification_);
    }
    void transition(size_t index,Stage stage,const std::wstring& error=L"") {
        auto& task=tasks_[index];if(!canTransition(task.stage,stage))return;
        Event event;event.task=task.id;event.index=task.index;event.stage=stage;event.error=error;
        if(stage==Stage::Completed){event.path=task.png;event.progress=100;}
        push(std::move(event));task.stage=stage;log_.write("task="+std::to_string(task.id)+" state="+std::to_string((int)stage)+" "+utf8(error));
    }
    void fault(const std::wstring& error) {
        log_.write("ERROR "+utf8(error));if(phase_!=Phase::Running)return;
        phase_=Phase::ErrorAwaitCancel;gate_.control(true,10);Event event;event.kind=Event::Kind::Fault;event.error=error;push(std::move(event));cv_.notify_all();
    }
    void panic(const char* message) noexcept {
        emergency(message);emergencyFault_=true;notifyUi(notification_);
        try{std::lock_guard<std::mutex> lock(mutex_);if(phase_==Phase::Running)phase_=Phase::ErrorAwaitCancel;gate_.control(true,10);cv_.notify_all();}catch(...){}
    }
    void release(Task& task){if(task.reserved){reserved_-=task.reserved;budget_.release(task.reserved);task.reserved=0;if(tickets_)--tickets_;}cv_.notify_all();}
    bool cancelled(){std::lock_guard<std::mutex> lock(mutex_);return phase_==Phase::Cancelling;}
    std::string statusLocked() const {
        size_t preparing=0,processing=0,encoding=0;
        for(const auto& task:tasks_){preparing+=task.stage==Stage::Preparing;processing+=task.stage==Stage::Processing;encoding+=task.stage==Stage::Encoding;}
        std::map<std::string,size_t> reasons;for(const auto& wait:waiting_)++reasons[wait.second];
        std::string result="requested="+std::to_string(config_.concurrency)+" admitted="+std::to_string(config_.device.index<0?std::min<size_t>(1,preparing+processing):admitted_)+" preparing="+std::to_string(preparing)+" processing="+std::to_string(processing)+" encoding="+std::to_string(encoding);
        for(const auto& reason:reasons)result+=" wait_"+reason.first+"="+std::to_string(reason.second);
        return result+" load_target="+std::to_string(loadPolicy_.target)+" high_samples="+std::to_string(loadPolicy_.high)+" sample_id="+std::to_string(gpu_.sampleId)+" sample_tick="+std::to_string(gpu_.sampleTick)+" protection_reason="+protectionReason_+" protection_wait_ms="+std::to_string(protectionStarted_?GetTickCount64()-protectionStarted_:0)+" duty="+std::to_string(feedbackDuty_)+" feedback_paused="+std::to_string(feedbackPaused_);
    }
    void monitor() {
        try {
            if(config_.device.index>=0)gpuBudget_->registerDevice(config_.device);
            // GpuBudget owns the PDH warm-up and sample freshness checks.
            // An already warm preview adapter must not pay a second fixed delay.
            auto previousTime=Clock::now();uint64_t previousCpu=budget_.cpuTime();
            while(!monitorStop_) {
                if(log_.failed())throw std::runtime_error("Batch log write/flush failed; emergency summary attempted");
                auto memory=budget_.sample();GpuSample sample;
                auto now=Clock::now();uint64_t cpu=budget_.cpuTime();double interval=std::chrono::duration<double>(now-previousTime).count();
                double cpuPercent=interval>=.5?(cpu-previousCpu)/100000.0/interval/std::max(1UL,GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)):-1;
                previousCpu=cpu;previousTime=now;
                if(config_.device.index>=0){bool failed=false;if(!gpuBudget_->snapshot(config_.device,sample,failed)){
                    {std::lock_guard<std::mutex> lock(mutex_);sampleReady_=false;loadPolicy_.invalidate();feedbackPaused_=true;feedbackDuty_=10;gate_.control(true,10);if(!protectionStarted_)protectionStarted_=GetTickCount64();protectionReason_="telemetry_stale";}
                    if(failed){throw std::runtime_error("Selected GPU telemetry unavailable; cancel before retry");}
                    Sleep(100);continue;}}
                std::string sampleLine,policyLine;
                {std::lock_guard<std::mutex> lock(mutex_);gpu_=sample;sampleReady_=true;
                 if(config_.device.index>=0){bool over=!gpuBudget_->admitted(config_.device);
                     feedbackPaused_=over||phase_!=Phase::Running;
                     if(feedbackPaused_){loadPolicy_.invalidate();if(!protectionStarted_)protectionStarted_=GetTickCount64();protectionReason_=over?"gpu_budget":"phase_protection";}
                     else {
                         const bool recovering=protectionStarted_!=0;
                         if(protectionStarted_){policyLine+="protection_resume reason="+protectionReason_+" wait_ms="+std::to_string(GetTickCount64()-protectionStarted_)+" ";protectionStarted_=0;protectionReason_.clear();}
                         bool changed=false;if(!recovering&&sample.previousId&&sample.sampleTick-sample.previousTick<=1500)changed=loadPolicy_.update(sample.previousUtilization,sample.previousId,sample.previousTick);
                         changed=loadPolicy_.update(sample.utilization,sample.sampleId,sample.sampleTick)||changed;
                         if(changed)policyLine+="load_target_change old=95 new=80 reason=two_distinct_samples_ge95 first_id="+std::to_string(loadPolicy_.firstHighId)+" first_tick="+std::to_string(loadPolicy_.firstHighTick)+" first_util="+std::to_string(loadPolicy_.firstHighUtil)+" trigger_id="+std::to_string(loadPolicy_.triggerId)+" trigger_tick="+std::to_string(loadPolicy_.triggerTick)+" trigger_util="+std::to_string(loadPolicy_.triggerUtil)+" sample_id="+std::to_string(sample.sampleId)+" sample_tick="+std::to_string(sample.sampleTick)+" previous_id="+std::to_string(sample.previousId)+" previous_tick="+std::to_string(sample.previousTick)+" previous_util="+std::to_string(sample.previousUtilization)+" utilization="+std::to_string(sample.utilization)+" since_batch_seconds="+std::to_string(seconds(batchStart_))+" ";
                     }
                     feedbackDuty_=loadPolicy_.duty;gate_.control(feedbackPaused_,feedbackDuty_);}
                 sampleLine="sample memory="+std::to_string(memory.application)+" available="+std::to_string(memory.available)+" reserved="+std::to_string(reserved_)+" cpu_percent="+std::to_string(cpuPercent)+" gpu_total="+std::to_string(sample.used)+" gpu_app="+std::to_string(sample.application)+" gpu_capacity="+std::to_string(sample.capacity)+" gpu_percent="+std::to_string(sample.utilization)+" gpu_source="+sample.source+" queue="+std::to_string(encoding_.size())+" "+statusLocked();
                 cv_.notify_all();}
                if(config_.device.index>=0)sampleLine+=" "+gpuBudget_->diagnostics(config_.device);
                if(!policyLine.empty())log_.write(policyLine);
                log_.write(sampleLine); // No synchronous sample write under the engine lock.
                for(int i=0;i<10&&!monitorStop_;++i)Sleep(100);
            }
        }catch(const std::exception& error){panic(error.what());}catch(...){panic("monitor thread exception");}
    }
    bool take(size_t& index,GpuLease& gpuSlot,MemoryLease& unifiedMemory) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto laneId=std::this_thread::get_id();
        struct WaitCleanup {std::map<std::thread::id,std::string>& waits;std::thread::id id;~WaitCleanup(){waits.erase(id);}} cleanup{waiting_,laneId};
        for(;;){
            waiting_[laneId]="telemetry_or_phase";
            if(phase_==Phase::Cancelling)return false;
            if(phase_==Phase::Running&&sampleReady_) {
                size_t next=tasks_.size();for(size_t i=0;i<tasks_.size();++i)if(tasks_[i].stage==Stage::Pending){next=i;break;}
                if(next==tasks_.size())return false;
                auto& task=tasks_[next];uint64_t need=memoryReservation(task.w,task.h,config_.scale);
                if(need>budget_.limit()){transition(next,Stage::Failed,L"图片所需内存超过容量预算");fault(L"内存容量不足，请取消后调整输入");continue;}
                bool gpuAllowed=true;std::string gpuWait="gpu_admission";uint64_t workspace=gpuWorkspaceEstimate(config_.scale);
                if(config_.device.index>=0&&workspace>gpu_.capacity){transition(next,Stage::Failed,L"GPU动态容量不足以容纳模型工作区估算");fault(L"GPU容量预算不足，请取消后手动选择其他设备");continue;}
                if(config_.device.index>=0&&!gpuSlot){gpuAllowed=gpuSlot.acquire(*gpuBudget_,config_.device,workspace);
                    if(gpuAllowed&&config_.device.unified&&!unifiedMemory.acquire(budget_,workspace)){gpuSlot.reset();gpuAllowed=false;gpuWait="unified_memory_admission";}
                    if(gpuAllowed)++admitted_;}
                waiting_[laneId]=!gpuAllowed?gpuWait:tickets_>=8?"encoding_backlog":"memory_or_disk_admission";
                if(tickets_<8&&gpuAllowed){
                    auto disk=std::make_shared<DiskLease>();
                    uint64_t diskNeed=uint64_t(task.w)*task.h*(4ull+12ull*config_.scale*config_.scale)+32*MiB;
                    bool diskAllowed=disk->acquire(config_.cache,diskNeed,{task.input,task.raw,task.raw+L".part",task.png,task.png+L".part"});
                    if(!diskAllowed&&!tickets_){transition(next,Stage::Failed,L"缓存卷可用空间不足（包含在途预留）");fault(L"缓存磁盘空间不足，请取消处理");continue;}
                    waiting_[laneId]=diskAllowed?"memory_admission":"disk_admission";
                    if(diskAllowed&&budget_.reserve(need)){
                        task.disk=std::move(disk);reserved_+=need;task.reserved=need;++tickets_;index=next;transition(next,Stage::Preparing);return true;}
                }
            }
            cv_.wait_for(lock,std::chrono::milliseconds(100));
        }
    }
    void lane() {
        GpuLease gpuSlot;MemoryLease unifiedMemory;size_t index=tasks_.size();Child worker;bool policyVerified=false;
        try {
            while(take(index,gpuSlot,unifiedMemory)) {
                auto& task=tasks_[index];auto prepareStart=Clock::now();const auto taskStart=prepareStart;
                log_.write("task="+std::to_string(task.id)+" admission_from_batch_seconds="+std::to_string(seconds(batchStart_)));
                {auto image=decode(task.source,task.w,task.h,[this]{return cancelled();});if(cancelled())break;
                 double decodeSeconds=seconds(prepareStart);auto handoffStart=Clock::now();writePixels(task.input,image);
                 log_.write("task="+std::to_string(task.id)+" cpu_decode_seconds="+std::to_string(decodeSeconds)+" input_handoff_seconds="+std::to_string(seconds(handoffStart)));}
                if(cancelled())break;
                if(config_.device.index<0) {
                    // The original CPU executable/CLI is retained: one image
                    // at a time, with multithreading inside that image.
                    worker.start(config_.exe,{task.input,task.raw,config_.models,std::to_wstring(config_.scale),std::to_wstring(config_.noise),std::to_wstring(config_.threads)},job_.get());
                }else {
                    if(!worker.process())worker.start(config_.exe,{L"--serve",config_.models,std::to_wstring(config_.scale),std::to_wstring(config_.noise),std::to_wstring(std::min(config_.threads,2)),std::to_wstring(config_.device.index),config_.token,config_.device.key()},job_.get());
                    gpuSlot.bind(worker.pid());
                    Request request;request.batch=config_.batch;request.task=task.id;request.width=task.w;request.height=task.h;worker.send(request,task.input,task.raw);
                }
                if(config_.device.index<0){std::lock_guard<std::mutex> lock(mutex_);transition(index,Stage::Processing);}
                auto lastProgress=Clock::now();const auto workerStart=lastProgress;bool done=false;
                while(!done&&!cancelled()) {
                    auto output=worker.lines();bool exited=worker.exited();if(exited){auto tail=worker.lines();output.insert(output.end(),tail.begin(),tail.end());}
                    for(const auto& line:output) {
                        if(line=="GPU_POLICY 1 duty_max=95"||line=="GPU_POLICY 1 duty_max=95\r")policyVerified=true;
                        if(config_.device.index>=0&&line.rfind("READY ",0)==0&&!policyVerified)throw std::runtime_error("GPU worker policy mismatch; use the complete matching package");
                        if(line.rfind("DEVICE_READY ",0)==0||line.rfind("READY ",0)==0)gpuSlot.commit();
                        if(line.rfind("PROGRESS ",0)!=0)log_.write("task="+std::to_string(task.id)+" stdout="+line);
                        unsigned long long batch=0,id=0;int percent=0;long remaining=0;
                        bool progress=sscanf(line.c_str(),"PROGRESS %llu %llu %d %ld",&batch,&id,&percent,&remaining)==4&&batch==config_.batch&&id==task.id;
                        if(config_.device.index<0)progress=sscanf(line.c_str(),"PROGRESS %d %ld",&percent,&remaining)==2;
                        if(progress) {
                            if(seconds(lastProgress)>=.1||percent==100){std::lock_guard<std::mutex> lock(mutex_);task.progress=std::clamp(percent,0,99);
                                Event event;event.kind=Event::Kind::Progress;event.task=task.id;event.index=task.index;event.progress=task.progress;push(std::move(event));lastProgress=Clock::now();}
                        }else if(sscanf(line.c_str(),"START %llu %llu",&batch,&id)==2&&batch==config_.batch&&id==task.id){std::lock_guard<std::mutex> lock(mutex_);transition(index,Stage::Processing);}
                        else if((config_.device.index<0&&(line=="DONE"||line=="DONE\r"))||(sscanf(line.c_str(),"DONE %llu %llu",&batch,&id)==2&&batch==config_.batch&&id==task.id))done=true;
                        else if(sscanf(line.c_str(),"FAILED %llu %llu",&batch,&id)==2&&batch==config_.batch&&id==task.id)throw std::runtime_error(line);
                    }
                    if(exited&&!(config_.device.index<0&&done&&worker.exitCode()==0))throw std::runtime_error("Worker exited code="+std::to_string(worker.exitCode()));
                    if(seconds(prepareStart)>600){log_.write("worker has not completed after 600s; cancel remains available");prepareStart=Clock::now();}
                    if(!done)Sleep(25);
                }
                removeFile(task.input);if(cancelled())break;
                if(config_.device.index<0)worker.stop();
                log_.write("task="+std::to_string(task.id)+" preparation_plus_inference_seconds="+std::to_string(seconds(taskStart))+" worker_wall_including_gate_seconds="+std::to_string(seconds(workerStart)));
                {std::lock_guard<std::mutex> lock(mutex_);transition(index,Stage::WaitingEncode);encoding_.push_back(index);cv_.notify_all();}
                index=tasks_.size();
            }
        }catch(const std::exception& error){std::lock_guard<std::mutex> lock(mutex_);
            if(phase_!=Phase::Cancelling){if(index<tasks_.size()){transition(index,Stage::Failed,wide(error.what()));}fault(wide(error.what()));}}
        catch(...){std::lock_guard<std::mutex> lock(mutex_);if(phase_!=Phase::Cancelling){if(index<tasks_.size()){transition(index,Stage::Failed,L"Worker未知异常");}fault(L"Worker未知异常");}}
        worker.stop();
        {std::lock_guard<std::mutex> lock(mutex_);if(index<tasks_.size()&&tasks_[index].reserved){if(phase_==Phase::Cancelling)transition(index,Stage::Cancelled);release(tasks_[index]);}
         if(gpuSlot&&admitted_){--admitted_;}gpuSlot.reset();unifiedMemory.reset();
cv_.notify_all();}
    }
    void encoder() {
        for(;;){size_t index=0;
            {std::unique_lock<std::mutex> lock(mutex_);cv_.wait(lock,[&]{return phase_==Phase::Cancelling||!encoding_.empty()||producerDone_;});
             if(phase_==Phase::Cancelling||(producerDone_&&encoding_.empty()))return;
             index=encoding_.front();encoding_.pop_front();transition(index,Stage::Encoding);}
            auto& task=tasks_[index];const auto part=task.png+L".part";auto start=Clock::now();
            TemporaryFile partGuard(part,true);
            try {
                encodePart(task.raw,part,task.w*config_.scale,task.h*config_.scale,[this]{return cancelled();});
                {std::lock_guard<std::mutex> lock(mutex_);
                 if(phase_!=Phase::Cancelling){if(!MoveFileExW(part.c_str(),task.png.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("PNG commit failed");transition(index,Stage::Completed);}
                 else transition(index,Stage::Cancelled);
                 release(task);}
                log_.write("task="+std::to_string(task.id)+" encode_seconds="+std::to_string(seconds(start))+" png_commit_from_batch_seconds="+std::to_string(seconds(batchStart_)));
            }catch(const std::exception& error){std::lock_guard<std::mutex> lock(mutex_);if(phase_!=Phase::Cancelling){transition(index,Stage::Failed,wide(error.what()));fault(wide(error.what()));}else transition(index,Stage::Cancelled);release(task);}
            catch(...){std::lock_guard<std::mutex> lock(mutex_);transition(index,phase_==Phase::Cancelling?Stage::Cancelled:Stage::Failed,L"PNG编码未知异常");if(phase_!=Phase::Cancelling)fault(L"PNG编码未知异常");release(task);}
            removeFile(part);removeFile(task.raw);task.disk.reset();
        }
    }
    void coordinate() noexcept {
        auto started=Clock::now();std::thread sampler,encoding;
        auto joinThread=[](std::thread& thread)noexcept{
            while(thread.joinable()){try{thread.join();}catch(...){emergency("thread join failed; resources remain locked");Sleep(250);}}
        };
        auto waitCancel=[&]()noexcept{
            try{std::unique_lock<std::mutex> lock(mutex_);cv_.wait(lock,[&]{return phase_==Phase::Cancelling;});}catch(...){emergency("cancellation wait failed");}
        };
        try{
            sampler=std::thread([this]()noexcept{try{monitor();}catch(...){panic("monitor outer exception");}});
            encoding=std::thread([this]()noexcept{try{encoder();}catch(...){panic("encoder outer exception");}});
            for(int i=0;i<lanes(config_.device.index,config_.concurrency);++i)
                lanes_.emplace_back([this]()noexcept{try{lane();}catch(...){panic("worker lane outer exception");}});
            for(auto& thread:lanes_)joinThread(thread);
            lanes_.clear();
            {std::lock_guard<std::mutex> lock(mutex_);producerDone_=true;cv_.notify_all();}
            joinThread(encoding);
        }catch(...){panic("coordinator execution exception");waitCancel();}
        if(job_&&!TerminateJobObject(job_.get(),1))emergency("cleanup Job termination failed",GetLastError());
        for(auto& thread:lanes_)joinThread(thread);
        lanes_.clear();
        try{std::lock_guard<std::mutex> lock(mutex_);producerDone_=true;cv_.notify_all();}catch(...){panic("producer completion exception");}
        joinThread(encoding);monitorStop_=true;joinThread(sampler);
        if(job_){JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            ULONGLONG notice=0;
            for(;;){bool queried=QueryInformationJobObject(job_.get(),JobObjectBasicAccountingInformation,&accounting,sizeof(accounting),nullptr)!=FALSE;
                if(queried&&!accounting.ActiveProcesses)break;
                cleanupPending_=true;if(GetTickCount64()>=notice){emergency("Job cleanup pending; new work forbidden",queried?0:GetLastError());notice=GetTickCount64()+5000;}Sleep(250);}
            cleanupPending_=false;job_.reset();}
        bool wasCancelled=true;
        try{
            std::unique_lock<std::mutex> lock(mutex_);
            if(phase_==Phase::ErrorAwaitCancel)cv_.wait(lock,[&]{return phase_==Phase::Cancelling;});
            wasCancelled=phase_==Phase::Cancelling;
            for(size_t i=0;i<tasks_.size();++i){auto& task=tasks_[i];
                try{if(!terminal(task.stage))transition(i,Stage::Cancelled);}catch(...){task.stage=Stage::Cancelled;emergency("cancel state notification unavailable");}
                release(task);
                bool clean=removeFile(task.input);clean=removeFile(task.raw)&&clean;
                try{clean=removeFile(task.raw+L".part")&&clean;clean=removeFile(task.png+L".part")&&clean;}catch(...){clean=false;emergency("cleanup path allocation failed");}
                if(!clean){emergency("task file cleanup incomplete");}
                task.disk.reset();
            }
            reserved_=0;tickets_=0;admitted_=0;waiting_.clear();feedbackDuty_=95;feedbackPaused_=true;loadPolicy_=GpuLoadPolicy{};protectionStarted_=0;protectionReason_.clear();phase_=Phase::Stopped;
            try{log_.write("STOPPED cancelled="+std::to_string(wasCancelled)+" wall_seconds="+std::to_string(seconds(started))+" worker_processes_confirmed_zero=1");Event event;event.kind=Event::Kind::Stopped;event.cancelled=wasCancelled;push(std::move(event));}
            catch(...){emergency("stop notification allocation failed; atomic notice retained");}
        }catch(...){panic("coordinator cleanup exception");cleanupPending_=true;return;}
        stopCancelled_=wasCancelled;stopNotice_=true;
    }
public:
    Engine(AppBudget& budget,GpuBudget& gpu,UiNotification* notification=nullptr):notification_(notification),budget_(budget),gpuBudget_(&gpu){}
    bool done()const{return finished_.load(std::memory_order_acquire);}
    bool emergencyFault()noexcept{return emergencyFault_.exchange(false);}
    bool cleanupPending()const{return cleanupPending_;}
    bool takeStopped(bool& cancelled){
        if(!done())return false;
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);
        // Completion cannot overtake task events that the UI has not drained.
        if(!lock.owns_lock()||!events_.empty()||!stopNotice_.exchange(false))return false;
        cancelled=stopCancelled_.load();return true;
    }
    ~Engine(){cancel();join();}
    void start(Config config,std::vector<Task> tasks) {
        join();std::lock_guard<std::mutex> lock(mutex_);if(!budget_.ready())throw std::runtime_error("Aggregate resource limits not available");
        batchStart_=Clock::now();config_=std::move(config);tasks_=std::move(tasks);events_.clear();encoding_.clear();producerDone_=sampleReady_=false;monitorStop_=false;
        reserved_=0;tickets_=0;admitted_=0;waiting_.clear();feedbackDuty_=95;feedbackPaused_=true;loadPolicy_=GpuLoadPolicy{};protectionStarted_=0;protectionReason_.clear();phase_=Phase::Stopped;
        log_.open(config_.logs,config_.batch);log_.write("batch="+std::to_string(config_.batch)+" gpu="+std::to_string(config_.device.index)+" concurrency="+std::to_string(config_.concurrency)+" scale="+std::to_string(config_.scale)+" noise="+std::to_string(config_.noise)+" threads="+std::to_string(config_.threads)+" tile=200 cpu_hard_cap=80 memory_limit="+std::to_string(budget_.limit())+" GPU_load_target=95 latched_load_target=80 duty_initial=95 duty_max=95 policy=two_distinct_valid_samples_ge95");
        log_.write(utf8(config_.hardware));
        job_=cleanupJob();gate_.close();if(config_.device.index>=0)gate_.open(config_.token,true);
        phase_=Phase::Running;finished_=false;emergencyFault_=false;
        stopNotice_=false;
        try{coordinator_=std::thread([this]()noexcept{coordinate();finished_.store(true,std::memory_order_release);notifyUi(notification_);});}
        catch(...){finished_=true;phase_=Phase::Stopped;throw;}
    }
    void cancel(){
        std::lock_guard<std::mutex> lock(mutex_);if(phase_==Phase::Stopped)return;
        phase_=Phase::Cancelling;gate_.cancel();
        if(job_&&!TerminateJobObject(job_.get(),1))emergency("cancel Job termination failed",GetLastError());
        cv_.notify_all();log_.write("CANCEL requested");
    }
    void join(){if(coordinator_.joinable())coordinator_.join();}
    void record(const std::string& line){log_.write(line);}
    bool logFailed()const{return log_.failed();}
    std::wstring statusLabel(bool countsOnly=false){
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock())return L"调度状态更新中";
        size_t processing=0,preparing=0;
        for(const auto& task:tasks_){processing+=task.stage==Stage::Processing;preparing+=task.stage==Stage::Preparing;}
        size_t admitted=config_.device.index<0?std::min<size_t>(1,processing+preparing):admitted_;
        if(countsOnly)return L"请求 "+std::to_wstring(config_.concurrency)+L" · 准入 "+std::to_wstring(admitted)+L" · 处理中 "+std::to_wstring(processing);
        if(config_.device.index>=0&&feedbackPaused_)return L"调度：保护暂停";
        if(!sampleReady_)return L"调度：等待采样";
        std::wstring reasons;
        for(const auto& wait:waiting_){
            const auto& reason=wait.second;
            const wchar_t* label=reason=="gpu_admission"?L"显存":reason=="unified_memory_admission"?L"共享内存":reason=="disk_admission"?L"磁盘":reason=="memory_admission"?L"内存":reason=="encoding_backlog"?L"编码队列":L"调度";
            if(reasons.find(label)==std::wstring::npos){if(!reasons.empty())reasons+=L" / ";reasons+=label;}
        }
        return reasons.empty()?(config_.device.index<0?L"调度：CPU串行":L"GPU负载目标 "+std::to_wstring(loadPolicy_.target)+L"% · 调度正常"):L"等待："+reasons;
    }
    std::deque<Event> drain(){std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);std::deque<Event> out;if(lock.owns_lock())out.swap(events_);return out;}
};
}
