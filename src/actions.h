// actions.h - 托盘菜单可执行的特权动作
//
// 设计原则：这一层与 UI 完全解耦 —— 不引用任何窗口/菜单类型，只做动作并返回
// 结果结构。托盘（tray.cpp）只负责把它接到菜单上，弹窗展示结果。
// 这样这些动作将来也能直接暴露成 CLI 子命令或单元测试。
#pragma once

#include "common.h"
// OutputNature / OutputNatureName / ClassifyOutputNature / IsLinkHealthy 已下沉到
// display 层：它们与 verify.cpp 的拓扑判定共用同一套判据，放在这一层会让
// verify 反过来依赖 UI 动作层。
#include "display.h"

namespace resprobe {

// ---------------------------------------------------------------------------
// 通用：带消息泵的等待 / 命令执行
// ---------------------------------------------------------------------------
// 在等待期间持续派发本线程消息。托盘与监听共用同一个消息泵，
// 若不泵消息，执行中的长操作会让托盘图标点击全部挂起。
void PumpSleep(DWORD ms);

// 运行命令行并捕获 stdout+stderr。返回 false 表示压根没启动起来。
// exitCodeOut 为子进程退出码；outputOut 为合并输出（UTF-8 解码后的宽字符）。
bool RunAndCapture(const std::wstring& commandLine, const std::wstring& workDir,
                   DWORD timeoutMs, DWORD* exitCodeOut, std::wstring* outputOut);

// ---------------------------------------------------------------------------
// 1) 项目脚本调用
//
// 脚本文件名是中文，而脚本内容必须纯 ASCII（见 DEVELOPMENT.md 第 3 节编码模型），
// 因此 C++ 侧不能硬编码中文文件名去调用。约定：每个可被调用的脚本在**独立的一行**
// 以 `@rem RESPROBE-ROLE:<marker>` 声明自己的角色，这里按角色定位文件。
// 匹配是整行精确相等，既锚定了行首，也避免脚本注释里提到其它角色时自匹配。
// ---------------------------------------------------------------------------
struct ScriptRunResult {
    bool found = false;          // 是否定位到脚本
    bool ok = false;             // 脚本退出码是否为 0
    DWORD exitCode = 0xFFFFFFFF; // 脚本退出码
    std::wstring scriptPath;
    std::wstring logPath;
    std::wstring tail;           // 日志尾部若干行，供弹窗展示
    bool timedOut = false;
    bool killed = false;         // 超时后是否成功终止了脚本（优先整棵进程树）
    DWORD elapsedMs = 0;
};

bool FindScriptByRole(const std::wstring& homeDir, const std::wstring& marker,
                      std::wstring* pathOut);

// 运行脚本并等待完成。args 会拼在脚本路径之后；日志写入 <OUT>\<logName>。
ScriptRunResult RunProjectScript(const std::wstring& homeDir,
                                 const std::wstring& marker,
                                 const std::wstring& args,
                                 const std::wstring& logName,
                                 DWORD timeoutMs = 300000);

// 等价于命令行里的 Windows 路径策略：<程序目录>\OUT
std::wstring GetHomeDir();
std::wstring GetOutDirPath();
bool EnsureOutDir();

// 注：这里曾经有 RequestDisplayOutputOff/On（用 SC_MONITORPOWER 关闭显示输出），
// 曾作为"隐私屏"的实现。真机验证后已删除，原因有两条硬事实：
//   1) rustdesk.exe 持有 DISPLAY 电源请求，会在关屏后不到 1 秒把显示输出要回去，
//      且没有 API 能释放别的进程的电源请求；
//   2) 就算关掉了，任何键鼠输入都会立刻点亮 —— 而在 LL 钩子里吞掉物理输入
//      并不能阻止这一点（实测吞掉 643 个事件后 GetLastInputInfo 仍被钉在 0~109ms）。
// 隐私屏现在由 src/privacy.cpp 实现（全屏遮罩 + WDA_EXCLUDEFROMCAPTURE）。

// 把本进程的 stdout/stderr 重定向到 <OUT>\<logName>（追加，超过 2MB 则重头写）。
//
// 托盘模式会把控制台窗口隐藏起来；若不重定向，所有 LogLine 输出都进了看不见的
// 隐藏控制台，**进程退出后现场全部丢失**（脚本路径有 guard.log，只有双击路径没有）。
// 这会让"双击启动出问题"变成无法事后排查的状态。
// 返回 false 表示重定向失败，此时输出仍走原控制台，功能不受影响。
bool RedirectStdioToLog(const std::wstring& logName, std::wstring* pathOut);

// ---------------------------------------------------------------------------
// 2) 开机自启动（委托给项目里的 autostart 脚本，避免与脚本逻辑重复）
// ---------------------------------------------------------------------------
struct AutostartState {
    bool initRegistered = false;
    bool guardRegistered = false;
    std::wstring initState;
    std::wstring guardState;
    bool guardRunning = false;
    int resprobeProcessCount = 0;
};

AutostartState QueryAutostart();

// guardMode=true 注册「虚拟屏初始化 + 常驻守卫」，false 只注册「虚拟屏初始化」
bool SetAutostart(bool enable, bool guardMode, std::wstring* message);

// ---------------------------------------------------------------------------
// 3) 分辨率
// ---------------------------------------------------------------------------
struct ModeEntry {
    UINT32 width = 0;
    UINT32 height = 0;
    std::wstring text;      // 形如 1920x1080
};

// 取某个 GDI 设备的可选分辨率（已按面积降序、去重、过滤掉 < 640x480 与低色深）
std::vector<ModeEntry> GetModeListOfDevice(const std::wstring& device);

// 取当前主输出的可选分辨率（已按面积降序、去重、过滤掉 < 640x480 与低色深）
std::vector<ModeEntry> GetPrimaryModeList(std::wstring* currentOut,
                                         std::wstring* deviceOut);

// 目标输出的性质（OutputNature / OutputNatureName / ClassifyOutputNature）
// 定义在 display.h —— 失败归因与 verify 的拓扑判定必须共用同一套判据。
struct ResolutionResult {
    bool applied = false;
    LONG resultCode = 0;
    std::wstring resultName;
    std::wstring device;
    std::wstring before;
    std::wstring after;

