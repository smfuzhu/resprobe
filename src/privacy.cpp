// privacy.cpp - 隐私屏实现，设计说明与踩坑记录见 privacy.h
#include "privacy.h"

#include "util.h"

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#ifndef WDA_NONE
#define WDA_NONE 0x00000000
#endif

namespace resprobe {

namespace {

const wchar_t kClassName[] = L"ResProbePrivacyOverlay";
const UINT_PTR kTopmostTimerId = 1;
// 100ms：既要能压住"后冒出来"的任务栏/弹出菜单，又不能太密到影响输入响应。
// 实测 100ms 足够（人眼能感知的最短闪烁也在 100ms 量级）。
const UINT kTopmostIntervalMs = 100;

// 1 = 正在吞物理键鼠。用文件级变量是因为钩子回调只能是普通函数。
volatile LONG g_swallow = 0;

LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode == HC_ACTION && g_swallow) {
        auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        // 只吞"非注入"的：远控方的 SendInput 带 INJECTED 标志，必须放行，
        // 否则会把远控的键盘一起吞掉（真机 probe 实测 RustDesk 键鼠都带该标志）。
        if (k && !(k->flags & LLKHF_INJECTED)) return 1;
    }
    return CallNextHookEx(nullptr, nCode, wp, lp);
}

LRESULT CALLBACK MouseProc(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode == HC_ACTION && g_swallow) {
        auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        if (m && !(m->flags & LLMHF_INJECTED)) return 1;
    }
    return CallNextHookEx(nullptr, nCode, wp, lp);
}

}  // namespace

// ---------------------------------------------------------------------------
PrivacyScreen::~PrivacyScreen() {
    Stop();
}

