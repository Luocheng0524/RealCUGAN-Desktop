#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace Batch {
inline void (*processDiagnostic)(const char*,DWORD) noexcept=nullptr;
inline void processNotice(const char* message,DWORD code=0) noexcept {
    if(processDiagnostic)processDiagnostic(message,code);else OutputDebugStringA(message);
}
class Handle {
    HANDLE value_=nullptr;
public:
    Handle()=default;
    explicit Handle(HANDLE h):value_(h){}
    ~Handle(){reset();}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    Handle(Handle&& other) noexcept:value_(other.release()){}
    Handle& operator=(Handle&& other) noexcept {if(this!=&other)reset(other.release());return *this;}
    HANDLE get()const{return value_;}
    explicit operator bool()const{return value_&&value_!=INVALID_HANDLE_VALUE;}
    HANDLE release(){auto v=value_;value_=nullptr;return v;}
    void reset(HANDLE v=nullptr){if(*this)CloseHandle(value_);value_=v;}
};
inline std::string utf8(const std::wstring& s) {
    int n=WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);
    std::string out(n,0);if(n)WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),out.data(),n,nullptr,nullptr);return out;
}
inline std::wstring wide(const std::string& s) {
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),(int)s.size(),nullptr,0);
    if(!n&&!s.empty())return L"Invalid UTF-8 diagnostic";
    std::wstring out(n,0);if(n)MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),out.data(),n);return out;
}
inline std::wstring arg(const std::wstring& s) {
    std::wstring out=L"\"";size_t slashes=0;
    for(wchar_t c:s){if(c==L'\\'){++slashes;continue;}out.append(c==L'"'?slashes*2+1:slashes,L'\\');slashes=0;out+=c;}
    out.append(slashes*2,L'\\');out+=L'"';return out;
}
inline void pipeWrite(HANDLE pipe,const void* bytes,size_t count) {
    const auto* p=static_cast<const unsigned char*>(bytes);
    while(count){DWORD n=0;if(!WriteFile(pipe,p,(DWORD)std::min<size_t>(count,65536),&n,nullptr)||!n)throw std::runtime_error("Worker input pipe failed");p+=n;count-=n;}
}
struct Request {
    uint32_t magic=0x32544352,version=2;
    uint64_t batch=0,task=0;
    uint32_t width=0,height=0,inputChars=0,outputChars=0;
};
static_assert(sizeof(Request)==40,"IPC request ABI");
struct PixelHeader {char magic[4];uint32_t width,height;};
static_assert(sizeof(PixelHeader)==12,"Pixel file ABI");

