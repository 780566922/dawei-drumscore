//==============================================================================
// dr_libs_impl.cpp — dr_libs 单文件解码库的实现单元
//
// 三个库（dr_wav / dr_mp3 / dr_flac）都采用「单文件 + 实现宏」模式：
// 头文件本身只有声明，只有在【某一个】翻译单元里定义 *_IMPLEMENTATION
// 才会编出实现。这里集中放实现，避免多个 TU 重复编译这几百 KB 代码。
//
// vendored 版本（来源 https://github.com/mackron/dr_libs，公共领域 / MIT-0）：
//   dr_wav  v0.14.6
//   dr_mp3  v0.7.4
//   dr_flac v0.13.4
//==============================================================================

// 关掉用不上的特性，减小体积与编译时间：
//   - 只要 stdio 读写，不需要 Win32 宽字符 API
//   - 不需要「Ogg 容器里的 FLAC」
#define DR_WAV_NO_WCHAR
#define DR_FLAC_NO_OGG

// 注意：不能定义 DR_WAV_NO_CONVERSION_API —— 我们要用 drwav_read_pcm_frames_f32
#define DR_WAV_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION

#include "third_party/dr_wav.h"
#include "third_party/dr_mp3.h"
#include "third_party/dr_flac.h"
