#pragma once
#include "batch_pipeline.h"

namespace Batch {
struct PreviewRequest {
    uint64_t generation=0,batch=0;size_t index=0;
    std::wstring source,png;uint32_t w=0,h=0,resultW=0,resultH=0;
};
struct PreviewResult {
    MemoryLease memory;
    PreviewRequest request;Image source,result;std::wstring error;
};
class PreviewLoader {
    AppBudget& budget_;UiNotification* notification_;std::mutex mutex_;std::condition_variable cv_;std::thread thread_;
    bool stopping_=false;std::unique_ptr<PreviewRequest> pending_;std::unique_ptr<PreviewResult> completed_;
    uint64_t generation_=0;std::atomic<bool> done_{true},failed_{false};
    bool cancelled(uint64_t generation){std::lock_guard<std::mutex> lock(mutex_);return stopping_||generation!=generation_;}
    void work() noexcept {
        try{for(;;){PreviewRequest request;
            {std::unique_lock<std::mutex> lock(mutex_);cv_.wait(lock,[&]{return stopping_||bool(pending_);});if(stopping_)break;request=std::move(*pending_);pending_.reset();}
            auto result=std::make_unique<PreviewResult>();result->request=std::move(request);auto& current=result->request;
            try{
                uint64_t need=imageBytes(current.w,current.h,16)+imageBytes(current.resultW,current.resultH,16)+64*MiB;
                if(!result->memory.acquire(budget_,need))throw std::runtime_error("Preview memory budget unavailable; retry after running tasks finish");
                auto stop=[&]{return cancelled(current.generation);};
                result->source=decode(current.source,current.w,current.h,stop);result->memory.realize(result->source.data.size());
                result->result=decode(current.png,current.resultW,current.resultH,stop);result->memory.realize(result->source.data.size()+result->result.data.size());
            }catch(const std::bad_alloc&){failed_=true;emergency("preview allocation failed; valid PNG retained");notifyUi(notification_);continue;}
             catch(const std::exception& error){result->source={};result->result={};result->memory.reset();result->error=wide(error.what());}
             catch(...){failed_=true;emergency("preview decode exception; valid PNG retained");notifyUi(notification_);continue;}
            {std::lock_guard<std::mutex> lock(mutex_);if(stopping_||current.generation!=generation_)continue;completed_=std::move(result);}
            notifyUi(notification_);
        }}catch(...){failed_=true;emergency("preview loader outer exception; valid PNG retained");}
        done_.store(true,std::memory_order_release);notifyUi(notification_);
    }
public:
    explicit PreviewLoader(AppBudget& budget,UiNotification* notification=nullptr):budget_(budget),notification_(notification){}
    ~PreviewLoader(){stop();}
    uint64_t request(PreviewRequest request){
        if(thread_.joinable()&&done())thread_.join();
        std::lock_guard<std::mutex> lock(mutex_);
        request.generation=++generation_;auto next=std::make_unique<PreviewRequest>(std::move(request));
        pending_=std::move(next);completed_.reset();
        if(!thread_.joinable()){stopping_=false;done_=false;try{thread_=std::thread([this]()noexcept{work();});}catch(...){done_=true;pending_.reset();throw;}}
        cv_.notify_all();return generation_;
    }
    std::unique_ptr<PreviewResult> drain(){std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);return lock.owns_lock()?std::move(completed_):nullptr;}
    bool failed()noexcept{return failed_.exchange(false);}
    bool done()const{return done_.load(std::memory_order_acquire);}
    void invalidate(){std::lock_guard<std::mutex> lock(mutex_);++generation_;pending_.reset();completed_.reset();}
    void requestStop(){std::lock_guard<std::mutex> lock(mutex_);stopping_=true;++generation_;pending_.reset();cv_.notify_all();}
    void stop(){requestStop();if(thread_.joinable())thread_.join();std::lock_guard<std::mutex> lock(mutex_);completed_.reset();}
};
enum class ExportFailureKind { None,Destination,Cache,DiskBudget,DiskQuery,Memory,TemporaryFile,Read,Write,Validation,Commit,Internal };
struct ExportResult {size_t exported=0;std::wstring error;double seconds=0;bool failed=false,cancelled=false;DWORD systemError=0;ExportFailureKind failure=ExportFailureKind::None;};
inline bool exportDestinationAvailable(const std::wstring& folder,DWORD& error) noexcept {
    DWORD attributes=GetFileAttributesW(folder.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES){error=GetLastError();return false;}
    if(!(attributes&FILE_ATTRIBUTE_DIRECTORY)){error=ERROR_DIRECTORY;return false;}
    error=ERROR_SUCCESS;return true;
}
inline const wchar_t* exportFailureMessage(const ExportResult& result) noexcept {
    switch(result.failure){
        case ExportFailureKind::Destination:return L"导出文件夹已失效，可能已被重命名、移动或删除。请重新选择文件夹后再导出。";
        case ExportFailureKind::Cache:return L"无法读取已完成的 PNG 缓存，请检查缓存文件是否仍存在。";
        case ExportFailureKind::DiskBudget:return L"导出磁盘的可用空间不足，请释放空间或选择其他文件夹。";
        case ExportFailureKind::DiskQuery:return L"无法查询导出磁盘，请确认目标文件夹和磁盘仍可访问。";
        case ExportFailureKind::Memory:return L"导出所需内存不足，请稍后重试。";
        case ExportFailureKind::TemporaryFile:return L"无法在目标文件夹创建导出文件，请检查目录、写入权限及文件占用情况。";
        case ExportFailureKind::Read:return L"读取 PNG 缓存失败，本次导出已停止。";
        case ExportFailureKind::Write:return L"写入导出文件失败，请检查磁盘空间、权限及磁盘连接。";
        case ExportFailureKind::Validation:return L"导出文件写入不完整或无法完成写盘，本次导出已停止。";
        case ExportFailureKind::Commit:return L"无法提交导出文件，请检查目标文件夹、同名文件权限及占用情况。";
        default:return L"导出失败，请查看故障日志，并重新选择目标文件夹后重试。";
    }
}
struct ExportFailure {ExportFailureKind kind;DWORD code;const char* diagnostic;};
struct ExportTemporaryOutput {
    Handle file;bool committed=false;
    ~ExportTemporaryOutput(){
        if(file&&!committed){FILE_DISPOSITION_INFO disposition{};disposition.DeleteFile=TRUE;
            if(!SetFileInformationByHandle(file.get(),FileDispositionInfo,&disposition,sizeof(disposition))){DWORD code=GetLastError();emergency("export temporary handle cleanup failed",code);}}
    }
    HANDLE get()const{return file.get();}
};
class Exporter {
    AppBudget* budget_=nullptr;UiNotification* notification_;std::thread thread_;std::atomic<bool> done_{true},cancel_{false};ExportResult result_;
public:
    explicit Exporter(AppBudget* budget=nullptr,UiNotification* notification=nullptr):budget_(budget),notification_(notification){}
    ~Exporter(){stop();}
    void start(std::vector<std::wstring> paths,std::wstring folder,std::wstring token){
        if(thread_.joinable()){if(!done())throw std::runtime_error("Export is still active");thread_.join();}
        result_={};done_=false;cancel_=false;
        try{thread_=std::thread([this,paths=std::move(paths),folder=std::move(folder),token=std::move(token)]()noexcept{
            auto started=Clock::now();
            try{
                auto destination=[&]{DWORD code=ERROR_SUCCESS;if(!exportDestinationAvailable(folder,code))throw ExportFailure{ExportFailureKind::Destination,code,"Export destination invalid or no longer available"};};
                destination();
                MemoryLease memory;if(budget_&&!memory.acquire(*budget_,16*MiB))throw ExportFailure{ExportFailureKind::Memory,0,"Export copy memory admission unavailable"};
                std::vector<unsigned char> buffer(1024*1024);
                for(size_t i=0;i<paths.size()&&!cancel_;++i){
                    destination();
                    auto final=folder+L"\\RC批处理"+std::to_wstring(i+1)+L".png";
                    const auto part=folder+L"\\.RCG-export-"+token+L"-"+std::to_wstring(i)+L".tmp";
                    ExportTemporaryOutput output;
                    WIN32_FILE_ATTRIBUTE_DATA data{};if(!GetFileAttributesExW(paths[i].c_str(),GetFileExInfoStandard,&data)){DWORD code=GetLastError();throw ExportFailure{ExportFailureKind::Cache,code,"Valid PNG cache is no longer readable"};}
                    uint64_t size=(uint64_t(data.nFileSizeHigh)<<32)|data.nFileSizeLow;DiskLease disk;
                    bool admitted=false;
                    try{admitted=disk.acquire(folder,size+MiB,{part});}
                    catch(const std::bad_alloc&){throw;}
                    catch(const std::exception& error){emergency(error.what());destination();throw ExportFailure{ExportFailureKind::DiskQuery,0,"Export disk query failed"};}
                    if(!admitted)throw ExportFailure{ExportFailureKind::DiskBudget,0,"Export volume has insufficient unreserved space"};
                    {Handle input(CreateFileW(paths[i].c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
                     if(!input){DWORD code=GetLastError();throw ExportFailure{ExportFailureKind::Cache,code,"PNG cache open failed"};}
                     output.file.reset(CreateFileW(part.c_str(),GENERIC_WRITE|DELETE,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
                     if(!output.file){DWORD code=GetLastError();destination();throw ExportFailure{ExportFailureKind::TemporaryFile,code,"Exclusive export temporary file creation failed"};}
                     uint64_t copied=0;
                     while(!cancel_){DWORD n=0;if(!ReadFile(input.get(),buffer.data(),(DWORD)buffer.size(),&n,nullptr)){DWORD code=GetLastError();throw ExportFailure{ExportFailureKind::Read,code,"PNG cache read failed"};}
                         if(!n){break;}
                         DWORD offset=0;
                         while(offset<n&&!cancel_){DWORD written=0;if(!WriteFile(output.get(),buffer.data()+offset,std::min<DWORD>(n-offset,65536),&written,nullptr)){DWORD code=GetLastError();throw ExportFailure{ExportFailureKind::Write,code,"PNG export write failed"};}if(!written)throw ExportFailure{ExportFailureKind::Write,0,"PNG export zero-byte write"};offset+=written;copied+=written;}}
                     if(!cancel_){
                         if(copied!=size)throw ExportFailure{ExportFailureKind::Validation,0,"Export size validation failed"};
                         if(!FlushFileBuffers(output.get())){DWORD code=GetLastError();throw ExportFailure{ExportFailureKind::Validation,code,"Export flush validation failed"};}}}
                    if(cancel_){result_.cancelled=true;break;}
                    destination();
                    if(!MoveFileExW(part.c_str(),final.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DWORD code=GetLastError();destination();throw ExportFailure{ExportFailureKind::Commit,code,"Export commit failed"};}
                    output.committed=true;++result_.exported;
                }
            }catch(const ExportFailure& error){result_.failed=true;result_.failure=error.kind;result_.systemError=error.code;emergency(error.diagnostic,error.code);try{result_.error=wide(error.diagnostic);}catch(...){}}
             catch(const std::bad_alloc&){result_.failed=true;result_.failure=ExportFailureKind::Memory;emergency("export allocation failure; PNG caches retained");}
             catch(const std::exception& error){result_.failed=true;result_.failure=ExportFailureKind::Internal;emergency(error.what(),result_.systemError);try{result_.error=wide(error.what());}catch(...){}}
             catch(...){result_.failed=true;result_.failure=ExportFailureKind::Internal;emergency("export outer exception; PNG caches retained");}
            result_.cancelled=result_.cancelled||cancel_;result_.seconds=seconds(started);done_.store(true,std::memory_order_release);notifyUi(notification_);
        });}catch(...){done_=true;throw;}
    }
    bool done()const{return done_.load(std::memory_order_acquire);}
    ExportResult finish(){if(!done())throw std::runtime_error("Export cleanup still active");if(thread_.joinable())thread_.join();return std::move(result_);}
    void requestStop()noexcept{cancel_=true;}
    void stop(){requestStop();if(thread_.joinable())thread_.join();}
};
}
