//==============================================================================
// ogg_vorbis.h — Ogg Vorbis 解码（三端共用）
//
// 为什么单独抽一份而不是各平台各写一套：
//   Ogg Vorbis 是【三端系统框架都没有】的格式 ——
//     · macOS  CoreAudio 从未提供 Vorbis 解码器
//     · Windows Media Foundation 内置解码器里也没有（微软把它放在商店的
//       可选包「Web Media Extensions」里，默认不装）
//     · Linux 我们自己 vendored 的 dr_libs 只做 WAV / MP3 / FLAC
//   所以这一份解码器三端都得自带，干脆写成平台无关的共享源码，
//   与 player.cpp / aplaysdk.cpp 一样被三端构建同时编进去。
//
// 实现基于 stb_vorbis（公共领域 / MIT-0，见 source/third_party/stb_vorbis.c）。
//
// ⚠ 只解 Ogg **Vorbis**，不解 Ogg **Opus** —— Opus 是另一套编码，
//   stb_vorbis 解不了。遇到 .opus 会给明确提示而不是静默失败。
//==============================================================================
#pragma once

#include "audiofile.h"

#include <string>

namespace ap {

//------------------------------------------------------------------------------
// 扩展名是否属于 Ogg 家族（.ogg / .oga）。
// 注意：这里只判断「值得送去试」，不代表一定能解开 ——
// 同样是 .ogg 后缀，里面装的可能是 Opus 或 FLAC。
//------------------------------------------------------------------------------
bool isOggExtension(const std::string& path);

//------------------------------------------------------------------------------
// 解码 Ogg Vorbis 到交错 float 立体声（保留文件原生采样率）。
// 采样率重采样交给上层 Player 处理，与其它格式路径一致。
//
// 失败时返回 false 并往 err 写入**用户可读**的中文原因。
// 本函数不抛异常（内部兜住 std::exception），插件绝不能把异常穿给宿主。
//------------------------------------------------------------------------------
bool decodeOggVorbis(const std::string& path, AudioData& out, std::string& err);

} // namespace ap
