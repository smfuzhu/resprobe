// tray.h - 系统托盘图标与右键菜单
//
// 集成方式（重要）：托盘**不侵入** Watcher。
// Watcher::Run 的消息循环是：
//     MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT)
//     while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
// hwnd 传 nullptr 表示"取本线程所有窗口的消息"，因此托盘只需在同一个线程上
// 另建一个隐藏的顶层窗口，它的事件就会由现成的消息泵派发。
// 好处：watcher.cpp 一行都不用改，两个窗口共用一个泵。
//
// 托盘窗口必须是顶层窗口：Shell_NotifyIcon 的回调消息与 TrackPopupMenu
// 都要求 owner 窗口属于当前线程且能被置为前台。
#pragma once

#include "actions.h"
#include "common.h"
#include "privacy.h"

#include <shellapi.h>

namespace resprobe {

// 「显示器关闭后自动进入隐私屏」的一次"在线→离线"周期内的相位。
//
// 单向、一次性：一次离线周期最多自动触发一次。这样用户手动关闭隐私屏后
// 不会被下一轮探测重新打开（自动条件"显示器关着"是持续成立的电平，
// 若按电平判断就会形成关不掉的死循环）。
//
//   Idle ──(在线→离线边沿)──> Pending ──(30 秒到、条件仍成立)──> Active
//     ^                          │                                 │
//     │                     (显示器回来)                    (锁屏/睡眠撤罩)
//     │                          │                                 ↓
//     │                          └──────────────────────────> Suspended
//     │                                                           │
//     │                                              (恢复 + 条件仍成立)
//     │                                                           ↓
//     │                                                        Pending
//     │
//   Idle <──(重新检测到在线)── Suppressed <──(用户手动解除自动开的隐私屏)
enum class AutoPrivacyPhase {
    Idle,        // 显示器在线，等待下一个"在线→离线"边沿
    Pending,     // 检测到离线边沿，30 秒延时中
    Active,      // 隐私屏由自动模式开启且仍在位
    Suspended,   // 被锁屏/睡眠兜底撤除，等系统恢复后按条件重进
    Suppressed,  // 用户手动解除过，本周期不再自动开，直到显示器重新在线
};

// 托盘图标的六种形态，资源 ID 见 res/resprobe.rc
// （1=默认, 2=绿, 3=蓝, 4=红, 5=黄, 6=紫底横条）。
//
// 取值优先级（UpdateTrayIcon），从高到低：
//   0) PrivacyScreen   resprobe 主动发起的隐私屏，且显示输出确实已关闭 -> 紫
//                      用户主动发起、瞬时、最能解释"屏幕为什么黑"，因此压过其余全部。
//                      注意它要求"确实已关闭"：若屏幕被本地输入唤醒，图标会回到真实
//                      状态（绿/黄/红），只有"意图标志"仍保留在菜单文案里。
//   1) NoFallback      没有可用的虚拟显示输出            -> 红
//   2) FallbackActive  没有健康的物理屏，但仍有输出在活动  -> 黄
//   3) DisplayOff      Windows 关闭了显示输出            -> 蓝
//   4) MonitorOnline   仍有物理显示器在线                -> 绿
//   5) Default         其余（含状态未知）                 -> 默认图标
//
// 为什么红/黄要压过蓝："OS 空闲关屏"是常态且无害，而"兜底中 / 无兜底"才是要看的。
// 若蓝压过黄，一次空闲关屏就会把兜底状态盖掉，新增的两态等于白加。
//
// 判据说明：healthyPhysical 同时要求 EDID 可读 + 监视器设备节点存在（与
// IsLinkHealthy 同源），因此"虚拟屏兜底中"不会被误判成"显示器在线"；
// 而 primaryMode 为空表示这次探测什么都没采到（枚举失败 / 无任何活动输出），
// 此时一律退回默认，避免把瞬态误报成红色告警。
enum class TrayIconState {
    Default = 0,
    MonitorOnline,
    DisplayOff,
    FallbackActive,
    NoFallback,
    PrivacyScreen,
};

// 由状态摘要与电源状态推导托盘形态。抽成纯函数是为了让它能在不碰硬件、
// 不需要管理员权限的情况下被完整验证（五种分支各构造一份 TrayStatus 即可）；
// 判据与优先级见上面 TrayIconState 的注释。
TrayIconState DecideTrayIconState(const TrayStatus& st, DisplayPowerState power);

class TrayIcon {
public:
    TrayIcon();
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // 创建隐藏宿主窗口并注册通知图标。refW/refH 为参考分辨率，
    // 菜单与状态查询都以它为准（与命令行 --reference 同一语义）。
    bool Create(UINT32 refW, UINT32 refH);

