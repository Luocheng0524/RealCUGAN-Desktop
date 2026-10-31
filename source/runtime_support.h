#pragma once
#include "process_io.h"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>

namespace Batch {
inline std::wstring executableDirectory() {
    wchar_t path[32768]{};DWORD n=GetModuleFileNameW(nullptr,path,32768);
    if(!n||n>=32768)throw std::runtime_error("Executable path unavailable");
    return std::filesystem::path(path).parent_path().wstring();
}
inline std::wstring runtimeDirectory(const wchar_t* name) {
    auto path=executableDirectory()+L"\\"+name;
    if(!CreateDirectoryW(path.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)
        throw std::runtime_error("Runtime directory cannot be created: "+utf8(path));
    auto probe=path+L"\\.write-check-"+std::to_wstring(GetCurrentProcessId());
    Handle file(CreateFileW(probe.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr));
    if(!file)throw std::runtime_error("Runtime directory not writable: "+utf8(path));
    return path;
}
// Fixed-size emergency reporting remains usable when ordinary error strings
// cannot be allocated. Set once, before any application thread is started.
inline wchar_t emergencyPath[32768]{};
inline std::atomic<bool> emergencyWriteFailed{false};
inline void setEmergencyDirectory(const std::wstring& directory) noexcept {
    _snwprintf(emergencyPath,32768,L"%ls\\runtime-fault-%lu.log",directory.c_str(),GetCurrentProcessId());
    emergencyPath[32767]=0;
}
inline void emergency(const char* text,DWORD code=0) noexcept {
    char line[1536];SYSTEMTIME time{};GetLocalTime(&time);
    int n=snprintf(line,sizeof(line),"%04u-%02u-%02u %02u:%02u:%02u %.*s system_error=%lu\r\n",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,1200,text,code);
    OutputDebugStringA(line);
    if(!emergencyPath[0]||n<=0){emergencyWriteFailed=true;return;}
    HANDLE file=CreateFileW(emergencyPath,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE){emergencyWriteFailed=true;return;}
    DWORD written=0,wanted=(DWORD)std::min<int>(n,sizeof(line)-1);
    if(!WriteFile(file,line,wanted,&written,nullptr)||written!=wanted||!FlushFileBuffers(file))emergencyWriteFailed=true;
    CloseHandle(file);
}
inline bool removeFile(const std::wstring& path) noexcept {
    if(path.empty()||DeleteFileW(path.c_str()))return true;
    DWORD code=GetLastError();if(code==ERROR_FILE_NOT_FOUND||code==ERROR_PATH_NOT_FOUND)return true;
    char location[1201]{};WideCharToMultiByte(CP_UTF8,0,path.c_str(),-1,location,1200,nullptr,nullptr);
    emergency("temporary file deletion failed",code);emergency(location,code);return false;
}
class TemporaryFile {
    std::wstring path_;bool owned_=false;
public:
    explicit TemporaryFile(std::wstring path,bool owned=false):path_(std::move(path)),owned_(owned){}
    ~TemporaryFile(){if(owned_)removeFile(path_);}
    void own()noexcept{owned_=true;}
    void committed()noexcept{owned_=false;}
    TemporaryFile(const TemporaryFile&)=delete;TemporaryFile& operator=(const TemporaryFile&)=delete;
};
inline uint64_t imageBytes(uint32_t w,uint32_t h,uint64_t bytesPerPixel=4) {
    if(!w||!h||uint64_t(w)*h>64000000||bytesPerPixel>1024)throw std::runtime_error("Image dimensions/size exceed limits");
    return uint64_t(w)*h*bytesPerPixel;
}
}
