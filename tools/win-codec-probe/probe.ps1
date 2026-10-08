# ==============================================================================
#  Windows 音频解码能力探针
#
#  目的：回答一个问题 —— 本机的 Media Foundation 到底能不能解
#        Ogg Vorbis / Opus？从而决定插件是否需要自带解码器。
#
#  用法：双击同目录的 run-probe.bat（会自动 bypass 执行策略）
#        或手动：powershell -ExecutionPolicy Bypass -File probe.ps1
#
#  输出：屏幕 + 同目录 report.txt（把 report.txt 发回来即可）
#
#  只读操作，不修改系统任何设置，不需要管理员权限。
# ==============================================================================

$ErrorActionPreference = 'Continue'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$reportPath = Join-Path $scriptDir 'report.txt'

$buf = New-Object System.Collections.Generic.List[string]
function Emit([string]$s) {
    Write-Host $s
    $buf.Add($s)
}
function Head([string]$s) {
    Emit ''
    Emit "----------------------------------------------------------------------"
    Emit $s
    Emit "----------------------------------------------------------------------"
}

Emit '======================================================================'
Emit ' Windows 音频解码能力探针  (Media Foundation / Ogg Vorbis / Opus)'
Emit (' 时间: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
Emit '======================================================================'

# ------------------------------------------------------------------ 1. 环境
Head '[1/5] 系统环境'

try {
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
    Emit ('  系统     : ' + $os.Caption)
    Emit ('  架构     : ' + $os.OSArchitecture)
} catch {
    Emit '  系统     : (读取失败，非 Windows 或 WMI 受限)'
}

$cv = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
if (Test-Path $cv) {
    $k = Get-ItemProperty $cv -ErrorAction SilentlyContinue
    $display = $k.DisplayVersion
    if (-not $display) { $display = $k.ReleaseId }
    Emit ('  版本     : ' + $display + '  (Build ' + $k.CurrentBuildNumber + '.' + $k.UBR + ')')
}
Emit ('  PowerShell: ' + $PSVersionTable.PSVersion + '   ' + ([IntPtr]::Size * 8) + ' 位进程')
Emit '  说明     : 插件是 64 位，下面的检测也以 64 位视图为主'

# ------------------------------------------- 2. Web Media Extensions 安装状态
Head '[2/5] Web Media Extensions（微软官方的 Ogg/Vorbis/Theora/Opus 解码器包）'

$wme = $null
try { $wme = Get-AppxPackage -Name 'Microsoft.WebMediaExtensions' -ErrorAction SilentlyContinue } catch { }

if ($wme) {
    foreach ($p in @($wme)) {
        Emit ('  [已安装] ' + $p.PackageFullName)
        Emit ('           版本 ' + $p.Version + '   位置 ' + $p.InstallLocation)
    }
} else {
    Emit '  [未检测到] 当前用下没有 Microsoft.WebMediaExtensions'
    Emit '             注意：该包是按【用户】安装的，换个用户登录可能结果不同'
}

# ------------------------------------------------- 3. .ogg 容器解析器是否注册
Head '[3/5] .ogg / .opus 字节流处理器（决定系统认不认得这种文件）'

$bsh = 'HKLM:\SOFTWARE\Microsoft\Windows Media Foundation\ByteStreamHandlers'
if (Test-Path $bsh) {
    foreach ($ext in @('.ogg', '.oga', '.opus', '.ogv', '.webm')) {
        $p = Join-Path $bsh $ext
        if (Test-Path $p) {
            $subs = @(Get-ChildItem $p -ErrorAction SilentlyContinue)
            if ($subs.Count -gt 0) {
                foreach ($s in $subs) {
                    $fn = $s.GetValue('FriendlyName')
                    if (-not $fn) { $fn = $s.GetValue('') }
                    Emit ('  [有] ' + $ext.PadRight(7) + ' -> ' + $s.PSChildName + '   ' + $fn)
                }
            } else {
                Emit ('  [空] ' + $ext.PadRight(7) + ' 键存在但没有处理器')
            }
        } else {
            Emit ('  [无] ' + $ext.PadRight(7) + ' 未注册')
        }
    }
} else {
    Emit '  未找到 ByteStreamHandlers 注册表键'
}

# ------------------------------------------- 4. Media Foundation 音频解码器表
Head '[4/5] Media Foundation 音频解码器（找 Vorbis / Opus / Theora）'

$roots = @(
    'HKLM:\SOFTWARE\Classes\MediaFoundation\Transforms',
    'HKLM:\SOFTWARE\WOW6432Node\Classes\MediaFoundation\Transforms'
)

$allNames = New-Object System.Collections.Generic.List[string]
foreach ($root in $roots) {
    if (-not (Test-Path $root)) { continue }
    $subKeys = @(Get-ChildItem $root -ErrorAction SilentlyContinue)
    Emit ('  [' + $root + ']  共 ' + $subKeys.Count + ' 个已注册变换')
    foreach ($sk in $subKeys) {
        $fn = $sk.GetValue('')
        if (-not $fn) { $fn = $sk.GetValue('FriendlyName') }
        if ($fn) {
            $allNames.Add([string]$fn)
            if ($fn -match '(?i)vorbis|opus|theora|ogg') {
                Emit ('    >>> [命中] ' + $fn + '   {' + $sk.PSChildName + '}')
            }
        }
    }
}

Emit ''
Emit '  -- 对照抽查（这些本该有，用来确认检测逻辑本身正常） --'
foreach ($probe in @('AAC', 'MP3', 'FLAC', 'WMA')) {
    $hit = @($allNames | Where-Object { $_ -match ('(?i)' + [regex]::Escape($probe)) })
    if ($hit.Count -gt 0) {
        Emit ('    [有] ' + $probe.PadRight(5) + '  ' + $hit[0])
    } else {
        Emit ('    [无] ' + $probe.PadRight(5) + '  未找到对应解码器')
    }
}

# ----------------------------------------- 5. 实测：真的打开一个 .ogg 文件试试
Head '[5/5] 实测：用 Media Foundation 打开测试用的 .ogg'

$oggFiles = @()
foreach ($cand in @(
        (Join-Path $scriptDir '*.ogg'),
        (Join-Path $scriptDir 'testdata\*.ogg'),
        (Join-Path $scriptDir '..\testdata\*.ogg'),
        (Join-Path $scriptDir '..\..\testdata\*.ogg'))) {
    $found = @(Get-Item $cand -ErrorAction SilentlyContinue)
    if ($found.Count -gt 0) { $oggFiles += $found }
}

if ($oggFiles.Count -eq 0) {
    Emit '  没找到 .ogg 测试文件，跳过。'
    Emit '  请把项目里的 testdata 文件夹（内含 ogg-*.ogg）和本脚本放一起再跑一次。'
} else {
    $cs = @'
using System;
using System.Runtime.InteropServices;

public static class MfProbe
{
    [DllImport("mfplat.dll", ExactSpelling = true)]
    static extern int MFStartup(int version, int dwFlags);

    [DllImport("mfplat.dll", ExactSpelling = true)]
    static extern int MFShutdown();

    [DllImport("mfreadwrite.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    static extern int MFCreateSourceReaderFromURL(string url, IntPtr attrs, out IMFSourceReader reader);

    [ComImport, Guid("70ae66f2-c809-4e4f-8915-bdcb406b7993"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMFSourceReader
    {
        [PreserveSig] int GetStreamSelection(uint idx, out int selected);
        [PreserveSig] int SetStreamSelection(uint idx, int selected);
        [PreserveSig] int GetNativeMediaType(uint idx, uint typeIdx, out IntPtr mediaType);
        [PreserveSig] int GetCurrentMediaType(uint idx, out IntPtr mediaType);
        [PreserveSig] int SetCurrentMediaType(uint idx, IntPtr reserved, IntPtr mediaType);
        [PreserveSig] int SetCurrentPositionGuid(uint idx, ref Guid pos);
        [PreserveSig] int ReadSample(uint idx, uint flags, out uint actualIdx, out uint streamFlags, out long timestamp, out IntPtr sample);
        [PreserveSig] int Flush(uint idx);
        [PreserveSig] int GetServiceForStream(uint idx, ref Guid svc, ref Guid riid, out IntPtr obj);
        [PreserveSig] int GetPresentationAttribute(uint idx, ref Guid attr, IntPtr var);
    }

    const uint FIRST_AUDIO_STREAM = 0xFFFFFFFD;
    const uint SEEK_TO_ANY_CLEANPOINT = 0x00000020;
    const uint MF_SOURCE_READERF_ERROR = 0x00000001;

    static string Hx(int hr) { return "0x" + ((uint)hr).ToString("X8"); }

    public static string TryOpen(string path)
    {
        int hr = MFStartup(0x00020070, 0);
        if (hr < 0) return "MFStartup 失败 hr=" + Hx(hr);
        IMFSourceReader reader = null;
        try
        {
            string url;
            try { url = new Uri(path).AbsoluteUri; } catch { url = path; }

            hr = MFCreateSourceReaderFromURL(url, IntPtr.Zero, out reader);
            if (hr < 0 || reader == null) return "打不开容器 hr=" + Hx(hr);

            IntPtr mt;
            hr = reader.GetNativeMediaType(FIRST_AUDIO_STREAM, 0, out mt);
            if (hr < 0) return "打开成功但没有音频流 hr=" + Hx(hr);

            uint ai, sf; long ts; IntPtr sample;
            hr = reader.ReadSample(FIRST_AUDIO_STREAM, 0, out ai, out sf, out ts, out sample);
            if (hr < 0) return "读取失败 hr=" + Hx(hr);
            if ((sf & MF_SOURCE_READERF_ERROR) != 0)
                return "解析报错 streamFlags=0x" + sf.ToString("X8");
            if (sample == IntPtr.Zero) return "读到空样本";
            return "成功读出数据（时间戳 " + ts + "，streamFlags=0x" + sf.ToString("X8") + "）";
        }
        catch (Exception ex)
        {
            return "异常: " + ex.GetType().Name + " " + ex.Message;
        }
        finally
        {
            if (reader != null) { try { Marshal.ReleaseComObject(reader); } catch { } }
            MFShutdown();
        }
    }
}
'@

    try {
        Add-Type -TypeDefinition $cs -Language CSharp -ErrorAction Stop
        $compiled = $true
    } catch {
        $compiled = $false
        Emit '  [跳过] 内联 C# 编译失败（不影响前面的注册表检测结果）：'
        Emit ('         ' + $_.Exception.Message)
    }

    if ($compiled) {
        foreach ($f in $oggFiles) {
            $r = [MfProbe]::TryOpen($f.FullName)
            Emit ('  ' + $f.Name.PadRight(28) + ' -> ' + $r)
        }
    }
}

# ------------------------------------------------------------------ 结论汇总
Head '结论'

$hasVorbisMft = @($allNames | Where-Object { $_ -match '(?i)vorbis' }).Count -gt 0
$hasOpusMft = @($allNames | Where-Object { $_ -match '(?i)opus' }).Count -gt 0

Emit ('  Web Media Extensions : ' + $(if ($wme) { '已安装' } else { '未安装 / 未检测到' }))
Emit ('  系统 Vorbis 解码器    : ' + $(if ($hasVorbisMft) { '有' } else { '没有' }))
Emit ('  系统 Opus  解码器     : ' + $(if ($hasOpusMft) { '有' } else { '没有' }))
Emit ''

if ($hasVorbisMft) {
    Emit '  => 结论：这台机器的 Media Foundation 能解 Ogg Vorbis，'
    Emit '          插件在 Windows 上其实可以直接交给系统解码。'
} else {
    Emit '  => 结论：这台机器的 Media Foundation 解不了 Ogg Vorbis，'
    Emit '          插件必须自带解码器（stb_vorbis）才能放。'
}

if ($hasOpusMft) {
    Emit '  => Opus：系统有解码器，可以考虑走系统路径白捡 Opus 支持。'
} else {
    Emit '  => Opus：系统没有解码器，想支持就得依赖第三方库或自带。'
}

Emit ''
Emit '======================================================================'

# ------------------------------------------------------------------ 写文件
try {
    $buf | Out-File -FilePath $reportPath -Encoding UTF8
    Write-Host ''
    Write-Host ('报告已保存: ' + $reportPath) -ForegroundColor Green
} catch {
    Write-Host ''
    Write-Host '报告写入失败，请手动复制上面的内容。' -ForegroundColor Yellow
}
