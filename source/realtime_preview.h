// Direct2D 原始分辨率分块纹理缓存：仅图像变化或设备丢失时上传。
// 拖动与缩放只修改目标坐标，绘制时剔除不可见分块，不生成新的预览位图。
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <vector>
#include <cstdint>
#include <algorithm>
#include "preview_geometry.h"
#include "resource_leases.h"
#include <d3d11.h>

namespace Realtime {
template<class T> void release(T*& p){if(p){p->Release();p=nullptr;}}
struct ImageView {uint32_t w=0,h=0;const unsigned char* data=nullptr;uint64_t serial=0;};
struct Tile {ID2D1Bitmap* bitmap=nullptr;ID2D1BitmapBrush* brush=nullptr;uint32_t x=0,y=0,w=0,h=0,offsetX=0,offsetY=0;};
struct ImageCache {
    uint64_t serial=0;uint32_t w=0,h=0;std::vector<Tile> tiles;
    void clear(){for(auto& t:tiles){release(t.brush);release(t.bitmap);}tiles.clear();serial=0;w=h=0;}
};
class Renderer {
    ID2D1Factory* factory=nullptr;
    IDWriteFactory* writeFactory=nullptr;
    IDWriteTextFormat *labelFormat=nullptr,*dividerFormat=nullptr;
    enum class TextStyle { Label,Divider };
    ID2D1RenderTarget* target=nullptr;
    ID3D11Device* graphicsDevice=nullptr;
    IDXGISwapChain* swapchain=nullptr;
    Batch::AppBudget* memoryBudget=nullptr;
    Batch::GpuBudget* gpuBudget=nullptr;
    Batch::Device adapterDevice;
    Batch::MemoryLease surfaceMemory,imageMemory;
    Batch::GpuLease surfaceLease,imageLease;
    ULONGLONG hardwareRetry=0;
    bool deferred=false,hardwareUnavailable=false;
    ID2D1SolidColorBrush* brush=nullptr;
    ID2D1BitmapBrush* checker=nullptr;
    ID2D1RoundedRectangleGeometry* roundedClip=nullptr;
    ID2D1Layer* clipLayer=nullptr;
    ImageCache original,result;
    UINT canvasW=0,canvasH=0;
    float scaleDpi=1;
    bool accelerated=false;
    static D2D1_COLOR_F color(unsigned r,unsigned g,unsigned b,float a=1){return D2D1::ColorF(r/255.f,g/255.f,b/255.f,a);}
    void deviceClear(){original.clear();result.clear();imageLease.reset();imageMemory.reset();release(clipLayer);release(roundedClip);release(checker);release(brush);release(target);release(swapchain);release(graphicsDevice);surfaceLease.reset();surfaceMemory.reset();canvasW=canvasH=0;accelerated=false;}
    bool hardwareTarget(HWND hwnd,UINT w,UINT h){
        if(!gpuBudget||hardwareUnavailable)return false;
        IDXGIFactory1* dxgi=nullptr;IDXGIAdapter1* adapter=nullptr;
        struct Adapters {IDXGIFactory1*& factory;IDXGIAdapter1*& selected;~Adapters(){release(selected);release(factory);}} cleanup{dxgi,adapter};
        if(FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),reinterpret_cast<void**>(&dxgi))))return false;
        for(UINT i=0;;++i){IDXGIAdapter1* candidate=nullptr;if(dxgi->EnumAdapters1(i,&candidate)==DXGI_ERROR_NOT_FOUND)break;
            if(!candidate){break;}
            DXGI_ADAPTER_DESC1 desc{};candidate->GetDesc1(&desc);
            if(!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)){adapter=candidate;break;}candidate->Release();}
        if(!adapter){release(dxgi);return false;}
        DXGI_ADAPTER_DESC1 desc{};adapter->GetDesc1(&desc);
        adapterDevice.luid=desc.AdapterLuid;adapterDevice.luidValid=true;adapterDevice.vendor=desc.VendorId;
        adapterDevice.unified=desc.DedicatedVideoMemory==0;
        adapterDevice.capacity=adapterDevice.unified?desc.SharedSystemMemory:desc.DedicatedVideoMemory;adapterDevice.name=desc.Description;
        bool admitted=false;
        try{gpuBudget->registerDevice(adapterDevice);admitted=surfaceLease.acquire(*gpuBudget,adapterDevice,uint64_t(w)*h*16+64*Batch::MiB,GetCurrentProcessId());}
        catch(...){Batch::emergency("preview GPU admission unavailable");}
        if(!admitted){release(adapter);release(dxgi);deferred=true;return false;}
        HRESULT hr=D3D11CreateDevice(adapter,D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&graphicsDevice,nullptr,nullptr);
        if(SUCCEEDED(hr)){
            DXGI_SWAP_CHAIN_DESC chain{};chain.BufferDesc.Width=w;chain.BufferDesc.Height=h;chain.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
            chain.SampleDesc.Count=1;chain.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;chain.BufferCount=1;chain.OutputWindow=hwnd;chain.Windowed=TRUE;chain.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
            hr=dxgi->CreateSwapChain(graphicsDevice,&chain,&swapchain);
            if(SUCCEEDED(hr)){dxgi->MakeWindowAssociation(hwnd,DXGI_MWA_NO_ALT_ENTER);IDXGISurface* surface=nullptr;
                hr=swapchain->GetBuffer(0,__uuidof(IDXGISurface),reinterpret_cast<void**>(&surface));
                if(SUCCEEDED(hr))hr=factory->CreateDxgiSurfaceRenderTarget(surface,D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),96,96),&target);
                release(surface);}
        }
        release(adapter);release(dxgi);
        if(FAILED(hr)){release(target);release(swapchain);release(graphicsDevice);surfaceLease.reset();hardwareUnavailable=true;return false;}
        surfaceLease.commit();accelerated=true;return true;
    }
    void box(float x,float y,float w,float h,D2D1_COLOR_F c,float radius=0){
        brush->SetColor(c);auto r=D2D1::RectF(x,y,x+w,y+h);
        if(radius)target->FillRoundedRectangle(D2D1::RoundedRect(r,radius,radius),brush);else target->FillRectangle(r,brush);
    }
    void label(const wchar_t* s,float x,float y,float w,float h,TextStyle style,D2D1_COLOR_F c,bool centered=false){
        auto* f=style==TextStyle::Divider?dividerFormat:labelFormat;f->SetTextAlignment(centered?DWRITE_TEXT_ALIGNMENT_CENTER:DWRITE_TEXT_ALIGNMENT_LEADING);
        brush->SetColor(c);target->DrawText(s,(UINT32)wcslen(s),f,D2D1::RectF(x,y,x+w,y+h),brush,D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    bool ensure(HWND hwnd,float dpi){
        if(!factory&&FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED,&factory)))return false;
        if(writeFactory&&(!labelFormat||!dividerFormat)){release(labelFormat);release(dividerFormat);release(writeFactory);}
        if(!writeFactory){
            if(FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),(IUnknown**)&writeFactory)))return false;
            if(FAILED(writeFactory->CreateTextFormat(L"Microsoft YaHei UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,12,L"zh-cn",&labelFormat)))return false;
            if(FAILED(writeFactory->CreateTextFormat(L"Microsoft YaHei UI",nullptr,DWRITE_FONT_WEIGHT_BOLD,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,18,L"zh-cn",&dividerFormat)))return false;
            labelFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);dividerFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        RECT r;GetClientRect(hwnd,&r);UINT w=std::max(1L,r.right),h=std::max(1L,r.bottom);
        if(target&&canvasW&&(w!=canvasW||h!=canvasH)){deferred=true;return false;}
        if(!target){
            if(memoryBudget&&!surfaceMemory.acquire(*memoryBudget,uint64_t(w)*h*20+64*Batch::MiB)){deferred=true;return false;}
            auto props=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_HARDWARE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),96,96);
            auto windowProps=D2D1::HwndRenderTargetProperties(hwnd,D2D1::SizeU(w,h),D2D1_PRESENT_OPTIONS_IMMEDIATELY);
            HRESULT created=E_FAIL;
            if(hardwareTarget(hwnd,w,h))created=S_OK;
            else {props.type=D2D1_RENDER_TARGET_TYPE_SOFTWARE;ID2D1HwndRenderTarget* software=nullptr;created=factory->CreateHwndRenderTarget(props,windowProps,&software);target=software;hardwareRetry=GetTickCount64()+1000;}
            if(FAILED(created)){deviceClear();return false;}
            if(FAILED(target->CreateSolidColorBrush(color(28,40,57),&brush))){deviceClear();return false;}
            unsigned char pixels[24*24*4];
            for(int y=0;y<24;++y)for(int x=0;x<24;++x){bool dark=(x/12)==(y/12);auto* p=pixels+(y*24+x)*4;
                p[0]=dark?236:253;p[1]=dark?228:251;p[2]=dark?223:250;p[3]=255;}
            ID2D1Bitmap* tile=nullptr;
            HRESULT hr=target->CreateBitmap(D2D1::SizeU(24,24),pixels,24*4,D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96),&tile);
            if(SUCCEEDED(hr))hr=target->CreateBitmapBrush(tile,D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP,D2D1_EXTEND_MODE_WRAP,D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR),&checker);
            release(tile);if(FAILED(hr)||FAILED(target->CreateLayer(&clipLayer))){deviceClear();return false;}
        }
        if(w!=canvasW||h!=canvasH||dpi!=scaleDpi){
            release(roundedClip);float lw=w/dpi,lh=h/dpi;
            if(FAILED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(0,0,lw,lh),10,10),&roundedClip)))return false;
            canvasW=w;canvasH=h;scaleDpi=dpi;
        }
        return true;
    }
    bool upload(ImageCache& cache,ImageView image){
        if(!image.data){cache.clear();return true;}
        if(cache.serial==image.serial&&cache.w==image.w&&cache.h==image.h)return true;
        cache.clear();
        UINT maximum=target->GetMaximumBitmapSize();if(maximum<3)return false;
        UINT edge=std::min(2046U,maximum-2);
        cache.tiles.reserve(size_t((image.w+edge-1)/edge)*((image.h+edge-1)/edge));
        std::vector<unsigned char> staging;
        for(UINT y=0;y<image.h;y+=edge)for(UINT x=0;x<image.w;x+=edge){
            UINT w=std::min(edge,image.w-x),h=std::min(edge,image.h-y);
            auto region=Preview::tileRegion(x,y,w,h,image.w,image.h);
            UINT uploadW=region.uploadW,uploadH=region.uploadH;staging.resize(size_t(uploadW)*uploadH*4);
            // 每块额外保留相邻一圈像素，避免线性采样在分块边缘产生接缝。
            for(UINT row=0;row<uploadH;++row)Preview::premultiplyRow(image.data+(size_t(region.uploadY+row)*image.w+region.uploadX)*4,staging.data()+size_t(row)*uploadW*4,uploadW);
            Tile tile{};tile.x=x;tile.y=y;tile.w=w;tile.h=h;tile.offsetX=x-region.uploadX;tile.offsetY=y-region.uploadY;
            HRESULT hr=target->CreateBitmap(D2D1::SizeU(uploadW,uploadH),staging.data(),uploadW*4,D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96),&tile.bitmap);
            if(SUCCEEDED(hr))hr=target->CreateBitmapBrush(tile.bitmap,
                D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP,D2D1_EXTEND_MODE_CLAMP,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),&tile.brush);
            if(FAILED(hr)){release(tile.brush);release(tile.bitmap);cache.clear();return false;}
            cache.tiles.push_back(tile);
        }
        cache.serial=image.serial;cache.w=image.w;cache.h=image.h;return true;
    }
    void drawImage(const ImageCache& cache,Preview::Rect image,Preview::Rect clip){
        if(!cache.w||!cache.h)return;
        auto region=D2D1::RectF(clip.x,clip.y,clip.x+clip.w,clip.y+clip.h);
        target->PushAxisAlignedClip(region,D2D1_ANTIALIAS_MODE_ALIASED);
        float sx=image.w/cache.w,sy=image.h/cache.h;
        // 分块边界按像素中心归属，禁止相邻矩形分别做覆盖率抗锯齿。
        // 仅关闭矩形边缘抗锯齿；纹理仍使用完整分辨率和双线性采样。
        auto previousAntialias=target->GetAntialiasMode();
        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        for(const auto& t:cache.tiles){
            float left=image.x+t.x*sx,top=image.y+t.y*sy,right=image.x+(t.x+t.w)*sx,bottom=image.y+(t.y+t.h)*sy;
            if(!Preview::intersects({left,top,right-left,bottom-top},clip))continue;
            // 画刷采样整个含边缘像素的纹理，不再用 DrawBitmap 的源矩形裁掉边缘。
            // 相邻块共用相同图像坐标公式，半透明像素只合成一次，不靠重叠遮缝。
            t.brush->SetTransform(D2D1::Matrix3x2F(sx,0,0,sy,
                image.x+(t.x-t.offsetX)*sx,image.y+(t.y-t.offsetY)*sy));
            target->FillRectangle(D2D1::RectF(left,top,right,bottom),t.brush);
        }
        target->SetAntialiasMode(previousAntialias);
        target->PopAxisAlignedClip();
    }
    // 原图、处理后和叠图都使用同一个 image 几何区域。
    // 这里集中处理显示模式，避免两张图分别维护缩放、平移或裁剪逻辑。
    void drawPreviewImages(const ImageCache& originalCache,const ImageCache& resultCache,
                           Preview::Rect image,int mode,float split,float canvasWidth,float canvasHeight){
        if(!originalCache.w||!originalCache.h)return;
        if(!resultCache.w||!resultCache.h||mode==1){
            drawImage(originalCache,image,{0,0,canvasWidth,canvasHeight});
            return;
        }
        if(mode==2){
            drawImage(resultCache,image,{0,0,canvasWidth,canvasHeight});
            return;
        }
        float divider=canvasWidth*split;
        drawImage(resultCache,image,{0,0,divider,canvasHeight});
        drawImage(originalCache,image,{divider,0,canvasWidth-divider,canvasHeight});
        brush->SetColor(color(255,255,255));target->DrawLine(D2D1::Point2F(divider,0),D2D1::Point2F(divider,canvasHeight),brush,3);
        brush->SetColor(color(49,106,233));target->DrawLine(D2D1::Point2F(divider,0),D2D1::Point2F(divider,canvasHeight),brush,1);
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(divider,canvasHeight/2),17,17),brush);
        label(L"‹  ›",divider-17,canvasHeight/2-17,34,34,TextStyle::Divider,color(255,255,255),true);
    }
