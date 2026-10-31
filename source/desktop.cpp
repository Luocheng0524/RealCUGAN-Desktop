#include <map>
// 原生 Win32/GDI+ 窗口，无浏览器与本地服务器。
// 主窗口负责图片读写、预览和任务控制；CPU/GPU 推理由独立子进程完成。
#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <objidl.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include "version.h"
#include "preview_geometry.h"
#include "realtime_preview.h"
#include "hardware_info.h"
#include "batch_reorder.h"
#include "batch_card_layout.h"
#include "ui_layout.h"
#include "batch_view_io.h"
using namespace Gdiplus;

namespace {
constexpr UINT WM_PROGRESS = WM_APP + 1, WM_FINISHED = WM_APP + 2,
               WM_HARDWARE_READY = WM_APP + 5,WM_PREVIEW_READY=WM_APP+6,WM_REFRESH_PERMISSIONS=WM_APP+7,WM_BATCH_READY=WM_APP+8;
constexpr uint64_t MAX_PIXELS = 64000000;
Batch::AppBudget appBudget;
Batch::GpuBudget gpuBudget(appBudget);
Batch::MemoryLease uiMemory;
constexpr size_t MAX_BATCH_IMAGES = 20;
constexpr uint32_t MAX_BATCH_SIDE = 5000;
enum Control { OPEN=101, RUN, SAVE, CANCEL, SCALE2, SCALE3, CONSERVATIVE, CLEAN, DENOISE,
               OVERLAY, ORIGINAL, RESULT, FIT, ACTUAL, ABOUT, ZOOM200, HARDWARE, CLEAR_IMAGE, LOCK_LEFT,
               SETTINGS, PAGE_SWITCH, BATCH_RUN, BATCH_EXPORT,
               BATCH_PREVIEW, BATCH_CLEAR, BATCH_CANCEL, BATCH_PREV, BATCH_NEXT, BATCH_BACK };
enum class Page { Single, Batch };
struct Pixels {
    std::shared_ptr<Batch::MemoryLease> memory;
    uint32_t w=0, h=0;
    uint64_t serial=0;
    std::vector<unsigned char> data;
    std::unique_ptr<Bitmap> bitmap;
    void build() { static uint64_t nextSerial=0;serial=++nextSerial;bitmap.reset(new Bitmap(w,h,w*4,PixelFormat32bppARGB,data.data())); }
};
struct Outcome { std::shared_ptr<Batch::MemoryLease> memory;std::unique_ptr<Pixels> pixels; std::wstring error; double seconds=0; };
struct Button { int id=0; HWND hwnd=nullptr; RectF box; std::wstring label; bool primary=false; };
using BatchItemState=Batch::Stage;
struct BatchItem {
    std::wstring path, name;
    uint32_t w=0,h=0;
    bool selected=true;
    std::shared_ptr<Batch::MemoryLease> thumbnailMemory;
    std::shared_ptr<Bitmap> thumbnail;
    BatchItemState state=BatchItemState::Pending;
    int progress=0;
    std::wstring error;
    std::wstring cache;
    uint64_t taskId=0;
    uint32_t resultW=0,resultH=0;
};
HWND window=nullptr;
HINSTANCE instance=nullptr;
HICON appIcon=nullptr;
std::unique_ptr<Bitmap> appLogo;
std::unique_ptr<Bitmap> loadEmbeddedLogo() {
    HRSRC resource=FindResourceW(instance,MAKEINTRESOURCEW(2),RT_RCDATA);
    if(!resource)return nullptr;
    HGLOBAL loaded=LoadResource(instance,resource);
    DWORD size=SizeofResource(instance,resource);
    void* bytes=loaded?LockResource(loaded):nullptr;
    if(!bytes||!size)return nullptr;
    HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,size);
    if(!memory)return nullptr;
    void* destination=GlobalLock(memory);
    if(!destination){GlobalFree(memory);return nullptr;}
    memcpy(destination,bytes,size);GlobalUnlock(memory);
    IStream* rawStream=nullptr;
    if(CreateStreamOnHGlobal(memory,TRUE,&rawStream)!=S_OK){GlobalFree(memory);return nullptr;}
    auto releaseStream=[](IStream* value){if(value)value->Release();};
    std::unique_ptr<IStream,decltype(releaseStream)> stream(rawStream,releaseStream);
    std::unique_ptr<Bitmap> decoded(Bitmap::FromStream(stream.get()));
    std::unique_ptr<Bitmap> result;
    if(decoded&&decoded->GetLastStatus()==Ok) {
        result.reset(decoded->Clone(0,0,(INT)decoded->GetWidth(),(INT)decoded->GetHeight(),PixelFormat32bppARGB));
    }
    return result;
}
ULONG_PTR gdiplusToken=0;
float dpi=1.f, clientW=1280, clientH=800;
template<class Function> Function userApi(const char* name) {
    FARPROC address=GetProcAddress(GetModuleHandleW(L"user32.dll"),name);
    Function function=nullptr;static_assert(sizeof(function)==sizeof(address));
    memcpy(&function,&address,sizeof(function));return function;
}
void adjustFrame(RECT& rect,DWORD style,DWORD exStyle,UINT nativeDpi) {
    using Adjust=BOOL(WINAPI*)(LPRECT,DWORD,BOOL,DWORD,UINT);
    auto adjust=userApi<Adjust>("AdjustWindowRectExForDpi");
    if(adjust)adjust(&rect,style,FALSE,exStyle,nativeDpi);
    else AdjustWindowRectEx(&rect,style,FALSE,exStyle);
}
RECT monitorWorkRect(const RECT* suggested=nullptr) {
    MONITORINFO info{};info.cbSize=sizeof(info);
    HMONITOR monitor=suggested?MonitorFromRect(suggested,MONITOR_DEFAULTTONEAREST):MonitorFromWindow(window,MONITOR_DEFAULTTOPRIMARY);
    if(GetMonitorInfoW(monitor,&info))return info.rcWork;
    RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);return work;
}
float fittedDpi(UINT nativeDpi,RECT work,float logicalW=1300,float logicalH=850) {
    return std::max(.6f,std::min({nativeDpi/96.f,(work.right-work.left-24)/logicalW,(work.bottom-work.top-24)/logicalH}));
}
UINT nativeWindowDpi(HWND hwnd) {
    using GetDpi=UINT(WINAPI*)(HWND);
    auto getDpi=userApi<GetDpi>("GetDpiForWindow");
    if(getDpi&&hwnd){UINT value=getDpi(hwnd);if(value)return value;}
    HDC dc=GetDC(nullptr);UINT value=(UINT)GetDeviceCaps(dc,LOGPIXELSX);ReleaseDC(nullptr,dc);return value;
}
std::vector<Button> buttons;
std::unique_ptr<Pixels> source, output;
std::wstring sourcePath, sourceName, directory, settingsDir, settingsPath, logsDir;
Page page=Page::Single;
std::vector<BatchItem> batchItems;
BatchReorder::Gesture batchDrag;
constexpr UINT_PTR BATCH_HOLD_TIMER=21,BATCH_REFLOW_TIMER=22;
constexpr UINT BATCH_HOLD_MS=60;
ULONGLONG batchPressedAt=0;
std::vector<BatchReorder::Motion> batchCardMotions;
std::vector<std::unique_ptr<Bitmap>> batchCardCache;
struct BatchCardCacheKey {
    std::wstring path,name;
    uint32_t w=0,h=0;
    const Bitmap* thumbnail=nullptr;
    BatchItemState state=BatchItemState::Pending;
    bool selected=false;
    float width=0,height=0,density=0;
    bool matches(const BatchItem& item,float cardWidth,float cardHeight) const {
        return path==item.path&&name==item.name&&w==item.w&&h==item.h&&thumbnail==item.thumbnail.get()&&
               state==item.state&&selected==item.selected&&width==cardWidth&&height==cardHeight&&density==dpi;
    }
};
std::vector<BatchCardCacheKey> batchCardCacheKeys;
bool batchPreviewMode=false;
size_t batchPreviewIndex=0;
std::unique_ptr<Pixels> batchPreviewSource,batchPreviewOutput;
std::wstring status=L"准备就绪 · 所有图片均在本机处理", lastSaved;
int scale=2, noise=-1, mode=OVERLAY, resultScale=2, resultNoise=-1, progress=0;
long remainingMs=0;
bool lockLeft=false,inSizeMove=false,resizingWindow=false,previewResizePending=false;
bool batchRefreshDeferred=false,singleFinishDeferred=false,hardwareReadyDeferred=false;
bool previewDisplaySuspended(){return resizingWindow||previewResizePending||IsIconic(window);}
ULONGLONG previewResizeReadyAt=0;
bool busy=false, closing=false, draggingSplit=false, draggingPan=false, fitView=true;
Hardware::Info hardware;
std::thread hardwareScan;
std::atomic<bool> hardwareDone{true};
std::unique_ptr<Hardware::Info> hardwarePending;
bool hardwareScanBusy=false;
HWND hardwareDialog=nullptr,tooltipWindow=nullptr;
std::wstring tooltipText;
int requestedThreads=0,resultThreads=1;
int effectiveThreads(){return Hardware::chooseThreads(hardware.available,hardware.availablePhysical,requestedThreads);}
int wheelRemainder=0;
Preview::SmoothZoom smoothZoom;
HANDLE frameTimer=nullptr;
bool frameArmed=false;
double zoomClock=0,nextFrameTime=0,frameInterval=1.0/60;
float zoomAnchorX=0,zoomAnchorY=0,zoomFractionX=0,zoomFractionY=0;
double monotonicSeconds(){static const double frequency=[](){LARGE_INTEGER v;QueryPerformanceFrequency(&v);return double(v.QuadPart);}();
    LARGE_INTEGER v;QueryPerformanceCounter(&v);return v.QuadPart/frequency;}
LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* exception) {
    unsigned code=exception&&exception->ExceptionRecord?exception->ExceptionRecord->ExceptionCode:0;
    Batch::emergency("Unhandled application exception",code);
    if(Batch::emergencyWriteFailed.load())MessageBoxW(nullptr,L"故障日志写入失败，请检查程序目录的 logs 权限和磁盘空间。",L"Real-CUGAN",MB_OK|MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}
void stopSmoothZoom(){smoothZoom.active=false;smoothZoom.velocity=0;frameArmed=false;if(frameTimer)CancelWaitableTimer(frameTimer);}

HWND previewWindow=nullptr;
Realtime::Renderer realtimePreview;
std::thread previewSetup;
std::unique_ptr<Realtime::Renderer> preparedPreview;
std::atomic<bool> previewSetupDone{true};
bool previewSetupBusy=false,previewSetupFailed=false;
UINT previewSetupWidth=0,previewSetupHeight=0;
float previewSetupDpi=1;
std::atomic<bool> preparedPreviewDeferred{false};
bool previewBudgetFallback=false;
void joinPreviewSetup() {
    if(!previewSetup.joinable())return;
    // Service synchronous window messages while a driver creates the HWND
    // target. Do not dispatch queued user commands during shutdown.
    while(!previewSetupDone.load(std::memory_order_acquire)) {
        MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_SENDMESSAGE,MWMO_INPUTAVAILABLE);
        MSG message{};PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE|PM_QS_SENDMESSAGE);
    }
    previewSetup.join();
}
void requestPreviewSetup(HWND target) {
    if(inSizeMove||previewDisplaySuspended()||previewSetupBusy||previewSetupFailed||realtimePreview.ready()||closing)return;
    joinPreviewSetup();previewSetupBusy=true;previewSetupDone.store(false,std::memory_order_release);
    RECT bounds{};GetClientRect(target,&bounds);
    previewSetupWidth=std::max(1L,bounds.right);previewSetupHeight=std::max(1L,bounds.bottom);previewSetupDpi=dpi;
    preparedPreviewDeferred=false;
    try {
        float density=dpi;HWND owner=window;
        previewSetup=std::thread([target,owner,density](){
            HRESULT apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
            try {
                auto renderer=std::make_unique<Realtime::Renderer>();renderer->setBudget(&appBudget,&gpuBudget);
                if(renderer->prepare(target,density)){preparedPreview=std::move(renderer);preparedPreviewDeferred=false;}
                else {preparedPreviewDeferred=renderer->wasDeferred();preparedPreview.reset();}
            }catch(...){preparedPreview.reset();}
            if(SUCCEEDED(apartment))CoUninitialize();
            previewSetupDone.store(true,std::memory_order_release);PostMessageW(owner,WM_PREVIEW_READY,0,0);
        });
    } catch(...) {previewSetupBusy=false;previewSetupFailed=true;previewSetupDone.store(true,std::memory_order_release);}
}
float splitGrabOffset=0;
float split=.5f, zoom=1, panX=0,panY=0, mouseX=0,mouseY=0;
HANDLE cancelEvent=nullptr;
std::thread task;
std::atomic<bool> taskDone{true},singleEmergency{false};
std::shared_ptr<Batch::MemoryLease> singleWorkMemory;
unsigned modalDepth=0;
struct ModalGuard {ModalGuard(){++modalDepth;}~ModalGuard(){--modalDepth;if(window)PostMessageW(window,WM_REFRESH_PERMISSIONS,0,0);}};
std::unique_ptr<Outcome> completed;
Batch::Session batchSession;
Batch::UiNotification batchNotification(WM_BATCH_READY);
Batch::Engine batchEngine(appBudget,gpuBudget,&batchNotification);
Batch::PreviewLoader batchPreviewLoader(appBudget,&batchNotification);
std::unique_ptr<Batch::PreviewResult> batchPreviewLease;
Batch::Exporter batchExporter(&appBudget,&batchNotification);
Batch::Phase batchPhase=Batch::Phase::Stopped;
bool batchExportBusy=false;
bool batchLocked(){return batchPhase!=Batch::Phase::Stopped||batchExportBusy;}
uint64_t batchGeneration=0,previewGeneration=0;
constexpr UINT_PTR BATCH_EVENT_TIMER=23,CLOSE_POLL_TIMER=24,PREVIEW_RETRY_TIMER=25,PREVIEW_RESIZE_TIMER=26;
bool batchStopPending=false,batchStopCancelled=false;
std::vector<Batch::Device> computeDevices,pendingComputeDevices;
std::string computeDiagnostics,pendingComputeDiagnostics;
std::wstring requestedGpuKey;
int requestedConcurrency=1;
Batch::Device batchDisplayDevice;
int batchDisplayConcurrency=1;
std::vector<std::wstring> retiredCaches;
void releaseBatchPreviewBuffers(){batchPreviewSource.reset();batchPreviewOutput.reset();batchPreviewLease.reset();realtimePreview.clearImages();}
void retireCache(const std::wstring& path) {
    auto prefix=batchSession.root()+L"\\";
    if(path.empty()||batchSession.root().empty()||path.compare(0,prefix.size(),prefix)!=0)return;
    if(!DeleteFileW(path.c_str())&&GetLastError()!=ERROR_FILE_NOT_FOUND)retiredCaches.push_back(path);
}
size_t batchCurrent=0,batchTotalSelected=0;
int batchOverallProgress=0;
void pollBatchEvents();
bool canExecute(int id);
std::wstring hardwareSummary();
int batchScale=2,batchThreads=1;
double batchActualSeconds=0;
std::chrono::steady_clock::time_point batchStarted=std::chrono::steady_clock::now();
auto started=std::chrono::steady_clock::now();
Color ink(255,28,40,57), muted(255,111,125,144), blue(255,49,106,233);
constexpr float FONT_TITLE=22, FONT_SECTION=14, FONT_BODY=12, FONT_SECONDARY=10, FONT_BUTTON=12, FONT_BUTTON_PRIMARY=14;
struct PaintEnd {
    HWND hwnd;PAINTSTRUCT& paint;bool active=true;
    PaintEnd(HWND owner,PAINTSTRUCT& value):hwnd(owner),paint(value){}
    ~PaintEnd(){finish();}
    void finish()noexcept{if(active){EndPaint(hwnd,&paint);active=false;}}
};
struct BackBuffer {
    Batch::MemoryLease memory;
    ~BackBuffer(){clear();}
    HDC dc=nullptr;HBITMAP bitmap=nullptr;HGDIOBJ original=nullptr;int w=0,h=0;
    void clear() {
        if(dc&&original)SelectObject(dc,original);
        if(bitmap){DeleteObject(bitmap);}
        if(dc){DeleteDC(dc);}
        dc=nullptr;bitmap=nullptr;original=nullptr;w=h=0;memory.reset();
    }
    bool ensure(HDC target,int width,int height,bool reserve=false) {
        if(width<=0||height<=0)return false;
        if(dc&&((reserve&&w>=width&&h>=height)||(w==width&&h==height)))return true;
        // 主窗口按 128 像素扩容，缩小时复用；不逐像素销毁和创建大位图。
        if(reserve){width=(std::max(w,width)+127)/128*128;height=(std::max(h,height)+127)/128*128;}
        BackBuffer next;
        if(width>32768||height>32768||!next.memory.acquire(appBudget,uint64_t(width)*height*4+16*Batch::MiB))return false;
        next.dc=CreateCompatibleDC(target);next.bitmap=CreateCompatibleBitmap(target,width,height);
        if(!next.dc||!next.bitmap)return false;
        next.original=SelectObject(next.dc,next.bitmap);
        if(!next.original||next.original==HGDI_ERROR){next.original=nullptr;return false;}
        next.w=width;next.h=height;
        using std::swap;swap(dc,next.dc);swap(bitmap,next.bitmap);swap(original,next.original);swap(w,next.w);swap(h,next.h);swap(memory,next.memory);
        return true;
    }
} backBuffer;
std::map<int,std::unique_ptr<BackBuffer>> buttonBuffers;
HDWP layoutBatch=nullptr;
struct Placement {HWND child;int x,y,w,h;};
std::vector<Placement> layoutPlacements;
bool layoutBatchFailed=false;
bool layoutReady=false;
Ui::Sidebar sidebar;
std::map<std::pair<int,bool>,std::unique_ptr<Font>> uiFonts;
std::unique_ptr<Bitmap> measureBitmap;
std::map<std::tuple<std::wstring,int,int,int,bool>,float> measuredHeights;
struct ImportSurface {float width=0,density=0;std::unique_ptr<Bitmap> bitmap;};
ImportSurface importSurfaces[2];
bool imageViewVisible() {return page==Page::Single?source!=nullptr:batchPreviewMode&&batchPreviewSource!=nullptr;}
void layout();

