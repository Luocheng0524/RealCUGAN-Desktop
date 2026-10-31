#pragma once
#include "process_io.h"
#include "batch_state.h"
#include "runtime_support.h"
#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <cmath>
#include <cwctype>
#include <map>
#include <mutex>

namespace Batch {
struct Device {
    int index=-1;
    uint64_t capacity=0;
    LUID luid{};
    bool luidValid=false;
    bool unified=false;
    std::string uuid;
    std::wstring name;
    unsigned vendor=0;
    std::wstring key()const {return uuid.empty()||uuid=="GPU-00000000-0000-0000-0000-000000000000"?std::to_wstring(luid.HighPart)+L":"+std::to_wstring(luid.LowPart):wide(uuid);}
};
struct MemorySample {uint64_t total=0,available=0,application=0;};
class AppBudget {
    Handle job_;
    uint64_t limit_=0;
    uint64_t reserved_=0;
    uint64_t realized_=0;
    std::mutex admissionMutex_;
public:
    void initialize() {
        if(job_)return;
        MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
        if(!GlobalMemoryStatusEx(&memory)||!memory.ullTotalPhys)throw std::runtime_error("System memory detection failed");
        auto candidate=Handle(CreateJobObjectW(nullptr,nullptr));
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};cpu.ControlFlags=JOB_OBJECT_CPU_RATE_CONTROL_ENABLE|JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;cpu.CpuRate=8000;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_JOB_MEMORY;
        limits.JobMemoryLimit=(SIZE_T)(memory.ullTotalPhys/10*8);
        if(!candidate||!SetInformationJobObject(candidate.get(),JobObjectCpuRateControlInformation,&cpu,sizeof(cpu))||
           !SetInformationJobObject(candidate.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits))||
           !AssignProcessToJobObject(candidate.get(),GetCurrentProcess()))
            throw std::runtime_error("Cannot apply aggregate CPU/memory Job limits");
        limit_=limits.JobMemoryLimit;job_=std::move(candidate);
    }
    bool ready()const{return bool(job_);}
    uint64_t limit()const{return limit_;}
    uint64_t cpuTime()const {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if(!job_||!QueryInformationJobObject(job_.get(),JobObjectBasicAccountingInformation,&accounting,sizeof(accounting),nullptr))throw std::runtime_error("Aggregate CPU sampling failed");
        return (uint64_t)accounting.TotalUserTime.QuadPart+(uint64_t)accounting.TotalKernelTime.QuadPart;
    }
    bool reserve(uint64_t bytes) {
        std::lock_guard<std::mutex> lock(admissionMutex_);auto memory=sample();
        uint64_t pending=reserved_>realized_?reserved_-realized_:0;
        if(!admit(memory.application,pending,bytes,limit_,memory.available))return false;
        reserved_+=bytes;return true;
    }
    void release(uint64_t bytes,uint64_t realized=0) noexcept {
        try {std::lock_guard<std::mutex> lock(admissionMutex_);reserved_=bytes<=reserved_?reserved_-bytes:0;realized_=realized<=realized_?realized_-realized:0;}
        catch(...){emergency("memory reservation release failed");}
    }
    void realization(uint64_t previous,uint64_t current) {
        std::lock_guard<std::mutex> lock(admissionMutex_);realized_=realized_-std::min(previous,realized_)+current;
    }
    std::vector<DWORD> processes()const {
        std::vector<unsigned char> bytes(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST)+64*sizeof(ULONG_PTR));
        auto* ids=reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(bytes.data());
        if(!job_||!QueryInformationJobObject(job_.get(),JobObjectBasicProcessIdList,ids,(DWORD)bytes.size(),nullptr))throw std::runtime_error("Job process sampling failed");
        if(ids->NumberOfAssignedProcesses!=ids->NumberOfProcessIdsInList)throw std::runtime_error("Job process sample truncated");
        std::vector<DWORD> out;for(DWORD i=0;i<ids->NumberOfProcessIdsInList;++i)out.push_back((DWORD)ids->ProcessIdList[i]);return out;
    }
    MemorySample sample()const {
        MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
        if(!GlobalMemoryStatusEx(&memory)||!memory.ullAvailPhys)throw std::runtime_error("System memory sampling failed");
        MemorySample result{memory.ullTotalPhys,memory.ullAvailPhys,0};
        for(DWORD pid:processes()) {
            Handle process(OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|SYNCHRONIZE,FALSE,pid));
            if(!process&&GetLastError()==ERROR_INVALID_PARAMETER)continue;
            PROCESS_MEMORY_COUNTERS_EX counters{};counters.cb=sizeof(counters);
            if(!process||!GetProcessMemoryInfo(process.get(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters))) {
                // Exited children can disappear between enumeration and sample.
                if(process&&WaitForSingleObject(process.get(),0)==WAIT_OBJECT_0)continue;
                throw std::runtime_error("Application memory sampling failed");
            }
            result.application+=std::max<uint64_t>(counters.PrivateUsage,counters.WorkingSetSize);
        }
        return result;
    }
};