bool PrivacyScreen::RegisterWindowClass() {
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;   // 漏了这句会得到 ERROR_INVALID_PARAMETER(87)
    if (!RegisterClassExW(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            error_ = L"注册窗口类失败，错误码 " + std::to_wstring(GetLastError());
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
bool PrivacyScreen::Start() {
    if (hwnd_) return true;
    error_.clear();

    if (!RegisterWindowClass()) return false;

    if (!brush_) brush_ = CreateSolidBrush(RGB(0, 0, 0));

    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) {
        error_ = L"取不到虚拟桌面尺寸（无可用显示器？）";
        return false;
    }

    // 样式说明见 privacy.h 第 2、3 条。
    //
    // 先建在屏幕外（-32000,-32000），校验通过后再摆到正确位置。
    // 这样"捕获排除不支持"时不会有任何一帧黑屏闪到用户眼前。
    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED |
            WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        kClassName, L"ResProbePrivacyOverlay", WS_POPUP,
        -32000, -32000, vw, vh, nullptr, nullptr, GetModuleHandleW(nullptr), this);

    if (!hwnd_) {
        error_ = L"创建遮罩窗口失败，错误码 " + std::to_wstring(GetLastError());
        return false;
    }

    // 分层窗口必须先设不透明度，否则它没有可用的合成表面，
    // SetWindowDisplayAffinity 会失败（实测返回 ERROR_NOT_ENOUGH_MEMORY 8）。
    // 顺序不能调换 —— 这一点在独立探针里因为别的原因侥幸通过，移植过来才暴露。
    if (!SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA)) {
        error_ = L"设置分层窗口不透明度失败，错误码 " +
                 std::to_wstring(GetLastError()) + L"，已放弃开启。";
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    // 有些系统还要求窗口已显示才允许设置捕获排除，所以先显示（仍在屏幕外）。
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);

    // ---- 关键一步：标记为"禁止被捕获"，并校验真的生效 ----------------------
    // 必须在这里就确认，失败就立刻放弃 —— 否则本地黑了、远控端也黑，
    // 等于把远控端一起弄瞎，比不开隐私屏更糟。
    SetLastError(0);
    if (!SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE)) {
        const DWORD err = GetLastError();
        error_ = L"当前系统不支持 WDA_EXCLUDEFROMCAPTURE（需 Windows 10 2004 及以上），"
                 L"错误码 " + std::to_wstring(err) +
                 L"。已放弃开启，屏幕未受影响。";
        ShowWindow(hwnd_, SW_HIDE);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    DWORD affinity = 0;
    if (!GetWindowDisplayAffinity(hwnd_, &affinity) ||
        affinity != WDA_EXCLUDEFROMCAPTURE) {
        error_ = L"捕获排除设置未能生效（读回值不符），已放弃开启，屏幕未受影响。";
        ShowWindow(hwnd_, SW_HIDE);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    // 校验通过，摆到真正的虚拟桌面位置
    SetWindowPos(hwnd_, HWND_TOP, vx, vy, vw, vh,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);

    // 周期置顶：压住后冒出来的任务栏 / 开始菜单 / 弹出菜单（见 privacy.h 第 4 条）
    topmostTimer_ = SetTimer(hwnd_, kTopmostTimerId, kTopmostIntervalMs, nullptr);

    InstallInputHooks();

    LogLine("[隐私屏] 已开启：遮罩 " + std::to_string(vw) + "x" + std::to_string(vh) +
            "，捕获排除已生效，本地键鼠" +
            ((kbHook_ && mouseHook_) ? "已屏蔽" : "屏蔽失败（仍可操作，但看不见）"));
    return true;
}

// ---------------------------------------------------------------------------
void PrivacyScreen::Stop() {
    if (!hwnd_) return;

    UninstallInputHooks();

    if (topmostTimer_) {
        KillTimer(hwnd_, kTopmostTimerId);
        topmostTimer_ = 0;
    }
    SetWindowDisplayAffinity(hwnd_, WDA_NONE);
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;

    if (brush_) {
        DeleteObject(brush_);
        brush_ = nullptr;
    }
    error_.clear();
}

// ---------------------------------------------------------------------------
void PrivacyScreen::InstallInputHooks() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    InterlockedExchange(&g_swallow, 1);
    kbHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, inst, 0);
    mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, inst, 0);
    // 钩子回调由安装线程的消息泵派发。托盘的线程本来就在泵消息，所以这里
    // 不需要自己建循环；但**绝不能**在没有消息泵的线程上调用（超时会被系统摘掉）。
    if (!kbHook_ && !mouseHook_) {
        InterlockedExchange(&g_swallow, 0);
        LogLine("[隐私屏] 提示：键鼠钩子安装失败，本地用户仍可操作（但看不见）");
    }
}

void PrivacyScreen::UninstallInputHooks() {
    InterlockedExchange(&g_swallow, 0);
    if (kbHook_) { UnhookWindowsHookEx(kbHook_); kbHook_ = nullptr; }
    if (mouseHook_) { UnhookWindowsHookEx(mouseHook_); mouseHook_ = nullptr; }
}

// ---------------------------------------------------------------------------
LRESULT CALLBACK PrivacyScreen::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PrivacyScreen* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<PrivacyScreen*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<PrivacyScreen*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->OnMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT PrivacyScreen::OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            if (wp == kTopmostTimerId) {
                // HWND_TOP 而不是 HWND_TOPMOST：后者对已置顶窗口是空操作，
                // 不会在置顶带内部重排，压不住同为置顶的任务栏与弹出菜单。
                SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                             SWP_NOOWNERZORDER);
                return 0;
            }
            break;

        case WM_ERASEBKGND:
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, brush_ ? brush_ : (HBRUSH)GetStockObject(BLACK_BRUSH));
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DISPLAYCHANGE:
            // 分辨率/显示器拓扑变了，重新铺满新的虚拟桌面
            SetWindowPos(hwnd, HWND_TOP,
                         GetSystemMetrics(SM_XVIRTUALSCREEN),
                         GetSystemMetrics(SM_YVIRTUALSCREEN),
                         GetSystemMetrics(SM_CXVIRTUALSCREEN),
                         GetSystemMetrics(SM_CYVIRTUALSCREEN),
                         SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;

        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return DefWindowProcW(hwnd, msg, wp, lp);

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace resprobe
