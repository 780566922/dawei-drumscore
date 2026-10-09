Windows 音频解码能力探针 —— 使用说明
======================================

【这东西是干什么的】
回答一个问题：你这台 Windows 电脑，系统自带的 Media Foundation
到底能不能解 Ogg Vorbis / Opus？

  能   -> 插件在 Windows 上可以少带一个解码器，甚至白捡 Opus 支持
  不能 -> 插件必须自带解码器（当前就是这么做的）

【怎么用】
1. 把本文件夹（win-codec-probe）和项目里的 testdata 文件夹
   放到 Windows 上的同一个目录下，比如都丢进桌面的一个新建文件夹：

     C:\Users\你的名字\Desktop\probe\
         win-codec-probe\
         testdata\

   （脚本会自己去找 .ogg 测试文件，找不着也不影响前面的检测，
     只是会跳过 [5] 那一步）

2. 双击 win-codec-probe\run-probe.bat

3. 等几秒钟跑完，同目录会生成 report.txt

4. 把 report.txt 发回来

【它做了什么】
只读检测，不改任何系统设置，不需要管理员权限：
  [1] 读系统版本
  [2] 查微软官方的 Web Media Extensions 包是否安装
  [3] 查 .ogg / .opus 的字节流处理器有没有注册
  [4] 列出 Media Foundation 里所有音频解码器，挑出 Vorbis / Opus
  [5] 真的用 Media Foundation 打开一个 .ogg 试试

【关于 Web Media Extensions】
这是微软官方的小包（内部基于 FFmpeg），装上之后 Media Foundation
就能解 Ogg Vorbis 和 Opus。Windows 10 1707 之后多数机器自带，
但精简版系统、企业镜像、Windows Server 上可能没有 —— 所以还得实测。

【正常结果长什么样】
装过 Web Media Extensions 的机器，[4] 里会看到类似：

    >>> [命中] Ogg Vorbis Decoder   {...}
    >>> [命中] ... Opus ...

没装的机器，[4] 里不会有任何 >>> [命中]，而且 [5] 会报
"打不开容器 hr=0xC00D36C4" 之类的错误。

【如果双击没反应 / 一闪而过】
右键 run-probe.bat -> 以管理员身份运行，或打开 PowerShell 手动跑：

    powershell -ExecutionPolicy Bypass -File probe.ps1

【安全说明】
脚本全文都是可读的纯文本，只查注册表和调用 Windows 自带的
Media Foundation 接口，不联网、不下载、不写系统目录。
