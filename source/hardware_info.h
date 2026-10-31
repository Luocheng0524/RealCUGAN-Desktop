// 本机 CPU 拓扑、进程可用处理器、内存和 PnP 显卡检测；不联网、不测速。
#pragma once
#include <windows.h>
#include <setupapi.h>
#include <set>
#include <cwctype>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdint>
#include "thread_policy.h"
namespace Hardware {
struct Info {
    std::wstring cpu=L"未知 CPU";
    int physical=1,logical=1,available=1,availablePhysical=1;
    uint64_t memory=0,freeMemory=0;
    std::vector<std::wstring> gpus,otherDisplays;
    bool gpuDetectionFailed=false,displaysScanned=false;
};
inline int bitCount(ULONG_PTR mask){int count=0;while(mask){mask&=mask-1;++count;}return count;}
inline Info detectCpu(){
    Info info;
    wchar_t name[512]{};DWORD bytes=sizeof(name);
    if(RegGetValueW(HKEY_LOCAL_MACHINE,L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",L"ProcessorNameString",RRF_RT_REG_SZ,nullptr,name,&bytes)==ERROR_SUCCESS)info.cpu=name;
    SYSTEM_INFO system{};GetSystemInfo(&system);
    info.logical=std::max(1UL,GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    DWORD_PTR processMask=0,systemMask=0;GROUP_AFFINITY group{};GetThreadGroupAffinity(GetCurrentThread(),&group);
    bool affinityKnown=GetProcessAffinityMask(GetCurrentProcess(),&processMask,&systemMask)&&processMask;
    if(!affinityKnown)processMask=system.dwActiveProcessorMask;
    info.available=std::max(1,bitCount(processMask));
    DWORD size=0;GetLogicalProcessorInformationEx(RelationProcessorCore,nullptr,&size);
    std::vector<unsigned char> buffer(size);
    int allCores=0,availableCores=0;
    if(size&&GetLogicalProcessorInformationEx(RelationProcessorCore,(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer.data(),&size)){
        for(DWORD offset=0;offset<size;){
            auto* item=(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buffer.data()+offset);
            if(!item->Size||offset+item->Size>size)break;
            ++allCores;
            for(WORD i=0;i<item->Processor.GroupCount;++i)
                if(item->Processor.GroupMask[i].Group==group.Group&&(item->Processor.GroupMask[i].Mask&processMask)){++availableCores;break;}
            offset+=item->Size;
        }
    }
    info.physical=std::max(1,allCores);info.availablePhysical=std::clamp(availableCores?availableCores:info.available,1,info.available);
    MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
    if(GlobalMemoryStatusEx(&memory)){info.memory=memory.ullTotalPhys;info.freeMemory=memory.ullAvailPhys;}
    return info;
}
inline Info detect(){
    Info info=detectCpu();
    // 以当前存在的 PnP 显示设备为准，不把 DXGI 逻辑适配器条目数当作显卡数。
    const GUID displayClass={0x4d36e968,0xe325,0x11ce,{0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18}};
    HDEVINFO devices=SetupDiGetClassDevsW(&displayClass,nullptr,nullptr,DIGCF_PRESENT);
    struct DeviceList {HDEVINFO value;~DeviceList(){if(value!=INVALID_HANDLE_VALUE)SetupDiDestroyDeviceInfoList(value);}} deviceList{devices};
    if(devices!=INVALID_HANDLE_VALUE){
        std::set<std::wstring> seen;
        for(DWORD i=0;;++i){
            SP_DEVINFO_DATA device{};device.cbSize=sizeof(device);
            if(!SetupDiEnumDeviceInfo(devices,i,&device)){
                if(GetLastError()!=ERROR_NO_MORE_ITEMS)info.gpuDetectionFailed=true;
                break;
            }
            DWORD needed=0;SetupDiGetDeviceInstanceIdW(devices,&device,nullptr,0,&needed);
            std::vector<wchar_t> id(needed+1);
            if(!needed||!SetupDiGetDeviceInstanceIdW(devices,&device,id.data(),(DWORD)id.size(),nullptr)){info.gpuDetectionFailed=true;continue;}
            std::wstring key=id.data();std::transform(key.begin(),key.end(),key.begin(),[](wchar_t c){return (wchar_t)towupper(c);});
            if(!seen.insert(key).second)continue;
            auto property=[&](DWORD prop){
                DWORD length=0,type=0;SetupDiGetDeviceRegistryPropertyW(devices,&device,prop,&type,nullptr,0,&length);
                std::vector<wchar_t> data(length/sizeof(wchar_t)+1);
                if(!length||!SetupDiGetDeviceRegistryPropertyW(devices,&device,prop,&type,(PBYTE)data.data(),length,nullptr))return std::wstring();
                return std::wstring(data.data());
            };
            std::wstring displayName=property(SPDRP_FRIENDLYNAME);if(displayName.empty())displayName=property(SPDRP_DEVICEDESC);
            if(displayName.empty())displayName=L"未命名显示设备";
            // ROOT/SWD 等设备单列，不冒充实体显卡；同名不同实例保留。
            if(key.rfind(L"PCI\\",0)==0||key.rfind(L"USB\\",0)==0||key.rfind(L"ACPI\\",0)==0)info.gpus.push_back(displayName);
            else info.otherDisplays.push_back(displayName);
        }
    }else info.gpuDetectionFailed=true;
    info.displaysScanned=true;
    return info;
}
}