public:
    Renderer()=default;
    Renderer(const Renderer&)=delete;
    Renderer& operator=(const Renderer&)=delete;
    bool ready() const {return target&&brush&&checker&&clipLayer&&roundedClip;}
    bool matchesSurface(HWND hwnd,float dpi) const {
        if(!hwnd||!ready())return false;
        RECT bounds{};if(!GetClientRect(hwnd,&bounds))return false;
        return canvasW==(UINT)std::max(1L,bounds.right)&&canvasH==(UINT)std::max(1L,bounds.bottom)&&scaleDpi==dpi;
    }
    void setBudget(Batch::AppBudget* memory,Batch::GpuBudget* gpu){memoryBudget=memory;gpuBudget=gpu;}
    bool needsBackgroundSetup(HWND hwnd){
        RECT r{};GetClientRect(hwnd,&r);UINT w=std::max(1L,r.right),h=std::max(1L,r.bottom);
        if(target&&canvasW&&(w!=canvasW||h!=canvasH))return true;
        if(target&&!accelerated&&!hardwareUnavailable&&gpuBudget&&adapterDevice.luidValid&&GetTickCount64()>=hardwareRetry){
            hardwareRetry=GetTickCount64()+1000;
            return gpuBudget->previewPermit(adapterDevice)&&gpuBudget->canReserve(adapterDevice,uint64_t(w)*h*16+64*Batch::MiB);}
        return false;
    }
    bool retryHardware()const{return !accelerated&&!hardwareUnavailable&&gpuBudget;}
    bool hardware()const{return accelerated;}
    bool wasDeferred()const{return deferred;}
    bool permitFrame()const{return !accelerated||!gpuBudget||gpuBudget->previewPermit(adapterDevice);}
    bool prepare(HWND hwnd,float dpi){return ensure(hwnd,dpi);}
    void adopt(Renderer& other) noexcept {
        using std::swap;
        swap(factory,other.factory);swap(writeFactory,other.writeFactory);swap(labelFormat,other.labelFormat);swap(dividerFormat,other.dividerFormat);
        swap(graphicsDevice,other.graphicsDevice);swap(swapchain,other.swapchain);swap(memoryBudget,other.memoryBudget);swap(gpuBudget,other.gpuBudget);
        swap(adapterDevice,other.adapterDevice);swap(surfaceMemory,other.surfaceMemory);swap(imageMemory,other.imageMemory);swap(surfaceLease,other.surfaceLease);swap(imageLease,other.imageLease);swap(hardwareRetry,other.hardwareRetry);swap(deferred,other.deferred);swap(hardwareUnavailable,other.hardwareUnavailable);
        swap(target,other.target);swap(brush,other.brush);swap(checker,other.checker);swap(roundedClip,other.roundedClip);swap(clipLayer,other.clipLayer);
        swap(original,other.original);swap(result,other.result);swap(canvasW,other.canvasW);swap(canvasH,other.canvasH);swap(scaleDpi,other.scaleDpi);swap(accelerated,other.accelerated);
    }
    const wchar_t* backend() const{return !target?L"尚未初始化":accelerated?L"GPU 硬件加速（Direct2D）":L"CPU 软件渲染（硬件加速不可用）";}
    ~Renderer(){clear();}
    void clearImages(){original.clear();result.clear();imageLease.reset();imageMemory.reset();}
    void clear(){deviceClear();release(labelFormat);release(dividerFormat);release(writeFactory);release(factory);}
    bool render(HWND hwnd,float dpi,ImageView src,ImageView dst,Preview::Rect image,int mode,float split){
        deferred=false;
        if(!ensure(hwnd,dpi))return false;
        if(!permitFrame()){deferred=true;return false;}
        Batch::GpuLease nextImages;
        bool changed=original.serial!=src.serial||result.serial!=dst.serial||original.w!=src.w||result.w!=dst.w;
        if(accelerated&&changed){
            uint64_t bytes=(uint64_t(src.w)*src.h+uint64_t(dst.w)*dst.h)*5+32*Batch::MiB;
            if(!nextImages.acquire(*gpuBudget,adapterDevice,bytes,GetCurrentProcessId())){deferred=true;return false;}}
        Batch::MemoryLease staging,nextMemory;
        if(changed&&memoryBudget){
            if(!staging.acquire(*memoryBudget,20*Batch::MiB)||!nextMemory.acquire(*memoryBudget,(uint64_t(src.w)*src.h+uint64_t(dst.w)*dst.h)*8+32*Batch::MiB)){deferred=true;return false;}}
        bool ok=false;
        try{ok=upload(original,src)&&upload(result,dst);}catch(...){original.clear();result.clear();}
        if(changed&&ok){imageMemory=std::move(nextMemory);if(accelerated){nextImages.commit();imageLease=std::move(nextImages);}}
        target->BeginDraw();target->SetTransform(D2D1::Matrix3x2F::Scale(dpi,dpi));target->Clear(color(244,246,250));
        float w=canvasW/dpi,h=canvasH/dpi;
        target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),roundedClip),clipLayer);
        box(0,0,w,h,color(233,237,243));
        if(src.data&&ok){
            target->FillRectangle(D2D1::RectF(image.x,image.y,image.x+image.w,image.y+image.h),checker);
            drawPreviewImages(original,result,image,mode,split,w,h);
            bool showResult=dst.data&&mode!=1;box(14,14,showResult?68:112,25,color(24,34,51,.86f),6);
            label(showResult?L"处理后":L"原图 · 原始像素",14,14,showResult?68:112,25,TextStyle::Label,color(255,255,255),true);
            if(dst.data&&mode==0){box(w-82,14,68,25,color(49,106,233,.86f),6);label(L"原图",w-82,14,68,25,TextStyle::Label,color(255,255,255),true);}
        }else if(src.data)label(L"预览显存或内存不足，请尝试较小的图片；结果仍可保存。",24,h/2-20,w-48,40,TextStyle::Label,color(111,125,144),true);
        target->PopLayer();HRESULT hr=target->EndDraw();
        if(hr==static_cast<HRESULT>(D2DERR_RECREATE_TARGET)){deviceClear();InvalidateRect(hwnd,nullptr,FALSE);return true;}
        if(SUCCEEDED(hr)&&swapchain)hr=swapchain->Present(0,0);
        if(FAILED(hr)){deviceClear();return false;}return true;
    }
};
}
