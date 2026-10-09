// tray.cpp - 系统托盘图标与右键菜单实现
#include "tray.h"

#include "report.h"
#include "util.h"
#include "watcher.h"   // RequestStop：菜单里"退出"复用 Ctrl+C 的停机路径

#include <cstdio>

namespace resprobe {

namespace {

const wchar_t kTrayClassName[] = L"ResProbeTrayHost";

// 自定义回调消息：Shell_NotifyIcon 会把鼠标事件投递到这个消息上
const UINT kTrayCallbackMsg = WM_APP + 101;

// 资源脚本里的图标 ID（res/resprobe.rc: `1 ICON "resprobe.ico"`）
const int kIconResourceId = 1;

// 菜单命令 ID
enum MenuId : int {
    kIdStatus        = 1001,
    kIdPaths         = 1002,
    kIdOpenOut       = 1003,
    kIdViewLog       = 1004,
    kIdAbout         = 1005,
    kIdExit          = 1006,

    kIdResBase       = 1100,   // + 索引
    kIdResLast       = 1199,

    kIdAutoGuard     = 1301,
    kIdAutoOnce      = 1302,
    kIdAutoOff       = 1303,
    kIdAutoStatus    = 1304,

    kIdInstall       = 1401,
    kIdUninstall     = 1402,
    kIdUninstallFull = 1403,
};

// 状态缓存有效期：右键菜单不应每次都做完整探测（EDID 走 SetupAPI，有毫秒级成本）。
// 代价是菜单首行最多滞后这么久；任何动作之后都会强制刷新。
const UINT64 kStatusTtlMs = 8000;
const UINT64 kModesTtlMs  = 8000;

std::wstring FormatCount(int n) {
    return std::to_wstring(n);
}

}  // namespace

// ===========================================================================
TrayIcon::TrayIcon() {}

TrayIcon::~TrayIcon() {
    Destroy();
}

bool TrayIcon::Create(UINT32 refW, UINT32 refH) {
    refW_ = refW;
    refH_ = refH;
    inst_ = GetModuleHandleW(nullptr);

    if (!CreateHostWindow()) return false;

    icon_ = LoadIconW(inst_, MAKEINTRESOURCEW(kIconResourceId));
    if (!icon_) {
        // 资源缺失时退化到系统图标，保证托盘仍可用
        icon_ = LoadIconW(nullptr, IDI_APPLICATION);
    }

    AddNotificationIcon();

    // 图标注册失败（例如 explorer.exe 未运行）时必须让调用方知道。
    // 否则进程在跑、控制台被隐藏、托盘却没有图标 —— 用户只会认为程序没启动。
    if (!iconInstalled_) {
        return false;
    }

    // 先做一次探测，让第一次右键就有内容
    RefreshStatusCache(true);
    RefreshModeCache(true);
    SetTip(L"ResProbe " + status_.primaryMode);

    return true;
}

void TrayIcon::Destroy() {
    RemoveNotificationIcon();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
bool TrayIcon::CreateHostWindow() {
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = inst_;
    wc.lpszClassName = kTrayClassName;

    if (!RegisterClassExW(&wc)) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) return false;
    }

    // 必须是顶层窗口（不能是 HWND_MESSAGE）：TrackPopupMenu 要求 owner 能被置为前台
    hwnd_ = CreateWindowExW(0, kTrayClassName, L"ResProbeTray", WS_OVERLAPPED,
                            0, 0, 0, 0, nullptr, nullptr, inst_, this);
    if (!hwnd_) return false;

    // 不调用 ShowWindow，窗口保持隐藏
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    return true;
}

void TrayIcon::AddNotificationIcon() {
    if (!hwnd_ || iconInstalled_) return;

    ZeroMemory(&nid_, sizeof(nid_));
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = kTrayCallbackMsg;
    nid_.hIcon = icon_;
    wcsncpy_s(nid_.szTip, L"ResProbe", _TRUNCATE);

    // 刻意不调用 NIM_SETVERSION：保持旧版回调语义
    // （lParam 直接就是鼠标消息），处理起来最直观。
    iconInstalled_ = (Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE);
}

void TrayIcon::RemoveNotificationIcon() {
    if (!iconInstalled_) return;
    Shell_NotifyIconW(NIM_DELETE, &nid_);
    iconInstalled_ = false;
}

