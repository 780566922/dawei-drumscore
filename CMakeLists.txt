#==============================================================================
# CMakeLists.txt — 跨平台构建「大伟鼓谱 MuseScore 音频播放器」
#
# macOS 本地开发仍可用 ./build.sh（含 6 项验证）；
# 本文件主要用于 Windows（MSVC）构建，同时也能构建 macOS 版。
#
# 用法（Windows）：
#   git clone --recurse-submodules https://github.com/steinbergmedia/vst3sdk.git vst3sdk
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release
#
# 产物：build/**/DaweiDrumScore.vst3
#==============================================================================
cmake_minimum_required(VERSION 3.20)

project(DaweiDrumScore VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 单配置生成器（Unix Makefiles / Ninja）必须显式给 CMAKE_BUILD_TYPE，
# 否则 SDK 的 fdebug.h 会因为 RELEASE / _DEBUG / NDEBUG 一个都没定义而 #error。
# 多配置生成器（Visual Studio / Xcode）走 --config 参数，无需在此设置。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

if(APPLE)
    enable_language(OBJCXX)
endif()

if(MSVC)
    # 源码里含中文（界面文案、注释），必须让 MSVC 按 UTF-8 解析，
    # 否则会报 C4819 或把字符串编译成乱码。
    add_compile_options(/utf-8 /MP)
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE _UNICODE)
endif()

#------------------------------------------------------------------------------
# VST3 SDK
#------------------------------------------------------------------------------
set(VST3_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vst3sdk"
    CACHE PATH "VST3 SDK 根目录（含 pluginterfaces/base/public.sdk）")

if(NOT EXISTS "${VST3_SDK_DIR}/pluginterfaces")
    message(FATAL_ERROR
        "找不到 VST3 SDK：${VST3_SDK_DIR}\n"
        "请执行：git clone --recurse-submodules "
        "https://github.com/steinbergmedia/vst3sdk.git vst3sdk")
endif()

# 只构建我们需要的部分，跳过示例 / VSTGUI / 验证器，加快 CI
set(SMTG_ADD_VSTGUI               OFF CACHE BOOL "" FORCE)
set(SMTG_ADD_VST3_HOSTING_SAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ADD_VST3_PLUGINS_SAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK       OFF CACHE BOOL "" FORCE)
set(SMTG_RUN_VST_VALIDATOR        OFF CACHE BOOL "" FORCE)

add_subdirectory("${VST3_SDK_DIR}" "${CMAKE_BINARY_DIR}/vst3sdk")

#------------------------------------------------------------------------------
# 源文件（按平台拆分）
#------------------------------------------------------------------------------
set(AP_SOURCES
    source/player.cpp        # 播放核心（纯 C++，跨平台）
    source/aplaysdk.cpp      # VST3 处理器 / 控制器 / 工厂（纯 C++）
)

if(APPLE)
    list(APPEND AP_SOURCES
        source/audiofile.mm      # 解码（AVFoundation）
        source/crashguard.mm     # 崩溃取证（signal + backtrace）
        source/gui.mm)           # 界面（AppKit）
elseif(WIN32)
    list(APPEND AP_SOURCES
        source/win/audiofile_win.cpp   # 解码（Media Foundation）
        source/win/crashguard_win.cpp  # 崩溃取证（SEH + DbgHelp）
        source/win/gui_win.cpp)        # 界面（Win32）
else()
    message(FATAL_ERROR "当前仅支持 macOS 与 Windows")
endif()

#------------------------------------------------------------------------------
# 目标
#------------------------------------------------------------------------------
if(COMMAND smtg_add_vst3plugin)
    smtg_add_vst3plugin(DaweiDrumScore ${AP_SOURCES})
else()
    # 兜底：SDK 未提供辅助宏时，手动建一个按 .vst3 命名的模块库
    add_library(DaweiDrumScore MODULE ${AP_SOURCES})
    set_target_properties(DaweiDrumScore PROPERTIES PREFIX "" SUFFIX ".vst3")
    target_link_libraries(DaweiDrumScore PRIVATE sdk pluginterfaces base)
endif()

target_include_directories(DaweiDrumScore PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/source"
    # 源码里用 "public.sdk/..." / "pluginterfaces/..." 全路径形式包含 SDK 头，
    # 所以要把 SDK 根目录加进 include 路径（与 build.sh 的 -I$SDK 对应）
    "${VST3_SDK_DIR}")

# SDK 实现库：smtg_add_vst3plugin 只建 bundle，不替你链接这些，
# 必须显式链接（SDK 自带示例插件也是这么写的）。顺序：被依赖者靠后。
target_link_libraries(DaweiDrumScore PRIVATE
    sdk sdk_hosting sdk_common pluginterfaces base)

if(WIN32)
    # Media Foundation（解码）+ DbgHelp（崩溃栈）+ 常用 Win32 库
    target_link_libraries(DaweiDrumScore PRIVATE
        mfplat mfreadwrite mfuuid
        dbghelp
        ole32 oleaut32 uuid
        user32 gdi32 comctl32 comdlg32 shell32)
elseif(APPLE)
    target_link_libraries(DaweiDrumScore PRIVATE
        "-framework AVFoundation"
        "-framework Foundation"
        "-framework AppKit"
        "-framework Cocoa")
endif()
