#include "realcugan.h"
#include "tile_border.h"
#include <cfloat>
#include <algorithm>
#include <cpu.h>
#include <iostream>
#include <thread>
#include <string>
#include "fmt/core.h"
#include <stdexcept>
#include <cstdio>
#ifndef _WIN32
#include <codecvt>
#include <locale>
#endif

RealCUGAN::RealCUGAN(int threads, int gpu) {
    // 不再限制为 3 线程，也不强制绑定到大核；明确传入本次任务的线程上限。
    thread_count=std::clamp(threads,1,std::max(1,ncnn::get_cpu_count()));
    ncnn::set_omp_num_threads(thread_count);
    ncnnNet.opt=ncnn::Option();
    ncnnNet.opt.num_threads=thread_count;
    if(gpu>=0) {
#if NCNN_VULKAN
        if(gpu>=ncnn::get_gpu_count())throw std::runtime_error("Invalid inference GPU");
        ncnnNet.set_vulkan_device(gpu);
        ncnnNet.opt.use_vulkan_compute=true;
        // Preserve FP32 weights and arithmetic; do not introduce quantization
        // or let the GPU backend select lower precision as an implicit default.
        ncnnNet.opt.use_fp16_packed=false;
        ncnnNet.opt.use_fp16_storage=false;
        ncnnNet.opt.use_fp16_arithmetic=false;
#else
        throw std::runtime_error("Worker was built without Vulkan support");
#endif
    }
    std::cout<<"THREADS "<<thread_count<<std::endl;
}

int RealCUGAN::load(int scaleOption, int noiseOption) {
    ncnnNet.clear();
    this->scale = scaleOption;
    this->noise = noiseOption;

    std::string paramFilePath;
    std::string binFilePath;
    if (this->noise == 0) {
        paramFilePath = fmt::format("up{}x-no-denoise.param", this->scale);
        binFilePath = fmt::format("up{}x-no-denoise.bin", this->scale);
    } else if (this->noise == -1) {
        paramFilePath = fmt::format("up{}x-conservative.param", this->scale);
        binFilePath = fmt::format("up{}x-conservative.bin", this->scale);
    } else {
        paramFilePath = fmt::format("up{}x-denoise{}x.param", this->scale, this->noise);
        binFilePath = fmt::format("up{}x-denoise{}x.bin", this->scale, this->noise);
    }
    // 使用宽字符路径打开文件，支持中文用户名及中文解压目录。
    auto open_model = [&](const std::string& name) {
        std::wstring path = model_directory + L"/" + std::wstring(name.begin(), name.end());
#ifdef _WIN32
        return _wfopen(path.c_str(), L"rb");
#else
        std::wstring_convert<std::codecvt_utf8<wchar_t>> convert;
        return fopen(convert.to_bytes(path).c_str(), "rb");
#endif
    };
    FILE* param = open_model(paramFilePath);
    FILE* weights = open_model(binFilePath);
    if (!param || !weights) {
        if (param) fclose(param);
        if (weights) fclose(weights);
        throw std::runtime_error("Model file missing or unreadable");
    }
    int param_result = ncnnNet.load_param(param);
    int model_result = param_result == 0 ? ncnnNet.load_model(weights) : -1;
    fclose(param);
    fclose(weights);
    if (param_result != 0 || model_result != 0) throw std::runtime_error("Model loading failed");

    if (scale == 2)
    {
        this->prepadding = 18;
    } else if (scale == 3)
    {
        this->prepadding = 14;
    } else if (scale == 4)
    {
        this->prepadding = 19;
    }
    return 0;
}