void TrayIcon::SetTip(const std::wstring& tip) {
    if (!iconInstalled_) return;

    std::wstring t = tip;
    if (t.size() > 120) t.resize(120);
    wcsncpy_s(nid_.szTip, t.c_str(), _TRUNCATE);
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

// ---------------------------------------------------------------------------
void TrayIcon::RefreshStatusCache(bool force) {
    const UINT64 now = MonotonicMs();
    if (!force && statusStampMs_ != 0 && (now - statusStampMs_) < kStatusTtlMs) {
        return;
    }
    status_ = QueryTrayStatus(refW_, refH_);
    statusStampMs_ = now;
}

void TrayIcon::RefreshModeCache(bool force) {
    const UINT64 now = MonotonicMs();
    if (!force && modesStampMs_ != 0 && (now - modesStampMs_) < kModesTtlMs) {
        return;
    }
    modes_ = GetPrimaryModeList(&currentMode_, nullptr);
    modesStampMs_ = now;
}

// ---------------------------------------------------------------------------
LRESULT CALLBACK TrayIcon::TrayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    TrayIcon* self = nullptr;

    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<TrayIcon*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->OnMessage(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TrayIcon::OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // explorer.exe 重启后任务栏会重建，需要重新注册图标，否则图标消失
    if (taskbarCreatedMsg_ != 0 && msg == taskbarCreatedMsg_) {
        iconInstalled_ = false;
        AddNotificationIcon();
        SetTip(L"ResProbe " + status_.primaryMode);
        return 0;
    }

    switch (msg) {
        case kTrayCallbackMsg: {
            switch (LOWORD(lp)) {
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    ShowContextMenu();
                    return 0;
                case WM_LBUTTONUP:
                case WM_LBUTTONDBLCLK:
                    ActShowStatus();
                    return 0;
                default:
                    return 0;
            }
        }

        case WM_CLOSE:
            // 菜单里的"退出"走这里，避免在 WndProc 内部同步 DestroyWindow 自己
            RequestStop();
            DestroyWindow(hwnd_);
            return 0;

        case WM_ENDSESSION:
            // 注销/关机时立刻摘掉图标，避免留下"幽灵图标"
            RemoveNotificationIcon();
            return 0;

        case WM_DESTROY:
            RemoveNotificationIcon();
            PostQuitMessage(0);
            return 0;

        case WM_NCDESTROY:
            // 窗口已进入销毁流程：必须把句柄清掉。
            // 否则 Run() 返回后调用的 Destroy() 会对一个已经销毁的 HWND 再调
            // DestroyWindow / Shell_NotifyIcon，属悬垂句柄使用。
            RemoveNotificationIcon();
            hwnd_ = nullptr;
            return DefWindowProcW(hwnd, msg, wp, lp);

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
void TrayIcon::ShowContextMenu() {
    if (!hwnd_ || busy_) return;

    RefreshStatusCache(false);
    RefreshModeCache(false);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    // ---- 状态首行（不可点，仅展示）-----------------------------------------
    {
        std::wstring line = status_.primaryMode.empty() ? L"(状态未知)" : status_.primaryMode;
        line += L"   虚拟屏 " + FormatCount(status_.virtualAvailable);
        if (status_.deadPhysical > 0) {
            line += L"   断链 " + FormatCount(status_.deadPhysical);
        }
        line += L"   ";
        if (!guardEnabled_) {
            line += L"守卫:关";
        } else if (status_.guardOwnedByOther) {
            line += L"守卫:其它实例";
        } else {
            line += L"守卫:本实例";
        }
        if (status_.degraded) line += L"   [异常]";
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, line.c_str());
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdStatus, L"完整状态报告...");
    AppendMenuW(menu, MF_STRING, kIdPaths, L"列出显示输出路径...");

    // ---- 分辨率 -------------------------------------------------------------
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    {
        HMENU res = CreatePopupMenu();
        if (modes_.empty()) {
            AppendMenuW(res, MF_STRING | MF_GRAYED, 0, L"(未取到模式列表)");
        }
        const size_t kMaxItems = (size_t)(kIdResLast - kIdResBase + 1);
        const size_t shown = (modes_.size() < kMaxItems) ? modes_.size() : kMaxItems;
        for (size_t i = 0; i < shown; ++i) {
            UINT flags = MF_STRING;
            if (modes_[i].text == currentMode_) flags |= MF_CHECKED;
            AppendMenuW(res, flags, (UINT_PTR)(kIdResBase + (int)i),
                        modes_[i].text.c_str());
        }
        // 超过菜单容量时必须**明说**，否则用户会以为列表就这么多，无从察觉被截断
        if (modes_.size() > shown) {
            AppendMenuW(res, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(res, MF_STRING | MF_GRAYED, 0,
                        (L"… 另有 " + std::to_wstring(modes_.size() - shown) +
                         L" 项未显示（菜单上限 " + std::to_wstring(kMaxItems) +
                         L" 项）").c_str());
        }
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)res,
                    (L"调整分辨率" + (currentMode_.empty() ? std::wstring()
                                                            : L"（当前 " + currentMode_ + L"）"))
                        .c_str());
    }

    // ---- 开机自启动 ---------------------------------------------------------
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    {
        HMENU autoMenu = CreatePopupMenu();
        AppendMenuW(autoMenu, MF_STRING, kIdAutoGuard,
                    L"启用（虚拟屏初始化 + 常驻守卫）");
        AppendMenuW(autoMenu, MF_STRING, kIdAutoOnce, L"启用（仅虚拟屏初始化）");
        AppendMenuW(autoMenu, MF_STRING, kIdAutoOff, L"停用（移除全部任务）");
        AppendMenuW(autoMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(autoMenu, MF_STRING, kIdAutoStatus, L"查看当前状态...");

        AppendMenuW(menu, MF_POPUP, (UINT_PTR)autoMenu, L"开机自启动");
    }

    // ---- 虚拟显示器 ---------------------------------------------------------
    {
        HMENU vd = CreatePopupMenu();
        AppendMenuW(vd, MF_STRING, kIdInstall, L"安装 / 修复");
        AppendMenuW(vd, MF_STRING, kIdUninstall, L"卸载（保留驱动）");
        AppendMenuW(vd, MF_STRING, kIdUninstallFull, L"彻底卸载（移除驱动）");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)vd, L"虚拟显示器");
    }

    // ---- 文件与退出 ---------------------------------------------------------
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdViewLog, L"查看最近的日志...");
    AppendMenuW(menu, MF_STRING, kIdOpenOut, L"打开输出目录");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdAbout, L"关于 ResProbe");
    AppendMenuW(menu, MF_STRING, kIdExit, L"退出");

    POINT pt;
    GetCursorPos(&pt);

    // TrackPopupMenu 的经典要求：先把自己置前台，否则点击菜单外部不会关闭
    SetForegroundWindow(hwnd_);
    const int cmd = (int)TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);

    // 同样来自经典做法：置前台后需补一条消息，否则菜单可能"粘住"
    PostMessageW(hwnd_, WM_NULL, 0, 0);

    if (cmd != 0) {
        HandleCommand(cmd);
    }
}

