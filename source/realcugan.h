#ifndef REALCUGAN_H
#define REALCUGAN_H

#include "net.h"
#include <functional>
#include <string>

class RealCUGAN {
public:
    explicit RealCUGAN(int threads = 3, int gpu = -1);
    int thread_count = 1;
    // 桌面版使用明确的模型目录与进度通知，不依赖浏览器虚拟文件系统。
    std::wstring model_directory;
    std::function<void(int, long)> on_progress;
    std::function<void()> on_tile_begin, on_tile_end;

    int load(int scale, int noise);

    int process(const ncnn::Mat &inimage, ncnn::Mat &outimage);

    int noise=0;
    int scale=0;
    int prepadding=0;
    int tilesize = 200;
    int syncgap = 0;
    bool tta_mode = false;

private:
    ncnn::Net ncnnNet;
};

#endif // REALCUGAN_H

typedef std::string path_t;
