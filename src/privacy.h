// privacy.h - 隐私屏（本地黑屏 + 屏蔽本地键鼠，远控端仍可见可操作）
//
// 六条实现要点，每一条都是真机踩出来的坑，改之前先读：
//
//   1) 黑必须发生在"本地看得见、捕获端看不见"的层面。
//      做法 = 全屏置顶黑窗口 + SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)。
//      不要用 SC_MONITORPOWER 关闭显示输出：rustdesk.exe 持着 DISPLAY 电源请求，
//      会在关屏后不到 1 秒把显示输出要回去，而且没有 API 能释放别的进程的请求。
//
//   2) 点击穿透必须用 WS_EX_LAYERED | WS_EX_TRANSPARENT（跨进程有效）。
//      绝不能用 WM_NCHITTEST 返回 HTTRANSPARENT —— 那只在"同一线程"的窗口之间传递，
//      桌面上的真实窗口属于别的进程，实测表现为"光标能动、点击失效"。
//
//   3) 必须 WS_EX_NOACTIVATE + SWP_NOACTIVATE。
//      否则黑窗口一当前台，远控方敲键盘会全部打到这块黑布上，应用收不到一个字符。
//
//   4) 重新置顶必须用 HWND_TOP。
//      对"已经置顶"的窗口再调 SetWindowPos(HWND_TOPMOST) 是空操作，不会在置顶带
//      内部重新排序，压不住同为置顶的任务栏与弹出菜单。
//
//   5) 本地键鼠用 WH_KEYBOARD_LL / WH_MOUSE_LL 按 INJECTED 标志过滤：
//      远控注入的（带 LLKHF_INJECTED / LLMHF_INJECTED）放行，物理输入吞掉。
//      注意：吞物理输入**并不能**阻止显示输出被唤醒（实测吞掉 643 个事件后
//      GetLastInputInfo 仍被钉在 0~109ms），它在这里的作用只是"不让本地人操作桌面"。
//
//   6) 光标盖不住。窗口是穿透的，光标在命中测试上落在下层窗口、由那个窗口决定
//      可见性，改本线程的 ShowCursor 计数无效。这是本方案的结构性代价：
//      本地看到的是"一块黑屏 + 一个光标"，不泄露内容。
//
// 已知边界：Ctrl+Alt+Del 走内核/CSRSS，不经钩子；UAC / Win+L 的安全桌面同理。
//
// 生命周期不归本类管：锁屏 / 休眠 / 注销时的自动撤罩由托盘负责（TrayIcon::
// StopPrivacyForLifecycle），本类**不**监听会话与电源消息。这样遮罩层保持"无状态、
// 只做遮罩"，撤销策略只有一份，不会出现两处各自判断的第三套状态。

#pragma once

#include "common.h"

namespace resprobe {

class PrivacyScreen {
public:
    PrivacyScreen() = default;
    ~PrivacyScreen();

    PrivacyScreen(const PrivacyScreen&) = delete;
    PrivacyScreen& operator=(const PrivacyScreen&) = delete;

    // 开启 / 关闭。可重复调用，内部幂等。
    //
    // Start 失败时**不会留下黑窗口**：先隐藏建窗、校验捕获排除真正生效，再显示。
    // 否则会出现"本地黑了、远控端也黑了"的最坏情况。
    bool Start();
    void Stop();

    bool Active() const { return hwnd_ != nullptr; }

    // Start 返回 false 时的人类可读原因
    const std::wstring& LastError() const { return error_; }

    // ---- 连点解除（逃生路径）-----------------------------------------------
    // 在 10 秒内累计 20 次鼠标按下（任意键）就**解除**隐私屏。
    //
    // 单向：只关不开。所以钩子可以只在遮罩期间装 —— 隐私屏没开时不需要数点击，
    // 也就不必让一个全局鼠标钩子常驻在系统里（少一份侵入、少一份被安全软件盯上的理由）。
    //
    // ⚠ 钩子回调由"安装它的线程"（托盘主线程）的消息泵派发。托盘线程若被长时间
    //   阻塞（不泵消息），超过 LowLevelHooksTimeout(约 300ms) 的事件会被系统直接
    //   跳过 —— 表现就是连点计数"丢点"。所以托盘线程不能做长时间阻塞操作。
    //   （曾据"MessageBox 的模态循环本身在泵消息"判定弹窗安全 —— 那只对钩子超时
    //   成立：2026-10-21 实测，开启后的模态确认框扣押了托盘线程数小时，且该框出现
    //   在遮罩之上却点不掉。隐私屏在位期间托盘线程的规则是**零模态弹窗、零长阻塞**，
    //   确认一律走托盘气球，见 TrayIcon::ShowBalloon。）
    //
    // 判定来源：**本地物理点击与远控注入点击都算**。这与"吞输入"的方向相反，
    // 但两者目的不同 —— 吞输入要放行注入事件好让远控能操作；计数器则要两种来源
    // 都认，否则在纯远程场合（看不到托盘界面）无法把隐私屏关掉。
    static constexpr int kTapCount = 20;
    static constexpr DWORD kTapWindowMs = 10000;

    // 连点达成时 PostMessage 的目标。钩子回调里绝不能直接做窗口操作，
    // 只能投递消息交给主线程。由托盘在 Create 时设置一次。
    void SetNotifyTarget(HWND notifyWnd, UINT notifyMsg);

    // 供钩子回调（普通函数）转发调用，不属于对外 API。
    void CountTap(DWORD now);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    bool RegisterWindowClass();
    void InstallInputHooks();
    void UninstallInputHooks();
    void ResetTapWindow();

    HWND hwnd_ = nullptr;
    HHOOK kbHook_ = nullptr;
    HHOOK mouseHook_ = nullptr;
    UINT_PTR topmostTimer_ = 0;
    HBRUSH brush_ = nullptr;
    std::wstring error_;

    // 连点计数的通知目标
    HWND notifyWnd_ = nullptr;
    UINT notifyMsg_ = 0;
    // 环形缓冲：最近 kTapCount 次按下时间戳。0 表示空槽。
    // 用固定数组而不是 deque，是为了让钩子回调里零堆分配。
    DWORD tapTimes_[kTapCount] = {};
    int tapHead_ = 0;
};

}  // namespace resprobe