    OutputNature nature = OutputNature::Unknown;
    bool linkDead = false;      // 仅当 nature == DeadPhysical 时才为 true
    std::vector<std::wstring> supportedModes;   // 失败时该输出实际支持的档位
    std::vector<std::string> attempts;
    std::wstring message;
};

ResolutionResult ApplyResolution(UINT32 width, UINT32 height);

// ---------------------------------------------------------------------------
// 单实例保护
//
// 为什么需要两把独立的锁：守卫与托盘是两个不同的关注点。
//   * 登录任务跑的常驻守卫**没有界面**，用户看不到也无法操作它；
//   * 因此"守卫已在运行"不能阻止用户再起一个托盘（否则用户就没界面可用）；
//   * 但"托盘已在运行"应当阻止重复启动（否则出现两个图标）。
// 另外守卫动作会改显示拓扑，两个进程同时做会互相打架 —— 冷却窗口 kGuardCooldownMs
// 是进程内的，拦不住跨进程，所以必须有一把跨进程的锁。
// ---------------------------------------------------------------------------
enum class GuardOwner {
    ThisProcess = 0,   // 本进程获得守卫权，可以执行守卫动作
    OtherProcess,      // 已被其它进程持有，本进程必须关闭守卫（只做界面）
    Unavailable,       // 连锁都创建不出来，保守起见按"不得守卫"处理
};

GuardOwner AcquireGuardLock();
bool AcquireTrayLock();

// 供界面显示：守卫是否由本进程以外的实例持有
bool IsGuardOwnedByOther();

// ---------------------------------------------------------------------------
// 4) 状态摘要（托盘菜单首行 + 状态对话框）
// ---------------------------------------------------------------------------
struct TrayStatus {
    std::wstring primaryMode;
    std::wstring primaryDevice;
    int totalActive = 0;
    int activeVirtual = 0;
    int healthyPhysical = 0;
    int deadPhysical = 0;
    int virtualAvailable = 0;
    bool elevated = false;
    bool remoteSession = false;
    // 守卫权是否被其它实例持有（登录任务里的常驻守卫）。为 true 时本进程只做界面，
    // 必须让用户看得见 —— 否则"我明明启动程序了，为什么没在守卫"无法解释。
    bool guardOwnedByOther = false;

    std::wstring verdictCode;
    std::wstring verdictSummary;
    std::wstring verdictConfidence;
    std::wstring verdictBlame;
    std::vector<std::wstring> verdictReasons;
    std::vector<std::wstring> verdictRecommendations;
    bool degraded = false;
};

TrayStatus QueryTrayStatus(UINT32 refW, UINT32 refH);

std::wstring BuildStatusReport(const TrayStatus& st, UINT32 refW, UINT32 refH);
std::wstring BuildPathsReport(UINT32 refW, UINT32 refH);

// ---------------------------------------------------------------------------
// 5) 杂项
// ---------------------------------------------------------------------------
// 返回 false 表示 ShellExecute 失败（例如目标不存在、无默认关联程序）
bool OpenFolderInExplorer(const std::wstring& dir);
bool OpenFileInDefaultApp(const std::wstring& path);

// 读取日志文件的尾部若干行，按**混合编码**解码后返回。
//
// 为什么需要它：捕获到的脚本日志天然是两种编码拼起来的 ——
// resprobe.exe 输出 UTF-8，而 cmd 回显中文路径/文件名、以及 schtasks 这类系统工具
// 按 OEM 代码页(936)输出。用记事本或 `Get-Content -Encoding UTF8` 直接看，
// 必然有一半是乱码。走本函数则两侧都能正确显示。
// 找不到文件或读取失败时返回 false。
bool ReadLogTail(const std::wstring& path, size_t maxLines, std::wstring* out);

// 在目录里找最近修改过的 *.log，返回其完整路径；没有则返回空串。
std::wstring FindNewestLogFile(const std::wstring& dir);

// 动作层自检：把托盘菜单里每条动作所依赖的函数都跑一遍（尽量选只读路径），
// 逐项打印结果并返回失败项数量。托盘菜单无法自动点击，这个入口是它唯一
// 可被脚本化验证的方式；同时也便于交付后排查"某条菜单项点了没反应"。
// 唯一会改动系统的两步：autostart 脚本以 /status（只读）调用，
// 分辨率只切到"当前分辨率"（等值写入，不产生实际变化）。
int RunActionSelfTest();

// 托盘模式隐藏控制台窗口。
// 方案 A：保持控制台子系统（脚本契约依赖 stdout 重定向与退出码），
// 托盘运行时只是把控制台窗口藏掉，不改变任何进程语义。
void HideConsoleWindow();

}  // namespace resprobe
