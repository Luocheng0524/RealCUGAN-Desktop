// Native CPU/GPU worker. Original CLI remains valid; batch IPC is versioned.
#include "realcugan.h"
#include "resource_budget.h"
#include <cpu.h>
#include <io.h>
#include <fcntl.h>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>
namespace {
using File=std::unique_ptr<FILE,decltype(&fclose)>;
using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
int integer(const wchar_t* text,int low,int high) {
    std::wstring value=text;size_t used=0;int result=std::stoi(value,&used);
    if(used!=value.size()||result<low||result>high)throw std::runtime_error("Invalid numeric argument");
    return result;
}
bool validModel(int scale,int noise){return (scale==2&&(noise==-1||noise==0||noise==3))||(scale==3&&noise==3);}
int threads(int requested) {
    int value=std::clamp(requested,1,std::max(1,ncnn::get_cpu_count()));DWORD_PTR allowed=0,system=0;
    if(GetProcessAffinityMask(GetCurrentProcess(),&allowed,&system)&&allowed){int count=0;while(allowed){allowed&=allowed-1;++count;}value=std::min(value,std::max(1,count));}
    return value;
}
struct ImageTimings {double decode=0,inference=0,handoff=0;};
ImageTimings processImage(RealCUGAN& engine,const std::wstring& inputPath,const std::wstring& outputPath,int scale,uint32_t expectedW=0,uint32_t expectedH=0) {
    ImageTimings timings;auto begin=Clock::now();
    using Batch::PixelHeader;
    File input(_wfopen(inputPath.c_str(),L"rb"),fclose);PixelHeader header{};
    if(!input||fread(&header,sizeof(header),1,input.get())!=1||memcmp(header.magic,"RCG1",4)||!header.width||!header.height||
       uint64_t(header.width)*header.height>64000000u/(scale*scale)||(expectedW&&header.width!=expectedW)||(expectedH&&header.height!=expectedH))throw std::runtime_error("Invalid input image or dimensions");
    size_t count=size_t(header.width)*header.height;std::vector<unsigned char> bgra(count*4),rgb(count*3);
    if(fread(bgra.data(),4,count,input.get())!=count||fgetc(input.get())!=EOF)throw std::runtime_error("Incorrect input pixel length");
    input.reset();
    for(size_t i=0;i<count;++i){rgb[i*3]=bgra[i*4+2];rgb[i*3+1]=bgra[i*4+1];rgb[i*3+2]=bgra[i*4];}
    ncnn::Mat src(header.width,header.height,rgb.data(),(size_t)3,3),dst(header.width*scale,header.height*scale,(size_t)3,3);
    if(dst.empty())throw std::bad_alloc();
    timings.decode=elapsed(begin);begin=Clock::now();engine.process(src,dst);timings.inference=elapsed(begin);begin=Clock::now();
    src.release();std::vector<unsigned char>().swap(rgb);
    PixelHeader out{{'R','C','G','1'},header.width*(uint32_t)scale,header.height*(uint32_t)scale};
    auto part=outputPath+L".part";
    try {
        File output(_wfopen(part.c_str(),L"wb"),fclose);
        if(!output||fwrite(&out,sizeof(out),1,output.get())!=1)throw std::runtime_error("Cannot write output header");
        std::vector<unsigned char> row(out.width*4);const auto* pixels=static_cast<const unsigned char*>(dst.data);
        // Same RGB order and bilinear alpha reconstruction as the baseline.
        for(uint32_t y=0;y<out.height;++y){
            double sy=std::max(0.0,std::min(double(header.height-1),(y+.5)/scale-.5));
            uint32_t y0=(uint32_t)sy,y1=std::min(y0+1,header.height-1);double fy=sy-y0;
            for(uint32_t x=0;x<out.width;++x){
                double sx=std::max(0.0,std::min(double(header.width-1),(x+.5)/scale-.5));
                uint32_t x0=(uint32_t)sx,x1=std::min(x0+1,header.width-1);double fx=sx-x0;
                auto alpha=[&](uint32_t xx,uint32_t yy){return bgra[(size_t(yy)*header.width+xx)*4+3];};
                double a0=alpha(x0,y0)*(1-fx)+alpha(x1,y0)*fx,a1=alpha(x0,y1)*(1-fx)+alpha(x1,y1)*fx;
                size_t p=(size_t(y)*out.width+x)*3;
                row[x*4]=pixels[p+2];row[x*4+1]=pixels[p+1];row[x*4+2]=pixels[p];row[x*4+3]=(unsigned char)(a0*(1-fy)+a1*fy+.5);
            }
            if(fwrite(row.data(),4,out.width,output.get())!=out.width)throw std::runtime_error("Output write failed");
        }
        if(fflush(output.get())||_commit(_fileno(output.get())))throw std::runtime_error("Output flush failed");
        FILE* closing=output.release();if(fclose(closing))throw std::runtime_error("Output close failed");
        if(!MoveFileExW(part.c_str(),outputPath.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Output commit failed");
    }catch(...){DeleteFileW(part.c_str());throw;}
    timings.handoff=elapsed(begin);return timings;
}
int probe() {
#if NCNN_VULKAN
    if(ncnn::create_gpu_instance()!=0)throw std::runtime_error("Vulkan instance unavailable");
    struct Cleanup {~Cleanup(){ncnn::destroy_gpu_instance();}} cleanup;
    for(int i=0;i<ncnn::get_gpu_count();++i){
        const auto& gpu=ncnn::get_gpu_info(i);if(gpu.type()==3)continue;
        const auto& memory=gpu.physical_device_memory_properties();uint64_t capacity=0;
        for(uint32_t j=0;j<memory.memoryHeapCount;++j)if(memory.memoryHeaps[j].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)capacity+=memory.memoryHeaps[j].size;
        VkPhysicalDeviceIDProperties id{};id.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 properties{};properties.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;properties.pNext=&id;
        if(ncnn::vkGetPhysicalDeviceProperties2KHR)ncnn::vkGetPhysicalDeviceProperties2KHR(gpu.physical_device(),&properties);
        uint32_t low=0;int32_t high=0;memcpy(&low,id.deviceLUID,4);memcpy(&high,id.deviceLUID+4,4);
        std::string name=gpu.device_name();std::replace(name.begin(),name.end(),'\t',' ');
        char uuid[48];const auto* u=id.deviceUUID;
        snprintf(uuid,sizeof(uuid),"GPU-%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",u[0],u[1],u[2],u[3],u[4],u[5],u[6],u[7],u[8],u[9],u[10],u[11],u[12],u[13],u[14],u[15]);
        printf("GPU\t%d\t%llu\t%u\t%d\t%d\t%s\t%u\t%u\t%d\t%s\n",i,(unsigned long long)capacity,low,high,id.deviceLUIDValid?1:0,name.c_str(),gpu.driver_version(),gpu.vendor_id(),gpu.type(),uuid);
    }
    puts("PROBE_DONE");fflush(stdout);return 0;
#else
    throw std::runtime_error("Worker built without Vulkan");
#endif
}
int serve(int argc,wchar_t** argv) {
    if(argc!=9)throw std::runtime_error("Server requires a stable GPU identity");
    int scale=integer(argv[3],2,3),noise=integer(argv[4],-1,3),count=integer(argv[5],1,std::max(1,ncnn::get_cpu_count())),gpu=integer(argv[6],-1,255);
    if(!validModel(scale,noise))throw std::runtime_error("Unsupported model mapping");
    _setmode(_fileno(stdin),_O_BINARY);Batch::GpuGate gate;
#if NCNN_VULKAN
    bool initialized=false;
    struct Cleanup {bool& initialized;~Cleanup(){if(initialized)ncnn::destroy_gpu_instance();}} cleanup{initialized};
    if(gpu>=0){if(ncnn::create_gpu_instance()!=0)throw std::runtime_error("Vulkan instance unavailable");initialized=true;gate.open(argv[7],false);}
    if(gpu>=0){
        if(gpu>=ncnn::get_gpu_count())throw std::runtime_error("Selected GPU index no longer exists");
        const auto& info=ncnn::get_gpu_info(gpu);
        VkPhysicalDeviceIDProperties id{};id.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 properties{};properties.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;properties.pNext=&id;
        if(!ncnn::vkGetPhysicalDeviceProperties2KHR)throw std::runtime_error("Stable GPU identity query unavailable");
        ncnn::vkGetPhysicalDeviceProperties2KHR(info.physical_device(),&properties);
        uint32_t low=0;int32_t high=0;memcpy(&low,id.deviceLUID,4);memcpy(&high,id.deviceLUID+4,4);
        char uuid[48];const auto* u=id.deviceUUID;
        snprintf(uuid,sizeof(uuid),"GPU-%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",u[0],u[1],u[2],u[3],u[4],u[5],u[6],u[7],u[8],u[9],u[10],u[11],u[12],u[13],u[14],u[15]);
        std::wstring expected=argv[8];bool matched=expected.rfind(L"GPU-",0)==0?expected==Batch::wide(uuid):id.deviceLUIDValid&&expected==std::to_wstring(high)+L":"+std::to_wstring(low);
        if(!matched)throw std::runtime_error("Selected GPU identity changed; cancel and select the device manually");
        printf("IDENTITY_VERIFIED index=%d uuid=%s luid=%d:%u\n",gpu,uuid,high,low);fflush(stdout);
    }
#else
    if(gpu>=0)throw std::runtime_error("Worker built without Vulkan");
#endif
    if(gpu>=0){puts("GPU_POLICY 1 duty_max=95");fflush(stdout);}
    auto begin=Clock::now();RealCUGAN engine(threads(count),gpu);engine.model_directory=argv[2];
    printf("DEVICE_READY %.3f\n",elapsed(begin));fflush(stdout);
    if(gpu>=0)gate.begin();
    begin=Clock::now();
    try{engine.load(scale,noise);}catch(...){if(gpu>=0)gate.end();throw;}
    if(gpu>=0)gate.end();
    printf("READY %.3f\n",elapsed(begin));fflush(stdout);
    bool firstTile=true;Clock::time_point tileStart;double budgetWait=0;
    engine.on_tile_begin=[&](){auto wait=Clock::now();if(gpu>=0)gate.begin();budgetWait+=elapsed(wait);tileStart=Clock::now();};
    engine.on_tile_end=[&](){if(gpu>=0)gate.end();if(firstTile){firstTile=false;printf("WARMUP %.3f\n",elapsed(tileStart));fflush(stdout);}};
    for(;;){
        Batch::Request request{};size_t n=fread(&request,1,sizeof(request),stdin);if(!n&&feof(stdin))return 0;
        if(n!=sizeof(request)||request.magic!=0x32544352||request.version!=2||!request.batch||!request.task||
           !request.inputChars||!request.outputChars||request.inputChars>32767||request.outputChars>32767)throw std::runtime_error("Invalid server request");
        std::wstring input(request.inputChars,L'\0'),output(request.outputChars,L'\0');
        if(fread(input.data(),sizeof(wchar_t),input.size(),stdin)!=input.size()||fread(output.data(),sizeof(wchar_t),output.size(),stdin)!=output.size()||input.find(L'\0')!=input.npos||output.find(L'\0')!=output.npos)throw std::runtime_error("Truncated server request");
        auto batch=request.batch,task=request.task;
        engine.on_progress=[=](int percent,long remaining){printf("PROGRESS %llu %llu %d %ld\n",(unsigned long long)batch,(unsigned long long)task,percent,remaining);fflush(stdout);};
        try{printf("START %llu %llu\n",(unsigned long long)batch,(unsigned long long)task);fflush(stdout);begin=Clock::now();budgetWait=0;
            auto times=processImage(engine,input,output,scale,request.width,request.height);
            printf("TIMINGS %llu %llu decode_ms=%.3f infer_wall_ms=%.3f budget_wait_ms=%.3f handoff_ms=%.3f\n",(unsigned long long)batch,(unsigned long long)task,times.decode,times.inference,budgetWait,times.handoff);
            printf("DONE %llu %llu %.3f\n",(unsigned long long)batch,(unsigned long long)task,elapsed(begin));fflush(stdout);}
        catch(const std::exception& error){printf("FAILED %llu %llu %s\n",(unsigned long long)batch,(unsigned long long)task,error.what());fflush(stdout);throw;}
    }
}
int legacy(int argc,wchar_t** argv) {
    if(argc!=6&&argc!=7)throw std::runtime_error("Invalid arguments");
    int scale=integer(argv[4],2,3),noise=integer(argv[5],-1,3),count=argc==7?integer(argv[6],1,std::max(1,ncnn::get_cpu_count())):3;
    if(!validModel(scale,noise))throw std::runtime_error("Unsupported model mapping");
    RealCUGAN engine(threads(count));engine.model_directory=argv[3];engine.load(scale,noise);
    engine.on_progress=[](int percent,long remaining){printf("PROGRESS %d %ld\n",percent,remaining);fflush(stdout);};
    auto times=processImage(engine,argv[1],argv[2],scale);printf("TIMINGS decode_ms=%.3f infer_wall_ms=%.3f handoff_ms=%.3f\n",times.decode,times.inference,times.handoff);puts("DONE");return 0;
}
}
int wmain(int argc,wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    try{Batch::setEmergencyDirectory(Batch::runtimeDirectory(L"logs"));Batch::processDiagnostic=Batch::emergency;
        if(argc==2&&wcscmp(argv[1],L"--probe")==0)return probe();if(argc>1&&wcscmp(argv[1],L"--serve")==0)return serve(argc,argv);return legacy(argc,argv);}
    catch(const std::bad_alloc&){Batch::emergency("Not enough memory");fputs("ERROR Not enough memory\n",stderr);return 4;}
    catch(const std::exception& error){Batch::emergency(error.what());fprintf(stderr,"ERROR %s\n",error.what());return 2;}
    catch(...){Batch::emergency("Unknown processing failure");fputs("ERROR Unknown processing failure\n",stderr);return 5;}
}