    // 注销图标并销毁宿主窗口
    void Destroy();

    HWND Window() const { return hwnd_; }

    // 本实例是否承担守卫。启动方（RunTray）知道真实结果（可能因 --no-guard
    // 或守卫权被其它实例占用而为 false），菜单首行要把它显示出来。
    void SetGuardEnabled(bool enabled) { guardEnabled_ = enabled; }

private:
    static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    bool CreateHostWindow();
    void AddNotificationIcon();
    void RemoveNotificationIcon();
    void SetTip(const std::wstring& tip);

    // 依据 status_ 与 powerState_ 决定用哪套图标，只在形态真的变化时才
    // Shell_NotifyIconW(NIM_MODIFY)（无谓的调用会让任务栏图标闪烁）。
    void UpdateTrayIcon();

    // 托盘提示与图标保持同步：状态变化或主输出分辨率变化时更新，
    // 文本没变就不发 NIM_MODIFY。
    void RefreshTip();

    // ---- 隐私屏（工具无关：本地黑屏且动不了，远程端仍看到桌面并能操作）------
    // 菜单开关。实际实现委托给 PrivacyScreen（全屏遮罩 + 捕获排除 + 吞物理键鼠）。
    void TogglePrivacyScreen();

    // ---- 显示器关闭后自动进入隐私屏 ----------------------------------------
    // 触发信号是"物理屏掉线"的**下降沿**（healthyPhysical 由 >0 变 0，且仍
    // 有活动输出排除探测失败），不是 Windows 关闭显示输出（后者紧接着就锁屏，
    // 会被 StopPrivacyForLifecycle 撤掉）。详见 AutoPrivacyPhase。
    //
    // 静默路径：自动触发时屏幕关着没人看，绝不能弹模态框（会无期限扣押托盘
    // 线程，2026-10-21 真机踩过）。失败只写日志，成功也只写日志 + 图标/提示。
    void OnAutoPrivacyTimer();
    void EnterAutoPrivacyPending(const char* reason);
    void CancelAutoPrivacyTimer();
    bool StartPrivacySilently(const char* reason);
    void NoteManualPrivacyToggle(bool nowActive);
    void MaybeResumeAutoPrivacy(const char* reason);
    void EvaluateAutoPrivacyEdge();

    // 隐私屏的"生命周期兜底"：会话锁定 / 系统休眠 / 注销时**主动撤除**遮罩。
    //
    // 为什么必须有：隐私屏生效期间本地键鼠被吞，撤销途径只有菜单、连点 20 下、
    // 进程退出三条。若这时系统空闲休眠或用户按了 Win+L，回来面对的是
    // "全黑 + 键鼠无响应"，看起来就是死机（连点那条是彩蛋，不能指望用户记得）。
    //
    // 语义：**只撤除，不恢复**。隐私屏是显式意图，系统替用户重开比不重开更糟
    //       （解锁后画面突然变黑，用户会当成故障）。
    void StopPrivacyForLifecycle(const char* reason);

    // GUID_CONSOLE_DISPLAY_STATE 通知：WM_POWERBROADCAST / PBT_POWERSETTINGCHANGE
    void OnPowerSetting(WPARAM wp, LPARAM lp);