std::wstring basename(const std::wstring& s) { size_t p=s.find_last_of(L"/\\"); return s.substr(p==s.npos?0:p+1); }
std::wstring stem(std::wstring s) { s=basename(s); auto p=s.find_last_of(L'.'); if(p!=s.npos)s.resize(p); return s; }
std::wstring number(int n) { return std::to_wstring(n); }
void repaint() { InvalidateRect(window,nullptr,FALSE); }
RectF viewport() { return RectF(312,164,std::max(1.f,clientW-336),std::max(1.f,clientH-242)); }
RectF batchViewport() { return RectF(312,112,std::max(1.f,clientW-336),std::max(1.f,clientH-190)); }
constexpr float BATCH_CARD_GAP_X=BatchCards::GapX, BATCH_CARD_GAP_Y=BatchCards::GapY;
RectF batchCardRect(size_t index) {
    RectF v=batchViewport();
    auto grid=BatchCards::grid(v.X,v.Y,v.Width,v.Height);
    return RectF(grid.left(index),grid.top(index),grid.width,grid.height);
}
// 继续添加卡片紧跟在当前最后一张图片之后，删除或新增图片后自动重新定位。
RectF batchContinueRect() { return batchCardRect(batchItems.size()); }
void repaintRegion(RectF r) {
    RECT physical{(LONG)floor(r.X*dpi),(LONG)floor(r.Y*dpi),(LONG)ceil(r.GetRight()*dpi),(LONG)ceil(r.GetBottom()*dpi)};
    InvalidateRect(window,&physical,FALSE);
}
void repaintPreview() {if(previewWindow)InvalidateRect(previewWindow,nullptr,FALSE);repaintRegion(RectF(312,clientH-68,std::max(1.f,clientW-336),24));}
void deferPreviewResize() {
    previewResizePending=true;
    previewResizeReadyAt=GetTickCount64()+120;
    KillTimer(window,PREVIEW_RETRY_TIMER);KillTimer(window,PREVIEW_RESIZE_TIMER);
    if(!closing&&!resizingWindow&&!IsIconic(window)&&!SetTimer(window,PREVIEW_RESIZE_TIMER,120,nullptr)) {
        previewResizePending=false;Batch::emergency("preview resize timer unavailable");
        if(previewSetupBusy&&previewSetupDone.load(std::memory_order_acquire))PostMessageW(window,WM_PREVIEW_READY,0,0);
        if(previewWindow)InvalidateRect(previewWindow,nullptr,FALSE);
        repaint();
    }
}
void invalidateSuspendedPreview(){
    if(!previewDisplaySuspended()||IsIconic(window))return;
    if(tooltipWindow)SendMessageW(tooltipWindow,TTM_POP,0,0);
    repaintRegion(batchViewport());
    if(previewWindow)InvalidateRect(previewWindow,nullptr,FALSE);
}
// 单独处理页的进度模块位于左侧栏 y=430～476，不能继续刷新旧版 y=538 区域。
void repaintProgress() {repaintRegion(sidebar.progress);}
void repaintBatchProgress() {repaintRegion(sidebar.progress);}
RectF batchDragCanvas() {return RectF(294,84,std::max(1.f,clientW-294),std::max(1.f,clientH-126));}
void dirtyBatchCard(RectF r) {
    // Include the floating outline, antialiasing and shadow, but never
    // invalidate the sidebar or status bar when the pointer leaves the grid.
    r.Inflate(6,15);r.Intersect(batchDragCanvas());
    if(r.Width>0&&r.Height>0)repaintRegion(r);
}
RectF batchFloatingRect() {
    return RectF(batchDrag.x-batchDrag.grabX,batchDrag.y-batchDrag.grabY,
                 batchDrag.grid.width,batchDrag.grid.height);
}
void finishBatchDrag(bool commit) {
    const bool active=batchDrag.active;
    KillTimer(window,BATCH_HOLD_TIMER);KillTimer(window,BATCH_REFLOW_TIMER);
    if(active&&commit&&batchDrag.order.size()==batchItems.size()) {
        std::vector<BatchItem> ordered;ordered.reserve(batchItems.size());
        for(size_t original:batchDrag.order)ordered.push_back(std::move(batchItems[original]));
        batchItems=std::move(ordered);
        if(batchCardCache.size()==batchItems.size()&&batchCardCacheKeys.size()==batchItems.size()) {
            std::vector<std::unique_ptr<Bitmap>> surfaces;surfaces.reserve(batchItems.size());
            std::vector<BatchCardCacheKey> keys;keys.reserve(batchItems.size());
            for(size_t original:batchDrag.order){surfaces.push_back(std::move(batchCardCache[original]));keys.push_back(std::move(batchCardCacheKeys[original]));}
            batchCardCache=std::move(surfaces);batchCardCacheKeys=std::move(keys);
        }
        releaseBatchPreviewBuffers();batchPreviewLoader.invalidate();previewGeneration=0;
        status=L"图片顺序已更新 · 结果预览与批量导出使用当前顺序";
    }
    batchDrag.reset();batchCardMotions.clear();
    if(GetCapture()==window)ReleaseCapture();
    if(active){repaint();UpdateWindow(window);}
}
void prepareBatchDragVisuals() {
    if(batchCardMotions.size()==batchItems.size())return;
    batchCardMotions.resize(batchItems.size());
    for(size_t i=0;i<batchItems.size();++i){auto r=batchCardRect(i);batchCardMotions[i].reset(r.X,r.Y);}
}
bool sampleBatchReflow(double now) {
    bool changed=false;
    for(size_t original=0;original<batchCardMotions.size();++original) {
        if(original==batchDrag.item)continue;
        auto& p=batchCardMotions[original];
        RectF old(p.x,p.y,batchDrag.grid.width,batchDrag.grid.height);
        if(p.sample(now)) {
            dirtyBatchCard(old);dirtyBatchCard(RectF(p.x,p.y,old.Width,old.Height));changed=true;
        }
    }
    return changed;
}
void activateBatchDrag() {
    if(!batchDrag.pending)return;
    if(page!=Page::Batch||batchPreviewMode||batchLocked()||GetCapture()!=window||!(GetKeyState(VK_LBUTTON)&0x8000)) {
        finishBatchDrag(false);return;
    }
    if(GetTickCount64()-batchPressedAt<BATCH_HOLD_MS)return;
    KillTimer(window,BATCH_HOLD_TIMER);batchDrag.activate();
    prepareBatchDragVisuals();SetTimer(window,BATCH_REFLOW_TIMER,16,nullptr);
    SetCursor(LoadCursorW(nullptr,IDC_SIZEALL));dirtyBatchCard(batchCardRect(batchDrag.slot));
    dirtyBatchCard(batchFloatingRect());UpdateWindow(window);
}
void moveBatchDrag(float x,float y) {
    // A timer may be delayed by the message queue. Enforce the same 60ms
    // deadline in the mouse path, without interpreting the activation as a crossing.
    if(batchDrag.pending)activateBatchDrag();
    if(batchDrag.pending){batchDrag.move(x,y);return;}
    if(batchDrag.active){
        prepareBatchDragVisuals();
        double now=monotonicSeconds();bool animated=sampleBatchReflow(now);
        auto oldFloating=batchFloatingRect();size_t oldSlot=batchDrag.slot;
        bool moved=x!=batchDrag.x||y!=batchDrag.y;
        size_t events=batchDrag.move(x,y);
        if(events) {
            dirtyBatchCard(batchCardRect(oldSlot));dirtyBatchCard(batchCardRect(batchDrag.slot));
            for(size_t slot=0;slot<batchDrag.order.size();++slot) {
                size_t original=batchDrag.order[slot];if(original==batchDrag.item)continue;
                auto target=batchCardRect(slot);auto& p=batchCardMotions[original];
                p.retarget(target.X,target.Y,now);
            }
        }
        if(moved){dirtyBatchCard(oldFloating);dirtyBatchCard(batchFloatingRect());}
        // Flush the current input's visual state now. WM_PAINT and WM_TIMER
        // alone can starve while fast pointer input is arriving continuously.
        if(moved||events||animated)UpdateWindow(window);
    }
}
void animateBatchReflow() {
    if(!batchDrag.active)return;
    if(sampleBatchReflow(monotonicSeconds()))UpdateWindow(window);
}
const wchar_t* batchStateLabel(BatchItemState state) {
    switch(state) {
        case BatchItemState::Processing:return L"处理中";
        case BatchItemState::Completed:return L"已完成";
        case BatchItemState::Preparing:return L"图片准备";
        case BatchItemState::WaitingEncode:return L"等待编码";
        case BatchItemState::Encoding:return L"PNG编码中";
        case BatchItemState::Failed:return L"失败";
        case BatchItemState::Cancelled:return L"已取消";
        default:return L"待处理";
    }
}
size_t selectedBatchCount() {
    size_t count=0;for(const auto& item:batchItems)if(item.selected)++count;return count;
}
bool batchItemSucceeded(const BatchItem& item) {
    // Completed is assigned only after build validation. GetLastStatus is the
    // most recent GDI+ operation, so a failed Save must not invalidate pixels.
    return item.state==BatchItemState::Completed&&!item.cache.empty();
}
std::wstring batchFailureReason(const BatchItem& item);
std::wstring batchFailureDetails(const std::map<std::wstring,size_t>& reasons);
struct BatchResultStats {
    size_t success=0,failed=0,cancelled=0,pending=0,processing=0;
    std::map<std::wstring,size_t> reasons;
};
BatchResultStats batchResultStats() {
    BatchResultStats stats;
    for(const auto& item:batchItems)if(item.selected) {
        if(batchItemSucceeded(item))++stats.success;
        else if(item.state==BatchItemState::Cancelled)++stats.cancelled;
        else if(item.state==BatchItemState::Pending)++stats.pending;
        else if(Batch::active(item.state))++stats.processing;
        else {++stats.failed;++stats.reasons[item.state==BatchItemState::Completed?L"结果不可用":batchFailureReason(item)];}
    }
    return stats;
}
std::wstring batchResultSummaryText(const BatchResultStats& stats) {
    std::wstring summary=L"完成 "+number((int)stats.success)+L" 张 · 失败 "+number((int)stats.failed)+L" 张";
    if(stats.cancelled)summary+=L" · 取消 "+number((int)stats.cancelled)+L" 张";
    return summary;
}
std::wstring batchResultDetailsText(const BatchResultStats& stats) {
    std::wstring details=batchFailureDetails(stats.reasons);
    auto append=[&](const std::wstring& part){if(!details.empty())details+=L"；";details+=part;};
    if(stats.pending)append(L"待处理 "+number((int)stats.pending)+L" 张");
    if(stats.processing)append(L"处理中 "+number((int)stats.processing)+L" 张");
    return details;
}
long batchEstimatedRemainingMs() {
    if(batchPhase!=Batch::Phase::Running)return 0;
    double total=0,finished=0;
    for(const auto& item:batchItems)if(item.selected){
        double work=double((item.w+199)/200)*double((item.h+199)/200);
        total+=work;double fraction=0;
        switch(item.state){
            case BatchItemState::Processing:fraction=item.progress*.009;break;
            case BatchItemState::WaitingEncode:fraction=.9;break;
            case BatchItemState::Encoding:fraction=.94;break;
            case BatchItemState::Completed:case BatchItemState::Failed:case BatchItemState::Cancelled:fraction=1;break;
            default:break;
        }
        finished+=work*fraction;
    }
    double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-batchStarted).count();
    if(elapsed<2||finished<=0||total<=finished)return 0;
    // Measured aggregate throughput includes load, budget waits, overlapping
    // inference, and encoding backlog. It is not serial time / lane count.
    return (long)std::clamp((total-finished)*elapsed/finished*1000.0,0.0,2147483000.0);
}
Pixels* activePreviewSource() {
    if(page==Page::Batch&&batchPreviewMode)return batchPreviewSource.get();
    return source.get();
}
Pixels* activePreviewOutput() {
    if(page==Page::Batch&&batchPreviewMode&&batchPreviewIndex<batchItems.size())return batchPreviewOutput.get();
    return output.get();
}
bool batchItemPreviewable(size_t index) {
    return index<batchItems.size()&&batchItems[index].selected&&batchItemSucceeded(batchItems[index]);
}
bool hasSuccessfulBatchResults() {
    for(const auto& item:batchItems)
        if(item.selected&&batchItemSucceeded(item))
            return true;
    return false;
}
bool batchResultsReady() {
    // 批处理被取消后允许使用已经成功完成的结果；未完成项目不阻塞预览或导出。
    return hasSuccessfulBatchResults();
}
bool loadBatchPreviewSource(size_t index) {
    if(!batchItemPreviewable(index))return false;
    const auto& item=batchItems[index];Batch::PreviewRequest request;
    request.batch=batchGeneration;request.index=index;request.source=item.path;request.png=item.cache;
    request.w=item.w;request.h=item.h;request.resultW=item.resultW;request.resultH=item.resultH;
    try {previewGeneration=batchPreviewLoader.request(std::move(request));status=L"正在加载完成项预览…";repaint();return true;}
    catch(...){status=L"无法启动预览加载";repaint();return false;}
}
// 交互只更新状态和合并重绘请求；不创建或重新采样预览位图。
void viewChanged() {repaintPreview();}
bool inside(const RectF& r,float x,float y) { return x>=r.X&&x<=r.GetRight()&&y>=r.Y&&y<=r.GetBottom(); }
void alert(const std::wstring& s) { ModalGuard modal;MessageBoxW(window,s.c_str(),L"Real-CUGAN",MB_OK|MB_ICONWARNING); }
void fill(Graphics& g,RectF r,Color c) { SolidBrush b(c);g.FillRectangle(&b,r); }
void line(Graphics& g,float x,float y,float xx,float yy,Color c,float width=1) { Pen p(c,width);g.DrawLine(&p,x,y,xx,yy); }
void roundRect(Graphics& g,RectF r,Color c,float radius=9) {
    GraphicsPath path; float d=radius*2;
    path.AddArc(r.X,r.Y,d,d,180,90); path.AddArc(r.GetRight()-d,r.Y,d,d,270,90);
    path.AddArc(r.GetRight()-d,r.GetBottom()-d,d,d,0,90);path.AddArc(r.X,r.GetBottom()-d,d,d,90,90);path.CloseFigure();
    SolidBrush b(c);g.FillPath(&b,&path);
}
Font* uiFont(float size,bool bold) {
    auto key=std::make_pair((int)std::lround(size*100),bold);
    auto& font=uiFonts[key];
    if(!font)font=std::make_unique<Font>(L"Microsoft YaHei UI",size,bold?FontStyleBold:FontStyleRegular,UnitPixel);
    return font.get();
}
float paragraphHeight(const std::wstring& value,float width,float size,int maxLines,bool bold=false) {
    if(value.empty())return 0;
    auto key=std::make_tuple(value,(int)std::lround(width*100),(int)std::lround(size*100),maxLines,bold);
    auto found=measuredHeights.find(key);if(found!=measuredHeights.end())return found->second;
    if(!measureBitmap)measureBitmap=std::make_unique<Bitmap>(1,1,PixelFormat32bppARGB);
    Graphics g(measureBitmap.get());StringFormat format;format.SetFormatFlags(StringFormatFlagsLineLimit);
    RectF measured;g.MeasureString(value.c_str(),-1,uiFont(size,bold),RectF(0,0,width,1000),&format,&measured);
    float height=std::clamp(std::ceil(measured.Height)+2,size+8,(size+8)*maxLines);
    if(measuredHeights.size()>=128)measuredHeights.clear();
    measuredHeights.emplace(std::move(key),height);return height;
}
void paragraph(Graphics& g,const std::wstring& value,RectF r,float size,Color color,bool bold=false,int align=0) {
    if(value.empty()||r.Width<=0||r.Height<=0)return;
    StringFormat format;format.SetTrimming(StringTrimmingEllipsisCharacter);format.SetFormatFlags(StringFormatFlagsLineLimit);
    format.SetAlignment(align==1?StringAlignmentCenter:StringAlignmentNear);format.SetLineAlignment(StringAlignmentNear);
    SolidBrush brush(color);g.DrawString(value.c_str(),-1,uiFont(size,bold),r,&format,&brush);
}
void text(Graphics& g,const std::wstring& s,RectF r,float size=13,Color color=Color(255,28,40,57),bool bold=false,int align=0) {
    StringFormat format;format.SetTrimming(StringTrimmingEllipsisCharacter);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    format.SetAlignment(align==1?StringAlignmentCenter:align==2?StringAlignmentFar:StringAlignmentNear);
    format.SetLineAlignment(StringAlignmentCenter);
    SolidBrush brush(color);g.DrawString(s.c_str(),-1,uiFont(size,bold),r,&format,&brush);
}
std::wstring modeHelpText() {
    return scale==3?L"3 倍模型固定使用强降噪。":noise==-1?L"保留更多原有纹理，适合插画。":noise==0?L"增强清晰度，不额外降噪。":L"适合有噪点、压缩痕迹的图片。";
}
void paintImportContents(Graphics& g,const Ui::ImportPanel& panel,bool batch) {
    text(g,batch?L"导入多张图片":L"导入单张图片",panel.title,FONT_TITLE,ink,true,1);
    paragraph(g,L"点击下方按钮，或将图片拖入此区域",panel.description,FONT_BODY,muted,false,1);
    roundRect(g,panel.button,blue,7);text(g,batch?L"选择多张图片":L"选择图片",panel.button,FONT_BUTTON_PRIMARY,Color::White,true,1);
    paragraph(g,batch?L"最多 20 张 · 单边不超过 5000 像素":L"PNG / JPG / JPEG / BMP · 本机处理",panel.note,FONT_SECONDARY,muted,false,1);
}
void paintImportPanel(Graphics& g,RectF canvas,bool batch) {
    roundRect(g,canvas,Color(255,233,237,243),10);
    if(previewDisplaySuspended())return;
    auto panel=Ui::importPanel(canvas);
    auto& surface=importSurfaces[batch?1:0];
    if(!surface.bitmap||surface.width!=panel.title.Width||surface.density!=dpi) {
        auto bitmap=std::make_unique<Bitmap>((INT)std::ceil(panel.title.Width*dpi),(INT)std::ceil(Ui::ImportPanelHeight*dpi),PixelFormat32bppPARGB);
        if(bitmap->GetLastStatus()==Ok) {
            {
                Graphics cached(bitmap.get());cached.Clear(Color(255,233,237,243));cached.ScaleTransform(dpi,dpi);
                cached.TranslateTransform(-panel.title.X,-panel.title.Y);cached.SetSmoothingMode(SmoothingModeAntiAlias);cached.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
                paintImportContents(cached,panel,batch);
            }
            surface.bitmap=std::move(bitmap);surface.width=panel.title.Width;surface.density=dpi;
        }
    }
    if(surface.bitmap&&surface.width==panel.title.Width&&surface.density==dpi) {
        auto saved=g.Save();g.SetInterpolationMode(InterpolationModeNearestNeighbor);g.SetPixelOffsetMode(PixelOffsetModeHalf);
        float x=std::round(panel.title.X*dpi)/dpi,y=std::round(panel.title.Y*dpi)/dpi;
        g.DrawImage(surface.bitmap.get(),RectF(x,y,surface.bitmap->GetWidth()/dpi,surface.bitmap->GetHeight()/dpi),0,0,(INT)surface.bitmap->GetWidth(),(INT)surface.bitmap->GetHeight(),UnitPixel);
        g.Restore(saved);
    } else paintImportContents(g,panel,batch);
}
std::wstring batchBackendText(bool detailed=false) {
    const bool activeBatch=batchPhase!=Batch::Phase::Stopped;
    const Batch::Device* device=nullptr;
    if(activeBatch)device=&batchDisplayDevice;
    else if(!requestedGpuKey.empty()) {
        auto found=std::find_if(computeDevices.begin(),computeDevices.end(),[](const auto& entry){return entry.key()==requestedGpuKey;});
        if(found==computeDevices.end())return L"GPU 不可用 · 请重新选择";
        device=&*found;
    }
    if(device&&device->index>=0) {
        return L"GPU "+number(device->index)+(detailed?L" · "+device->name:L"")+
            L" · 最多 "+number(Batch::lanes(device->index,activeBatch?batchDisplayConcurrency:requestedConcurrency))+L" 路";
    }
    return L"CPU "+number(activeBatch?batchThreads:effectiveThreads())+L" 线程 · 串行";
}
std::wstring processingFooterText(bool detailed=false) {
    if(page==Page::Single)return L"本机处理 · CPU "+number(busy?resultThreads:effectiveThreads())+L" 线程";
    if(batchExportBusy)return L"本机导出 · PNG 缓存复制";
    return L"本机处理 · "+batchBackendText(detailed);
}
void paintFooter(Graphics& g) {
    fill(g,RectF(0,clientH-36,clientW,36),Color(255,235,239,245));
    text(g,status,RectF(20,clientH-34,std::max(1.f,clientW-380),32),FONT_SECONDARY,muted);
    text(g,processingFooterText(),RectF(clientW-350,clientH-34,220,32),FONT_SECONDARY,muted,false,2);
    text(g,std::wstring(L"v")+RCG_VERSION_WIDE,RectF(clientW-112,clientH-34,92,32),FONT_SECONDARY,muted,false,2);
}
std::wstring tooltipAt(float x,float y) {
    if(previewDisplaySuspended()&&inside(batchViewport(),x,y))return L"";
    if(batchDrag.pending||batchDrag.active||draggingPan||draggingSplit)return L"";
    if(y>=clientH-36&&x<clientW-360)return status;
    if(y>=clientH-36&&x>=clientW-350&&x<clientW-130)return processingFooterText(true);
    if(page==Page::Batch&&!batchPreviewMode) {
        for(size_t i=0;i<batchItems.size();++i)if(inside(batchCardRect(i),x,y))return batchItems[i].path;
        if(batchLocked()&&inside(sidebar.progress,x,y)&&batchCurrent<batchItems.size())return batchItems[batchCurrent].name;
        if(inside(sidebar.progress,x,y))return batchResultDetailsText(batchResultStats());
    }
    if(batchPreviewMode&&x<288) {
        if(inside(sidebar.filename,x,y)&&batchPreviewIndex<batchItems.size())return batchItems[batchPreviewIndex].path;
        if(inside(sidebar.details,x,y))return batchResultDetailsText(batchResultStats());
    }
    if(page==Page::Single&&source&&y>=clientH-76&&y<clientH-36)return sourcePath;
    return L"";
}
void createTooltip() {
    INITCOMMONCONTROLSEX controls{};controls.dwSize=sizeof(controls);controls.dwICC=ICC_WIN95_CLASSES;InitCommonControlsEx(&controls);
    tooltipWindow=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,
                                  CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,window,nullptr,instance,nullptr);
    if(!tooltipWindow)return;
    TOOLINFOW tool{};tool.cbSize=sizeof(tool);tool.uFlags=TTF_SUBCLASS;tool.hwnd=window;tool.uId=1;tool.lpszText=LPSTR_TEXTCALLBACKW;
    GetClientRect(window,&tool.rect);SendMessageW(tooltipWindow,TTM_ADDTOOLW,0,(LPARAM)&tool);
    SendMessageW(tooltipWindow,TTM_SETMAXTIPWIDTH,0,(LPARAM)std::lround(480*dpi));
    SendMessageW(tooltipWindow,TTM_SETDELAYTIME,TTDT_INITIAL,650);
}
bool selected(int id) {
    return (id==SCALE2&&scale==2)||(id==SCALE3&&scale==3)||
        (id==CONSERVATIVE&&noise==-1)||(id==CLEAN&&noise==0)||(id==DENOISE&&noise==3)||id==mode||(id==LOCK_LEFT&&lockLeft);
}
Button* getButton(int id) { for(auto& b:buttons)if(b.id==id)return &b;return nullptr; }
void queuePlacement(HWND child,int x,int y,int w,int h) {
    RECT old{};GetWindowRect(child,&old);MapWindowPoints(nullptr,window,(POINT*)&old,2);
    if(old.left==x&&old.top==y&&old.right-old.left==w&&old.bottom-old.top==h)return;
    constexpr UINT flags=SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS;
    layoutPlacements.push_back({child,x,y,w,h});
    if(layoutBatch){layoutBatch=DeferWindowPos(layoutBatch,child,nullptr,x,y,w,h,flags);if(!layoutBatch)layoutBatchFailed=true;}
    else SetWindowPos(child,nullptr,x,y,w,h,flags);
}
void place(int id,float x,float y,float w,float h) {
    auto* b=getButton(id);if(!b)return;b->box=RectF(x,y,w,h);
    queuePlacement(b->hwnd,int(x*dpi),int(y*dpi),int(w*dpi),int(h*dpi));
}
void setButtonVisible(int id,bool visible) {
    if(auto* b=getButton(id))if(bool(GetWindowLongPtrW(b->hwnd,GWL_STYLE)&WS_VISIBLE)!=visible)ShowWindow(b->hwnd,visible?SW_SHOW:SW_HIDE);
}
void updateButtons() {
    layout();
    for(auto& b:buttons) {
        bool enabled=true;
        if(page==Page::Single)switch(b.id) {
            case OPEN:case SCALE2:case SCALE3:case DENOISE: enabled=!busy;break;
            case CONSERVATIVE:case CLEAN:enabled=!busy&&scale==2;break;
            case RUN:enabled=source&&!busy;break;
            case SAVE:enabled=output&&!busy;break;
            case CANCEL:enabled=busy;break;
            case CLEAR_IMAGE:enabled=source&&!busy;break;
            case OVERLAY:case ORIGINAL:case RESULT:enabled=output!=nullptr;break;
            case FIT:case ACTUAL:case ZOOM200:enabled=source!=nullptr;break;
            case PAGE_SWITCH:enabled=!busy;break;
            case SETTINGS:enabled=true;break;
            default:break;
        } else switch(b.id) {
            case SETTINGS: enabled=true;break;
            case LOCK_LEFT: enabled=batchPreviewMode;break;
            case PAGE_SWITCH: enabled=!batchLocked();break;
            case BATCH_RUN: enabled=!batchLocked()&&selectedBatchCount()>0;break;
            case BATCH_CANCEL: enabled=batchExportBusy||(batchPhase!=Batch::Phase::Stopped&&batchPhase!=Batch::Phase::Cancelling);break;
            case BATCH_EXPORT: enabled=!batchLocked()&&hasSuccessfulBatchResults();break;
            case BATCH_PREVIEW: enabled=!batchPreviewMode&&batchResultsReady();break;
            case BATCH_PREV:case BATCH_NEXT:case BATCH_BACK: enabled=batchPreviewMode;break;
            case SCALE2:case SCALE3:case DENOISE: enabled=!batchLocked()&&!batchPreviewMode;break;
            case CONSERVATIVE:case CLEAN: enabled=!batchLocked()&&!batchPreviewMode&&scale==2;break;
            case BATCH_CLEAR:enabled=!batchLocked()&&!batchItems.empty();break;
            case OVERLAY:case ORIGINAL:case RESULT:case FIT:case ACTUAL:case ZOOM200: enabled=batchPreviewMode&&activePreviewSource()&&activePreviewOutput();break;
            default:enabled=false;break;
        }
        enabled=enabled&&canExecute(b.id);
        if(bool(IsWindowEnabled(b.hwnd))!=enabled)EnableWindow(b.hwnd,enabled);
        InvalidateRect(b.hwnd,nullptr,FALSE);
    }
    repaint();
}
void layout() {
    const bool single=page==Page::Single,preview=imageViewVisible();
    Ui::TextHeights heights;
    heights.help=paragraphHeight(modeHelpText(),248,FONT_BODY,2);
    if(batchPreviewMode) {
        heights.detail=paragraphHeight(batchResultDetailsText(batchResultStats()),248,FONT_SECONDARY,3);
        heights.name=batchPreviewIndex<batchItems.size()?paragraphHeight(batchItems[batchPreviewIndex].name,248,FONT_BODY,2,true):22;
    }
    sidebar=Ui::sidebar(single?Ui::Screen::Single:batchPreviewMode?Ui::Screen::Results:Ui::Screen::Batch,clientH,heights);
    layoutPlacements.clear();layoutPlacements.reserve(buttons.size()+1);layoutBatchFailed=false;
    layoutBatch=BeginDeferWindowPos((int)buttons.size()+1);
    if(auto* switchButton=getButton(PAGE_SWITCH)){
        std::wstring label=single?L"批处理":L"单独处理";
        if(switchButton->label!=label){switchButton->label=std::move(label);SetWindowTextW(switchButton->hwnd,switchButton->label.c_str());}
    }
    for(int id:{RUN,SAVE,CANCEL,CLEAR_IMAGE})setButtonVisible(id,single);
    for(int id:{OVERLAY,ORIGINAL,RESULT,FIT,ACTUAL,ZOOM200})setButtonVisible(id,preview);
    for(int id:{SCALE2,SCALE3,CONSERVATIVE,CLEAN,DENOISE})setButtonVisible(id,!batchPreviewMode);
    setButtonVisible(BATCH_RUN,!single&&!batchPreviewMode);
    setButtonVisible(BATCH_EXPORT,!single&&(!batchPreviewMode||(batchPhase==Batch::Phase::Stopped&&!batchExportBusy)));setButtonVisible(BATCH_PREVIEW,!single&&!batchPreviewMode);
    setButtonVisible(BATCH_CLEAR,!single&&!batchPreviewMode);setButtonVisible(BATCH_CANCEL,!single&&(!batchPreviewMode||batchPhase!=Batch::Phase::Stopped||batchExportBusy));
    setButtonVisible(BATCH_PREV,!single&&batchPreviewMode);setButtonVisible(BATCH_NEXT,!single&&batchPreviewMode);setButtonVisible(BATCH_BACK,!single&&batchPreviewMode);
    setButtonVisible(PAGE_SWITCH,true);setButtonVisible(SETTINGS,true);setButtonVisible(LOCK_LEFT,preview);
    auto at=[&](int id,RectF r){place(id,r.X,r.Y,r.Width,r.Height);};
    if(!batchPreviewMode) {
        at(SCALE2,sidebar.scale2);at(SCALE3,sidebar.scale3);
        at(CONSERVATIVE,sidebar.conservative);at(CLEAN,sidebar.clean);at(DENOISE,sidebar.denoise);
    }
    if(single){at(RUN,sidebar.run);at(CANCEL,sidebar.cancel);at(CLEAR_IMAGE,sidebar.clear);at(SAVE,sidebar.save);}
    else if(!batchPreviewMode) {
        at(BATCH_RUN,sidebar.run);at(BATCH_EXPORT,sidebar.exportResults);at(BATCH_PREVIEW,sidebar.preview);
        at(BATCH_CLEAR,sidebar.clear);at(BATCH_CANCEL,sidebar.cancel);
    } else {
        at(BATCH_PREV,sidebar.previous);at(BATCH_NEXT,sidebar.next);at(BATCH_BACK,sidebar.back);at(BATCH_EXPORT,sidebar.exportResults);at(BATCH_CANCEL,sidebar.exportResults);
    }
    if(preview){
        place(OVERLAY,312,108,112,36);place(ORIGINAL,432,108,90,36);place(RESULT,530,108,90,36);
        place(FIT,clientW-314,108,92,36);place(ACTUAL,clientW-214,108,90,36);place(ZOOM200,clientW-114,108,90,36);
    }
    place(SETTINGS,78,24,64,28);place(PAGE_SWITCH,clientW-184,25,160,28);place(LOCK_LEFT,clientW-154,clientH-68,130,24);
    if(previewWindow) {
        auto v=viewport();queuePlacement(previewWindow,(int)lround(v.X*dpi),(int)lround(v.Y*dpi),
            std::max(1,(int)lround(v.Width*dpi)),std::max(1,(int)lround(v.Height*dpi)));
    }
    if(layoutBatch&&!EndDeferWindowPos(layoutBatch))layoutBatchFailed=true;
    layoutBatch=nullptr;
    if(layoutBatchFailed)for(const auto& p:layoutPlacements)
        SetWindowPos(p.child,nullptr,p.x,p.y,p.w,p.h,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS);
    if(previewWindow&&bool(GetWindowLongPtrW(previewWindow,GWL_STYLE)&WS_VISIBLE)!=preview)ShowWindow(previewWindow,preview?SW_SHOW:SW_HIDE);
    if(tooltipWindow) {
        TOOLINFOW tool{};tool.cbSize=sizeof(tool);tool.hwnd=window;tool.uId=1;
        GetClientRect(window,&tool.rect);SendMessageW(tooltipWindow,TTM_NEWTOOLRECTW,0,(LPARAM)&tool);
    }
    layoutReady=true;viewChanged();repaint();
}
RectF imageRect() {
    // 单独处理预览必须使用单图页面的 viewport；批处理卡片区域有独立布局，
    // 不能参与单图缩放中心、锚点和拖动位置计算。
    RectF v=viewport();
    Pixels* previewSource=activePreviewSource(),*previewOutput=activePreviewOutput();
    if(!previewSource)return v;
    float iw=previewOutput?previewOutput->w:previewSource->w, ih=previewOutput?previewOutput->h:previewSource->h;
    float ratio=fitView?std::min((v.Width-48)/iw,(v.Height-48)/ih):zoom;
    float w=iw*ratio,h=ih*ratio;
    return RectF(v.X+(v.Width-w)/2+panX,v.Y+(v.Height-h)/2+panY,w,h);
}
void resetView() {stopSmoothZoom();wheelRemainder=0;fitView=true;panX=panY=0;split=.5f;viewChanged();}
// 一次只保留下一帧的唤醒时间，按显示器刷新率调度；不积压旧帧。
void armZoomFrame(){
    if(!smoothZoom.active||!frameTimer)return;
    double now=monotonicSeconds();
    if(nextFrameTime<=now)nextFrameTime=now+frameInterval;
    LARGE_INTEGER due;due.QuadPart=-std::max<LONGLONG>(1,(LONGLONG)((nextFrameTime-now)*10000000));
    frameArmed=SetWaitableTimer(frameTimer,&due,0,nullptr,nullptr,FALSE)!=FALSE;
}
void advanceZoom(double now){
    smoothZoom.advance(std::max(0.0,now-zoomClock));zoomClock=now;
    zoom=(float)(smoothZoom.scale()/dpi);fitView=false;
    Pixels* previewSource=activePreviewSource(),*previewOutput=activePreviewOutput();
    if(!previewSource)return;
    auto v=viewport();float iw=previewOutput?previewOutput->w:previewSource->w,ih=previewOutput?previewOutput->h:previewSource->h;
    panX=Preview::anchoredPan(zoomAnchorX,v.X,v.Width,iw,zoom,zoomFractionX);
    panY=Preview::anchoredPan(zoomAnchorY,v.Y,v.Height,ih,zoom,zoomFractionY);
}
void renderZoomFrame(){
    frameArmed=false;if(!activePreviewSource()||!smoothZoom.active)return;
    if(closing||previewDisplaySuspended()){stopSmoothZoom();return;}
    advanceZoom(monotonicSeconds());repaintPreview();
    if(previewWindow)UpdateWindow(previewWindow);
    if(smoothZoom.active){nextFrameTime+=frameInterval;armZoomFrame();}
}
void paintSoftwarePreview(HDC dc) {
    auto view=viewport(),r=imageRect();r.X-=view.X;r.Y-=view.Y;
    Graphics g(dc);g.ScaleTransform(dpi,dpi);g.SetSmoothingMode(SmoothingModeAntiAlias);g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    fill(g,RectF(0,0,view.Width,view.Height),Color(255,244,246,250));roundRect(g,RectF(0,0,view.Width,view.Height),Color(255,233,237,243),10);
    GraphicsPath clip;float w=view.Width,h=view.Height;
    clip.AddArc(RectF(0,0,20,20),180,90);clip.AddArc(RectF(w-20,0,20,20),270,90);clip.AddArc(RectF(w-20,h-20,20,20),0,90);clip.AddArc(RectF(0,h-20,20,20),90,90);clip.CloseFigure();g.SetClip(&clip);
    auto* original=activePreviewSource();auto* result=activePreviewOutput();
    if(!original||!original->bitmap)return;
    auto saved=g.Save();g.SetClip(r,CombineModeIntersect);
    for(int y=0;y<(int)h;y+=12)for(int x=0;x<(int)w;x+=12)fill(g,RectF((float)x,(float)y,12,12),((x/12+y/12)%2)?Color(255,250,251,253):Color(255,223,228,236));
    g.Restore(saved);g.SetInterpolationMode(InterpolationModeBilinear);
    auto draw=[&](Pixels* pixels,RectF region){if(!pixels||!pixels->bitmap)return;auto state=g.Save();g.SetClip(region,CombineModeIntersect);g.DrawImage(pixels->bitmap.get(),r);g.Restore(state);};
    if(!result||mode==ORIGINAL)draw(original,RectF(0,0,w,h));
    else if(mode==RESULT)draw(result,RectF(0,0,w,h));
    else {
        float divider=w*split;draw(result,RectF(0,0,divider,h));draw(original,RectF(divider,0,w-divider,h));
        line(g,divider,0,divider,h,Color::White,3);line(g,divider,0,divider,h,blue,1);
        SolidBrush brush(blue);g.FillEllipse(&brush,RectF(divider-17,h/2-17,34,34));text(g,L"‹  ›",RectF(divider-17,h/2-17,34,34),18,Color::White,true,1);
    }
    roundRect(g,RectF(14,14,200,25),Color(235,24,34,51),6);
    text(g,previewSetupBusy?L"正在准备图像预览…":L"软件预览",RectF(22,14,184,25),FONT_SECONDARY,Color::White);
}
// 预览窗口独立呈现，主窗口刷新按钮或进度不会覆盖预览。
LRESULT previewProcImpl(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg){
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{
            PAINTSTRUCT ps;BeginPaint(hwnd,&ps);PaintEnd paintEnd(hwnd,ps);
            if(!imageViewVisible()){paintEnd.finish();return 0;}
            if(closing){paintEnd.finish();return 0;}
            // Resize is a lightweight path, before setup, admission, uploads or software DrawImage.
            // Keep the current texture caches intact until the final surface dimensions settle.
            if(previewDisplaySuspended()){
                RECT r;GetClientRect(hwnd,&r);HBRUSH bg=CreateSolidBrush(RGB(233,237,243));
                FillRect(ps.hdc,&r,bg);DeleteObject(bg);paintEnd.finish();return 0;}
            // Moving a window changes no preview geometry. Keep its presented
            // surface intact, without device work or large software scaling.
            if(inSizeMove){paintEnd.finish();return 0;}
            if(!realtimePreview.ready()) {
                requestPreviewSetup(hwnd);
                if(previewSetupBusy){RECT r;GetClientRect(hwnd,&r);HBRUSH bg=CreateSolidBrush(RGB(233,237,243));FillRect(ps.hdc,&r,bg);DeleteObject(bg);}
                else paintSoftwarePreview(ps.hdc);
                paintEnd.finish();return 0;
            }
            if(realtimePreview.needsBackgroundSetup(hwnd)){
                realtimePreview.clear();requestPreviewSetup(hwnd);
                RECT r;GetClientRect(hwnd,&r);HBRUSH bg=CreateSolidBrush(RGB(233,237,243));FillRect(ps.hdc,&r,bg);DeleteObject(bg);
                paintEnd.finish();return 0;}
            RectF ir=imageRect(),v=viewport();
            previewBudgetFallback=!realtimePreview.hardware();
            if(!realtimePreview.permitFrame()){
                previewBudgetFallback=true;paintSoftwarePreview(ps.hdc);
                SetTimer(window,PREVIEW_RETRY_TIMER,100,nullptr);paintEnd.finish();return 0;}
            auto image=[](Pixels* p){return p?Realtime::ImageView{p->w,p->h,p->data.data(),p->serial}:Realtime::ImageView{};};
            bool ok=realtimePreview.render(hwnd,dpi,image(activePreviewSource()),image(activePreviewOutput()),
                {ir.X-v.X,ir.Y-v.Y,ir.Width,ir.Height},mode==ORIGINAL?1:mode==RESULT?2:0,split);
            if(!ok){
                if(realtimePreview.wasDeferred())SetTimer(window,PREVIEW_RETRY_TIMER,100,nullptr);
                else {realtimePreview.clearImages();requestPreviewSetup(hwnd);}
                paintSoftwarePreview(ps.hdc);
            }
            if(realtimePreview.retryHardware())SetTimer(window,PREVIEW_RETRY_TIMER,1000,nullptr);
            paintEnd.finish();return 0;
        }
        case WM_LBUTTONDOWN:case WM_LBUTTONUP:case WM_LBUTTONDBLCLK:case WM_MOUSEMOVE:{
            POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};MapWindowPoints(hwnd,window,&p,1);
            return SendMessageW(window,msg,wp,MAKELPARAM(p.x,p.y));
        }
        case WM_MOUSEWHEEL:case WM_SETCURSOR:return SendMessageW(window,msg,wp,lp);
        case WM_RBUTTONDOWN:case WM_RBUTTONUP:case WM_RBUTTONDBLCLK:case WM_CONTEXTMENU:return 0;
        case WM_DROPFILES:return SendMessageW(window,msg,wp,lp);
        case WM_DESTROY:realtimePreview.clear();return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
