//==============================================================================
// ogg_vorbis_test.cpp — Ogg Vorbis 解码回归测试
//
// 为什么值得单独守一条：
//   1) 这一格解码器是【我们自己带的】。macOS / Windows / Linux 的系统框架
//      都没有 Vorbis 解码器，所以没有「系统帮我兜住」这回事 ——
//      三端能不能放 OGG，完全取决于这一份 stb_vorbis 路径。
//   2) 有个很容易踩的坑：stb_vorbis 在「要的声道数比源多」时会给多余声道
//      【补 0】，而不是复制。mono 文件若直接按 2 声道索取，右声道就是死的。
//      我们改成「按源声道数取、自己做声道合并」，这条测试替它守着。
//   3) Ogg Opus 和 Ogg Vorbis 后缀一样、长得也像，用户极容易混。必须给出
//      准确提示（"这是 Opus"），而不是含糊的"解码失败"。
//
// 用法：  ogg_vorbis_test [testdata 目录]      默认 testdata
// 退出码：有任何一项 FAIL 就返回 1
//==============================================================================
#include "ogg_vorbis.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_pass = 0;
int g_fail = 0;

void check (const char* name, bool ok)
{
    if (ok) { std::printf ("PASS %s\n", name); ++g_pass; }
    else    { std::printf ("FAIL %s\n", name); ++g_fail; }
}

std::string join (const char* dir, const char* file)
{
    return std::string (dir) + "/" + file;
}

// 平均绝对差：用来判断两个声道是不是同一份数据
double meanAbsDiff (const std::vector<float>& s)
{
    const size_t frames = s.size () / 2;
    if (frames == 0)
        return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < frames; ++i)
        sum += std::fabs (static_cast<double> (s[2 * i]) - static_cast<double> (s[2 * i + 1]));
    return sum / static_cast<double> (frames);
}

double peakOf (const std::vector<float>& s)
{
    double p = 0.0;
    for (float v : s)
        p = std::max (p, std::fabs (static_cast<double> (v)));
    return p;
}

//------------------------------------------------------------------------------
// 写一个内容垃圾、但后缀是 .ogg 的文件：验证文件头校验挡得住
//------------------------------------------------------------------------------
void writeJunk (const char* path)
{
    std::FILE* f = std::fopen (path, "wb");
    if (!f)
        return;
    const char junk[64] = "this is definitely not an ogg vorbis stream at all...";
    std::fwrite (junk, 1, sizeof junk, f);
    std::fclose (f);
}

} // namespace

int main (int argc, char** argv)
{
    const char* dir = (argc > 1) ? argv[1] : "testdata";

    //---- 扩展名判定 ------------------------------------------------------
    check ("isOggExtension(.ogg)",      ap::isOggExtension ("song.ogg"));
    check ("isOggExtension(.OGG 大写)", ap::isOggExtension ("song.OGG"));
    check ("isOggExtension(.oga)",      ap::isOggExtension ("song.oga"));
    check ("isOggExtension(.mp3=否)",   !ap::isOggExtension ("song.mp3"));
    check ("isOggExtension(.ogg.mp3=否)", !ap::isOggExtension ("song.ogg.mp3"));
    check ("isOggExtension(无扩展名=否)", !ap::isOggExtension ("song"));
    check ("isOggExtension(目录里的点=否)", !ap::isOggExtension ("dir.d/song"));

    //---- 立体声：两个声道必须真的是两份不同的数据 --------------------------
    {
        ap::AudioData d;
        std::string err;
        const bool ok = ap::decodeOggVorbis (join (dir, "ogg-stereo-44100.ogg"), d, err);
        check ("立体声 OGG 解码成功", ok);
        if (ok)
        {
            check ("立体声 采样率保留 44100", d.sampleRate == 44100);
            check ("立体声 输出声道数为 2",   d.numChannels == 2);
            check ("立体声 有音频数据",       d.numFrames () > 1000);
            check ("立体声 时长约 0.4 秒",
                   d.durationSec () > 0.30 && d.durationSec () < 0.55);
            check ("立体声 左右是两份数据（非复制）", meanAbsDiff (d.samples) > 0.05);
            check ("立体声 峰值在合理范围", peakOf (d.samples) > 0.05 && peakOf (d.samples) < 1.05);
            check ("立体声 样本数为偶数",   d.samples.size () % 2 == 0);
        }
        else
        {
            std::printf ("       错误信息：%s\n", err.c_str ());
        }
    }

    //---- 单声道：必须复制到左右两路，不能右声道静音 ------------------------
    {
        ap::AudioData d;
        std::string err;
        const bool ok = ap::decodeOggVorbis (join (dir, "ogg-mono-44100.ogg"), d, err);
        check ("单声道 OGG 解码成功", ok);
        if (ok)
        {
            check ("单声道 采样率保留 44100", d.sampleRate == 44100);
            check ("单声道 仍输出 2 声道",     d.numChannels == 2);
            check ("单声道 有音频数据",       d.numFrames () > 1000);
            check ("单声道 左右两路完全相同（未补 0）", meanAbsDiff (d.samples) == 0.0);
            check ("单声道 不是静音",         peakOf (d.samples) > 0.05);
        }
        else
        {
            std::printf ("       错误信息：%s\n", err.c_str ());
        }
    }

    //---- Ogg Opus：必须给出准确提示，而不是含糊的失败 ----------------------
    {
        ap::AudioData d;
        std::string err;
        const bool ok = ap::decodeOggVorbis (join (dir, "opus-mono-48000.ogg"), d, err);
        check ("Ogg Opus 判定为解码失败", !ok);
        check ("Ogg Opus 提示里点名 Opus", ok ? false : err.find ("Opus") != std::string::npos);
        check ("Ogg Opus 失败后不留残余数据", d.samples.empty ());
    }

    //---- 后缀骗人：内容不是 Ogg，文件头校验必须挡住 ------------------------
    {
        const char* junk = "/tmp/aplay_junk.ogg";
        writeJunk (junk);

        ap::AudioData d;
        std::string err;
        const bool ok = ap::decodeOggVorbis (junk, d, err);
        check ("非 Ogg 内容（改后缀）解码失败", !ok);
        check ("非 Ogg 内容 提示提到 OggS",
               ok ? false : err.find ("OggS") != std::string::npos);
        std::remove (junk);
    }

    //---- 空路径 ----------------------------------------------------------
    {
        ap::AudioData d;
        std::string err;
        check ("空路径解码失败", !ap::decodeOggVorbis ("", d, err));
    }

    std::printf ("\n%d 项通过，%d 项失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
