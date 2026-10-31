# 构建与版本维护

## 修改版本

唯一版本定义为 `source/VERSION`。当前使用三段十进制版本号，每段范围 0–65535，不带前导零、后缀或其他文字。只修改这一行，然后重新构建即可。

CMake 从此文件生成构建目录 `generated/` 内的 `version.h`、`resources.rc` 和 `app.manifest`，用于页脚、关于、Windows 数字与字符串版本、应用身份及 CMake 项目版本。Windows 四段格式的最后一段固定为 0。不要手动编辑生成文件，也不要把生成文件复制回 source。VERSION 改动后，`cmake --build` 会自动重新配置。

系统 Common-Controls 版本、worker 的 IPC 版本和第三方依赖版本有独立含义，不随应用版本修改。worker 当前没有应用版本资源，应用版本不会用来改变推理参数。

## Windows 全量构建

需要 Windows x64、PowerShell 7、MinGW-w64 GCC、CMake、Ninja、Vulkan-Headers 1.3.296 和 MinGW 兼容的 Vulkan import library。完整固定第三方源码位于 `source/third_party`；模型在根目录 `models`。工具和 SDK 另行安装。

在开发包根目录运行，按实际安装位置修改参数：

```powershell
pwsh -File ./source/build.ps1 -ToolsRoot C:/tools/w64devkit -VulkanInclude C:/sdk/Vulkan-Headers-1.3.296/include -VulkanLibrary C:/sdk/libvulkan-1.dll.a -BuildRoot C:/build/realcugan -EvidenceRoot C:/build/realcugan-records -Parallel 4
```

ToolsRoot 支持 bin 中含 gcc.exe、g++.exe、windres.exe、cmake.exe、ninja.exe 的布局，也兼容原分目录布局。脚本分别配置 CPU/GPU 目录并构建两个 worker 和主程序；输入哈希前后吻合才发布产物，同时保留旧产物和完整记录。发布中途失败时恢复旧文件，原先不存在的目标会移除。构建和证据目录应放在 source 及 ToolsRoot 之外，以免生成内容混入输入快照。

固定旧 glslang 缺少独立包含的标准整数声明；MinGW 构建在自有 CMake 配置中为其相关目标预包含 cstdint，没有修改第三方源文件。主程序严格启用 -Wall/-Wextra/-Wpedantic/-Wshadow/-Werror。

## 仅构建主程序

worker 及其依赖未改动时，可从匹配的用户包复制两个 worker 到开发包根目录，再运行：

```powershell
pwsh -File ./source/build-main.ps1 -ToolsRoot C:/tools/w64devkit -BuildRoot C:/build/realcugan-main -EvidenceRoot C:/build/realcugan-main-records -Parallel 2
```

此脚本调用同一 `build.ps1 -MainOnly` 流程和 CMake 主程序目标，使用相同的版本生成、编译选项和发布校验，不再维护独立的 windres/g++ 命令。只构建主程序时不需要 Vulkan SDK，但仍需要 CMake、Ninja 和完整自有/第三方源码配置。

## 本次交付的实际构建

本次在 Linux 交叉编译 Windows x64 的三个生产 EXE，使用 Ubuntu GCC 13-posix（安装包 13.2.0）、MinGW-w64 11.0.1、CMake 和 Ninja。具体工具版本、参数、生成文件、源码与成品哈希见 `review/build-record.json`。构建记录列出 Linux 下为交叉 windres 指定的预处理器参数；Windows 原生 MinGW 构建不需要这些路径设置。

本次没有修改 realcugan.cpp、realcugan.h、worker.cpp、8 个模型以及第三方源码。独立辅助逻辑检查和版本生成/自动重配置检查见 review。Windows GUI、真实 GPU 推理以及新单图保存流程仍需 Windows 运行回归，本轮未执行。开发包不含 EXE、编译工具或构建缓存；直接使用请下载用户包。