void paintPreviewInfo(Graphics& g) {
    std::wstring info=source?sourceName+L"   "+number(source->w)+L" × "+number(source->h):L"叠图对比：左侧处理后 / 右侧原图";
    if(output)info+=L"   →   "+number(output->w)+L" × "+number(output->h);
    if(source)info+=L"   /   "+number((int)lround(imageRect().Width*dpi/(output?output->w:source->w)*100))+L"%";
    text(g,info,RectF(312,clientH-68,clientW-650,24),FONT_SECONDARY,muted);

}
size_t firstBatchPreviewable() {
    for(size_t i=0;i<batchItems.size();++i)if(batchItemPreviewable(i))return i;
    return batchItems.size();
}
size_t adjacentBatchPreviewable(size_t index,int direction) {
    if(batchItems.empty())return batchItems.size();
    size_t i=index;
    for(size_t step=0;step<batchItems.size();++step){
        if(direction<0){if(i==0)i=batchItems.size()-1;else --i;}
        else i=(i+1)%batchItems.size();
        if(batchItemPreviewable(i))return i;
    }
    return index;
}
void paintHeaderSidebar(Graphics& g,const wchar_t* title,const wchar_t* subtitle) {
    fill(g,RectF(0,0,clientW,clientH),Color(255,244,246,250));
    fill(g,RectF(0,0,clientW,78),Color(255,24,34,51));
    if(appLogo&&appLogo->GetLastStatus()==Ok) {
        auto saved=g.Save();g.SetInterpolationMode(InterpolationModeHighQualityBicubic);g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.DrawImage(appLogo.get(),RectF(24,19,40,40),0,0,(INT)appLogo->GetWidth(),(INT)appLogo->GetHeight(),UnitPixel);g.Restore(saved);
    } else {
        roundRect(g,RectF(24,19,40,40),blue,11);
        line(g,35,45,49,31,Color::White,3);line(g,39,31,49,31,Color::White,3);line(g,49,31,49,41,Color::White,3);
    }
    text(g,title,RectF(160,15,300,26),FONT_TITLE,Color::White,true);
    text(g,subtitle,RectF(160,43,340,20),FONT_SECONDARY,Color(255,164,180,202));
    fill(g,RectF(0,78,288,clientH-114),Color::White);line(g,288,78,288,clientH-36,Color(255,225,230,238));
}
void paintProcessingOptions(Graphics& g) {
    text(g,L"01   放大倍率",sidebar.headingScale,FONT_SECTION,ink,true);
    text(g,L"02   处理模式",sidebar.headingMode,FONT_SECTION,ink,true);
    paragraph(g,modeHelpText(),sidebar.help,FONT_BODY,muted);
}
void paintProgressBlock(Graphics& g,bool batch) {
    auto r=sidebar.progress;
    if(batch?batchPhase!=Batch::Phase::Stopped:busy) {
        int percent=batch?batchOverallProgress:progress;
        auto begin=batch?batchStarted:started;
        double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
        text(g,(batch?L"批处理 ":L"处理中 ")+number(percent)+L"% · 已用 "+number((int)seconds)+L" 秒",RectF(r.X,r.Y,r.Width,22),FONT_BODY,blue,true);
        roundRect(g,RectF(r.X,r.Y+28,r.Width,4),Color(255,232,237,246),2);
        if(percent)roundRect(g,RectF(r.X,r.Y+28,std::max(4.f,r.Width*percent/100),4),blue,2);
        long remaining=batch?batchEstimatedRemainingMs():remainingMs;
        std::wstring estimate=remaining>0?L"预计剩余 "+number((int)((remaining+999)/1000))+L" 秒":percent>=100?L"正在整理结果…":L"预计剩余：估算中…";
        if(batch) {
            if(batchPhase==Batch::Phase::ErrorAwaitCancel)estimate=L"处理失败，请取消以生成完整日志";
            else if(batchPhase==Batch::Phase::Cancelling)estimate=L"正在回收资源…";

            text(g,batchEngine.statusLabel(),RectF(r.X,r.Y+40,r.Width,20),FONT_SECONDARY,muted);
            text(g,batchEngine.statusLabel(true),RectF(r.X,r.Y+64,r.Width,20),FONT_SECONDARY,muted);
            text(g,estimate,RectF(r.X,r.Y+84,r.Width,20),FONT_SECONDARY,muted);
        } else paragraph(g,estimate,RectF(r.X,r.Y+40,r.Width,40),FONT_SECONDARY,muted);
    } else if(batch) {
        if(batchActualSeconds>0||hasSuccessfulBatchResults()) {
            auto stats=batchResultStats();text(g,batchResultSummaryText(stats),RectF(r.X,r.Y,r.Width,24),FONT_BODY,blue,true);
            if(batchActualSeconds>0){wchar_t elapsed[64];swprintf(elapsed,64,L"实际总耗时：%.1f 秒",batchActualSeconds);text(g,elapsed,RectF(r.X,r.Y+28,r.Width,22),FONT_SECONDARY,muted);}
            else text(g,L"统计范围：当前勾选图片",RectF(r.X,r.Y+28,r.Width,22),FONT_SECONDARY,muted);
            paragraph(g,batchResultDetailsText(stats),RectF(r.X,r.Y+56,r.Width,44),FONT_SECONDARY,muted);
        }
    } else paragraph(g,output?L"点击 100% / 200% 查看结果细节。":L"支持拖入图片，也可按 Ctrl+O 导入。",r,FONT_BODY,muted);
}
std::wstring batchPreviewOrdinalText() {
    if(!batchPreviewMode||!batchPreviewSource||!batchPreviewOutput||!batchItemPreviewable(batchPreviewIndex))return {};
    size_t total=0,current=0;
    for(size_t i=0;i<batchItems.size();++i)if(batchItemPreviewable(i)){++total;if(i==batchPreviewIndex)current=total;}
    return current?std::to_wstring(current)+L"/"+std::to_wstring(total):L"";
}
void paintBatchPreview(Graphics& g) {
    paintHeaderSidebar(g,L"结果预览",L"原图与处理结果对比");
    text(g,L"当前勾选结果",sidebar.resultHeading,FONT_SECTION,ink,true);
    if(batchPreviewIndex<batchItems.size()) {
        const auto& item=batchItems[batchPreviewIndex];auto stats=batchResultStats();
        text(g,batchResultSummaryText(stats),sidebar.summary,FONT_BODY,blue,true);
        paragraph(g,batchResultDetailsText(stats),sidebar.details,FONT_SECONDARY,stats.failed?Color(255,190,86,82):muted);
        paragraph(g,item.name,sidebar.filename,FONT_BODY,ink,true);
        if(batchPreviewSource&&batchPreviewOutput)text(g,number((int)batchPreviewSource->w)+L" × "+number((int)batchPreviewSource->h)+L"  →  "+number((int)batchPreviewOutput->w)+L" × "+number((int)batchPreviewOutput->h),sidebar.dimensions,FONT_SECONDARY,muted);
        paragraph(g,L"滚轮缩放，左键拖动平移画面。",sidebar.interaction,FONT_SECONDARY,muted);
        paragraph(g,L"锁定后仍可缩放和拖动分界线。",sidebar.lock,FONT_SECONDARY,muted);
    }
    text(g,batchPreviewOrdinalText(),sidebar.previewOrdinal,FONT_SECONDARY,muted,false,1);
    paintFooter(g);
}
void paintBatchCard(Graphics& g,const BatchItem& item,RectF card,bool floating=false) {
    if(floating) {
        roundRect(g,RectF(card.X-4,card.Y+5,card.Width+8,card.Height+8),Color(36,24,43,75),13);
        roundRect(g,RectF(card.X-2,card.Y-2,card.Width+4,card.Height+4),blue,12);
    }
    roundRect(g,card,Color(255,255,255),10);
    auto content=BatchCards::content(card.Width,card.Height);
    auto at=[&](BatchCards::Box box){return RectF(card.X+box.x,card.Y+box.y,box.w,box.h);};
    if(item.thumbnail){
        auto image=BatchCards::fitImage(content.thumbnail,(float)item.thumbnail->GetWidth(),(float)item.thumbnail->GetHeight());
        auto saved=g.Save();g.SetInterpolationMode(InterpolationModeHighQualityBicubic);g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.DrawImage(item.thumbnail.get(),at(image));g.Restore(saved);
    }
    text(g,item.name,at(content.name),FONT_SECONDARY,ink,false);
    std::wstring detail=number((int)item.w)+L" × "+number((int)item.h)+L" · "+batchStateLabel(item.state);
    text(g,detail,at(content.detail),FONT_SECONDARY,
         item.state==BatchItemState::Failed?Color(255,190,86,82):item.state==BatchItemState::Completed?Color(255,53,135,92):muted);
    auto selection=at(content.selection);
    roundRect(g,selection,item.selected?blue:Color(255,224,230,238),4);
    if(item.selected){line(g,selection.X+3,selection.Y+9,selection.X+7,selection.Y+13,Color::White,1.6f);line(g,selection.X+7,selection.Y+13,selection.X+15,selection.Y+4,Color::White,1.6f);}
    text(g,L"×",RectF(card.GetRight()-22,card.Y+2,18,18),FONT_SECONDARY,Color(255,140,150,165),false,1);
}
void ensureBatchCardCache(size_t original,RectF card) {
    // Normal list painting creates each surface once. Starting a drag never
    // builds twenty text/thumbnail surfaces on the input handler's critical path.
    if(batchDrag.active||previewDisplaySuspended())return;
    try {
        batchCardCache.resize(batchItems.size());
        batchCardCacheKeys.resize(batchItems.size());
        auto& item=batchItems[original];auto& key=batchCardCacheKeys[original];
        if(batchCardCache[original]&&key.matches(item,card.Width,card.Height))return;
        auto bitmap=std::make_unique<Bitmap>((INT)std::ceil((card.Width+4)*dpi),(INT)std::ceil((card.Height+4)*dpi),PixelFormat32bppPARGB);
        if(bitmap->GetLastStatus()!=Ok){batchCardCache[original].reset();return;}
        {
            Graphics cached(bitmap.get());cached.Clear(Color(0,0,0,0));cached.ScaleTransform(dpi,dpi);
            cached.SetSmoothingMode(SmoothingModeAntiAlias);cached.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            paintBatchCard(cached,item,RectF(2,2,card.Width,card.Height));
        }
        key={item.path,item.name,item.w,item.h,item.thumbnail.get(),item.state,item.selected,card.Width,card.Height,dpi};
        batchCardCache[original]=std::move(bitmap);
    } catch(...) {batchCardCache.clear();batchCardCacheKeys.clear();} // Drawing without the cache remains valid.
}
void paintBatchDisplayCard(Graphics& g,size_t original,RectF card,bool floating=false) {
    ensureBatchCardCache(original,card);
    if(original>=batchCardCache.size()||original>=batchCardCacheKeys.size()||!batchCardCache[original]||
       !batchCardCacheKeys[original].matches(batchItems[original],card.Width,card.Height)) {
        paintBatchCard(g,batchItems[original],card,floating);return;
    }
    if(floating) {
        roundRect(g,RectF(card.X-4,card.Y+5,card.Width+8,card.Height+8),Color(36,24,43,75),13);
        roundRect(g,RectF(card.X-2,card.Y-2,card.Width+4,card.Height+4),blue,12);
    }
    auto* cached=batchCardCache[original].get();
    auto saved=g.Save();g.SetInterpolationMode(InterpolationModeNearestNeighbor);g.SetPixelOffsetMode(PixelOffsetModeHalf);
    // Keep cache texels aligned to physical pixels at fractional UI scales;
    // otherwise clipping can change GDI+'s nearest-neighbor sampling phase.
    float drawX=std::round((card.X-2)*dpi)/dpi,drawY=std::round((card.Y-2)*dpi)/dpi;
    g.DrawImage(cached,RectF(drawX,drawY,cached->GetWidth()/dpi,cached->GetHeight()/dpi),
                0,0,(INT)cached->GetWidth(),(INT)cached->GetHeight(),UnitPixel);
    g.Restore(saved);
}
void paintBatchCanvas(Graphics& g) {
    RectF v=batchViewport();
    if(previewDisplaySuspended()){roundRect(g,v,Color(255,233,237,243),10);return;}
    if(!batchItems.empty())roundRect(g,v,Color(255,233,237,243),10);
    if(batchItems.empty()){
        batchCardCache.clear();batchCardCacheKeys.clear();paintImportPanel(g,v,true);
    }else{
        text(g,L"已导入 "+number((int)batchItems.size())+L" / 20 张图片",RectF(v.X+16,v.Y+2,240,18),FONT_SECONDARY,muted);
        if(batchDrag.active)roundRect(g,batchCardRect(batchDrag.slot),Color(255,212,224,248),10);
        for(size_t slot=0;slot<batchItems.size();++slot){
            size_t original=batchDrag.active?batchDrag.order[slot]:slot;
            if(batchDrag.active&&original==batchDrag.item)continue;
            RectF card=batchCardRect(slot);
            if(batchDrag.active){card.X=batchCardMotions[original].x;card.Y=batchCardMotions[original].y;}
            if(g.IsVisible(card))paintBatchDisplayCard(g,original,card);
        }
        if(batchItems.size()<MAX_BATCH_IMAGES){
            RectF add=batchContinueRect();
            roundRect(g,add,Color(255,246,249,253),10);
            Pen border(Color(255,180,194,217),1.5f);border.SetDashStyle(DashStyleDash);
            g.DrawRectangle(&border,add.X+1,add.Y+1,add.Width-2,add.Height-2);
            float cx=add.X+add.Width/2,cy=add.Y+(add.Height-48)/2;
            line(g,cx,cy-16,cx,cy+16,blue,3);line(g,cx-16,cy,cx+16,cy,blue,3);
            text(g,L"继续添加",RectF(add.X+8,cy+26,add.Width-16,24),FONT_BODY,blue,true,1);
            text(g,number((int)batchItems.size())+L" / 20",RectF(add.X+8,add.GetBottom()-26,add.Width-16,16),FONT_SECONDARY,muted,false,1);
        }
    }
    if(batchDrag.active) {
        auto saved=g.Save();g.SetClip(batchDragCanvas(),CombineModeIntersect);
        paintBatchDisplayCard(g,batchDrag.item,batchFloatingRect(),true);g.Restore(saved);
    }
}
void paintBatch(Graphics& g) {
    if(batchPreviewMode){paintBatchPreview(g);return;}
    // During a drag, local invalidations can repaint the canvas directly.
    // The header, sidebar and status text are outside these dirty rectangles.
    RectF clip;g.GetVisibleClipBounds(&clip);auto canvas=batchDragCanvas();float tolerance=2/dpi;
    if(batchDrag.active&&clip.X>=canvas.X-tolerance&&clip.Y>=canvas.Y-tolerance&&
       clip.GetRight()<=canvas.GetRight()+tolerance&&clip.GetBottom()<=canvas.GetBottom()+tolerance) {
        fill(g,canvas,Color(255,244,246,250));paintBatchCanvas(g);return;
    }
    paintHeaderSidebar(g,L"批处理",L"多图处理 · 拖动排序");
    paintProcessingOptions(g);
    paragraph(g,L"长按图片 60ms 排序，松手后生效。",sidebar.hint,FONT_SECONDARY,muted);
    paintBatchCanvas(g);paintProgressBlock(g,true);paintFooter(g);
}

