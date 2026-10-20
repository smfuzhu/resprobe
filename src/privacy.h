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

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    bool RegisterWindowClass();
    void InstallInputHooks();
    void UninstallInputHooks();

    HWND hwnd_ = nullptr;
    HHOOK kbHook_ = nullptr;
    HHOOK mouseHook_ = nullptr;
    UINT_PTR topmostTimer_ = 0;
    HBRUSH brush_ = nullptr;
    std::wstring error_;
};

}  // namespace resprobe
