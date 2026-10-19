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

#include <shellapi.h>

namespace resprobe {

// 托盘图标的三种形态，资源 ID 见 res/resprobe.rc（1=默认, 2=绿, 3=蓝）。
//
// 取值优先级（UpdateTrayIcon）：
//   1) Windows 已关闭显示输出（GUID_CONSOLE_DISPLAY_STATE = Off） -> DisplayOff（蓝）
//      放在第一位：这是操作系统层面的事实，比"显示器在不在"更能解释"屏幕为什么黑"。
//   2) 仍有物理显示器挂在线上（活动输出带 EDID 且设备节点在）      -> MonitorOnline（绿）
//   3) 其余情况                                                   -> Default（原图标）
//
// 注意第 2 条的判据是 healthyPhysical：它同时要求 EDID 可读 + 监视器设备节点存在，
// 与 verify.cpp 的 IsLinkHealthy 同源，避免"虚拟屏兜底中"被误当成"显示器在线"。
enum class TrayIconState {
    Default = 0,
    MonitorOnline,
    DisplayOff,
};

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

    HWND hwnd_ = nullptr;
    HINSTANCE inst_ = nullptr;
    HICON icon_ = nullptr;          // 默认（resprobe.rc ID 1）
    HICON iconGreen_ = nullptr;     // 显示器在线（ID 2）
    HICON iconBlue_ = nullptr;      // 显示输出已关闭（ID 3）
    HPOWERNOTIFY powerNotify_ = nullptr;
    DisplayPowerState powerState_ = DisplayPowerState::Unknown;
    TrayIconState iconState_ = TrayIconState::Default;
    UINT_PTR refreshTimerId_ = 0;
    bool iconInstalled_ = false;
    UINT taskbarCreatedMsg_ = 0;
    NOTIFYICONDATAW nid_{};

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
};

}  // namespace resprobe