void paint(Graphics& g,const RectF& dirty) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    if(page==Page::Batch){paintBatch(g);return;}
    if(source&&dirty.X>=300&&dirty.Y>=clientH-76&&dirty.GetBottom()<=clientH-36) {
        fill(g,dirty,Color(255,244,246,250));paintPreviewInfo(g);return;
    }
    paintHeaderSidebar(g,L"单独处理",L"单张图片放大与增强");paintProcessingOptions(g);paintProgressBlock(g,false);
    if(!source)paintImportPanel(g,batchViewport(),false);
    else paintPreviewInfo(g);
    paintFooter(g);
}

bool loadImage(const std::wstring& path) {
    if(busy||batchLocked()||closing||page!=Page::Single)return false;
    Batch::MemoryLease metadata;if(!metadata.acquire(appBudget,32*Batch::MiB)){alert(L"当前可用内存不足，无法打开图片。");return false;}
    auto image=std::unique_ptr<Bitmap>(Bitmap::FromFile(path.c_str()));
    if(!image||image->GetLastStatus()!=Ok||!image->GetWidth()||!image->GetHeight()) {
        alert(L"无法读取这张图片，请确认图片格式受支持且文件没有损坏。");return false;
    }
    uint32_t w=image->GetWidth(),h=image->GetHeight();
    if(uint64_t(w)*h>MAX_PIXELS) {alert(L"图片过大，输入限制为 6400 万像素以内。");return false;}
    try {
        auto p=std::make_unique<Pixels>();p->memory=std::make_shared<Batch::MemoryLease>(appBudget,Batch::imageBytes(w,h,12)+64*Batch::MiB);p->w=w;p->h=h;p->data.resize(size_t(w)*h*4);p->memory->realize(p->data.size());
        BitmapData data{};Rect area(0,0,w,h);
        if(image->LockBits(&area,ImageLockModeRead,PixelFormat32bppARGB,&data)!=Ok) {alert(L"读取图片像素失败。");return false;}
        for(uint32_t y=0;y<h;++y)memcpy(p->data.data()+size_t(y)*w*4,(unsigned char*)data.Scan0+ptrdiff_t(y)*data.Stride,size_t(w)*4);
        image->UnlockBits(&data);image.reset();p->build();p->memory->shrink(p->data.size()+16*Batch::MiB,p->data.size());source=std::move(p);output.reset();
        sourcePath=path;sourceName=basename(path);status=L"已导入 "+sourceName+L" · 设置参数后点击开始处理";
        lastSaved.clear();mode=OVERLAY;resetView();updateButtons();return true;
    } catch(const std::exception&) {alert(L"内存准入不足或图片读取失败，请稍后尝试较小的图片。");return false;}
}
void clearCurrentImage() {
    if(busy||!source)return;
    stopSmoothZoom();draggingPan=draggingSplit=false;
    if(GetCapture()==window||GetCapture()==previewWindow)ReleaseCapture();
    source.reset();output.reset();completed.reset();realtimePreview.clearImages();
    sourcePath.clear();sourceName.clear();lastSaved.clear();progress=0;remainingMs=0;
    mode=OVERLAY;zoom=1;status=L"已删除当前图片 · 可重新选择或拖入图片";
    resetView();updateButtons();
}
void openImage() {
    if(!canExecute(OPEN))return;
    uint64_t generation=batchGeneration;
    std::vector<wchar_t> file(32768,0);
    OPENFILENAMEW ofn{};ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=window;
    ofn.lpstrFilter=L"图片文件 (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0所有文件\0*.*\0";
    ofn.lpstrFile=file.data();ofn.nMaxFile=file.size();ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    ofn.lpstrTitle=L"选择需要处理的图片";
    BOOL chosen=FALSE;{ModalGuard modal;chosen=GetOpenFileNameW(&ofn);}
    if(chosen&&generation==batchGeneration&&canExecute(OPEN))loadImage(file.data());
}

bool supportedBatchImage(const std::wstring& path) {
    size_t dot=path.find_last_of(L'.');
    if(dot==std::wstring::npos)return false;
    const std::wstring ext=path.substr(dot);
    return !_wcsicmp(ext.c_str(),L".png")||!_wcsicmp(ext.c_str(),L".jpg")||
           !_wcsicmp(ext.c_str(),L".jpeg")||!_wcsicmp(ext.c_str(),L".bmp");
}
std::shared_ptr<Bitmap> makeBatchThumbnail(Bitmap* image) {
    if(!image||image->GetLastStatus()!=Ok||!image->GetWidth()||!image->GetHeight())return nullptr;
    constexpr INT tw=BatchCards::ThumbnailWidth,th=BatchCards::ThumbnailHeight;
    auto result=std::shared_ptr<Bitmap>(new Bitmap(tw,th,PixelFormat32bppARGB));
    if(!result||result->GetLastStatus()!=Ok)return nullptr;
    Graphics graphics(result.get());
    graphics.SetCompositingMode(CompositingModeSourceCopy);
    graphics.Clear(Color(255,247,249,252));
    graphics.SetCompositingMode(CompositingModeSourceOver);
    graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    float ratio=std::min(float(tw)/image->GetWidth(),float(th)/image->GetHeight());
    float w=image->GetWidth()*ratio,h=image->GetHeight()*ratio;
    graphics.DrawImage(image,RectF((tw-w)/2.f,(th-h)/2.f,w,h),0,0,
                       (INT)image->GetWidth(),(INT)image->GetHeight(),UnitPixel);
    return result;
}
bool batchPathEquals(const std::wstring& left,const std::wstring& right) { return _wcsicmp(left.c_str(),right.c_str())==0; }
bool batchContainsPath(const std::wstring& path) {
    for(const auto& item:batchItems)if(batchPathEquals(item.path,path))return true;
    return false;
}
size_t countNewBatchImages(const std::vector<std::wstring>& paths) {
    std::vector<std::wstring> unique;
    for(const auto& path:paths){
        if(!supportedBatchImage(path)||batchContainsPath(path))continue;
        bool duplicate=false;for(const auto& seen:unique)if(batchPathEquals(seen,path)){duplicate=true;break;}
        if(!duplicate)unique.push_back(path);
    }
    return unique.size();
}
bool addBatchImage(const std::wstring& path) {
    if(page!=Page::Batch||busy||batchLocked()||batchPreviewMode)return false;
    if(batchContainsPath(path))return false;
    if(batchItems.size()>=MAX_BATCH_IMAGES){alert(L"图片数量超出上限(20张) 请减少图片数量！");return false;}
    if(!supportedBatchImage(path)){alert(L"不支持的图片格式。仅支持 PNG、JPG、JPEG 和 BMP。");return false;}
    Batch::MemoryLease metadata;if(!metadata.acquire(appBudget,32*Batch::MiB)){alert(L"缩略图准备的内存准入不足。");return false;}
    auto image=std::unique_ptr<Bitmap>(Bitmap::FromFile(path.c_str()));
    if(!image||image->GetLastStatus()!=Ok||!image->GetWidth()||!image->GetHeight()){
        alert(L"无法读取图片："+basename(path));return false;
    }
    uint32_t w=image->GetWidth(),h=image->GetHeight();
    if(w>MAX_BATCH_SIDE||h>MAX_BATCH_SIDE){alert(L"图片尺寸超过限制，最大支持 5000×5000px。\n\n"+basename(path));return false;}
    BatchItem item;item.thumbnailMemory=std::make_shared<Batch::MemoryLease>(appBudget,Batch::imageBytes(w,h,12)+32*Batch::MiB);item.path=path;item.name=basename(path);item.w=w;item.h=h;
    item.thumbnail=makeBatchThumbnail(image.get());
    if(!item.thumbnail){alert(L"无法生成图片缩略图："+basename(path));return false;}
    image.reset();constexpr uint64_t thumbnailBytes=uint64_t(BatchCards::ThumbnailWidth)*BatchCards::ThumbnailHeight*4;
    item.thumbnailMemory->shrink(thumbnailBytes+65536,thumbnailBytes);
    batchItems.push_back(std::move(item));return true;
}
void appendBatchImages(const std::vector<std::wstring>& paths) {
    if(page!=Page::Batch||busy||batchLocked()||batchPreviewMode)return;
    size_t available=MAX_BATCH_IMAGES-batchItems.size();
    if(countNewBatchImages(paths)>available){alert(L"图片数量超出上限(20张) 请减少图片数量！");return;}
    int added=0;for(const auto& path:paths)if(addBatchImage(path))++added;
    if(added)batchActualSeconds=0;
    status=added?L"已导入 "+number((int)batchItems.size())+L" 张图片":L"没有新增图片";
    updateButtons();
}
void openBatchImages() {
    if(busy||batchLocked()||batchPreviewMode)return;
    std::vector<wchar_t> files(65536,0);
    OPENFILENAMEW ofn{};ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=window;
    ofn.lpstrFilter=L"图片文件 (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0所有文件\0*.*\0";
    ofn.lpstrFile=files.data();ofn.nMaxFile=(DWORD)files.size();
    ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_ALLOWMULTISELECT|OFN_EXPLORER|OFN_NOCHANGEDIR;
    ofn.lpstrTitle=L"批量选择图片";
    uint64_t generation=batchGeneration;BOOL chosen=FALSE;{ModalGuard modal;chosen=GetOpenFileNameW(&ofn);}
    if(!chosen||generation!=batchGeneration||closing||batchLocked()||page!=Page::Batch)return;
    std::vector<std::wstring> paths;
    wchar_t* cursor=files.data();std::wstring first=cursor;cursor+=first.size()+1;
    if(*cursor==L'\0')paths.push_back(first);
    else {while(*cursor){std::wstring name=cursor;cursor+=name.size()+1;paths.push_back(first+L"\\"+name);}}
    appendBatchImages(paths);
}