// ---------------------------------------------------------------------------
bool TrayIcon::HandleCommand(int id) {
    if (id == 0) return true;
    if (busy_) {
        // 上一个操作尚未结束：模态弹窗的嵌套消息循环里用户可能又点了托盘
        return true;
    }

    switch (id) {
        case kIdStatus:
            ActShowStatus();
            return true;
        case kIdPaths:
            ActShowPaths();
            return true;
        case kIdOpenOut:
            // ShellExecute 会失败（资源管理器被替换、路径不存在、被策略拦截），
            // 静默失败会让人以为菜单点了没反应
            if (!OpenFolderInExplorer(GetOutDirPath())) {
                Info(L"打开输出目录失败",
                     L"无法调用资源管理器打开：\r\n" + GetOutDirPath() +
                         L"\r\n\r\nShellExecute 返回失败，可能是资源管理器未运行或被策略限制。");
            }
            return true;
        case kIdViewLog:
            ActShowRecentLog();
            return true;
        case kIdAbout:
            ActShowAbout();
            return true;
        case kIdExit:
            // 投递 WM_CLOSE 而不是直接 DestroyWindow：这里是本窗口的 WndProc
            // 调用栈内部，异步关闭更稳。WM_CLOSE 里会置停止标志，
            // Watcher 的循环随即退出，进程正常收尾。
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            return true;

        case kIdAutoGuard:
            ActAutostart(true, true);
            return true;
        case kIdAutoOnce:
            ActAutostart(true, false);
            return true;
        case kIdAutoOff:
            ActAutostart(false, false);
            return true;
        case kIdAutoStatus: {
            AutostartState as = QueryAutostart();
            std::wstring t;
            t += L"虚拟屏初始化任务 : ";
            t += as.initRegistered ? L"已注册" : L"未注册";
            t += L"\r\n    任务名 " + std::wstring(L"ResProbe VDisplay Init") + L"\r\n\r\n";
            t += L"常驻守卫任务     : ";
            t += as.guardRegistered ? L"已注册" : L"未注册";
            t += L"\r\n    任务名 " + std::wstring(L"ResProbe Guard") + L"\r\n\r\n";
            t += L"其它 resprobe 进程 : " + FormatCount(as.resprobeProcessCount) +
                 L" 个（通常即常驻守卫）\r\n";
            Info(L"开机自启动状态", t);
            return true;
        }

        case kIdInstall:
            ActRunScriptAction(
                L"install", L"", L"tray-install.log", L"安装 / 修复虚拟显示器",
                L"安装完成。虚拟显示器已就绪，物理屏断电时桌面不会再回退。");
            return true;
        case kIdUninstall:
            ActRunScriptAction(
                L"uninstall", L"", L"tray-uninstall.log", L"卸载（保留驱动）",
                L"已卸载虚拟显示器与计划任务。驱动包保留，可随时重新安装。");
            return true;
        case kIdUninstallFull:
            ActRunScriptAction(
                L"uninstall", L"/full", L"tray-uninstall-full.log", L"彻底卸载（移除驱动）",
                L"已移除驱动。需要重新安装时会重新创建驱动与虚拟显示器。");
            return true;

        default:
            break;
    }

    // 分辨率项
    if (id >= kIdResBase && id <= kIdResLast) {
        const size_t idx = (size_t)(id - kIdResBase);
        if (idx < modes_.size()) {
            ActApplyResolution(modes_[idx].width, modes_[idx].height);
        }
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
void TrayIcon::ActShowStatus() {
    if (busy_) return;

    busy_ = true;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    RefreshStatusCache(true);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));

    const std::wstring text = BuildStatusReport(status_, refW_, refH_);
    SetTip(L"ResProbe " + status_.primaryMode);

    // busy_ 保持到弹窗之后才清：MessageBox 有自己的嵌套消息循环，
    // 期间用户若又点了托盘，busy_ 能拦住叠菜单
    Info(L"ResProbe 状态报告", text);
    busy_ = false;
}