// Shared tile preparation/assembly; the extractor uses the selected backend.
int RealCUGAN::process(const ncnn::Mat &inimage, ncnn::Mat &outimage) {
    if(scale<2||scale>4||prepadding<=0||tilesize<=0||inimage.empty()||inimage.elempack!=3)
        throw std::runtime_error("Invalid image or model not loaded");
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    const unsigned char *pixeldata = (const unsigned char *) inimage.data;
    const int w = inimage.w;
    const int h = inimage.h;
    const int channels = inimage.elempack;

    const int TILE_SIZE_X = tilesize;
    const int TILE_SIZE_Y = tilesize;

    // each tile 200x200
    const int xtiles = (w + TILE_SIZE_X - 1) / TILE_SIZE_X;
    const int ytiles = (h + TILE_SIZE_Y - 1) / TILE_SIZE_Y;

    for (int yi = 0; yi < ytiles; yi++) {
        const int tile_h_nopad = std::min((yi + 1) * TILE_SIZE_Y, h) - yi * TILE_SIZE_Y;

        int prepadding_bottom = prepadding;
        if (scale == 1 || scale == 3) {
            prepadding_bottom += (tile_h_nopad + 3) / 4 * 4 - tile_h_nopad;
        }
        if (scale == 2 || scale == 4) {
            prepadding_bottom += (tile_h_nopad + 1) / 2 * 2 - tile_h_nopad;
        }

        int in_tile_y0 = std::max(yi * TILE_SIZE_Y - prepadding, 0);
        int in_tile_y1 = std::min((yi + 1) * TILE_SIZE_Y + prepadding_bottom, h);
        for (int xi = 0; xi < xtiles; xi++) {
            if(on_tile_begin)on_tile_begin();
            struct TileRelease {std::function<void()>& end;~TileRelease(){if(end)end();}} tileRelease{on_tile_end};

            const int tile_w_nopad = std::min((xi + 1) * TILE_SIZE_X, w) - xi * TILE_SIZE_X;

            int prepadding_right = prepadding;
            if (scale == 1 || scale == 3) {
                prepadding_right += (tile_w_nopad + 3) / 4 * 4 - tile_w_nopad;
            }
            if (scale == 2 || scale == 4) {
                prepadding_right += (tile_w_nopad + 1) / 2 * 2 - tile_w_nopad;
            }

            int in_tile_x0 = std::max(xi * TILE_SIZE_X - prepadding, 0);
            int in_tile_x1 = std::min((xi + 1) * TILE_SIZE_X + prepadding_right, w);

            // crop tile
            ncnn::Mat in;
            {
                if (channels == 3) {
                    in = ncnn::Mat::from_pixels_roi(pixeldata, ncnn::Mat::PIXEL_RGB, w, h, in_tile_x0, in_tile_y0,
                                                    in_tile_x1 - in_tile_x0, in_tile_y1 - in_tile_y0);
                }
            }
            ncnn::Mat out;

            {
                // split alpha and preproc
                ncnn::Mat in_tile;
                {
                    in_tile.create(in.w, in.h, 3);
                    if(in.empty()||in_tile.empty())throw std::bad_alloc();
                    for (int q = 0; q < 3; q++) {
                        const float *ptr = in.channel(q);
                        float *outptr = in_tile.channel(q);

                        for (int i = 0; i < in.w * in.h; i++) {
                            *outptr++ = *ptr++ * (1 / 255.f);
                        }
                    }
                }

                // border padding
                {
                    int pad_top = std::max(prepadding - yi * TILE_SIZE_Y, 0);
                    int pad_bottom = std::max(
                            std::min((yi + 1) * TILE_SIZE_Y + prepadding_bottom - h, prepadding_bottom), 0);
                    int pad_left = std::max(prepadding - xi * TILE_SIZE_X, 0);
                    int pad_right = std::max(std::min((xi + 1) * TILE_SIZE_X + prepadding_right - w, prepadding_right),
                                             0);

                    ncnn::Mat in_tile_padded;
                    if(TileBorder::needsRepeatedReflection(in.w,in.h,pad_top,pad_bottom,pad_left,pad_right)){
                        // The pinned ncnn type=2 implementation reflects once;
                        // padding >= the source axis would read outside that axis.
                        // Keep the same reflection convention, repeating it only
                        // where a single reflection cannot provide valid pixels.
                        in_tile_padded.create(in.w+pad_left+pad_right,in.h+pad_top+pad_bottom,3,4u,ncnnNet.opt.blob_allocator);
                        if(in_tile_padded.empty())throw std::bad_alloc();
                        for(int q=0;q<3;++q)for(int y=0;y<in_tile_padded.h;++y){
                            const float* src=in_tile.channel(q).row(TileBorder::reflect(y-pad_top,in.h));
                            float* dst=in_tile_padded.channel(q).row(y);
                            for(int x=0;x<in_tile_padded.w;++x)dst[x]=src[TileBorder::reflect(x-pad_left,in.w)];
                        }
                    }else{
                        ncnn::copy_make_border(in_tile, in_tile_padded, pad_top, pad_bottom, pad_left, pad_right, 2, 0.f,
                                               ncnnNet.opt);
                        if(in_tile_padded.empty())throw std::bad_alloc();
                    }
                    in_tile = in_tile_padded;
                }
                // realcugan
                ncnn::Mat out_tile;
                {
                    ncnn::Extractor ex = ncnnNet.create_extractor();

                    if (ex.input("in0", in_tile) != 0 || ex.extract("out0", out_tile) != 0 || out_tile.empty())
                        throw std::runtime_error("Inference failed; insufficient memory or invalid model");
                }

                // postproc and merge alpha
                {
                    out.create(tile_w_nopad * scale, tile_h_nopad * scale, channels);
                    if(out.empty())throw std::bad_alloc();
                    if(out_tile.w<out.w||out_tile.h<out.h||out_tile.c<3||out_tile.elempack!=1||out_tile.elemsize!=4u)
                        throw std::runtime_error("Inference output shape mismatch");
                    if (scale == 4) {
                        for (int q = 0; q < 3; q++) {
                            float *outptr = out.channel(q);

                            for (int i = 0; i < out.h; i++) {
                                const float *inptr = in_tile.channel(q).row(prepadding + i / 4) + prepadding;
                                const float *ptr = out_tile.channel(q).row(i);

                                for (int j = 0; j < out.w; j++) {
                                    if (outptr) {
                                        *outptr++ = *ptr++ * 255.f + 0.5f + inptr[j / 4] * 255.f;
                                    } else {
                                    }
                                }
                            }
                        }
                    } else {
                        for (int q = 0; q < 3; q++) {
                            float *outptr = out.channel(q);

                            for (int i = 0; i < out.h; i++) {
                                const float *ptr = out_tile.channel(q).row(i);

                                for (int j = 0; j < out.w; j++) {
                                    *outptr++ = *ptr++ * 255.f + 0.5f;
                                }
                            }
                        }
                    }
                }
            }
            {
                if (channels == 3) {
                    out.to_pixels((unsigned char *) outimage.data + yi * scale * TILE_SIZE_Y * w * scale * channels +
                                  xi * scale * TILE_SIZE_X * channels, ncnn::Mat::PIXEL_RGB, w * scale * channels);
                }
            }

            auto end = std::chrono::steady_clock::now();
            auto total_cost = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
            float progress_rate = (float) (xtiles * yi + xi + 1) / (float) (xtiles * ytiles);
            if (on_progress) on_progress((int)(progress_rate * 100), (long)(total_cost / progress_rate - total_cost));
        }
    }

    return 0;
}