std::wstring batchFailureReason(const BatchItem& item) {
    if(item.state==BatchItemState::Cancelled||item.state==BatchItemState::Pending||item.state==BatchItemState::Processing)
        return L"用户取消处理";
    if(item.error.empty())return L"处理失败";
    std::wstring reason=item.error;
    size_t newline=reason.find_first_of(L"\r\n");
    if(newline!=std::wstring::npos)reason.resize(newline);
    const std::wstring prefix=L"图片处理失败：";
    if(reason.compare(0,prefix.size(),prefix)==0)reason.erase(0,prefix.size());
    if(reason.size()>28)reason.resize(28),reason+=L"…";
    return reason;
}
std::wstring batchFailureDetails(const std::map<std::wstring,size_t>& reasons) {
    std::wstring details;bool first=true;
    for(const auto& entry:reasons){
        if(!first)details+=L"；";
        first=false;
        details+=entry.first+L" "+number((int)entry.second)+L" 张";
    }
    return details;
}
void finishBatch(bool cancelled) {
    batchActualSeconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-batchStarted).count();
    batchPhase=Batch::Phase::Stopped;KillTimer(window,1);
    for(auto& item:batchItems)if(item.selected){
        if(cancelled&&item.state!=BatchItemState::Completed&&item.state!=BatchItemState::Failed){
            item.state=BatchItemState::Cancelled;
            if(item.error.empty())item.error=L"用户取消处理";
        }
    }
    wchar_t elapsed[64];swprintf(elapsed,64,L"%.1f",batchActualSeconds);
    auto stats=batchResultStats();auto details=batchResultDetailsText(stats);
    status=std::wstring(cancelled?L"批处理已中断 · ":L"批处理完成 · ")+
           batchResultSummaryText(stats)+(details.empty()?L"":L"（"+details+L"）")+L" · 实际耗时 "+elapsed+L" 秒";
    if(batchEngine.logFailed())status+=L" · 日志写入异常，请检查故障摘要";
    if(!cancelled)batchOverallProgress=100;
    updateButtons();
}
void runBatchTask() {
    if(batchLocked()||batchPreviewMode)return;
    size_t count=selectedBatchCount();if(!count){alert(L"请先导入并勾选需要处理的图片。");return;}
    for(const auto& item:batchItems)if(item.selected&&uint64_t(item.w)*item.h*scale*scale>MAX_PIXELS){alert(L"图片 "+item.name+L" 在当前倍率下输出超过6400万像素。");return;}
    bool clears=false;for(const auto& item:batchItems)if(item.selected&&batchItemSucceeded(item))clears=true;
    if(clears){int answer=IDNO;{ModalGuard modal;answer=MessageBoxW(window,L"重新开始会清除当前勾选项的旧结果，包括尚未导出的结果。未勾选项保持原状。是否继续？",L"重新处理",MB_YESNO|MB_ICONWARNING);}
        if(answer!=IDYES||!canExecute(BATCH_RUN))return;}
    try {
        appBudget.initialize();if(!uiMemory.bytes()&&!uiMemory.acquire(appBudget,64*Batch::MiB))throw std::runtime_error("UI memory admission unavailable");batchSession.create();
        Batch::Config config;config.batch=++batchGeneration;config.cache=batchSession.batchDirectory(config.batch);
        config.scale=scale;config.noise=noise;config.threads=effectiveThreads();config.concurrency=requestedConcurrency;
        if(!requestedGpuKey.empty()){
            auto device=std::find_if(computeDevices.begin(),computeDevices.end(),[](const auto& d){return d.key()==requestedGpuKey;});
            if(device==computeDevices.end())throw std::runtime_error("Selected GPU unavailable; choose GPU or CPU explicitly in Hardware settings");
            config.device=*device;
        }
        if(config.device.index>=0)config.threads=std::min(config.threads,2);
        config.exe=directory+(config.device.index<0?L"\\realcugan-worker.exe":L"\\realcugan-gpu-worker.exe");
        config.models=directory+L"\\models";config.logs=Batch::runtimeDirectory(L"logs");config.token=batchSession.token(config.batch);config.hardware=hardwareSummary()+L"\r\n"+Batch::wide(computeDiagnostics);
        std::vector<Batch::Task> tasks;
        for(size_t i=0;i<batchItems.size();++i)if(batchItems[i].selected){
            const auto& item=batchItems[i];Batch::Task entry;entry.id=i+1;entry.index=i;entry.source=item.path;entry.w=item.w;entry.h=item.h;
            auto base=config.cache+L"\\task_"+number((int)entry.id);entry.input=base+L".input";entry.raw=base+L".rgba";entry.png=base+L".png";tasks.push_back(std::move(entry));
        }
        batchPreviewLoader.invalidate();previewGeneration=0;releaseBatchPreviewBuffers();
        batchScale=scale;batchThreads=requestedGpuKey.empty()?effectiveThreads():std::min(effectiveThreads(),2);batchTotalSelected=count;batchOverallProgress=0;batchActualSeconds=0;
        batchDisplayDevice=config.device;batchDisplayConcurrency=config.concurrency;
        for(size_t i=0;i<batchItems.size();++i)if(batchItems[i].selected){auto& item=batchItems[i];retireCache(item.cache);item.cache.clear();item.taskId=i+1;item.state=BatchItemState::Pending;item.progress=0;item.error.clear();}
        batchEngine.start(std::move(config),std::move(tasks));
        batchPhase=Batch::Phase::Running;batchStarted=std::chrono::steady_clock::now();
        status=L"批处理已启动 · 完成缓存后即可预览";SetTimer(window,1,1000,nullptr);updateButtons();
    }catch(const std::exception& error){status=L"批处理未启动，详情见故障日志";Batch::emergency(error.what());updateButtons();alert(L"批处理无法启动："+Batch::wide(error.what()));}
}
void cancelBatchTask() {
    if(batchExportBusy){batchExporter.requestStop();status=L"正在取消导出并回收文件句柄…";updateButtons();return;}
    if(batchPhase==Batch::Phase::Stopped||batchPhase==Batch::Phase::Cancelling)return;
    batchPhase=Batch::Phase::Cancelling;batchEngine.cancel();status=L"正在取消并回收资源…";updateButtons();
}
void pollBatchEvents() {

    if(inSizeMove&&!closing){batchRefreshDeferred=true;return;}
    static bool active=false,again=false;
    if(active){again=true;return;}
    active=true;struct DrainScope {bool& active;bool& again;~DrainScope(){active=false;if(again){again=false;batchNotification.notify();}}} drainScope{active,again};
    static bool faultNoticeShown=false;
    if(Batch::emergencyWriteFailed.exchange(false)) {
        status=L"故障日志写入失败，请检查程序目录的 logs 权限和磁盘空间";
        if(!closing&&!faultNoticeShown){faultNoticeShown=true;alert(status);}
    }
    bool changed=false,progressChanged=false;
    if(batchEngine.emergencyFault()&&batchPhase!=Batch::Phase::Stopped&&batchPhase!=Batch::Phase::Cancelling){batchPhase=Batch::Phase::ErrorAwaitCancel;status=L"后台异常，已停止派发；请取消并查看故障日志";changed=true;}
    if(batchPreviewLoader.failed()){status=L"预览准备失败；有效PNG缓存保留，可稍后重试";changed=true;}
    static ULONGLONG nextCacheCleanup=0;
    if(!retiredCaches.empty()&&GetTickCount64()>=nextCacheCleanup){
        nextCacheCleanup=GetTickCount64()+1000;
        for(auto it=retiredCaches.begin();it!=retiredCaches.end();){if(DeleteFileW(it->c_str())||GetLastError()==ERROR_FILE_NOT_FOUND)it=retiredCaches.erase(it);else ++it;}
    }
    for(auto& event:batchEngine.drain()){
        if(event.batch!=batchGeneration)continue;
        if(event.kind==Batch::Event::Kind::Fault){if(batchPhase!=Batch::Phase::Cancelling){batchPhase=Batch::Phase::ErrorAwaitCancel;status=L"处理失败，请取消处理后查看日志："+event.error;stopSmoothZoom();draggingPan=draggingSplit=false;if(GetCapture()==window||GetCapture()==previewWindow)ReleaseCapture();}changed=true;continue;}
        if(event.kind==Batch::Event::Kind::Stopped){batchStopPending=true;batchStopCancelled=event.cancelled;continue;}
        if(event.index>=batchItems.size())continue;
        auto& item=batchItems[event.index];
        if(item.taskId!=event.task)continue;
        if(event.kind==Batch::Event::Kind::Progress){if(item.state==BatchItemState::Processing){item.progress=event.progress;progressChanged=true;}continue;}
        if(!Batch::canTransition(item.state,event.stage))continue;
        item.state=event.stage;item.error=event.error;
        if(event.stage==BatchItemState::Completed){item.cache=event.path;item.progress=100;item.resultW=item.w*batchScale;item.resultH=item.h*batchScale;}
        changed=true;
    }
    if(batchEngine.takeStopped(batchStopCancelled))batchStopPending=true;
    if(batchStopPending&&batchEngine.done()&&!batchEngine.cleanupPending()){batchEngine.join();batchStopPending=false;finishBatch(batchStopCancelled);changed=false;}
    unsigned progressSum=0;batchCurrent=batchItems.size();
    for(size_t i=0;i<batchItems.size();++i)if(batchItems[i].selected){const auto& item=batchItems[i];
        if(Batch::terminal(item.state)){progressSum+=100;}
        else {progressSum+=item.state==BatchItemState::WaitingEncode?90:item.state==BatchItemState::Encoding?94:item.progress*9/10;if(batchCurrent==batchItems.size()&&Batch::active(item.state))batchCurrent=i;}}
    batchOverallProgress=(int)(progressSum/std::max<size_t>(1,batchTotalSelected));
    if(auto preview=batchPreviewLoader.drain()){
        const auto& request=preview->request;
        if(request.batch==batchGeneration&&request.generation==previewGeneration&&page==Page::Batch&&request.index<batchItems.size()&&batchItemPreviewable(request.index)&&batchItems[request.index].cache==request.png&&batchItems[request.index].path==request.source&&batchPhase!=Batch::Phase::ErrorAwaitCancel){
            if(!preview->error.empty())status=L"预览加载失败："+preview->error;
            else try {
                auto from=[](Batch::Image image){auto pixels=std::make_unique<Pixels>();pixels->w=image.w;pixels->h=image.h;pixels->data=std::move(image.data);pixels->build();if(!pixels->bitmap||pixels->bitmap->GetLastStatus()!=Ok)throw std::bad_alloc();return pixels;};
                auto original=from(std::move(preview->source)),result=from(std::move(preview->result));
                batchPreviewSource=std::move(original);batchPreviewOutput=std::move(result);batchPreviewIndex=request.index;batchPreviewLease=std::move(preview);
                bool first=!batchPreviewMode;batchPreviewMode=true;if(first)mode=OVERLAY;resetView();status=L"正在预览 "+batchItems[request.index].name;changed=true;
            }catch(...){status=L"预览显示内存不足；有效PNG缓存已保留";}
            repaint();
        }
    }
    if(batchExportBusy&&batchExporter.done()){
        auto result=batchExporter.finish();batchExportBusy=false;status=L"批量导出 "+number((int)result.exported)+L" 张";
        batchEngine.record("export_seconds="+std::to_string(result.seconds)+" exported="+std::to_string(result.exported)+" failure="+std::to_string((int)result.failure)+" system_error="+std::to_string(result.systemError)+" error="+Batch::utf8(result.error));
        if(result.failed||!result.error.empty()){
            status+=L" · 导出失败，缓存已保留";
            if(!closing){
                std::wstring message=Batch::exportFailureMessage(result);
                if(result.systemError)message+=L"\n系统错误码："+std::to_wstring(result.systemError);
                message+=L"\n\n已导出 "+number((int)result.exported)+L" 张；已提交的文件和成功 PNG 缓存保留。";
                ModalGuard modal;alert(message);
            }
        }else status+=result.cancelled?L" · 已取消，已提交文件保留":L" · 完成";
        changed=true;
    }
    if(changed)updateButtons();
    else if(progressChanged&&batchPhase!=Batch::Phase::Stopped)repaintBatchProgress();
}
// 每次任务创建独立临时目录；路径作为独立引用参数传入，不经过 cmd.exe。
void processTask() {
    auto result=std::make_unique<Outcome>();result->memory=singleWorkMemory;auto begin=std::chrono::steady_clock::now();std::string log;
    Batch::Session session;Batch::DiskLease disk;Batch::Handle job;Batch::Child worker;
    try{
        session.create();auto inPath=session.root()+L"\\input.rgba",outPath=session.root()+L"\\output.rgba";
        uint64_t diskNeed=uint64_t(source->w)*source->h*(4ull+4ull*resultScale*resultScale)+16*Batch::MiB;
        if(!disk.acquire(session.root(),diskNeed,{inPath,outPath,outPath+L".part"}))throw std::runtime_error("Single-image cache volume admission unavailable");
        using File=std::unique_ptr<FILE,decltype(&fclose)>;
        {File file(_wfopen(inPath.c_str(),L"wb"),fclose);Batch::PixelHeader header{{'R','C','G','1'},source->w,source->h};
         if(!file||fwrite(&header,sizeof(header),1,file.get())!=1||fwrite(source->data.data(),1,source->data.size(),file.get())!=source->data.size()||fflush(file.get()))throw std::runtime_error("Input image cache write failed");}
        if(WaitForSingleObject(cancelEvent,0)==WAIT_OBJECT_0)throw std::runtime_error("CANCELLED");
        job=Batch::cleanupJob();worker.start(directory+L"\\realcugan-worker.exe",{inPath,outPath,directory+L"\\models",number(resultScale),number(resultNoise),number(resultThreads)},job.get());
        auto drain=[&]{for(const auto& line:worker.lines()){
            if(log.size()+line.size()>65536){log.erase(0,std::min(log.size(),line.size()));}
            log+=line+"\n";int percent=0;long remaining=0;
            if(sscanf(line.c_str(),"PROGRESS %d %ld",&percent,&remaining)==2)PostMessageW(window,WM_PROGRESS,std::clamp(percent,0,100),std::max(0L,remaining));}};
        for(;;){if(WaitForSingleObject(cancelEvent,0)==WAIT_OBJECT_0){if(!TerminateJobObject(job.get(),1))Batch::emergency("single-image cancel termination failed",GetLastError());worker.stop();throw std::runtime_error("CANCELLED");}
            drain();if(worker.exited())break;Sleep(25);}
        drain();DWORD code=worker.exitCode();worker.stop();if(code)throw std::runtime_error("CPU worker failed; see exit-code diagnostics");
        auto image=Batch::readPixels(outPath,source->w*resultScale,source->h*resultScale);
        result->pixels=std::make_unique<Pixels>();result->pixels->w=image.w;result->pixels->h=image.h;result->pixels->data=std::move(image.data);result->pixels->memory=result->memory;
        result->memory->shrink(result->pixels->data.size()+16*Batch::MiB,result->pixels->data.size());
    }catch(const std::bad_alloc&){Batch::emergency("single-image allocation failed");singleEmergency=true;result->pixels.reset();}
     catch(const std::exception& error){result->pixels.reset();if(strcmp(error.what(),"CANCELLED")){Batch::emergency(error.what());Batch::emergency(log.c_str());try{result->error=L"图片处理失败："+Batch::wide(error.what());}catch(...){singleEmergency=true;}}}
     catch(...){result->pixels.reset();singleEmergency=true;Batch::emergency("single-image processing exception");}
    worker.stop();job.reset();
    if(!session.clear()){Batch::emergency("single-image session cleanup failed");}
    disk.reset();
    result->seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();completed=std::move(result);
}
void runTask() {
    if(!appBudget.ready()||!uiMemory.bytes()){alert(L"资源预算初始化失败，无法开始处理。");return;}
    if(!canExecute(RUN)||!source)return;
    if(uint64_t(source->w)*source->h*scale*scale>MAX_PIXELS) {singleWorkMemory.reset();
        alert(L"输出将超过 6400 万像素限制。请选择较低倍率或先缩小原图。");return;
    }
    try{uint64_t need=Batch::imageBytes(source->w,source->h,24+uint64_t(scale)*scale*16)+384*Batch::MiB;singleWorkMemory=std::make_shared<Batch::MemoryLease>(appBudget,need);}
    catch(...){alert(L"当前共享内存准入不足，暂不能开始处理。");return;}
    if(GetFileAttributesW((directory+L"\\realcugan-worker.exe").c_str())==INVALID_FILE_ATTRIBUTES) {
        singleWorkMemory.reset();alert(L"找不到处理引擎。请完整解压整个压缩包，不要只移动 RealCUGAN.exe。");return;
    }
    if(task.joinable()){task.join();}
    ResetEvent(cancelEvent);completed.reset();
    resultScale=scale;resultNoise=noise;resultThreads=effectiveThreads();output.reset();busy=true;progress=0;remainingMs=0;
    started=std::chrono::steady_clock::now();status=L"正在使用 CPU 处理 · 可以取消，关闭窗口会结束任务";
    resetView();updateButtons();SetTimer(window,1,1000,nullptr);
    taskDone=false;singleEmergency=false;
    try{task=std::thread([]()noexcept{try{processTask();}catch(...){Batch::emergency("single-image outer exception");singleEmergency=true;}taskDone.store(true,std::memory_order_release);PostMessageW(window,WM_FINISHED,0,0);});}catch(...){taskDone=true;singleWorkMemory.reset();busy=false;KillTimer(window,1);status=L"无法创建处理任务";updateButtons();alert(status);}
}
void cancelTask() {if(busy){SetEvent(cancelEvent);status=L"正在取消处理…";repaint();}}
void saveOutput() {
    if(busy||!output)return;
    // 默认只使用原图文件名；保存对话框允许手动改名和选择目标文件夹。
    std::wstring name=stem(sourceName)+L".png";
    std::wstring initialDir=sourcePath;
    size_t separator=initialDir.find_last_of(L"/\\");
    if(separator!=std::wstring::npos)initialDir.resize(separator);
    std::vector<wchar_t> file(32768,0);wcsncpy(file.data(),name.c_str(),file.size()-1);
    OPENFILENAMEW ofn{};ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=window;ofn.lpstrFilter=L"PNG 图片（保留透明通道）\0*.png\0";
    ofn.lpstrFile=file.data();ofn.nMaxFile=file.size();ofn.lpstrDefExt=L"png";ofn.lpstrInitialDir=initialDir.empty()?nullptr:initialDir.c_str();ofn.lpstrTitle=L"保存超分结果";
    ofn.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    BOOL chosen=FALSE;{ModalGuard modal;chosen=GetSaveFileNameW(&ofn);}
    if(!chosen||!canExecute(SAVE))return;
    // PNG 编码器为系统 GDI+ 内置组件，无额外安装依赖。
    const CLSID& encoder=Batch::pngEncoder();
    try {
        Batch::MemoryLease encoding(appBudget,output->data.size()*2+32*Batch::MiB);
        GUID id{};wchar_t token[64]{};
        if(FAILED(CoCreateGuid(&id))||!StringFromGUID2(id,token,64))throw std::runtime_error("Save temporary identity creation failed");
        std::wstring destination=file.data();
        std::wstring savedStatus=L"已保存："+destination;
        const auto folder=std::filesystem::path(destination).parent_path().wstring();
        const auto part=folder+L"\\.RCG-save-"+token+L".tmp";
        Batch::DiskLease disk;
        if(!disk.acquire(folder,output->data.size()*2+32*Batch::MiB,{part}))throw std::runtime_error("Save disk admission unavailable");
        Batch::TemporaryFile temporary(part);
        {Batch::Handle created(CreateFileW(part.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
         if(!created)throw std::runtime_error("Exclusive save temporary file creation failed");
         temporary.own();}
        if(output->bitmap->Save(part.c_str(),&encoder,nullptr)!=Ok)throw std::runtime_error("PNG save failed");
        {Batch::Handle saved(CreateFileW(part.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
         if(!saved||!FlushFileBuffers(saved.get()))throw std::runtime_error("PNG save flush failed");}
        if(!MoveFileExW(part.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("PNG save commit failed");
        temporary.committed();lastSaved=std::move(destination);status=std::move(savedStatus);repaint();
    }catch(const std::exception& error){Batch::emergency(error.what());alert(L"保存失败，请检查可用内存、目标文件夹权限和磁盘空间。处理结果仍保留，可重新保存。");}
     catch(...){Batch::emergency("PNG save exception");alert(L"保存失败，处理结果仍保留，请查看 logs 后重试。");}
}
struct HardwareDialog {bool done=false;HFONT font=nullptr;float density=1;std::wstring gpuKey;int concurrency=1;};
void layoutHardware(HWND hwnd,HardwareDialog& ctx,float density) {
    HFONT old=ctx.font;ctx.density=density;
    ctx.font=CreateFontW(-(int)std::lround(12*density),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    struct ControlBox {int id,x,y,w,h;};
    const ControlBox boxes[]={{5001,14,14,452,224},{5004,14,249,452,20},{5002,14,274,452,180},
        {5005,14,310,452,24},{5006,14,344,452,20},{5007,14,370,452,180},
        {5008,14,408,452,20},{5009,14,434,452,120},{5010,14,470,452,34},
        {5003,14,520,90,28},{IDOK,274,520,90,28},{IDCANCEL,376,520,90,28}};
    for(const auto& box:boxes) {
        HWND control=GetDlgItem(hwnd,box.id);
        SendMessageW(control,WM_SETFONT,(WPARAM)ctx.font,TRUE);
        SetWindowPos(control,nullptr,(int)std::lround(box.x*density),(int)std::lround(box.y*density),
            (int)std::lround(box.w*density),(int)std::lround(box.h*density),SWP_NOACTIVATE|SWP_NOZORDER);
    }
    if(old)DeleteObject(old);
}
void populateHardware(HWND hwnd,bool preserveChoice=false);
void startHardwareScan() {
    if(hardwareScanBusy||closing||!window)return;
    if(hardwareScan.joinable())hardwareScan.join();
    hardwareScanBusy=true;hardwareDone=false;
    try {
        HWND target=window;
        hardwareScan=std::thread([target]()noexcept{
            try{
            try {hardwarePending=std::make_unique<Hardware::Info>(Hardware::detect());}
            catch(...) {hardwarePending.reset();}
            pendingComputeDevices.clear();pendingComputeDiagnostics.clear();
            try{pendingComputeDevices=Batch::probeDevices(directory+L"\\realcugan-gpu-worker.exe",pendingComputeDiagnostics);}
            catch(const std::exception& error){pendingComputeDiagnostics+=error.what();}
            catch(...){pendingComputeDiagnostics+="Unknown GPU probe failure";}
            }catch(...){Batch::emergency("hardware scan outer exception");}
            hardwareDone.store(true,std::memory_order_release);PostMessageW(target,WM_HARDWARE_READY,0,0);
        });
    } catch(...) {hardwareDone=true;hardwareScanBusy=false;hardware.gpuDetectionFailed=true;}
}
std::wstring hardwareSummary(){
    std::wstring report=L"CPU："+hardware.cpu+L"\r\n物理核心："+number(hardware.physical)+L"    逻辑处理器："+number(hardware.logical)+
        L"\r\n当前进程可用："+number(hardware.available)+L" 个逻辑处理器 / "+number(hardware.availablePhysical)+L" 个物理核心";
    report+=L"\r\n内存："+number((int)(hardware.memory/(1024*1024)))+L" MB    当前可用："+number((int)(hardware.freeMemory/(1024*1024)))+L" MB\r\n\r\n显卡（当前设备）：";
    if(!hardware.displaysScanned)report+=hardwareScanBusy?L"检测中…":hardware.gpuDetectionFailed?L"检测失败":L"尚未检测";
    else report+=number((int)hardware.gpus.size())+L" 张"+(hardwareScanBusy?L"（刷新中）":L"");
    if(hardware.gpuDetectionFailed)report+=L"（检测不完整）";
    for(auto& gpu:hardware.gpus)report+=L"\r\n• "+gpu;
    if(!hardware.otherDisplays.empty())report+=L"\r\n其他显示设备（虚拟或类型待确认，不计入上方数量）：";
    for(auto& device:hardware.otherDisplays)report+=L"\r\n• "+device;
    report+=L"\r\n\r\n预览当前后端："+std::wstring(previewBudgetFallback?L"软件预览（资源预算）":previewSetupBusy?L"初始化中（临时软件预览）":previewSetupFailed?L"软件预览（Direct2D 不可用）":realtimePreview.backend())+L"\r\n单图超分：原CPU路径；批处理设备独立选择";
    report+=L"\r\nVulkan推理设备："+number((int)computeDevices.size())+L" 个";
    for(const auto& device:computeDevices)report+=L"\r\n• "+device.name+L" · "+number((int)(device.capacity/Batch::MiB))+L" MB";
    if(computeDevices.empty())report+=L"（未检测到可映射的推理GPU，或GPU worker不可用）";
    report+=std::wstring(L"\r\n")+(batchPhase!=Batch::Phase::Stopped?L"批处理当前设备：":L"批处理已选设备：")+batchBackendText(true);
    report+=L"\r\nCPU硬配额与全程序内存准入："+std::wstring(appBudget.ready()?L"已启用80%上限":L"初始化失败")+L"；GPU采用共享提交节奏与采样反馈，瞬时峰值不作硬保证。";
    report+=L"\r\n自动推荐："+number(Hardware::chooseThreads(hardware.available,hardware.availablePhysical,0))+L" 线程";
    if(busy||batchLocked())report+=L"\r\n当前任务："+number(batchLocked()?batchThreads:resultThreads)+L" 线程；处理期间只能查看设置。";
    return report;
}
void populateHardware(HWND hwnd,bool preserveChoice){
    int draft=preserveChoice?(int)SendMessageW(GetDlgItem(hwnd,5002),CB_GETCURSEL,0,0):requestedThreads;
    SetWindowTextW(GetDlgItem(hwnd,5001),hardwareSummary().c_str());
    HWND combo=GetDlgItem(hwnd,5002);SendMessageW(combo,CB_RESETCONTENT,0,0);
    std::wstring automatic=L"自动（平衡，当前 "+number(Hardware::chooseThreads(hardware.available,hardware.availablePhysical,0))+L" 线程）";
    SendMessageW(combo,CB_ADDSTRING,0,(LPARAM)automatic.c_str());
    for(int i=1;i<=hardware.available;++i){std::wstring label=number(i)+L" 线程"+(i==hardware.available?L"（使用全部可用逻辑处理器）":L"");SendMessageW(combo,CB_ADDSTRING,0,(LPARAM)label.c_str());}
    SendMessageW(combo,CB_SETCURSEL,std::clamp(draft,0,hardware.available),0);
    auto* context=reinterpret_cast<HardwareDialog*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    std::wstring gpuKey=preserveChoice&&context?context->gpuKey:requestedGpuKey;
    int concurrency=preserveChoice&&context?context->concurrency:requestedConcurrency;
    HWND deviceCombo=GetDlgItem(hwnd,5007);SendMessageW(deviceCombo,CB_RESETCONTENT,0,0);
    SendMessageW(deviceCombo,CB_ADDSTRING,0,(LPARAM)L"CPU（原版串行批处理）");int gpuChoice=gpuKey.empty()?0:-1;
    for(size_t i=0;i<computeDevices.size();++i){
        std::wstring label=L"GPU "+number(computeDevices[i].index)+L" · "+computeDevices[i].name;
        SendMessageW(deviceCombo,CB_ADDSTRING,0,(LPARAM)label.c_str());if(computeDevices[i].key()==gpuKey)gpuChoice=(int)i+1;
    }
    if(gpuChoice<0){gpuChoice=(int)computeDevices.size()+1;SendMessageW(deviceCombo,CB_ADDSTRING,0,(LPARAM)L"先前选择的GPU不可用，请手动重新选择");}
    SendMessageW(deviceCombo,CB_SETCURSEL,gpuChoice,0);
    HWND lanes=GetDlgItem(hwnd,5009);SendMessageW(lanes,CB_RESETCONTENT,0,0);
    for(const wchar_t* label:{L"1 路",L"2 路",L"4 路"})SendMessageW(lanes,CB_ADDSTRING,0,(LPARAM)label);
    SendMessageW(lanes,CB_SETCURSEL,concurrency==4?2:concurrency==2?1:0,0);
    if(context){context->gpuKey=gpuKey;context->concurrency=concurrency;}
    bool mutableSettings=!busy&&!batchLocked();EnableWindow(deviceCombo,mutableSettings);EnableWindow(lanes,mutableSettings);
    EnableWindow(combo,mutableSettings);EnableWindow(GetDlgItem(hwnd,IDOK),mutableSettings);
    EnableWindow(GetDlgItem(hwnd,5003),!hardwareScanBusy);
}
LRESULT hardwareProcImpl(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    auto* ctx=(HardwareDialog*)GetWindowLongPtrW(hwnd,GWLP_USERDATA);
    if(msg==WM_NCCREATE){ctx=(HardwareDialog*)((CREATESTRUCTW*)lp)->lpCreateParams;SetWindowLongPtrW(hwnd,GWLP_USERDATA,(LONG_PTR)ctx);}
    switch(msg){
        case WM_CREATE:{
            ctx->density=dpi;
            auto control=[&](const wchar_t* cls,const wchar_t* label,DWORD style,int id,int x,int y,int w,int h){
                HWND c=CreateWindowExW(0,cls,label,WS_CHILD|WS_VISIBLE|style,(int)(x*dpi),(int)(y*dpi),(int)(w*dpi),(int)(h*dpi),hwnd,(HMENU)(INT_PTR)id,instance,nullptr);
                SendMessageW(c,WM_SETFONT,(WPARAM)ctx->font,TRUE);return c;};
            control(L"EDIT",L"",ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL|WS_BORDER,5001,14,14,452,224);
            control(L"STATIC",L"CPU 推理线程（下次处理生效）",0,5004,14,249,452,20);
            HWND combo=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,5002,14,274,452,180);EnableWindow(combo,!busy&&!batchLocked());
            control(L"STATIC",L"自动按可用物理核心选择并预留余量；线程更多不保证更快。",0,5005,14,310,452,24);
            control(L"STATIC",L"批处理计算设备（单图页仍使用原CPU路径）",0,5006,14,344,452,20);
            control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,5007,14,370,452,180);
            control(L"STATIC",L"GPU最大并发（CPU模式固定串行）",0,5008,14,408,452,20);
            control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,5009,14,434,452,120);
            control(L"STATIC",L"预算不足时等待；失败后请取消，再手动选择其他GPU或CPU。",0,5010,14,470,452,34);
            control(L"BUTTON",L"重新检测",BS_PUSHBUTTON|WS_TABSTOP,5003,14,520,90,28);
            HWND apply=control(L"BUTTON",L"保存设置",BS_DEFPUSHBUTTON|WS_TABSTOP,IDOK,274,350,90,28);EnableWindow(apply,!busy&&!batchLocked());
            control(L"BUTTON",L"关闭",BS_PUSHBUTTON|WS_TABSTOP,IDCANCEL,376,350,90,28);
            layoutHardware(hwnd,*ctx,dpi);populateHardware(hwnd);return 0;
        }
        case WM_DPICHANGED: {
            auto suggested=*(RECT*)lp;auto work=monitorWorkRect(&suggested);UINT nativeDpi=HIWORD(wp);
            float density=fittedDpi(nativeDpi,work,500,610);
            RECT size{0,0,(LONG)std::lround(480*density),(LONG)std::lround(566*density)};
            adjustFrame(size,WS_CAPTION|WS_SYSMENU|WS_POPUP,WS_EX_DLGMODALFRAME,nativeDpi);
            SetWindowPos(hwnd,nullptr,suggested.left,suggested.top,size.right-size.left,size.bottom-size.top,SWP_NOACTIVATE|SWP_NOZORDER);
            layoutHardware(hwnd,*ctx,density);return 0;
        }
        case WM_COMMAND:
            if(HIWORD(wp)==CBN_SELCHANGE&&ctx&&!busy&&!batchLocked()&&!closing){
                int choice=(int)SendMessageW(GetDlgItem(hwnd,LOWORD(wp)),CB_GETCURSEL,0,0);
                if(LOWORD(wp)==5007){if(choice==0)ctx->gpuKey.clear();else if(choice>0&&(size_t)choice<=computeDevices.size())ctx->gpuKey=computeDevices[choice-1].key();}
                if(LOWORD(wp)==5009)ctx->concurrency=choice==2?4:choice==1?2:1;
            }
            if(LOWORD(wp)==5003&&!closing){startHardwareScan();populateHardware(hwnd,true);return 0;}
            if(LOWORD(wp)==IDOK&&!busy&&!batchLocked()&&!closing){int value=(int)SendMessageW(GetDlgItem(hwnd,5002),CB_GETCURSEL,0,0);
                int selected=std::clamp(value,0,hardware.available);
                if(!WritePrivateProfileStringW(L"Preferences",L"Threads",number(selected).c_str(),settingsPath.c_str())){
                    MessageBoxW(hwnd,L"设置保存失败，请检查配置目录的写入权限。",L"无法保存设置",MB_OK|MB_ICONERROR);return 0;}
                int deviceChoice=(int)SendMessageW(GetDlgItem(hwnd,5007),CB_GETCURSEL,0,0);
                if(deviceChoice<0||(size_t)deviceChoice>computeDevices.size()){
                    MessageBoxW(hwnd,L"请选择当前可用GPU，或明确选择CPU。",L"计算设备不可用",MB_OK|MB_ICONWARNING);return 0;
                }
                int lanesChoice=(int)SendMessageW(GetDlgItem(hwnd,5009),CB_GETCURSEL,0,0);int laneCount=lanesChoice==2?4:lanesChoice==1?2:1;
                std::wstring key=deviceChoice?computeDevices[deviceChoice-1].key():L"";
                if(!WritePrivateProfileStringW(L"Preferences",L"BatchGpu",key.c_str(),settingsPath.c_str())||
                   !WritePrivateProfileStringW(L"Preferences",L"GpuConcurrency",number(laneCount).c_str(),settingsPath.c_str())){
                    MessageBoxW(hwnd,L"设备设置保存失败。",L"无法保存设置",MB_OK|MB_ICONERROR);return 0;
                }
                requestedGpuKey=key;requestedConcurrency=laneCount;requestedThreads=selected;
                status=L"硬件设置已保存 · 批处理："+batchBackendText()+L"；单图：CPU "+number(effectiveThreads())+L" 线程";repaint();DestroyWindow(hwnd);return 0;}
            if(LOWORD(wp)==IDCANCEL){DestroyWindow(hwnd);return 0;}break;
        case WM_CLOSE:DestroyWindow(hwnd);return 0;
        case WM_DESTROY:if(ctx){if(ctx->font)DeleteObject(ctx->font);ctx->done=true;}hardwareDialog=nullptr;return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
void showHardware(){
    ModalGuard modal;
    stopSmoothZoom();
    if(!hardware.displaysScanned)startHardwareScan();
    MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
    if(GlobalMemoryStatusEx(&memory)){hardware.memory=memory.ullTotalPhys;hardware.freeMemory=memory.ullAvailPhys;}
    if(previewWindow)UpdateWindow(previewWindow);
    HardwareDialog ctx;RECT parent;GetWindowRect(window,&parent);RECT rect{0,0,(LONG)(480*dpi),(LONG)(566*dpi)};
    adjustFrame(rect,WS_CAPTION|WS_SYSMENU|WS_POPUP,WS_EX_DLGMODALFRAME,nativeWindowDpi(window));
    HWND dialog=CreateWindowExW(WS_EX_DLGMODALFRAME,L"RealCUGANHardware",L"硬件与批处理计算设置",WS_CAPTION|WS_SYSMENU|WS_POPUP,
        parent.left+((parent.right-parent.left)-(rect.right-rect.left))/2,parent.top+((parent.bottom-parent.top)-(rect.bottom-rect.top))/2,
        rect.right-rect.left,rect.bottom-rect.top,window,nullptr,instance,&ctx);
    if(!dialog){alert(L"无法打开硬件设置。");return;}
    hardwareDialog=dialog;
    EnableWindow(window,FALSE);ShowWindow(dialog,SW_SHOW);
    SetFocus(GetDlgItem(dialog,(busy||batchLocked())?IDCANCEL:5002));MSG msg{};BOOL received=1;
    while(!ctx.done&&(received=GetMessageW(&msg,nullptr,0,0))>0){if(!IsDialogMessageW(dialog,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    if(IsWindow(dialog))DestroyWindow(dialog);
    EnableWindow(window,TRUE);SetForegroundWindow(window);updateButtons();
    if(received==0)PostQuitMessage((int)msg.wParam);
}
void command(int id);
void showSettingsMenu(){
    auto* settings=getButton(SETTINGS);if(!settings)return;
    HMENU menu=CreatePopupMenu();if(!menu)return;
    AppendMenuW(menu,batchLocked()?MF_GRAYED:MF_STRING,HARDWARE,L"硬件");
    AppendMenuW(menu,MF_STRING,ABOUT,L"关于");
    POINT point{(LONG)lround(settings->box.X*dpi),(LONG)lround((settings->box.GetBottom()+2)*dpi)};ClientToScreen(window,&point);
    int selectedId=0;{ModalGuard modal;selectedId=TrackPopupMenu(menu,TPM_LEFTALIGN|TPM_TOPALIGN|TPM_RETURNCMD|TPM_RIGHTBUTTON,
                                  point.x,point.y,0,window,nullptr);}
    DestroyMenu(menu);
    if(selectedId==HARDWARE||selectedId==ABOUT)command(selectedId);
}
void leaveBatchPreview() {
    if(!batchPreviewMode)return;
    stopSmoothZoom();batchPreviewMode=false;batchPreviewLoader.invalidate();previewGeneration=0;releaseBatchPreviewBuffers();
    if(previewWindow)ShowWindow(previewWindow,SW_HIDE);
    status=L"已返回批处理列表 · 按卡片顺序处理勾选图片";
    resetView();updateButtons();
}
void enterBatchPreview() {
    if(!batchResultsReady())return;
    size_t first=firstBatchPreviewable();if(first<batchItems.size())loadBatchPreviewSource(first);
}
void switchBatchPreview(int direction) {
    if(!batchPreviewMode||batchItems.empty())return;
    size_t next=adjacentBatchPreviewable(batchPreviewIndex,direction);if(next!=batchPreviewIndex)loadBatchPreviewSource(next);
}
void exportBatchResults() {
    if(batchLocked())return;
    std::vector<std::wstring> successful;for(const auto& item:batchItems)if(item.selected&&batchItemSucceeded(item))successful.push_back(item.cache);
    if(successful.empty()){alert(L"没有可导出的成功结果。");return;}
    BROWSEINFOW browse{};browse.hwndOwner=window;browse.lpszTitle=L"选择批处理导出文件夹";browse.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;
    uint64_t generation=batchGeneration;PIDLIST_ABSOLUTE pidl=nullptr;{ModalGuard modal;pidl=SHBrowseForFolderW(&browse);}
    if(!pidl)return;
    if(generation!=batchGeneration||!canExecute(BATCH_EXPORT)){CoTaskMemFree(pidl);return;}
    wchar_t folder[MAX_PATH]{};bool got=SHGetPathFromIDListW(pidl,folder)!=FALSE;CoTaskMemFree(pidl);if(!got){alert(L"无法读取导出文件夹。");return;}
    DWORD folderError=ERROR_SUCCESS;
    if(!Batch::exportDestinationAvailable(folder,folderError)){
        Batch::emergency("Export selection is no longer a valid directory",folderError);
        std::wstring message=L"导出文件夹已失效，可能已被重命名、移动或删除。请重新选择文件夹后再导出。\n系统错误码："+std::to_wstring(folderError)+L"\n\n成功 PNG 缓存保留。";
        ModalGuard modal;alert(message);return;
    }
    try {batchExporter.start(std::move(successful),folder,number((int)GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));batchExportBusy=true;status=L"正在复制完成项PNG缓存…";updateButtons();}
    catch(...){alert(L"无法启动后台导出；PNG缓存已保留。");}
}
bool canExecute(int id){
    if(closing)return false;
    if(modalDepth&&id!=CANCEL&&id!=BATCH_CANCEL)return false;
    if(page==Page::Batch&&batchPhase==Batch::Phase::ErrorAwaitCancel)return id==BATCH_CANCEL;
    if(page==Page::Batch&&(id==OPEN||id==RUN||id==SAVE||id==CANCEL||id==CLEAR_IMAGE))return false;
    if(page==Page::Single&&id>=BATCH_RUN&&id<=BATCH_BACK)return false;
    switch(id){
        case OPEN:return !busy&&!batchLocked();case RUN:return source&&!busy&&!batchLocked();case SAVE:return output&&!busy&&!batchLocked();
        case CANCEL:return busy;case CLEAR_IMAGE:return source&&!busy&&!batchLocked();
        case PAGE_SWITCH:return !busy&&!batchLocked();case HARDWARE:return !batchLocked();case SETTINGS:case ABOUT:return true;
        case BATCH_RUN:return !busy&&!batchLocked()&&!batchPreviewMode&&selectedBatchCount()>0;
        case BATCH_EXPORT:return !busy&&!batchLocked()&&hasSuccessfulBatchResults();
        case BATCH_CANCEL:return batchExportBusy||(batchPhase!=Batch::Phase::Stopped&&batchPhase!=Batch::Phase::Cancelling);
        case BATCH_CLEAR:return !busy&&!batchLocked()&&!batchPreviewMode&&!batchItems.empty();
        case BATCH_PREVIEW:return !batchPreviewMode&&hasSuccessfulBatchResults();
        case BATCH_PREV:case BATCH_NEXT:case BATCH_BACK:return batchPreviewMode;
        case SCALE2:case SCALE3:case DENOISE:return !busy&&!batchLocked()&&!batchPreviewMode;
        case CONSERVATIVE:case CLEAN:return !busy&&!batchLocked()&&!batchPreviewMode&&scale==2;
        case LOCK_LEFT:return imageViewVisible();
        case OVERLAY:case ORIGINAL:case RESULT:return activePreviewSource()&&activePreviewOutput();
        case FIT:case ACTUAL:case ZOOM200:return activePreviewSource()!=nullptr;
        default:return false;
    }
}
void command(int id) {
    if(!canExecute(id))return;
    if(page==Page::Batch&&batchPhase==Batch::Phase::ErrorAwaitCancel&&id!=BATCH_CANCEL)return;
    if(id==HARDWARE&&batchLocked())return;
    if(batchDrag.pending||batchDrag.active)finishBatchDrag(false);
    // Hidden single-image actions must never start or save a single-image
    // task from the batch page, even if a stale command is still queued.
    if(page==Page::Batch&&(id==OPEN||id==RUN||id==SAVE||id==CANCEL||id==CLEAR_IMAGE))return;
    if(page==Page::Single&&id>=BATCH_RUN&&id<=BATCH_BACK)return;
    switch(id) {
        case OPEN:openImage();break;case RUN:runTask();break;case CANCEL:cancelTask();break;case CLEAR_IMAGE:clearCurrentImage();break;
        case SETTINGS:showSettingsMenu();return;
        case PAGE_SWITCH:
            if(busy||batchLocked())return;
            if(batchPreviewMode){stopSmoothZoom();batchPreviewMode=false;resetView();}
            page=page==Page::Single?Page::Batch:Page::Single;
            releaseBatchPreviewBuffers();batchPreviewLoader.invalidate();previewGeneration=0;
            if(previewWindow)ShowWindow(previewWindow,page==Page::Single?SW_SHOW:SW_HIDE);
            status=page==Page::Batch?L"已进入批处理界面 · 按卡片顺序处理勾选图片":L"已返回单独处理界面";
            updateButtons();return;
        case BATCH_CLEAR:
            if(!batchLocked()&&!batchPreviewMode&&!batchItems.empty()){batchPreviewLoader.invalidate();for(const auto& item:batchItems)retireCache(item.cache);batchItems.clear();batchActualSeconds=0;status=L"批处理图片列表已清空";updateButtons();}
            return;
        case BATCH_RUN:runBatchTask();return;
        case BATCH_CANCEL:cancelBatchTask();return;
        case BATCH_EXPORT:exportBatchResults();return;
        case BATCH_PREVIEW:enterBatchPreview();return;
        case BATCH_PREV:switchBatchPreview(-1);return;
        case BATCH_NEXT:switchBatchPreview(1);return;
        case BATCH_BACK:leaveBatchPreview();return;
        case LOCK_LEFT:
            lockLeft=!lockLeft;draggingPan=draggingSplit=false;
            if(GetCapture()==window||GetCapture()==previewWindow)ReleaseCapture();
            break;
        case SAVE:saveOutput();break;
        case SCALE2:if(!busy&&!batchLocked()&&!batchPreviewMode){scale=2;}break;
        case SCALE3:if(!busy&&!batchLocked()&&!batchPreviewMode){scale=3;noise=3;}break;
        case CONSERVATIVE:if(!busy&&!batchLocked()&&!batchPreviewMode&&scale==2){noise=-1;}break;
        case CLEAN:if(!busy&&!batchLocked()&&!batchPreviewMode&&scale==2){noise=0;}break;
        case DENOISE:if(!busy&&!batchLocked()&&!batchPreviewMode){noise=3;}break;
        case OVERLAY:case ORIGINAL:case RESULT:
            if(page==Page::Batch){if(batchPreviewMode){mode=id;repaintPreview();updateButtons();}return;}
            if(output){mode=id;repaintPreview();updateButtons();}return;
        case FIT:
            if(page==Page::Batch){if(batchPreviewMode){resetView();}return;}
            resetView();break;
        case ACTUAL:
            if(page==Page::Batch){if(batchPreviewMode){stopSmoothZoom();fitView=false;zoom=1/dpi;panX=panY=0;viewChanged();}return;}
            stopSmoothZoom();fitView=false;zoom=1/dpi;panX=panY=0;viewChanged();break;
        case ZOOM200:
            if(page==Page::Batch){if(batchPreviewMode){stopSmoothZoom();fitView=false;zoom=2/dpi;panX=panY=0;viewChanged();}return;}
            stopSmoothZoom();fitView=false;zoom=2/dpi;panX=panY=0;viewChanged();break;
        case HARDWARE:showHardware();break;
        case ABOUT:{ModalGuard modal;MessageBoxW(window,(std::wstring(L"Real-CUGAN ")+RCG_VERSION_WIDE+
            L"\n\n原生 Windows 窗口 · 所有图片均在本机处理"
            L"\n单图处理：CPU；批处理：CPU 串行或 Vulkan GPU。"
            L"\nGPU 设备手动选择，最大并发可选 1 / 2 / 4 路。"
            L"\n并发数为上限，预算不足时等待。"
            L"\n完成项可立即预览；整批停止并回收后可导出成功项。"
            L"\n导出复制 PNG 缓存，不重复编码。"
            L"\n支持透明 PNG、叠图拖动对比、缩放和平移。"
            L"\n预览加速与超分计算分别管理。"
            L"\n\n基于 hanFengSan/realcugan-ncnn-webassembly、ncnn 和 Real-CUGAN 模型。"
            L"\n第三方许可与来源见随包 licenses 文件夹。").c_str(),L"关于 Real-CUGAN",MB_OK|MB_ICONINFORMATION);break;}
    }
    updateButtons();
}
enum class BatchKeyAction { Unrecognized,Ignore,Import,Export,Run,CancelDrag,CancelTask };
BatchKeyAction batchKeyAction(UINT key,bool control,bool mainFocus) {
    if(page!=Page::Batch)return BatchKeyAction::Unrecognized;
    if(control&&key=='O')return batchLocked()||batchPreviewMode?BatchKeyAction::Ignore:BatchKeyAction::Import;
    if(control&&key=='S')return batchLocked()||!hasSuccessfulBatchResults()?BatchKeyAction::Ignore:BatchKeyAction::Export;
    if(key==VK_ESCAPE) {
        if(batchDrag.pending||batchDrag.active)return BatchKeyAction::CancelDrag;
        return batchLocked()?BatchKeyAction::CancelTask:BatchKeyAction::Ignore;
    }
    if(key==VK_RETURN&&mainFocus)return batchLocked()||batchPreviewMode||!selectedBatchCount()?BatchKeyAction::Ignore:BatchKeyAction::Run;
    return BatchKeyAction::Unrecognized;
}
void executeBatchKeyAction(BatchKeyAction action) {
    switch(action) {
        case BatchKeyAction::Import:
            if(batchDrag.pending||batchDrag.active)finishBatchDrag(false);
            openBatchImages();break;
        case BatchKeyAction::Export:command(BATCH_EXPORT);break;
        case BatchKeyAction::Run:command(BATCH_RUN);break;
        case BatchKeyAction::CancelDrag:finishBatchDrag(false);break;
        case BatchKeyAction::CancelTask:command(BATCH_CANCEL);break;
        default:break;
    }
}
bool handleBatchCanvasClick(float x,float y) {
    if(previewDisplaySuspended())return inside(batchViewport(),x,y);
    if(closing||modalDepth||batchLocked())return true;
    if(!inside(batchViewport(),x,y))return false;
    if(batchItems.empty()){
        if(Ui::importPanel(batchViewport()).hit(x,y)){openBatchImages();return true;}
        return true;
    }
    if(batchItems.size()<MAX_BATCH_IMAGES&&inside(batchContinueRect(),x,y)){openBatchImages();return true;}
    for(size_t i=0;i<batchItems.size();++i){
        RectF card=batchCardRect(i);
        auto content=BatchCards::content(card.Width,card.Height);
        auto at=[&](BatchCards::Box box){return RectF(card.X+box.x,card.Y+box.y,box.w,box.h);};
        if(inside(at(content.removeHit),x,y)){
            retireCache(batchItems[i].cache);batchPreviewLoader.invalidate();batchItems.erase(batchItems.begin()+i);batchActualSeconds=0;
            status=L"已从批处理列表移除图片";updateButtons();return true;
        }
        if(inside(at(content.selectionHit),x,y)){
            batchItems[i].selected=!batchItems[i].selected;
            batchActualSeconds=0;
            status=batchItems[i].selected?L"已勾选图片":L"已取消勾选图片";updateButtons();return true;
        }
        if(inside(card,x,y)) {
            auto first=batchCardRect(0);
            batchDrag.press(batchItems.size(),i,x,y,{first.X,first.Y,first.Width,first.Height,BATCH_CARD_GAP_X,BATCH_CARD_GAP_Y,BatchCards::Columns});
            batchPressedAt=GetTickCount64();SetFocus(window);SetCapture(window);
            SetTimer(window,BATCH_HOLD_TIMER,BATCH_HOLD_MS,nullptr);return true;
        }
    }
    return true;
}
void drawButton(const DRAWITEMSTRUCT* dis) {
    auto* b=getButton(dis->CtlID);if(!b)return;
    auto& cached=buttonBuffers[dis->CtlID];if(!cached)cached=std::make_unique<BackBuffer>();
    auto& buttonBuffer=*cached;
    if(!buttonBuffer.ensure(dis->hDC,dis->rcItem.right,dis->rcItem.bottom))return;
    // 先用 GDI 清满物理像素，避免圆角抗锯齿在边缘混入未初始化像素。
    bool toolbar=b->id==OVERLAY||b->id==ORIGINAL||b->id==RESULT||b->id==FIT||b->id==ACTUAL||b->id==ZOOM200;
    bool headerNav=b->id==SETTINGS||b->id==PAGE_SWITCH;
    bool modeOption=b->id==CONSERVATIVE||b->id==CLEAN||b->id==DENOISE;
    HBRUSH base=CreateSolidBrush(headerNav?RGB(24,34,51):toolbar||modeOption?RGB(244,246,250):RGB(255,255,255));
    FillRect(buttonBuffer.dc,&dis->rcItem,base);DeleteObject(base);
    {
    Graphics g(buttonBuffer.dc);g.SetSmoothingMode(SmoothingModeAntiAlias);g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    float w=dis->rcItem.right,h=dis->rcItem.bottom;g.ScaleTransform(dpi,dpi);w/=dpi;h/=dpi;
    bool disabled=dis->itemState&ODS_DISABLED,pressed=dis->itemState&ODS_SELECTED,chosen=selected(b->id);
    Color background;
    if(headerNav)background=disabled?Color(255,38,48,64):pressed?Color(255,58,78,108):Color(255,40,54,76);
    else if(modeOption)background=disabled?Color(255,238,241,246):chosen?blue:Color(255,232,237,246);
    else background=disabled?Color(255,243,245,248):b->primary?blue:chosen?Color(255,226,235,255):Color(255,239,242,247);
    if(pressed&&!disabled&&!headerNav&&!modeOption)background=b->primary?Color(255,34,83,194):Color(255,212,224,249);
    if(b->id==LOCK_LEFT){
        fill(g,RectF(0,0,w,h),Color(255,244,246,250));
        float cy=(h-14)/2;
        roundRect(g,RectF(2,cy,14,14),chosen?blue:Color(255,190,200,215),3);
        if(chosen){line(g,5,cy+7,8,cy+10,Color::White,1.6f);line(g,8,cy+10,13,cy+4,Color::White,1.6f);}
        else roundRect(g,RectF(3,cy+1,12,12),Color::White,2);
        text(g,b->label,RectF(23,0,w-25,h),FONT_BUTTON,ink);
    }else{
    roundRect(g,RectF(0,0,w,h),background,7);
    Color foreground=disabled?Color(255,177,186,198):headerNav?Color(255,214,226,242):b->primary?Color::White:chosen?Color::White:ink;
    float buttonFont=headerNav?FONT_SECONDARY:modeOption?FONT_BUTTON: b->primary?FONT_BUTTON_PRIMARY:FONT_BUTTON;
    text(g,b->label,RectF(2,0,w-4,h),buttonFont,foreground,b->primary||chosen||headerNav,1);
    }
    }
    BitBlt(dis->hDC,0,0,dis->rcItem.right,dis->rcItem.bottom,buttonBuffer.dc,0,0,SRCCOPY);
}
// 独立控件不使用系统 BUTTON 类，边框、背景和文字均由同一双缓冲提交。
LRESULT buttonProcImpl(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
        case WM_ERASEBKGND:case WM_NCPAINT:return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);PaintEnd paintEnd(hwnd,ps);RECT rect;GetClientRect(hwnd,&rect);
            DRAWITEMSTRUCT item{};item.CtlID=GetDlgCtrlID(hwnd);item.hwndItem=hwnd;item.hDC=dc;item.rcItem=rect;
            if(!IsWindowEnabled(hwnd))item.itemState|=ODS_DISABLED;
            if(GetCapture()==hwnd&&GetWindowLongPtrW(hwnd,GWLP_USERDATA))item.itemState|=ODS_SELECTED;
            drawButton(&item);paintEnd.finish();return 0;
        }
        case WM_ENABLE:case WM_SETFOCUS:case WM_KILLFOCUS:InvalidateRect(hwnd,nullptr,FALSE);return 0;
        case WM_GETDLGCODE:return DLGC_BUTTON|DLGC_UNDEFPUSHBUTTON;
        case BM_SETSTYLE:return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);SetWindowLongPtrW(hwnd,GWLP_USERDATA,1);SetCapture(hwnd);InvalidateRect(hwnd,nullptr,FALSE);return 0;
        }
        case WM_MOUSEMOVE:
            if(GetCapture()==hwnd){RECT r;GetClientRect(hwnd,&r);POINT pt{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
                LONG_PTR pressed=PtInRect(&r,pt);if(pressed!=GetWindowLongPtrW(hwnd,GWLP_USERDATA)){
                    SetWindowLongPtrW(hwnd,GWLP_USERDATA,pressed);InvalidateRect(hwnd,nullptr,FALSE);}}
            return 0;
        case WM_LBUTTONUP: {
            bool click=GetCapture()==hwnd&&GetWindowLongPtrW(hwnd,GWLP_USERDATA)&&IsWindowEnabled(hwnd);
            if(GetCapture()==hwnd){ReleaseCapture();}
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,0);InvalidateRect(hwnd,nullptr,FALSE);
            if(click){SendMessageW(GetParent(hwnd),WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(hwnd),BN_CLICKED),(LPARAM)hwnd);}
            return 0;
        }
        case WM_CAPTURECHANGED:case WM_CANCELMODE:SetWindowLongPtrW(hwnd,GWLP_USERDATA,0);InvalidateRect(hwnd,nullptr,FALSE);return 0;
        case WM_KEYDOWN:if(wp==VK_SPACE||wp==VK_RETURN){if(IsWindowEnabled(hwnd)&&!(lp&(1L<<30)))SendMessageW(GetParent(hwnd),WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(hwnd),BN_CLICKED),(LPARAM)hwnd);return 0;}break;
        case WM_LBUTTONDBLCLK:case WM_RBUTTONDOWN:case WM_RBUTTONUP:case WM_CONTEXTMENU:return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
void addButton(int id,const wchar_t* label,bool primary=false) {
    HWND h=CreateWindowExW(0,L"RealCUGANFlatButton",label,WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,1,1,window,(HMENU)(INT_PTR)id,instance,nullptr);
    buttons.push_back({id,h,{},label,primary});
}
LRESULT wndProcImpl(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
        case WM_CREATE:
            window=hwnd;DragAcceptFiles(hwnd,TRUE);
            {auto work=monitorWorkRect();HDC dc=GetDC(hwnd);
             if(dc){backBuffer.ensure(dc,work.right-work.left,work.bottom-work.top,true);ReleaseDC(hwnd,dc);}}
            addButton(SCALE2,L"2 倍");addButton(SCALE3,L"3 倍");
            addButton(CONSERVATIVE,L"保守");addButton(CLEAN,L"无降噪");addButton(DENOISE,L"强降噪");
            addButton(RUN,L"开始处理",true);addButton(CANCEL,L"取消处理");addButton(CLEAR_IMAGE,L"删除图片");addButton(SAVE,L"保存 PNG",true);
            addButton(OVERLAY,L"叠图");addButton(ORIGINAL,L"原图");addButton(RESULT,L"处理后");
            addButton(FIT,L"适应窗口");addButton(ACTUAL,L"100%");addButton(ZOOM200,L"200%");addButton(LOCK_LEFT,L"锁定左键");
            addButton(SETTINGS,L"设置");addButton(PAGE_SWITCH,L"批处理");
            addButton(BATCH_RUN,L"开始批处理",true);
            addButton(BATCH_EXPORT,L"批量导出",true);addButton(BATCH_PREVIEW,L"进入预览",true);addButton(BATCH_CLEAR,L"清空列表");
            addButton(BATCH_CANCEL,L"取消处理");addButton(BATCH_PREV,L"上一张");addButton(BATCH_NEXT,L"下一张");addButton(BATCH_BACK,L"返回批处理列表");
            previewWindow=CreateWindowExW(0,L"RealCUGANRealtimePreview",L"",WS_CHILD,0,0,1,1,hwnd,nullptr,instance,nullptr);
            DragAcceptFiles(previewWindow,TRUE);
            createTooltip();batchNotification.bind(hwnd);SetTimer(hwnd,BATCH_EVENT_TIMER,250,nullptr);
            updateButtons();return 0;
        case WM_NOTIFY:
            if(((NMHDR*)lp)->hwndFrom==tooltipWindow&&((NMHDR*)lp)->code==TTN_GETDISPINFOW) {
                POINT point{};GetCursorPos(&point);ScreenToClient(hwnd,&point);
                tooltipText=tooltipAt(point.x/dpi,point.y/dpi);((NMTTDISPINFOW*)lp)->lpszText=(LPWSTR)tooltipText.c_str();return 0;
            }
            break;
        case WM_SYSCOMMAND:
            if(!closing&&((wp&0xfff0)==SC_MAXIMIZE||(wp&0xfff0)==SC_RESTORE||(wp&0xfff0)==SC_MINIMIZE)){
                stopSmoothZoom();deferPreviewResize();invalidateSuspendedPreview();
            }
            break;
        case WM_ENTERSIZEMOVE:
            if(batchDrag.pending||batchDrag.active)finishBatchDrag(false);
            inSizeMove=true;resizingWindow=false;stopSmoothZoom();KillTimer(hwnd,PREVIEW_RETRY_TIMER);return 0;
        case WM_SIZING:
            if(!resizingWindow){resizingWindow=true;deferPreviewResize();invalidateSuspendedPreview();}
            return TRUE;
        case WM_EXITSIZEMOVE:
            inSizeMove=false;
            if(resizingWindow){resizingWindow=false;deferPreviewResize();invalidateSuspendedPreview();}
            if(batchRefreshDeferred){batchRefreshDeferred=false;batchNotification.notify();}
            if(singleFinishDeferred){singleFinishDeferred=false;PostMessageW(hwnd,WM_FINISHED,0,0);}
            if(hardwareReadyDeferred){hardwareReadyDeferred=false;PostMessageW(hwnd,WM_HARDWARE_READY,0,0);}
            if(previewSetupBusy&&previewSetupDone.load(std::memory_order_acquire))PostMessageW(hwnd,WM_PREVIEW_READY,0,0);
            if(imageViewVisible())repaintPreview();
            return 0;
        case WM_SIZE:{
            if(batchDrag.pending||batchDrag.active)finishBatchDrag(false);
            if(wp==SIZE_MINIMIZED){stopSmoothZoom();deferPreviewResize();return 0;}
            float width=LOWORD(lp)/dpi,height=HIWORD(lp)/dpi;
            if(layoutReady&&width==clientW&&height==clientH){if(previewResizePending){deferPreviewResize();invalidateSuspendedPreview();}return 0;}
            if(inSizeMove)resizingWindow=true;
            stopSmoothZoom();deferPreviewResize();clientW=width;clientH=height;layout();invalidateSuspendedPreview();return 0;
        }
        case WM_DPICHANGED: {
            if(batchDrag.pending||batchDrag.active)finishBatchDrag(false);
            stopSmoothZoom();deferPreviewResize();invalidateSuspendedPreview();auto suggested=*(RECT*)lp;auto work=monitorWorkRect(&suggested);
            float logicalW=clientW,logicalH=clientH;UINT nativeDpi=HIWORD(wp);
            dpi=fittedDpi(nativeDpi,work);batchCardCache.clear();batchCardCacheKeys.clear();
            for(auto& item:buttonBuffers){item.second->clear();}
            RECT target{0,0,(LONG)std::lround(logicalW*dpi),(LONG)std::lround(logicalH*dpi)};
            adjustFrame(target,(DWORD)GetWindowLongPtrW(hwnd,GWL_STYLE),(DWORD)GetWindowLongPtrW(hwnd,GWL_EXSTYLE),nativeDpi);
            int width=std::min((int)(target.right-target.left),(int)(work.right-work.left));
            int height=std::min((int)(target.bottom-target.top),(int)(work.bottom-work.top));
            int x=std::clamp((int)suggested.left,(int)work.left,std::max((int)work.left,(int)work.right-width));
            int y=std::clamp((int)suggested.top,(int)work.top,std::max((int)work.top,(int)work.bottom-height));
            SetWindowPos(hwnd,nullptr,x,y,width,height,SWP_NOACTIVATE|SWP_NOZORDER);
            if(tooltipWindow)SendMessageW(tooltipWindow,TTM_SETMAXTIPWIDTH,0,(LPARAM)std::lround(480*dpi));
            updateButtons();return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* m=(MINMAXINFO*)lp;RECT minimum{0,0,(LONG)std::ceil(1050*dpi),(LONG)std::ceil(760*dpi)};
            adjustFrame(minimum,WS_OVERLAPPEDWINDOW,0,nativeWindowDpi(hwnd));
            m->ptMinTrackSize.x=minimum.right-minimum.left;m->ptMinTrackSize.y=minimum.bottom-minimum.top;return 0;
        }
        case WM_ERASEBKGND:return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);PaintEnd paintEnd(hwnd,ps);RECT r;GetClientRect(hwnd,&r);
            RectF dirty(ps.rcPaint.left/dpi,ps.rcPaint.top/dpi,(ps.rcPaint.right-ps.rcPaint.left)/dpi,(ps.rcPaint.bottom-ps.rcPaint.top)/dpi);
            auto draw=[&](HDC target){Graphics g(target);g.ScaleTransform(dpi,dpi);g.SetClip(dirty);
                    // 后备位图也排除子窗口区域，避免重复绘制大幅预览背后的背景。
                    if(imageViewVisible())g.SetClip(viewport(),CombineModeExclude);
                    for(const auto& b:buttons)if(IsWindowVisible(b.hwnd))g.SetClip(b.box,CombineModeExclude);
                    paint(g,dirty);};
            if(backBuffer.ensure(dc,r.right,r.bottom,true)) {
                draw(backBuffer.dc);
                BitBlt(dc,ps.rcPaint.left,ps.rcPaint.top,ps.rcPaint.right-ps.rcPaint.left,ps.rcPaint.bottom-ps.rcPaint.top,
                       backBuffer.dc,ps.rcPaint.left,ps.rcPaint.top,SRCCOPY);
            }else draw(dc); // Allocation failure still paints a valid background.
            paintEnd.finish();return 0;
        }
        case WM_DRAWITEM:drawButton((DRAWITEMSTRUCT*)lp);return TRUE;
        case WM_COMMAND:if(HIWORD(wp)==BN_CLICKED)command(LOWORD(wp));return 0;
        case WM_DROPFILES: {
            if(batchDrag.pending||batchDrag.active)finishBatchDrag(false);
            HDROP drop=(HDROP)wp;if(closing||modalDepth){DragFinish(drop);return 0;}UINT count=DragQueryFileW(drop,0xFFFFFFFF,nullptr,0);
            if(page==Page::Batch){
                if(batchLocked()||batchPreviewMode){DragFinish(drop);return 0;}
                std::vector<std::wstring> paths;paths.reserve(count);
                for(UINT i=0;i<count;++i){UINT length=DragQueryFileW(drop,i,nullptr,0);std::vector<wchar_t> path(length+1);DragQueryFileW(drop,i,path.data(),(UINT)path.size());paths.emplace_back(path.data());}
                appendBatchImages(paths);
            }
            else if(!busy&&count){std::vector<wchar_t> p(DragQueryFileW(drop,0,nullptr,0)+1);DragQueryFileW(drop,0,p.data(),p.size());loadImage(p.data());}
            DragFinish(drop);return 0;
        }
        case WM_LBUTTONDOWN: {
            if(batchPhase==Batch::Phase::ErrorAwaitCancel)return 0;
            if(page==Page::Batch&&!batchPreviewMode){handleBatchCanvasClick(GET_X_LPARAM(lp)/dpi,GET_Y_LPARAM(lp)/dpi);return 0;}
            float x=GET_X_LPARAM(lp)/dpi,y=GET_Y_LPARAM(lp)/dpi;
            if(previewDisplaySuspended()&&inside(batchViewport(),x,y))return 0;
            Pixels* previewSource=activePreviewSource();Pixels* previewOutput=activePreviewOutput();
            if(!previewSource&&page==Page::Single) {
                if(Ui::importPanel(batchViewport()).hit(x,y))openImage();
                return 0;
            }
            if(previewSource&&inside(viewport(),x,y)) {
                SetFocus(hwnd);mouseX=x;mouseY=y;
                auto v=viewport();
                float divider=v.X+v.Width*split;
                draggingSplit=previewOutput&&mode==OVERLAY&&Preview::hitDivider(
                    x,y,divider,v.Y,v.Height);
                draggingPan=!lockLeft&&!draggingSplit&&inside(imageRect(),x,y);
                if(draggingPan)stopSmoothZoom();
                // 按下分界条时保留抓取偏移，不让分界线突然跳到鼠标位置。
                if(draggingSplit)splitGrabOffset=x-divider;
                if(draggingPan||draggingSplit)SetCapture(hwnd);
            }return 0;
        }
        case WM_MOUSEMOVE: {
            if(page==Page::Batch&&!batchPreviewMode){
                if(batchDrag.pending||batchDrag.active){
                    if(!(wp&MK_LBUTTON))finishBatchDrag(false);
                    else moveBatchDrag(GET_X_LPARAM(lp)/dpi,GET_Y_LPARAM(lp)/dpi);
                }
                return 0;
            }
            if(page!=Page::Single&&page!=Page::Batch)return 0;
            float x=GET_X_LPARAM(lp)/dpi,y=GET_Y_LPARAM(lp)/dpi;
            if(draggingSplit){auto v=viewport();split=std::clamp((x-splitGrabOffset-v.X)/v.Width,0.f,1.f);repaintPreview();}
            if(draggingPan){stopSmoothZoom();panX+=x-mouseX;panY+=y-mouseY;viewChanged();}
            if((draggingPan||draggingSplit)&&previewWindow)UpdateWindow(previewWindow);
            mouseX=x;mouseY=y;return 0;
        }
        case WM_LBUTTONUP:
            if(batchDrag.pending||batchDrag.active){
                // Account for a final pointer position even if Windows coalesced
                // the last move. A short click never activates on release.
                if(batchDrag.active)moveBatchDrag(GET_X_LPARAM(lp)/dpi,GET_Y_LPARAM(lp)/dpi);
                finishBatchDrag(true);return 0;
            }
            draggingSplit=draggingPan=false;if(GetCapture()==hwnd)ReleaseCapture();return 0;
        case WM_RBUTTONDOWN:case WM_RBUTTONUP:case WM_RBUTTONDBLCLK:return 0;
        case WM_CAPTURECHANGED:
            if(batchDrag.pending||batchDrag.active)finishBatchDrag(false);
            draggingSplit=draggingPan=false;return 0;
        case WM_CANCELMODE:
            if(batchDrag.pending||batchDrag.active){finishBatchDrag(false);return 0;}
            break;
        case WM_KEYDOWN:
            if(wp==VK_ESCAPE&&(batchDrag.pending||batchDrag.active)){finishBatchDrag(false);return 0;}
            break;
        case WM_LBUTTONDBLCLK:
            if(batchPhase==Batch::Phase::ErrorAwaitCancel)return 0;
            if(!previewDisplaySuspended()&&(page==Page::Single||batchPreviewMode)&&!lockLeft&&activePreviewSource())resetView();
            return 0;
        case WM_MOUSEWHEEL: {
            if(closing||previewDisplaySuspended())return 0;
            if(batchPhase==Batch::Phase::ErrorAwaitCancel)return 0;
            if(page==Page::Batch&&!batchPreviewMode)return 0;
            if(page!=Page::Single&&page!=Page::Batch)return 0;
            Pixels* previewSource=activePreviewSource();Pixels* previewOutput=activePreviewOutput();
            if(!previewSource){return 0;}
            POINT pt{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(hwnd,&pt);
            float x=pt.x/dpi,y=pt.y/dpi;if(!inside(viewport(),x,y))return 0;
            double now=monotonicSeconds();
            if(smoothZoom.active)advanceZoom(now);
            RectF ir=imageRect();float iw=previewOutput?previewOutput->w:previewSource->w;
            wheelRemainder+=GET_WHEEL_DELTA_WPARAM(wp);
            int steps=wheelRemainder/WHEEL_DELTA;wheelRemainder%=WHEEL_DELTA;
            if(!steps)return 0;
            float old=ir.Width/iw;
            // 保留正在运动的速度与目标倍率，追加滚轮输入不重置运动。
            if(!smoothZoom.active){smoothZoom.reset(old*dpi);zoomClock=now;}
            zoomAnchorX=x;zoomAnchorY=y;zoomFractionX=(x-ir.X)/ir.Width;zoomFractionY=(y-ir.Y)/ir.Height;
            smoothZoom.wheel(steps);zoom=old;fitView=false;
            repaintPreview();if(previewWindow)UpdateWindow(previewWindow);
            if(!frameArmed){nextFrameTime=now;armZoomFrame();}
            return 0;
        }
        case WM_SETCURSOR: {
            if(LOWORD(lp)==HTCLIENT&&previewDisplaySuspended()){
                POINT point{};GetCursorPos(&point);ScreenToClient(hwnd,&point);
                if(inside(batchViewport(),point.x/dpi,point.y/dpi)){SetCursor(LoadCursorW(nullptr,IDC_ARROW));return TRUE;}
            }
            if(LOWORD(lp)==HTCLIENT&&!imageViewVisible()&&(page==Page::Single||batchItems.empty())) {
                POINT point{};GetCursorPos(&point);ScreenToClient(hwnd,&point);
                SetCursor(LoadCursorW(nullptr,Ui::importPanel(batchViewport()).hit(point.x/dpi,point.y/dpi)?IDC_HAND:IDC_ARROW));return TRUE;
            }
            if(page==Page::Batch&&!batchPreviewMode){
                if(batchDrag.active){SetCursor(LoadCursorW(nullptr,IDC_SIZEALL));return TRUE;}
                return DefWindowProcW(hwnd,msg,wp,lp);
            }
            if(page!=Page::Single&&page!=Page::Batch)return DefWindowProcW(hwnd,msg,wp,lp);
            POINT p;GetCursorPos(&p);ScreenToClient(hwnd,&p);
            if(LOWORD(lp)==HTCLIENT&&inside(viewport(),p.x/dpi,p.y/dpi)&&activePreviewSource()) {
                auto v=viewport();
                bool overDivider=activePreviewOutput()&&mode==OVERLAY&&Preview::hitDivider(
                    p.x/dpi,p.y/dpi,v.X+v.Width*split,v.Y,v.Height);
                SetCursor(LoadCursorW(nullptr,draggingPan?IDC_SIZEALL:draggingSplit||overDivider?IDC_SIZEWE:
                    !lockLeft&&inside(imageRect(),p.x/dpi,p.y/dpi)?IDC_HAND:IDC_ARROW));return TRUE;
            }break;
        }
        case WM_CONTEXTMENU:return 0;
        case WM_REFRESH_PERMISSIONS:updateButtons();return 0;
        case WM_BATCH_READY:batchNotification.acknowledge();pollBatchEvents();return 0;
        case WM_TIMER:
            if(wp==PREVIEW_RETRY_TIMER){KillTimer(hwnd,PREVIEW_RETRY_TIMER);if(!closing&&!previewDisplaySuspended()&&imageViewVisible())repaintPreview();return 0;}
            if(wp==PREVIEW_RESIZE_TIMER){
                KillTimer(hwnd,PREVIEW_RESIZE_TIMER);
                if(!previewResizePending||closing||resizingWindow||IsIconic(hwnd))return 0;
                ULONGLONG now=GetTickCount64();
                // KillTimer does not remove already queued messages for an older size.
                if(now<previewResizeReadyAt){
                    if(SetTimer(hwnd,PREVIEW_RESIZE_TIMER,(UINT)(previewResizeReadyAt-now),nullptr))return 0;
                    Batch::emergency("preview resize timer rearm unavailable");
                }
                previewResizePending=false;
                if(previewSetupBusy&&previewSetupDone.load(std::memory_order_acquire))PostMessageW(hwnd,WM_PREVIEW_READY,0,0);
                repaint();
                if(imageViewVisible())repaintPreview();
                return 0;
            }
            if(wp==CLOSE_POLL_TIMER){
                if(!taskDone||!hardwareDone||!previewSetupDone||!batchEngine.done()||batchEngine.cleanupPending()||!batchPreviewLoader.done()||!batchExporter.done()||!gpuBudget.done()){
                    static ULONGLONG notice=0;if(GetTickCount64()>=notice){Batch::emergency("close pending; live resources remain locked");notice=GetTickCount64()+5000;}return 0;}
                KillTimer(hwnd,CLOSE_POLL_TIMER);KillTimer(hwnd,BATCH_EVENT_TIMER);
                if(task.joinable()){task.join();}
                if(hardwareScan.joinable()){hardwareScan.join();}
                joinPreviewSetup();
                batchEngine.join();batchPreviewLoader.stop();batchExporter.stop();gpuBudget.stop();
                WritePrivateProfileStringW(L"Preferences",L"Scale",number(scale).c_str(),settingsPath.c_str());
                WritePrivateProfileStringW(L"Preferences",L"Noise",number(noise).c_str(),settingsPath.c_str());DestroyWindow(hwnd);return 0;
            }
            if(wp==BATCH_EVENT_TIMER){
                pollBatchEvents();
                if(!closing&&!inSizeMove){
                    if(busy&&taskDone.load(std::memory_order_acquire))PostMessageW(hwnd,WM_FINISHED,0,0);
                    if(hardwareScanBusy&&hardwareDone.load(std::memory_order_acquire))PostMessageW(hwnd,WM_HARDWARE_READY,0,0);
                }
                if(previewSetupBusy&&previewSetupDone.load(std::memory_order_acquire)&&!closing&&!inSizeMove&&!previewDisplaySuspended())
                    PostMessageW(hwnd,WM_PREVIEW_READY,0,0);
                return 0;
            }
            if(wp==BATCH_HOLD_TIMER){activateBatchDrag();return 0;}
            if(wp==BATCH_REFLOW_TIMER){animateBatchReflow();return 0;}
            if(wp==1&&!inSizeMove&&busy)repaintProgress();
            if(wp==1&&!inSizeMove&&batchLocked())repaintBatchProgress();
            return 0;
        case WM_HARDWARE_READY:
            if(!hardwareScanBusy||!hardwareDone.load(std::memory_order_acquire))return 0;
            if(inSizeMove&&!closing){hardwareReadyDeferred=true;return 0;}
            if(hardwareScan.joinable())hardwareScan.join();
            hardwareScanBusy=false;
            if(hardwarePending){hardware=std::move(*hardwarePending);hardwarePending.reset();requestedThreads=std::clamp(requestedThreads,0,hardware.available);}
            else hardware.gpuDetectionFailed=true;
            computeDevices=std::move(pendingComputeDevices);computeDiagnostics=std::move(pendingComputeDiagnostics);
            if(hardwareDialog&&IsWindow(hardwareDialog))populateHardware(hardwareDialog,true);
            updateButtons();return 0;
        case WM_PREVIEW_READY:
            // A queued/stale message must never make the UI wait for a live setup thread.
            if(!previewSetupBusy||!previewSetupDone.load(std::memory_order_acquire))return 0;
            if(!closing&&(inSizeMove||previewDisplaySuspended()))return 0;
            joinPreviewSetup();previewSetupBusy=false;
            if(closing){preparedPreview.reset();return 0;}
            if(preparedPreview&&preparedPreview->matchesSurface(previewWindow,dpi)){
                realtimePreview.adopt(*preparedPreview);preparedPreview.reset();previewSetupFailed=false;
            }else {
                RECT bounds{};GetClientRect(previewWindow,&bounds);
                bool stale=preparedPreview||previewSetupWidth!=(UINT)std::max(1L,bounds.right)||previewSetupHeight!=(UINT)std::max(1L,bounds.bottom)||previewSetupDpi!=dpi;
                preparedPreview.reset();previewSetupFailed=!stale&&!preparedPreviewDeferred.load();
                if(!previewSetupFailed)SetTimer(window,PREVIEW_RETRY_TIMER,stale?120:1000,nullptr);
            }
            if(imageViewVisible())repaintPreview();
            if(hardwareDialog&&IsWindow(hardwareDialog))populateHardware(hardwareDialog,true);
            return 0;
        case WM_PROGRESS:progress=(int)wp;remainingMs=(long)lp;if(!inSizeMove)repaintProgress();return 0;

        case WM_FINISHED:
            if(!busy||!taskDone.load(std::memory_order_acquire))return 0;
            if(inSizeMove&&!closing){singleFinishDeferred=true;return 0;}
            if(task.joinable()){task.join();}
            if(closing){completed.reset();singleWorkMemory.reset();busy=false;return 0;}
            busy=false;KillTimer(hwnd,1);
            if(completed&&completed->pixels) {
                try {
                    output=std::move(completed->pixels);output->build();mode=OVERLAY;
                    if(output->bitmap->GetLastStatus()!=Ok)throw std::bad_alloc();
                    wchar_t t[128];swprintf(t,128,L"处理完成 · 耗时 %.1f 秒 · 可保存透明 PNG",completed->seconds);status=t;resetView();
                } catch(...) {output.reset();completed->error=L"结果预览所需内存不足，请尝试较小的图片。";status=L"预览加载失败";}
            } else {status=completed&&!completed->error.empty()?L"处理失败 · 可重新尝试":L"已取消处理";}
            if(singleEmergency.exchange(false)){status=L"单图后台异常，详情见故障日志";}
            updateButtons();if(completed&&!completed->error.empty()&&!closing)alert(completed->error);completed.reset();singleWorkMemory.reset();return 0;
        case WM_CLOSE:
            if(closing)return 0;
            if(batchDrag.pending||batchDrag.active)finishBatchDrag(false);
            stopSmoothZoom();closing=true;KillTimer(hwnd,PREVIEW_RETRY_TIMER);KillTimer(hwnd,PREVIEW_RESIZE_TIMER);SetEvent(cancelEvent);
            batchEngine.cancel();batchPreviewLoader.requestStop();batchExporter.requestStop();gpuBudget.requestStop();
            SetTimer(hwnd,CLOSE_POLL_TIMER,100,nullptr);status=L"正在关闭并回收资源…";updateButtons();return 0;
        case WM_DESTROY:
            batchNotification.bind(nullptr);
            joinPreviewSetup();preparedPreview.reset();
            for(auto& surface:importSurfaces)surface.bitmap.reset();
            uiFonts.clear();measuredHeights.clear();measureBitmap.reset();batchCardCache.clear();batchCardCacheKeys.clear();
            backBuffer.clear();for(auto& item:buttonBuffers)item.second->clear();buttonBuffers.clear();PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
LRESULT CALLBACK previewProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) noexcept {try{return previewProcImpl(hwnd,msg,wp,lp);}catch(...){Batch::emergency("previewProc exception");return 0;}}
LRESULT CALLBACK hardwareProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) noexcept {try{return hardwareProcImpl(hwnd,msg,wp,lp);}catch(...){Batch::emergency("hardwareProc exception");return 0;}}
LRESULT CALLBACK buttonProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) noexcept {try{return buttonProcImpl(hwnd,msg,wp,lp);}catch(...){Batch::emergency("buttonProc exception");return 0;}}
LRESULT CALLBACK wndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) noexcept {
    try{return wndProcImpl(hwnd,msg,wp,lp);}catch(...){Batch::emergency("UI dispatch exception; no automatic retry");return 0;}
}
} // namespace