class MemoryLease {
    AppBudget* budget_=nullptr;uint64_t bytes_=0,realized_=0;
public:
    MemoryLease()=default;
    MemoryLease(AppBudget& budget,uint64_t bytes){if(!acquire(budget,bytes))throw std::runtime_error("Shared memory admission unavailable");}
    ~MemoryLease(){reset();}
    MemoryLease(const MemoryLease&)=delete;MemoryLease& operator=(const MemoryLease&)=delete;
    MemoryLease(MemoryLease&& other) noexcept:budget_(other.budget_),bytes_(other.bytes_),realized_(other.realized_){other.budget_=nullptr;other.bytes_=other.realized_=0;}
    MemoryLease& operator=(MemoryLease&& other) noexcept {if(this!=&other){reset();budget_=other.budget_;bytes_=other.bytes_;realized_=other.realized_;other.budget_=nullptr;other.bytes_=other.realized_=0;}return *this;}
    bool acquire(AppBudget& budget,uint64_t bytes){reset();if(!budget.reserve(bytes))return false;budget_=&budget;bytes_=bytes;return true;}
    void realize(uint64_t bytes){bytes=std::min(bytes,bytes_);if(budget_)budget_->realization(realized_,bytes);realized_=bytes;}
    void shrink(uint64_t bytes,uint64_t realized=0){bytes=std::min(bytes,bytes_);realize(std::min(realized,bytes));if(budget_&&bytes<bytes_)budget_->release(bytes_-bytes);bytes_=bytes;}
    void reset() noexcept {if(budget_)budget_->release(bytes_,realized_);budget_=nullptr;bytes_=realized_=0;}
    uint64_t bytes()const{return bytes_;}
};