    // 缓存：避免每次右键都做完整探测（EDID 走 SetupAPI，有毫秒级成本）
    void RefreshStatusCache(bool force);
    void RefreshModeCache(bool force);

    // 菜单
    void ShowContextMenu();
    bool HandleCommand(int id);

    // 各菜单项动作
    void ActShowStatus();
    void ActShowPaths();
    void ActShowRecentLog();
    void ActApplyResolution(UINT32 w, UINT32 h);
    void ActAutostart(bool enable, bool guardMode);
    void ActRunScriptAction(const std::wstring& role, const std::wstring& args,
                            const std::wstring& logName, const std::wstring& title,
                            const std::wstring& successText);
    void ActShowAbout();

    void Info(const std::wstring& title, const std::wstring& text);

    // 非阻塞通知（托盘气球）。隐私屏开启/关闭的确认一律走这里，不用模态弹窗：
    // 模态框会无期限扣押托盘线程，且开启确认框会出现在全屏遮罩之上、却因钩子
    // 吞掉物理点击而点不掉（2026-10-21 真机实证，详见 TogglePrivacyScreen）。
    // 被系统策略静默时不算失败：图标与提示的变化仍是有效反馈。
    void ShowBalloon(const std::wstring& title, const std::wstring& text);

    HWND hwnd_ = nullptr;
    HINSTANCE inst_ = nullptr;
    HICON icon_ = nullptr;          // 默认（resprobe.rc ID 1）
    HICON iconGreen_ = nullptr;     // 显示器在线（ID 2）
    HICON iconBlue_ = nullptr;      // 显示输出已关闭（ID 3）
    HICON iconRed_ = nullptr;       // 无兜底（ID 4）
    HICON iconYellow_ = nullptr;    // 兜底中（ID 5）
    HICON iconPrivacy_ = nullptr;   // 隐私屏（ID 6）
    HPOWERNOTIFY powerNotify_ = nullptr;
    DisplayPowerState powerState_ = DisplayPowerState::Unknown;
    TrayIconState iconState_ = TrayIconState::Default;
    UINT_PTR refreshTimerId_ = 0;
    // 隐私屏实体（全屏遮罩窗口 + 键鼠钩子）。它的 Active() 同时就是菜单文案
    // 与第 6 态图标的唯一依据 —— 不再有"意图标志 / 实际状态"两套状态。
    PrivacyScreen privacy_;
    bool iconInstalled_ = false;
    // 会话通知（WM_WTSSESSION_CHANGE）是否注册成功。通知投递给"注册它的那个窗口"，
    // Watcher 的注册只覆盖它自己的窗口，所以托盘必须自己再注册一份。
    bool wtsRegistered_ = false;
    UINT taskbarCreatedMsg_ = 0;
    NOTIFYICONDATAW nid_{};
    std::wstring lastTip_;          // 上次下发的提示文本，用于避免重复 NIM_MODIFY

    UINT32 refW_ = 1920;
    UINT32 refH_ = 1080;

    TrayStatus status_;
    UINT64 statusStampMs_ = 0;

    std::vector<ModeEntry> modes_;
    std::wstring currentMode_;
    UINT64 modesStampMs_ = 0;

    // 忙标志：任何会弹窗或改系统状态的操作期间置位，
    // 期间忽略新的菜单命令，避免模态对话框叠菜单。
    bool busy_ = false;

    // 本实例是否承担守卫（由启动方设置）
    bool guardEnabled_ = false;

    // ---- 自动隐私屏状态 ----------------------------------------------------
    bool autoPrivacyEnabled_ = true;         // 菜单可勾选，默认启用
    AutoPrivacyPhase autoPhase_ = AutoPrivacyPhase::Idle;
    UINT_PTR autoPrivacyTimerId_ = 0;
    // 边沿检测基线。-1 = 尚未建立：启动时只记基线，不对"显示器本来就关着"
    // 的情况补触发（否则程序一重启就突然黑屏）。
    int lastHealthyPhysical_ = -1;
    bool autoBaselineReady_ = false;
};

}  // namespace resprobe