class Child {
    Handle process_,input_,output_;
    std::string pending_;
public:
    Child()=default;Child(const Child&)=delete;Child& operator=(const Child&)=delete;
    ~Child(){stop();}
    HANDLE process()const{return process_.get();}
    DWORD pid()const{return process_?GetProcessId(process_.get()):0;}
    void start(const std::wstring& exe,const std::vector<std::wstring>& args,HANDLE job) {
        stop();SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};HANDLE r=nullptr,w=nullptr;
        if(!CreatePipe(&r,&w,&sa,0))throw std::runtime_error("CreatePipe input failed");
        Handle childInput(r);input_.reset(w);
        if(!SetHandleInformation(input_.get(),HANDLE_FLAG_INHERIT,0))throw std::runtime_error("Pipe inheritance failed");
        if(!CreatePipe(&r,&w,&sa,0))throw std::runtime_error("CreatePipe output failed");
        output_.reset(r);Handle childOutput(w);
        if(!SetHandleInformation(output_.get(),HANDLE_FLAG_INHERIT,0))throw std::runtime_error("Pipe inheritance failed");
        SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,1,0,&size);
        std::vector<unsigned char> attributes(size);
        auto* list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if(!InitializeProcThreadAttributeList(list,1,0,&size))throw std::runtime_error("Worker attributes failed");
        struct DeleteList {LPPROC_THREAD_ATTRIBUTE_LIST p;~DeleteList(){DeleteProcThreadAttributeList(p);}} cleanup{list};
        HANDLE inherited[]{childInput.get(),childOutput.get()};
        if(!UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr))throw std::runtime_error("Worker handle list failed");
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.lpAttributeList=list;
        startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput=childInput.get();startup.StartupInfo.hStdOutput=childOutput.get();startup.StartupInfo.hStdError=childOutput.get();
        std::wstring command=arg(exe);for(const auto& a:args)command+=L" "+arg(a);
        PROCESS_INFORMATION pi{};
        if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&startup.StartupInfo,&pi))throw std::runtime_error("Cannot start worker");
        process_.reset(pi.hProcess);Handle thread(pi.hThread);
        if(!job||!AssignProcessToJobObject(job,process_.get())){stop();throw std::runtime_error("Cannot attach worker to cleanup job");}
        if(ResumeThread(thread.get())==DWORD(-1)){stop();throw std::runtime_error("Cannot resume worker");}
    }
    void send(Request request,const std::wstring& input,const std::wstring& output) {
        if(input.size()>32767||output.size()>32767)throw std::runtime_error("IPC path too long");
        request.inputChars=(uint32_t)input.size();request.outputChars=(uint32_t)output.size();
        pipeWrite(input_.get(),&request,sizeof(request));pipeWrite(input_.get(),input.data(),input.size()*sizeof(wchar_t));pipeWrite(input_.get(),output.data(),output.size()*sizeof(wchar_t));
    }
    // Each poll is nonblocking, output is bounded even for a noisy/crashed child.
    std::vector<std::string> lines() {
        std::vector<std::string> result;char buffer[4096];DWORD available=0,n=0;
        while(result.size()<128&&PeekNamedPipe(output_.get(),nullptr,0,nullptr,&available,nullptr)&&available){
            if(!ReadFile(output_.get(),buffer,std::min<DWORD>(available,sizeof(buffer)),&n,nullptr)||!n)break;
            pending_.append(buffer,n);size_t end=0;
            while((end=pending_.find('\n'))!=std::string::npos){result.push_back(pending_.substr(0,end));pending_.erase(0,end+1);}
            if(pending_.size()>65536)throw std::runtime_error("Worker protocol line exceeds bound");
        }
        return result;
    }
    bool exited()const{return process_&&WaitForSingleObject(process_.get(),0)==WAIT_OBJECT_0;}
    DWORD exitCode()const{DWORD code=STILL_ACTIVE;if(process_)GetExitCodeProcess(process_.get(),&code);return code;}
    void stop() {
        input_.reset();
        if(process_){
            if(WaitForSingleObject(process_.get(),100)!=WAIT_OBJECT_0&& !TerminateProcess(process_.get(),1))processNotice("worker termination request failed",GetLastError());
            ULONGLONG next=GetTickCount64()+5000;
            for(;;){DWORD status=WaitForSingleObject(process_.get(),250);if(status==WAIT_OBJECT_0)break;
                if(status==WAIT_FAILED)processNotice("worker wait failed; recovery not confirmed",GetLastError());
                if(GetTickCount64()>=next){processNotice("worker termination exceeds 5 seconds; keeping resources locked");next=GetTickCount64()+5000;}}
            DWORD code=0;if(GetExitCodeProcess(process_.get(),&code)){char message[128];snprintf(message,sizeof(message),"worker pid=%lu reaped exit_code=%lu",GetProcessId(process_.get()),code);processNotice(message);}
            else processNotice("worker exit code query failed",GetLastError());
            process_.reset();
        }
        output_.reset();pending_.clear();
    }
};
inline Handle cleanupJob() {
    Handle job(CreateJobObjectW(nullptr,nullptr));JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job||!SetInformationJobObject(job.get(),JobObjectExtendedLimitInformation,&info,sizeof(info)))throw std::runtime_error("Cannot create cleanup job");
    return job;
}
}