void TrayIcon::ActShowRecentLog() {
    if (busy_) return;

    busy_ = true;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    const std::wstring dir = GetOutDirPath();
    const std::wstring newest = FindNewestLogFile(dir);

    SetCursor(LoadCursorW(nullptr, IDC_ARROW));

    if (newest.empty()) {
        Info(L"最近的日志",
             L"输出目录里还没有 .log 文件：\r\n" + dir +
                 L"\r\n\r\n先执行一次监听或托盘菜单里的安装动作即可生成。");
        busy_ = false;
        return;
    }

    std::wstring tail;
    const bool ok = ReadLogTail(newest, 40, &tail);

    std::wstring text = L"文件：" + newest + L"\r\n\r\n";
    if (ok && !tail.empty()) {
        // 日志里 UTF-8 与 GBK 并存（resprobe 输出 UTF-8，cmd 回显中文路径、
        // schtasks 等系统工具输出 OEM 936），因此这里由程序解码后展示，
        // 直接用记事本打开会有一半是乱码。
        text += tail;
    } else {
        text += L"(读取失败或日志为空)";
    }

    Info(L"最近的日志（尾部 40 行）", text);
    busy_ = false;
}

void TrayIcon::ActShowPaths() {
    if (busy_) return;

    busy_ = true;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const std::wstring text = BuildPathsReport(refW_, refH_);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));

    Info(L"显示输出路径", text);
    busy_ = false;
}

void TrayIcon::ActApplyResolution(UINT32 w, UINT32 h) {
    if (busy_) return;

    const std::wstring want = FormatModeText(w, h);
    if (want == currentMode_) {
        Info(L"调整分辨率", L"当前已经是 " + want + L"，无需调整。");
        return;
    }

    busy_ = true;
    SetTip(L"ResProbe 正在切换到 " + want + L" ...");
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    ResolutionResult rr = ApplyResolution(w, h);

    SetCursor(LoadCursorW(nullptr, IDC_ARROW));

    // 分辨率变了，缓存全部作废
    RefreshModeCache(true);
    RefreshStatusCache(true);
    SetTip(L"ResProbe " + status_.primaryMode);

    const bool ok = (rr.applied && rr.after == want);
    std::wstring text = rr.message;
    if (ok) {
        text += L"\r\n\r\n回写前：" + rr.before + L"\r\n回写后：" + rr.after;
    }

    Info(ok ? L"调整分辨率成功" : L"调整分辨率失败", text);
    busy_ = false;
}