int WINAPI wWinMain(HINSTANCE h,HINSTANCE,LPWSTR,int show) {
    instance=h;
    if(!SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL))Batch::processNotice("UI thread priority unavailable",GetLastError());
    HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(com)){MessageBoxW(nullptr,L"无法初始化主线程COM环境，程序未启动。",L"启动失败",MB_OK|MB_ICONERROR);return 7;}
    struct ComLifetime {~ComLifetime(){CoUninitialize();}} comLifetime;
    using Awareness=BOOL(WINAPI*)(HANDLE);
    auto awareness=userApi<Awareness>("SetProcessDpiAwarenessContext");
    if(awareness)awareness((HANDLE)(INT_PTR)-4);else SetProcessDPIAware();
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    // 在小屏幕或较高系统缩放下，确保初始窗口完整位于工作区内。
    auto work=monitorWorkRect();UINT nativeDpi=nativeWindowDpi(nullptr);dpi=fittedDpi(nativeDpi,work);
    GdiplusStartupInput gdip;if(GdiplusStartup(&gdiplusToken,&gdip,nullptr)!=Ok)return 1;
    try {
        directory=Batch::executableDirectory();
        wchar_t appData[MAX_PATH]{};
        if(FAILED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,SHGFP_TYPE_CURRENT,appData))||!appData[0])throw std::runtime_error("Local application data directory unavailable");
        settingsDir=std::wstring(appData)+L"\\RealCUGAN-Desktop";
        if(!CreateDirectoryW(settingsDir.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)throw std::runtime_error("Settings directory creation failed");
        DWORD attributes=GetFileAttributesW(settingsDir.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_DIRECTORY))throw std::runtime_error("Settings path is not a directory");
        settingsPath=settingsDir+L"\\settings.ini";
        logsDir=Batch::runtimeDirectory(L"logs");Batch::setEmergencyDirectory(logsDir);
        hardware=Hardware::detectCpu();
    }catch(const std::exception& error){Batch::emergency(error.what());MessageBoxW(nullptr,L"初始化目录或硬件信息失败。请检查用户配置目录及程序目录的写入权限。",L"启动失败",MB_OK|MB_ICONERROR);GdiplusShutdown(gdiplusToken);return 1;}
     catch(...){Batch::emergency("Startup initialization exception");MessageBoxW(nullptr,L"程序初始化失败，请检查系统资源后重试。",L"启动失败",MB_OK|MB_ICONERROR);GdiplusShutdown(gdiplusToken);return 1;}
    Batch::processDiagnostic=Batch::emergency;
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
    try{appBudget.initialize();if(!uiMemory.acquire(appBudget,64*Batch::MiB))throw std::runtime_error("UI memory admission unavailable");batchSession.create();}catch(const std::exception& error){status=L"资源预算初始化失败："+Batch::wide(error.what());}
    wchar_t gpuKey[128]{};GetPrivateProfileStringW(L"Preferences",L"BatchGpu",L"",gpuKey,128,settingsPath.c_str());requestedGpuKey=gpuKey;
    requestedConcurrency=(int)GetPrivateProfileIntW(L"Preferences",L"GpuConcurrency",1,settingsPath.c_str());
    if(requestedConcurrency!=2&&requestedConcurrency!=4)requestedConcurrency=1;
    requestedThreads=(int)GetPrivateProfileIntW(L"Preferences",L"Threads",0,settingsPath.c_str());
    requestedThreads=std::clamp(requestedThreads,0,hardware.available);
    scale=GetPrivateProfileIntW(L"Preferences",L"Scale",2,settingsPath.c_str());if(scale!=2&&scale!=3)scale=2;
    noise=(int)GetPrivateProfileIntW(L"Preferences",L"Noise",-1,settingsPath.c_str());if(noise!=-1&&noise!=0&&noise!=3)noise=-1;if(scale==3)noise=3;
    cancelEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!cancelEvent)return 2;
    WNDCLASSW wc{};wc.style=CS_DBLCLKS;wc.lpfnWndProc=wndProc;wc.hInstance=h;
    appIcon=LoadIconW(h,MAKEINTRESOURCEW(1));
    appLogo=loadEmbeddedLogo();
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=appIcon;wc.lpszClassName=L"RealCUGANDesktopWindow";RegisterClassW(&wc);
    WNDCLASSW buttonClass{};buttonClass.lpfnWndProc=buttonProc;buttonClass.hInstance=h;
    buttonClass.hCursor=LoadCursorW(nullptr,IDC_ARROW);buttonClass.lpszClassName=L"RealCUGANFlatButton";
    if(!RegisterClassW(&buttonClass))return 4;
    WNDCLASSW previewClass{};previewClass.style=CS_DBLCLKS;previewClass.lpfnWndProc=previewProc;previewClass.hInstance=h;
    previewClass.hCursor=LoadCursorW(nullptr,IDC_ARROW);previewClass.lpszClassName=L"RealCUGANRealtimePreview";
    if(!RegisterClassW(&previewClass))return 5;
    WNDCLASSW hardwareClass{};hardwareClass.lpfnWndProc=hardwareProc;hardwareClass.hInstance=h;
    hardwareClass.hCursor=LoadCursorW(nullptr,IDC_ARROW);hardwareClass.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);hardwareClass.lpszClassName=L"RealCUGANHardware";
    if(!RegisterClassW(&hardwareClass))return 6;
    RECT rect{0,0,int(1280*dpi),int(800*dpi)};adjustFrame(rect,WS_OVERLAPPEDWINDOW,0,nativeDpi);
    HWND hwnd=CreateWindowW(wc.lpszClassName,L"Real-CUGAN · 图片超分辨率",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
        work.left+((work.right-work.left)-(rect.right-rect.left))/2,work.top+((work.bottom-work.top)-(rect.bottom-rect.top))/2,
        rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,h,nullptr);
    if(!hwnd){CloseHandle(cancelEvent);GdiplusShutdown(gdiplusToken);return 3;}
    ShowWindow(hwnd,show);UpdateWindow(hwnd);
    startHardwareScan();
    int argc=0;wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(argc>1){try{loadImage(argv[1]);}catch(...){Batch::emergency("command-line image import failed");}}
    if(argv){LocalFree(argv);}
    DEVMODEW display{};display.dmSize=sizeof(display);
    if(EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&display)&&display.dmDisplayFrequency>=30)
        frameInterval=1.0/std::clamp((double)display.dmDisplayFrequency,30.0,240.0);
    // 优先使用高精度可等待计时器，旧系统支持普通可等待计时器回退。
    frameTimer=CreateWaitableTimerExW(nullptr,nullptr,0x00000002,TIMER_ALL_ACCESS);
    if(!frameTimer)frameTimer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
    if(!frameTimer){alert(L"无法创建预览帧计时器，请重启程序后重试。");DestroyWindow(hwnd);}
    MSG msg;bool running=true;
    while(running) {
        HANDLE handles[]{frameTimer};
        DWORD ready=MsgWaitForMultipleObjectsEx(frameTimer?1:0,handles,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        if(frameTimer&&ready==WAIT_OBJECT_0)renderZoomFrame();
        int batch=0;
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){
        if(msg.message==WM_QUIT){running=false;break;}
        if(msg.message==WM_KEYDOWN) {
            auto batchAction=batchKeyAction((UINT)msg.wParam,(GetKeyState(VK_CONTROL)&0x8000)!=0,GetFocus()==hwnd);
            if(batchAction!=BatchKeyAction::Unrecognized){try{executeBatchKeyAction(batchAction);}catch(...){Batch::emergency("batch keyboard dispatch failed");}continue;}
            if((GetKeyState(VK_CONTROL)&0x8000)&&msg.wParam=='O'){SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(OPEN,BN_CLICKED),0);continue;}
            if((GetKeyState(VK_CONTROL)&0x8000)&&msg.wParam=='S'){SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(SAVE,BN_CLICKED),0);continue;}
            if(msg.wParam==VK_ESCAPE&&busy){cancelTask();continue;}
            if(msg.wParam==VK_RETURN&&GetFocus()==hwnd){SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(RUN,BN_CLICKED),0);continue;}
        }
        if(!IsDialogMessageW(hwnd,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}
        if(++batch>=64)break;
        }
        if(running&&frameTimer&&WaitForSingleObject(frameTimer,0)==WAIT_OBJECT_0)renderZoomFrame();
    }
    stopSmoothZoom();if(frameTimer)CloseHandle(frameTimer);frameTimer=nullptr;
    if(task.joinable()){SetEvent(cancelEvent);task.join();}
    if(hardwareScan.joinable())hardwareScan.join();
    joinPreviewSetup();preparedPreview.reset();
    hardwarePending.reset();uiFonts.clear();measureBitmap.reset();
    completed.reset();releaseBatchPreviewBuffers();batchItems.clear();appLogo.reset();
    batchCardCache.clear();batchCardCacheKeys.clear();realtimePreview.clear();output.reset();source.reset();
    batchEngine.cancel();batchEngine.join();batchPreviewLoader.stop();batchExporter.stop();gpuBudget.stop();
    if(!batchSession.clear())batchEngine.record("Session cache cleanup failed");
    uiMemory.reset();CloseHandle(cancelEvent);GdiplusShutdown(gdiplusToken);return 0;
}
