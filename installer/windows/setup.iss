;==============================================================================
; setup.iss — 「大伟鼓谱 MuseScore 音频播放器」Windows 安装器（Inno Setup）
;
; 由 GitHub Actions 调用：
;   ISCC.exe installer\windows\setup.iss
; 本机（macOS）无法编译 Inno 脚本，一切以 CI 结果为准。
;
; 安装目标：C:\Program Files\Common Files\VST3\DaweiDrumScore.vst3\
;   —— 这是 VST3 的【系统级】标准目录，MuseScore 等所有宿主、所有账号都能扫到。
;
; 想改用中文安装向导：把简体中文语言包（ChineseSimplified.isl，非官方翻译，
;   需匹配 Inno 版本）放到本目录，然后把下面 [Languages] 段换成：
;   Name: "cn"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
;==============================================================================

; 构建目录（相对本 .iss 文件）：<仓库根>/build
#ifndef MyBuildDir
  #define MyBuildDir "..\..\build"
#endif

#define MyAppName      "大伟鼓谱 MuseScore 音频播放器"
#define MyAppVersion   "1.0.2"
#define MyAppPublisher "大伟鼓谱"
#define MyVst3Folder   "DaweiDrumScore.vst3"
; ↓ 想指向你的 B 站空间，把这里换成 https://space.bilibili.com/你的UID
#define MySiteUrl      "https://search.bilibili.com/all?keyword=%E5%A4%A7%E4%BC%9F%E9%BC%93%E8%B0%B1"

[Setup]
; AppId 是卸载识别的唯一键，一旦发布【不要改】，否则升级会被当成两个软件
AppId={{8F3A9C21-4B7E-4D6A-9C15-7E2B04A6D913}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppSupportURL={#MySiteUrl}
DefaultDirName={commoncf64}\VST3
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=no
OutputDir={#MyBuildDir}\setup-out
OutputBaseFilename=DaweiDrumScore-Setup-{#MyAppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
Uninstallable=yes
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\{#MyVst3Folder}\PlugIn.ico
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Files]
; 整个 .vst3 bundle 原样拷过去（含 x86_64-win 内核 + moduleinfo.json + 图标）
Source: "{#MyBuildDir}\VST3\Release\{#MyVst3Folder}\*"; \
    DestDir: "{app}\{#MyVst3Folder}"; \
    Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
; Inno 不追踪运行时生成的文件，卸载时把整个 bundle 目录清掉，避免残留
Type: filesandordirs; Name: "{app}\{#MyVst3Folder}"

[Run]
Filename: "{#MySiteUrl}"; \
    Description: "关注 B 站「大伟鼓谱」（鼓谱 / 教学 / 伴奏持续更新）"; \
    Flags: postinstall shellexec nowait skipifsilent unchecked

[Code]
// 安装前提示：若 MuseScore 正在运行，插件文件可能被占用
function InitializeSetup(): Boolean;
begin
  Result := True;
end;