void TrayIcon::ActAutostart(bool enable, bool guardMode) {
    if (busy_) return;

    if (!enable) {
        // 停用会移除兜底保护，先确认
        const int ans = MessageBoxW(
            nullptr,
            L"停用开机自启动会移除全部 ResProbe 计划任务。\n\n"
            L"之后物理关闭显示器仍可能让桌面回退到 1024x768，需要手动运行程序恢复。\n\n"
            L"确定要停用吗？",
            L"停用开机自启动", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (ans != IDYES) return;
    }

    busy_ = true;
    SetTip(L"ResProbe 正在更新开机自启动 ...");
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    std::wstring message;
    const bool ok = SetAutostart(enable, guardMode, &message);

    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    AutostartState as = QueryAutostart();
    SetTip(L"ResProbe " + status_.primaryMode);

    std::wstring text = message;
    text += L"\r\n\r\n当前状态：\r\n";
    text += L"  虚拟屏初始化 : ";
    text += as.initRegistered ? L"已注册" : L"未注册";
    text += L"\r\n  常驻守卫     : ";
    text += as.guardRegistered ? L"已注册" : L"未注册";

    Info(ok ? L"开机自启动" : L"开机自启动设置失败", text);
    busy_ = false;
}

void TrayIcon::ActRunScriptAction(const std::wstring& role, const std::wstring& args,
                                  const std::wstring& logName,
                                  const std::wstring& title,
                                  const std::wstring& successText) {
    if (busy_) return;

    const int ans = MessageBoxW(
        nullptr,
        (L"即将执行：" + title +
         L"\n\n这会修改系统状态（显示驱动 / 设备节点 / 显示拓扑 / 计划任务），"
         L"可能需要 10~60 秒。\n执行期间请勿重复点击托盘菜单。\n\n确定继续吗？")
            .c_str(),
        title.c_str(), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (ans != IDYES) return;

    busy_ = true;
    SetTip(L"ResProbe 正在执行：" + title + L" ...");
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    ScriptRunResult sr = RunProjectScript(GetHomeDir(), role, args, logName, 300000);

    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    RefreshStatusCache(true);
    RefreshModeCache(true);
    SetTip(L"ResProbe " + status_.primaryMode);

    std::wstring text;

    if (!sr.found) {
        text = L"未找到所需的脚本。\n\n"
               L"该脚本需要在独立一行声明角色标记：\n"
               L"    @rem RESPROBE-ROLE:" + role;
        Info(L"找不到脚本", text);
        busy_ = false;
        return;
    }

    if (sr.ok) {
        text = successText;
    } else {
        text = L"执行未成功。";
        wchar_t codeBuf[32];
        _snwprintf_s(codeBuf, _countof(codeBuf), _TRUNCATE, L"0x%08lX",
                     (unsigned long)sr.exitCode);
        text += L"\r\n退出码：" + std::to_wstring(sr.exitCode) + L"（" + codeBuf + L"）";
        if (sr.timedOut) {
            text += sr.killed
                        ? L"\r\n注意：等待超时（5 分钟），脚本及其子进程已被强制终止，"
                          L"系统可能停在中间状态，请重跑一次。"
                        : L"\r\n注意：等待超时（5 分钟）且未能终止脚本，"
                          L"它可能仍在后台继续修改系统。";
        }
    }

    text += L"\r\n耗时：" + std::to_wstring(sr.elapsedMs / 1000) + L" 秒";
    text += L"\r\n日志：" + sr.logPath;
    if (!sr.tail.empty()) {
        text += L"\r\n\r\n--- 日志尾部 ---\r\n" + sr.tail;
    }

    Info(sr.ok ? title : (title + L"（失败）"), text);
    busy_ = false;
}

void TrayIcon::ActShowAbout() {
    std::wstring t;
    t += std::wstring(L"ResProbe ") + U82W(RESPROBE_VERSION) + L"\r\n\r\n";
    t += L"Windows 显示器分辨率变化探测与远程桌面诊断工具。\r\n\r\n";
    t += L"本工具用于定位「关闭显示器后分辨率回退到 1024x768」这类问题：\r\n"
         L"  - 区分是主机显示链路（EDID/HPD）问题，还是远程软件问题\r\n"
         L"  - 通过 usbmmidd 虚拟显示器提供兜底，物理屏断电时保持 1920x1080\r\n"
         L"  - 常驻监听显示事件，链路断开时自动接管\r\n\r\n";
    t += L"程序目录：" + GetHomeDir() + L"\r\n";
    t += L"输出目录：" + GetOutDirPath() + L"\r\n\r\n";
    t += L"命令行子命令：snapshot / watch / verify / paths / record / ensure / restore / tray";

    Info(L"关于 ResProbe", t);
}

void TrayIcon::Info(const std::wstring& title, const std::wstring& text) {
    // 宿主窗口是隐藏的，用 nullptr 作为 owner 让对话框成为顶层窗口，
    // 并显式要求置前台，否则可能被其它窗口挡住。
    MessageBoxW(nullptr, text.c_str(), title.c_str(),
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

}  // namespace resprobe