// Shared by all GPU workers. One busy interval can contain several overlapping
// tiles; new submissions stop after 100 ms until every tile drains. The shared
// rest interval and feedback duty apply to the GPU as a whole, not per worker.
struct SharedControl {
    volatile LONG stop=0,pause=1,duty=95;
    LONG active=0;
    ULONGLONG busyStart=0,nextAllowed=0;
};
class GpuGate {
    Handle map_,mutex_;
    SharedControl* view_=nullptr;
public:
    GpuGate()=default;GpuGate(const GpuGate&)=delete;GpuGate& operator=(const GpuGate&)=delete;
    ~GpuGate(){close();}
    void close(){if(view_){UnmapViewOfFile(view_);view_=nullptr;}map_.reset();mutex_.reset();}
    void open(const std::wstring& token,bool create) {
        close();
        const auto name=token+L"_control",lock=token+L"_mutex";
        map_.reset(create?CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(SharedControl),name.c_str()):OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name.c_str()));
        mutex_.reset(create?CreateMutexW(nullptr,FALSE,lock.c_str()):OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,lock.c_str()));
        if(!map_||!mutex_)throw std::runtime_error("GPU control IPC unavailable");
        view_=static_cast<SharedControl*>(MapViewOfFile(map_.get(),FILE_MAP_ALL_ACCESS,0,0,sizeof(SharedControl)));
        if(!view_)throw std::runtime_error("GPU control mapping failed");
        if(create)*view_=SharedControl{};
    }
    void control(bool pause,int duty) {
        if(!view_)return;
        InterlockedExchange(&view_->duty,std::clamp(duty,10,95));InterlockedExchange(&view_->pause,pause?1:0);
    }
    void cancel(){if(view_)InterlockedExchange(&view_->stop,1);}
    void begin() {
        if(!view_)return;
        for(;;) {
            if(InterlockedCompareExchange(&view_->stop,0,0))throw std::runtime_error("CANCELLED");
            DWORD wait=WaitForSingleObject(mutex_.get(),25);
            if(wait==WAIT_ABANDONED){ReleaseMutex(mutex_.get());throw std::runtime_error("GPU control owner died");}
            if(wait==WAIT_TIMEOUT)continue;
            if(wait!=WAIT_OBJECT_0)throw std::runtime_error("GPU control wait failed");
            ULONGLONG now=GetTickCount64();
            bool enter=!view_->stop&&!view_->pause&&now>=view_->nextAllowed&&(!view_->active||now-view_->busyStart<100);
            if(enter){if(!view_->active)view_->busyStart=now;++view_->active;}
            ReleaseMutex(mutex_.get());if(enter)return;Sleep(10);
        }
    }
    void end()noexcept {
        if(!view_)return;
        DWORD wait=WaitForSingleObject(mutex_.get(),1000);
        if(wait!=WAIT_OBJECT_0&&wait!=WAIT_ABANDONED)return;
        if(view_->active>0&&--view_->active==0){ULONGLONG now=GetTickCount64();int duty=std::clamp((int)view_->duty,10,95);
            view_->nextAllowed=now+std::max<ULONGLONG>(1,(now-view_->busyStart)*(100-duty)/duty);}
        ReleaseMutex(mutex_.get());
    }
};
struct GpuSample {uint64_t sampleId=0,sampleTick=0,previousId=0,previousTick=0;double previousUtilization=0;uint64_t capacity=0,used=0,application=0;double utilization=0;std::string source;std::map<DWORD,uint64_t> perProcess;};
class NvidiaSample {
    HMODULE library_=nullptr;void* device_=nullptr;
    using Shutdown=int(__cdecl*)();
    struct Utilization {unsigned gpu,memory;};
    struct Memory {unsigned long long total,free,used;};
    using GetUtilization=int(__cdecl*)(void*,Utilization*);
    using GetMemory=int(__cdecl*)(void*,Memory*);
    Shutdown shutdown_=nullptr;GetUtilization utilization_=nullptr;GetMemory memory_=nullptr;
    template<class Function> Function symbol(const char* name){FARPROC raw=GetProcAddress(library_,name);Function fn=nullptr;static_assert(sizeof(fn)==sizeof(raw));memcpy(&fn,&raw,sizeof(fn));return fn;}
public:
    NvidiaSample()=default;
    NvidiaSample(const NvidiaSample&)=delete;
    NvidiaSample& operator=(const NvidiaSample&)=delete;
    ~NvidiaSample(){if(shutdown_)shutdown_();if(library_)FreeLibrary(library_);}
    void initialize(const std::string& uuid) {
        if(uuid.empty())return;
        wchar_t system[32768];if(!GetSystemDirectoryW(system,32768))return;
        library_=LoadLibraryExW((std::wstring(system)+L"\\nvml.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!library_)return;
        using Init=int(__cdecl*)();using Lookup=int(__cdecl*)(const char*,void**);
        auto init=symbol<Init>("nvmlInit_v2");auto shutdown=symbol<Shutdown>("nvmlShutdown");auto lookup=symbol<Lookup>("nvmlDeviceGetHandleByUUID");
        utilization_=symbol<GetUtilization>("nvmlDeviceGetUtilizationRates");memory_=symbol<GetMemory>("nvmlDeviceGetMemoryInfo");
        if(!init||!shutdown||!lookup||!utilization_||!memory_||init()!=0)return;
        shutdown_=shutdown;
        if(lookup(uuid.c_str(),&device_)!=0)device_=nullptr;
    }
    bool sample(GpuSample& result) {
        if(!device_)return false;
        Utilization utilization{};Memory memory{};
        if(utilization_(device_,&utilization)!=0||memory_(device_,&memory)!=0||!memory.total)return false;
        result.used=memory.used;result.capacity=std::min<uint64_t>(result.capacity,memory.total/10*8);result.utilization=utilization.gpu;result.source="NVML-UUID+DXGI";return true;
    }
};
class GpuMonitor {
    PDH_HQUERY query_=nullptr;
    PDH_HCOUNTER usage_=nullptr,processUsage_=nullptr,engines_=nullptr;
    IDXGIAdapter3* adapter_=nullptr;
    Device device_;
    NvidiaSample nvidia_;
    std::wstring match_;
    std::vector<std::pair<std::wstring,double>> values(PDH_HCOUNTER counter) {
        DWORD bytes=0,count=0;PDH_STATUS status=PdhGetFormattedCounterArrayW(counter,PDH_FMT_DOUBLE,&bytes,&count,nullptr);
        if(status!=static_cast<PDH_STATUS>(PDH_MORE_DATA)||!bytes)throw std::runtime_error("GPU counter array unavailable");
        std::vector<unsigned char> buffer(bytes);auto* data=reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if(PdhGetFormattedCounterArrayW(counter,PDH_FMT_DOUBLE,&bytes,&count,data)!=ERROR_SUCCESS)throw std::runtime_error("GPU counter sample failed");
        std::vector<std::pair<std::wstring,double>> out;
        for(DWORD i=0;i<count;++i){if(data[i].FmtValue.CStatus!=PDH_CSTATUS_VALID_DATA&&data[i].FmtValue.CStatus!=PDH_CSTATUS_NEW_DATA)continue;
            if(std::isfinite(data[i].FmtValue.doubleValue)&&data[i].FmtValue.doubleValue>=0){
                std::wstring name=data[i].szName;std::transform(name.begin(),name.end(),name.begin(),[](wchar_t c){return (wchar_t)towlower(c);});
                out.emplace_back(std::move(name),data[i].FmtValue.doubleValue);}}
        return out;
    }
public:
    ~GpuMonitor(){if(query_)PdhCloseQuery(query_);if(adapter_)adapter_->Release();}
    void initialize(const Device& device) {
        device_=device;if(!device.luidValid||!device.capacity)throw std::runtime_error("GPU LUID/capacity unavailable");
        wchar_t name[96];swprintf(name,96,L"luid_0x%08x_0x%08x",(unsigned)device.luid.HighPart,(unsigned)device.luid.LowPart);match_=name;
        IDXGIFactory1* factory=nullptr;
        if(FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),reinterpret_cast<void**>(&factory))))throw std::runtime_error("DXGI factory unavailable");
        for(UINT i=0;;++i){IDXGIAdapter1* candidate=nullptr;if(factory->EnumAdapters1(i,&candidate)==DXGI_ERROR_NOT_FOUND)break;
            if(!candidate)break;
            DXGI_ADAPTER_DESC1 desc{};candidate->GetDesc1(&desc);
            if(desc.AdapterLuid.HighPart==device.luid.HighPart&&desc.AdapterLuid.LowPart==device.luid.LowPart)
                candidate->QueryInterface(__uuidof(IDXGIAdapter3),reinterpret_cast<void**>(&adapter_));
            candidate->Release();if(adapter_)break;}
        factory->Release();if(!adapter_)throw std::runtime_error("Inference GPU has no matching DXGI budget adapter");
        if(device.vendor==0x10de)nvidia_.initialize(device.uuid);
        const wchar_t* adapterCounter=device.unified?L"\\GPU Adapter Memory(*)\\Shared Usage":L"\\GPU Adapter Memory(*)\\Dedicated Usage";
        const wchar_t* processCounter=device.unified?L"\\GPU Process Memory(*)\\Shared Usage":L"\\GPU Process Memory(*)\\Dedicated Usage";
        if(PdhOpenQueryW(nullptr,0,&query_)!=ERROR_SUCCESS||
           PdhAddEnglishCounterW(query_,adapterCounter,0,&usage_)!=ERROR_SUCCESS||
           PdhAddEnglishCounterW(query_,processCounter,0,&processUsage_)!=ERROR_SUCCESS||
           PdhAddEnglishCounterW(query_,L"\\GPU Engine(*)\\Utilization Percentage",0,&engines_)!=ERROR_SUCCESS||
           PdhCollectQueryData(query_)!=ERROR_SUCCESS)throw std::runtime_error("Selected GPU performance counters unavailable");
    }
    GpuSample sample(const std::vector<DWORD>& pids) {
        if(PdhCollectQueryData(query_)!=ERROR_SUCCESS)throw std::runtime_error("GPU telemetry collection failed");
        DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
        if(FAILED(adapter_->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&memory))||!memory.Budget)throw std::runtime_error("Selected GPU dynamic budget unavailable");
        GpuSample out;out.capacity=std::min<uint64_t>(device_.capacity/10*8,memory.Budget/10*9);
        bool nvml=nvidia_.sample(out);bool found=nvml;
        if(!nvml)for(const auto& entry:values(usage_))if(entry.first.find(match_)!=std::wstring::npos){out.used+=(uint64_t)entry.second;found=true;}
        if(!found)throw std::runtime_error("Selected GPU memory counter not found");
        for(const auto& entry:values(processUsage_))if(entry.first.find(match_)!=std::wstring::npos)
            for(DWORD pid:pids)if(entry.first.rfind(L"pid_"+std::to_wstring(pid)+L"_",0)==0){out.application+=(uint64_t)entry.second;out.perProcess[pid]+=(uint64_t)entry.second;break;}
        // Sum process utilization on the same physical engine, then take the
        // busiest engine. Summing all engines would count overlapping work twice.
        if(!nvml){std::map<std::wstring,double> totals;
            for(const auto& entry:values(engines_))if(entry.first.find(match_)!=std::wstring::npos){
                auto engine=entry.first.find(L"_eng_");if(engine!=std::wstring::npos)totals[entry.first.substr(engine)]+=entry.second;}
            if(totals.empty())throw std::runtime_error("Selected GPU engine counters not found");
            for(const auto& entry:totals)out.utilization=std::max(out.utilization,entry.second);
            out.source="PDH-LUID+DXGI";}
        return out;
    }
};
}
