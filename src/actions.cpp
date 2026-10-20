// actions.cpp - 托盘菜单可执行的特权动作实现
//
// 与 UI 解耦：本文件不引用任何窗口、菜单或托盘类型，只做动作并返回结果结构。
// 唯一的 UI 耦合是"等待期间泵消息"（PumpSleep），这是为了让托盘在长操作
// （装驱动、跑脚本）期间不假死 —— 托盘与监听共用同一个消息泵。
#include "actions.h"

#include "display.h"
#include "encoding.h"
#include "report.h"
#include "session.h"
#include "util.h"
#include "verdict.h"
#include "verify.h"
#include "watcher.h"   // RequestStop：PumpSleep 收到 WM_QUIT 时用于统一停机

#include <shellapi.h>
#include <tlhelp32.h>

#include <cstdlib>

namespace resprobe {

namespace {

// 与 开机自启.cmd 的 CONFIG BLOCK 保持一致（改一处必须同步另一处）。
// 之所以不放进公共头文件：只有本文件需要查询这两个任务名。
const wchar_t kTaskNameInit[]  = L"ResProbe VDisplay Init";
const wchar_t kTaskNameGuard[] = L"ResProbe Guard";

// 脚本角色标记的前缀。脚本里必须**独占一行**写 `@rem RESPROBE-ROLE:<marker>`。
const wchar_t kRolePrefix[] = L"@rem RESPROBE-ROLE:";

// ---------------------------------------------------------------------------
// 编解码实现（TryDecodeWhole / Utf8SequenceLen / DecodeMixed / DecodeBestEffort）
// 已移到 encoding.cpp —— 抽出匿名命名空间后，这些纯函数可被单元测试直接链接。
// 本文件只保留文件读取工具。
// ---------------------------------------------------------------------------

// 读取文件字节。大文件只读 64 KB，但**读哪一端由 keepHead 决定**，这一点很关键：
//   keepHead = false（默认）-> 保留**尾部**：日志很长时我们要的是最后的现场。
//   keepHead = true          -> 保留**头部**：脚本的角色标记写在第 2 行，
//                               若用尾部读取，超过 64 KB 的脚本会定位失败。
bool ReadFileBytes(const std::wstring& path, std::string* out, bool keepHead = false) {
    if (!out) return false;
    out->clear();

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    ZeroMemory(&size, sizeof(size));
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0) {
        CloseHandle(h);
        return false;
    }

    const long long kMaxRead = 64 * 1024;
    long long toRead = size.QuadPart;
    long long skip = 0;
    if (toRead > kMaxRead) {
        toRead = kMaxRead;
        skip = keepHead ? 0 : (size.QuadPart - kMaxRead);
    }

    LARGE_INTEGER pos;
    ZeroMemory(&pos, sizeof(pos));
    pos.QuadPart = skip;
    if (skip > 0 && !SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) {
        CloseHandle(h);
        return false;
    }

    out->resize((size_t)toRead);
    DWORD got = 0;
    BOOL ok = ReadFile(h, &(*out)[0], (DWORD)toRead, &got, nullptr);
    CloseHandle(h);
    if (!ok) { out->clear(); return false; }
    out->resize(got);
    return true;
}

void TrimW(std::wstring* s) {
    if (!s) return;
    size_t b = s->find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) { s->clear(); return; }
    size_t e = s->find_last_not_of(L" \t\r\n");
    *s = s->substr(b, e - b + 1);
}

// 取文本尾部 maxLines 行
std::wstring TailLines(const std::wstring& text, size_t maxLines) {
    if (text.empty()) return std::wstring();

    std::vector<std::wstring> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find(L'\n', pos);
        std::wstring line = (eol == std::wstring::npos) ? text.substr(pos)
                                                        : text.substr(pos, eol - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back(line);
        if (eol == std::wstring::npos) break;
        pos = eol + 1;
    }
    // 去掉纯尾部空行
    while (!lines.empty() && lines.back().empty()) lines.pop_back();

    size_t begin = (lines.size() > maxLines) ? (lines.size() - maxLines) : 0;
    std::wstring out;
    for (size_t i = begin; i < lines.size(); ++i) {
        if (!out.empty()) out += L"\r\n";
        out += lines[i];
    }
    return out;
}

int CountOtherProcesses(const std::wstring& exeName) {
    int n = 0;
    DWORD self = GetCurrentProcessId();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        const std::wstring want = UpperW(exeName);
        do {
            if (pe.th32ProcessID == self) continue;   // 不把自己算进去
            if (UpperW(pe.szExeFile) == want) ++n;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return n;
}

// 直接读取某个 GDI 设备当前的模式文本
std::wstring CurrentModeOfDevice(const std::wstring& dev) {
    DEVMODEW dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsExW(dev.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0)) {
        return std::wstring();
    }
    return FormatModeText(dm.dmPelsWidth, dm.dmPelsHeight);
}

// 找到主输出的 GDI 设备名
std::wstring FindPrimaryGdiDevice() {
    for (DWORD i = 0; i < 16; ++i) {
        DISPLAY_DEVICEW dd;
        ZeroMemory(&dd, sizeof(dd));
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(nullptr, i, &dd, 0)) break;
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
            return dd.DeviceName;
        }
    }
    return L"\\\\.\\DISPLAY1";
}

// 创建"句柄关闭即杀掉整棵进程树"的作业对象。
//
// 用途：脚本超时后必须把 cmd 及其派生的子进程（deviceinstaller64 / pnputil /
// powershell 等）一并终止。只 TerminateProcess(cmd) 会留下子进程继续修改系统状态，
// 而用户已经在对话框里看到"执行失败"了 —— 这种"报告与现实不一致"比超时本身更危险。
//
// 返回 nullptr 表示环境不支持（例如本进程处在一个不允许嵌套的作业里）。
// Win8+ 支持嵌套作业，所以正常环境下应当成功；失败时调用方退化为只杀直接子进程。
HANDLE CreateKillOnCloseJob() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
    ZeroMemory(&li, sizeof(li));
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li,
                                 sizeof(li))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

struct WaitResult {
    bool finished = false;   // 进程自行结束
    bool timedOut = false;
    bool killed = false;     // 因超时被执行了终止
    DWORD elapsedMs = 0;
};

// 等待一个进程结束，期间持续泵消息（这是托盘不假死的关键）。
// 超时后先尝试杀整棵进程树，再退回只终止直接子进程。
WaitResult WaitWithPump(HANDLE proc, HANDLE job, DWORD timeoutMs) {
    WaitResult r;
    const DWORD start = GetTickCount();

    for (;;) {
        if (WaitForSingleObject(proc, 0) == WAIT_OBJECT_0) {
            r.finished = true;
            break;
        }
        if ((GetTickCount() - start) > timeoutMs) {
            r.timedOut = true;
            break;
        }
        PumpSleep(50);
    }

    if (r.timedOut) {
        if (job) r.killed = (TerminateJobObject(job, 1) != FALSE);
        if (!r.killed) r.killed = (TerminateProcess(proc, 1) != FALSE);
        // 给内核时间回收，避免随后读到中间态
        WaitForSingleObject(proc, 5000);
    }

    r.elapsedMs = GetTickCount() - start;
    return r;
}

}  // namespace

// ===========================================================================
// 通用
// ===========================================================================
void PumpSleep(DWORD ms) {
    DWORD start = GetTickCount();
    for (;;) {
        DWORD elapsed = GetTickCount() - start;
        if (elapsed >= ms) break;

        DWORD remain = ms - elapsed;
        DWORD slice = (remain > 50) ? 50 : remain;

        // 0 个句柄 + QS_ALLINPUT：只等消息。与 Watcher::Run 的循环同一模式，
        // 保证托盘与监听共用同一个泵时都不会饿死对方。
        DWORD w = MsgWaitForMultipleObjects(0, nullptr, FALSE, slice, QS_ALLINPUT);
        if (w == WAIT_OBJECT_0) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    RequestStop();
                    return;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
}

bool RunAndCapture(const std::wstring& commandLine, const std::wstring& workDir,
                   DWORD timeoutMs, DWORD* exitCodeOut, std::wstring* outputOut) {
    if (exitCodeOut) *exitCodeOut = 0xFFFFFFFF;
    if (outputOut) outputOut->clear();

    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 64 * 1024)) return false;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    HANDLE hNul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = (hNul != INVALID_HANDLE_VALUE) ? hNul : hRead;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<wchar_t> buf(commandLine.begin(), commandLine.end());
    buf.push_back(L'\0');

    BOOL created = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW, nullptr,
                                  workDir.empty() ? nullptr : workDir.c_str(),
                                  &si, &pi);

    // 父进程必须立刻关掉写端，否则子进程退出后 ReadFile 不会看到 EOF
    CloseHandle(hWrite);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);

    if (!created) {
        CloseHandle(hRead);
        return false;
    }
    CloseHandle(pi.hThread);

    HANDLE job = CreateKillOnCloseJob();
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        CloseHandle(job);
        job = nullptr;
    }

    std::string collected;
    DWORD start = GetTickCount();
    bool timedOut = false;

    for (;;) {
        // 抽干当前可读数据（用 PeekNamedPipe 避免阻塞在 ReadFile 上）
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(hRead, nullptr, 0, nullptr, &avail, nullptr)) break;
            if (avail == 0) break;

            char chunk[4096];
            DWORD want = (avail > sizeof(chunk)) ? (DWORD)sizeof(chunk) : avail;
            DWORD got = 0;
            if (!ReadFile(hRead, chunk, want, &got, nullptr) || got == 0) break;
            collected.append(chunk, got);
        }

        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;

        if ((GetTickCount() - start) > timeoutMs) { timedOut = true; break; }
        PumpSleep(20);
    }

    // 超时必须真的把它停掉，否则会有个后台进程继续跑
    if (timedOut) {
        bool killed = false;
        if (job) killed = (TerminateJobObject(job, 1) != FALSE);
        if (!killed) TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }

    // 收尾：把管道里残余数据读干净
    for (;;) {
        char chunk[4096];
        DWORD got = 0;
        if (!ReadFile(hRead, chunk, sizeof(chunk), &got, nullptr) || got == 0) break;
        collected.append(chunk, got);
    }
    CloseHandle(hRead);

    DWORD exitCode = 0xFFFFFFFF;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    // 关闭作业：进程已结束（或已被终止），KILL_ON_JOB_CLOSE 不会再误杀什么
    if (job) CloseHandle(job);

    if (exitCodeOut) *exitCodeOut = exitCode;
    if (outputOut) *outputOut = DecodeBestEffort(collected, CP_ACP);
    return !timedOut;
}

// ===========================================================================
// 脚本定位与运行
// ===========================================================================
bool FindScriptByRole(const std::wstring& homeDir, const std::wstring& marker,
                      std::wstring* pathOut) {
    if (!pathOut) return false;
    pathOut->clear();
    if (homeDir.empty() || marker.empty()) return false;

    const std::wstring needle = std::wstring(kRolePrefix) + marker;

    WIN32_FIND_DATAW fd;
    ZeroMemory(&fd, sizeof(fd));
    HANDLE h = FindFirstFileW((homeDir + L"\\*.cmd").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    std::wstring found;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        const std::wstring full = homeDir + L"\\" + fd.cFileName;
        std::string bytes;
        // keepHead = true：角色标记写在脚本第 2 行，必须从头部读。
        // （用默认的"读尾部"会在脚本超过 64 KB 时静默定位失败）
        if (!ReadFileBytes(full, &bytes, /*keepHead=*/true) || bytes.empty()) continue;
        const std::wstring text = DecodeBestEffort(bytes, CP_ACP);

        // 逐行精确比较：既锚定行首，也防止某脚本的注释里提到别的角色时误匹配
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t eol = text.find(L'\n', pos);
            std::wstring line = (eol == std::wstring::npos) ? text.substr(pos)
                                                            : text.substr(pos, eol - pos);
            if (!line.empty() && line.back() == L'\r') line.pop_back();

            size_t b = line.find_first_not_of(L" \t");
            if (b != std::wstring::npos) {
                std::wstring t = line.substr(b);
                TrimW(&t);
                if (t == needle) { found = full; break; }
            }
            if (eol == std::wstring::npos) break;
            pos = eol + 1;
        }
        if (!found.empty()) break;
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    if (found.empty()) return false;
    *pathOut = found;
    return true;
}

ScriptRunResult RunProjectScript(const std::wstring& homeDir,
                                 const std::wstring& marker,
                                 const std::wstring& args,
                                 const std::wstring& logName,
                                 DWORD timeoutMs) {
    ScriptRunResult r;

    if (!FindScriptByRole(homeDir, marker, &r.scriptPath)) {
        return r;
    }
    r.found = true;

    const std::wstring outDir = GetOutDirPath();
    EnsureDirectoryExists(outDir);
    r.logPath = outDir + L"\\" + logName;

    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hLog = CreateFileW(r.logPath.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE hNul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    HANDLE hOut = (hLog != INVALID_HANDLE_VALUE) ? hLog : hNul;

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = (hNul != INVALID_HANDLE_VALUE) ? hNul : hOut;
    si.hStdOutput = hOut;
    si.hStdError = hOut;

    // cmd /c 处理带引号的程序路径时，需要额外一层引号包裹整条命令
    std::wstring inner = L"\"" + r.scriptPath + L"\"";
    if (!args.empty()) inner += L" " + args;
    std::wstring cmd = L"cmd.exe /d /c \"" + inner + L"\"";

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    BOOL created = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW, nullptr, homeDir.c_str(),
                                  &si, &pi);

    if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);

    if (!created) {
        r.tail = L"无法启动脚本：" + r.scriptPath;
        return r;
    }
    CloseHandle(pi.hThread);

    // 放进"句柄关闭即杀树"的作业，超时时才能把 cmd 及其派生的
    // deviceinstaller64 / pnputil / powershell 一起停掉
    HANDLE job = CreateKillOnCloseJob();
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        CloseHandle(job);
        job = nullptr;
    }

    const WaitResult wr = WaitWithPump(pi.hProcess, job, timeoutMs);

    DWORD exitCode = 0xFFFFFFFF;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);

    r.timedOut = wr.timedOut;
    r.killed = wr.killed;
    r.elapsedMs = wr.elapsedMs;
    r.exitCode = exitCode;
    r.ok = (wr.finished && exitCode == 0);

    std::string bytes;
    if (ReadFileBytes(r.logPath, &bytes)) {
        r.tail = TailLines(DecodeBestEffort(bytes, CP_UTF8), 24);
    }
    if (r.tail.empty()) r.tail = L"(日志为空)";

    return r;
}

std::wstring GetHomeDir() {
    return ExeDirectory();
}

std::wstring GetOutDirPath() {
    return DefaultOutDir();
}

bool EnsureOutDir() {
    return EnsureDirectoryExists(GetOutDirPath());
}

// ---------------------------------------------------------------------------
// 关闭 / 恢复显示输出
// ---------------------------------------------------------------------------
namespace {

// 用 SendMessageTimeout 而不是 SendMessage：目标是 HWND_BROADCAST，
// 只要有一个窗口不响应广播，SendMessage 就会把本进程挂死。
// SMTO_ABORTIFHUNG 让不响应的窗口被跳过，1 秒超时足够。
bool BroadcastMonitorPower(LPARAM mode) {
    DWORD_PTR ignored = 0;
    const LRESULT r = SendMessageTimeoutW(
        (HWND)HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, mode,
        SMTO_NORMAL | SMTO_ABORTIFHUNG, 1000, &ignored);
    return r != 0;
}

}  // namespace

bool RequestDisplayOutputOff() {
    // lParam = 2 -> "the display is being shut off"
    return BroadcastMonitorPower((LPARAM)2);
}

bool RequestDisplayOutputOn() {
    // lParam = -1 -> "the display is being turned on"
    return BroadcastMonitorPower((LPARAM)-1);
}

bool RedirectStdioToLog(const std::wstring& logName, std::wstring* pathOut) {
    if (logName.empty()) return false;

    const std::wstring dir = GetOutDirPath();
    EnsureDirectoryExists(dir);
    const std::wstring path = dir + L"\\" + logName;
    if (pathOut) *pathOut = path;

    // 防止长期驻留把日志撑爆：超过 2 MB 就从头开始写
    const wchar_t* mode = L"a";
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        ULARGE_INTEGER sz;
        sz.LowPart = fad.nFileSizeLow;
        sz.HighPart = fad.nFileSizeHigh;
        if (sz.QuadPart > 2ULL * 1024 * 1024) mode = L"w";
    }

    // LogLine 用的是 fputs(stdout) + fflush，所以 freopen 足以覆盖程序自身输出。
    // 子进程（脚本）的输出去哪不受影响 —— 它们各自有自己的重定向。
    bool ok = true;
    if (!_wfreopen(path.c_str(), mode, stdout)) ok = false;
    if (!_wfreopen(path.c_str(), mode, stderr)) ok = false;

    if (ok) {
        LogLine("");
        LogLine("===== ResProbe 托盘会话开始 " + NowIso8601Utc() + " =====");
    }
    return ok;
}

// ===========================================================================
// 开机自启动
// ===========================================================================
AutostartState QueryAutostart() {
    AutostartState st;

    const wchar_t* names[2] = { kTaskNameInit, kTaskNameGuard };
    for (int i = 0; i < 2; ++i) {
        std::wstring cmd = L"schtasks /query /tn \"" + std::wstring(names[i]) +
                           L"\" /fo list";
        DWORD rc = 1;
        std::wstring out;
        bool ran = RunAndCapture(cmd, GetHomeDir(), 10000, &rc, &out);

        const bool exists = (ran && rc == 0);
        std::wstring state = out;
        TrimW(&state);

        if (i == 0) {
            st.initRegistered = exists;
            st.initState = exists ? state : L"(未注册)";
        } else {
            st.guardRegistered = exists;
            st.guardState = exists ? state : L"(未注册)";
        }
    }

    st.resprobeProcessCount = CountOtherProcesses(L"resprobe.exe");
    st.guardRunning = (st.resprobeProcessCount > 0);
    return st;
}

bool SetAutostart(bool enable, bool guardMode, std::wstring* message) {
    const std::wstring args = !enable ? L"/remove"
                                      : (guardMode ? L"/mode guard" : L"/mode once");

    ScriptRunResult sr = RunProjectScript(GetHomeDir(), L"autostart", args,
                                          L"tray-autostart.log", 120000);
    if (!sr.found) {
        if (message) {
            *message = L"未找到自启动脚本。\n\n"
                       L"该脚本需要在独立一行声明：\n"
                       L"    @rem RESPROBE-ROLE:autostart";
        }
        return false;
    }

    if (message) {
        if (sr.ok) {
            *message = enable
                           ? (guardMode ? L"已注册：虚拟屏初始化 + 常驻守卫"
                                        : L"已注册：仅虚拟屏初始化")
                           : L"已注销全部 ResProbe 计划任务";
        } else {
            wchar_t codeBuf[32];
            _snwprintf_s(codeBuf, _countof(codeBuf), _TRUNCATE, L"0x%08lX",
                         (unsigned long)sr.exitCode);
            *message = L"脚本退出码 " + std::to_wstring(sr.exitCode) + L"（" +
                       codeBuf + L"）";
            if (sr.timedOut) {
                *message += sr.killed
                                ? L"\n注意：等待超时，脚本（含子进程）已被强制终止。"
                                : L"\n注意：等待超时，但未能终止脚本，它可能仍在后台运行。";
            }
            if (!sr.tail.empty()) *message += L"\n\n--- 日志尾部 ---\n" + sr.tail;
        }
    }
    return sr.ok;
}

// ===========================================================================
// 分辨率
// ===========================================================================
std::vector<ModeEntry> GetModeListOfDevice(const std::wstring& device) {
    std::vector<ModeEntry> list;
    if (device.empty()) return list;

    std::vector<std::pair<UINT32, UINT32>> seen;
    for (int i = 0; i < 4000; ++i) {
        DEVMODEW dm;
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsExW(device.c_str(), (DWORD)i, &dm, 0)) break;

        if (dm.dmPelsWidth < 640 || dm.dmPelsHeight < 480) continue;
        if (dm.dmBitsPerPel != 0 && dm.dmBitsPerPel < 16) continue;

        std::pair<UINT32, UINT32> key(dm.dmPelsWidth, dm.dmPelsHeight);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
    }

    std::sort(seen.begin(), seen.end(),
              [](const std::pair<UINT32, UINT32>& a,
                 const std::pair<UINT32, UINT32>& b) {
                  UINT64 aa = (UINT64)a.first * a.second;
                  UINT64 bb = (UINT64)b.first * b.second;
                  if (aa != bb) return aa > bb;
                  return a.first > b.first;
              });

    for (const auto& k : seen) {
        ModeEntry e;
        e.width = k.first;
        e.height = k.second;
        e.text = FormatModeText(k.first, k.second);
        list.push_back(e);
    }
    return list;
}

std::vector<ModeEntry> GetPrimaryModeList(std::wstring* currentOut,
                                         std::wstring* deviceOut) {
    const std::wstring dev = FindPrimaryGdiDevice();
    if (deviceOut) *deviceOut = dev;
    if (currentOut) *currentOut = CurrentModeOfDevice(dev);
    return GetModeListOfDevice(dev);
}

// OutputNature / OutputNatureName / ClassifyOutputNature 已下沉到 display.cpp，
// 与 verify.cpp 的拓扑判定共用同一套判据（判据集中才不会有"一处健康、一处断链"的漂移）。

ResolutionResult ApplyResolution(UINT32 width, UINT32 height) {
    ResolutionResult r;

    std::wstring before;
    r.device = FindPrimaryGdiDevice();
    before = CurrentModeOfDevice(r.device);
    r.before = before;

    std::vector<ApplyAttempt> attempts;
    r.resultCode = ApplyDisplayMode(r.device, width, height, 0, true, &attempts);
    r.applied = (r.resultCode == DISP_CHANGE_SUCCESSFUL);
    r.resultName = DispHangeResultName(r.resultCode);

    for (const ApplyAttempt& a : attempts) {
        r.attempts.push_back(W2U8(a.description) + " -> " +
                             W2U8(DispHangeResultName(a.resultCode)));
    }

    if (r.applied) {
        // 给驱动一点时间稳定，再复核实际生效的模式
        PumpSleep(1200);
        r.after = CurrentModeOfDevice(r.device);

        const std::wstring want = FormatModeText(width, height);
        if (r.after == want) {
            r.message = L"已切换到 " + want + L"（" + r.device + L"）";
        } else {
            r.message = L"驱动接受了请求，但实际生效为 " + r.after +
                        L"，与目标 " + want + L" 不一致";
        }
        return r;
    }

    // ---- 失败：按目标输出的**真实性质**给出原因 -----------------------------
    //
    // 这里曾经只用"EDID 不可读"就断言"显示链路已断开"。那是错的：
    // 虚拟/间接显示输出的 EDID 本来就为空（与链路毫无关系），
    // 于是"虚拟屏不支持 1680x1050"会被报成"链路断了，硬件限制，无法绕过"，
    // 把排查引向完全错误的方向 —— 恰好抵消了本项目最核心的精确归因能力。
    {
        std::wstring diag;
        int totalPaths = 0;
        std::vector<DisplayTarget> targets =
            EnumerateDisplays(width, height, &diag, &totalPaths, false);
        r.nature = ClassifyOutputNature(targets, r.device);
    }

    for (const ModeEntry& m : GetModeListOfDevice(r.device)) {
        r.supportedModes.push_back(m.text);
    }

    const std::wstring want = FormatModeText(width, height);
    r.message = L"切换到 " + want + L" 失败：" + r.resultName;
    r.message += L"\n\n目标输出：" + r.device + L"（" + OutputNatureName(r.nature) + L"）";

    switch (r.nature) {
        case OutputNature::DeadPhysical:
            r.linkDead = true;
            r.message += L"\n\n该物理输出的 EDID / 监视器设备节点当前不可见，"
                         L"说明显示链路已断开。驱动拒绝在没有接收端的情况下提交非回退模式 ——"
                         L"这是硬件层面的限制，软件无法绕过。";
            break;
        case OutputNature::Virtual:
            r.message += L"\n\n这是虚拟/间接显示输出，它的 EDID 本来就为空，与链路无关。"
                         L"失败原因是该虚拟显示器不支持这个模式。";
            break;
        case OutputNature::Mixed:
            r.message += L"\n\n当前是克隆（镜像）拓扑：同一个 GDI 名下同时挂着虚拟与物理目标，"
                         L"驱动只会接受两者的交集。请先解开镜像，或改选两边都支持的档位。";
            break;
        case OutputNature::HealthyPhysical:
            r.message += L"\n\n该物理输出的链路正常（EDID 与设备节点都在），"
                         L"失败属于驱动层面拒绝该模式。";
            break;
        default:
            r.message += L"\n\n未能判定该输出的性质（可能正处于重配置过程中）。";
            break;
    }

    if (!r.supportedModes.empty()) {
        r.message += L"\n\n该输出当前支持的档位（" +
                     std::to_wstring(r.supportedModes.size()) + L" 项）：";
        const size_t kShow = 12;
        for (size_t i = 0; i < r.supportedModes.size() && i < kShow; ++i) {
            r.message += (i == 0 ? L" " : L", ") + r.supportedModes[i];
        }
        if (r.supportedModes.size() > kShow) {
            r.message += L" ...（还有 " +
                         std::to_wstring(r.supportedModes.size() - kShow) + L" 项）";
        }
    }

    if (!r.attempts.empty()) {
        r.message += L"\n\n尝试明细：";
        for (const std::string& s : r.attempts) {
            r.message += L"\n  - " + U82W(s);
        }
    }
    return r;
}

// ===========================================================================
// 状态摘要
// ===========================================================================
TrayStatus QueryTrayStatus(UINT32 refW, UINT32 refH) {
    TrayStatus st;

    // 只做**一轮**探测，下面所有判断都复用它。
    // 之前这里是 3~4 轮全量枚举：EvaluateTopology 一轮、CountAvailableVirtualOutputs
    // 内部再一轮（还会再调一次 EvaluateTopology）、CaptureSnapshot 一轮。
    // 每轮都是 QueryDisplayConfig + SetupAPI 读 EDID，在托盘右键路径上能明显感觉到延迟。
    std::wstring diag;
    int totalPaths = 0;
    std::vector<DisplayTarget> allPaths =
        EnumerateDisplays(refW, refH, &diag, &totalPaths, /*includePlaceholderPaths=*/true);

    TopologyState ts = EvaluateTopology(refW, refH);

    st.primaryMode = ts.primaryMode;
    st.primaryDevice = ts.primaryGdiDevice;
    st.totalActive = ts.totalActive;
    st.activeVirtual = ts.activeVirtualCount;
    st.healthyPhysical = ts.healthyPhysicalCount;
    st.deadPhysical = ts.deadPhysicalCount;
    st.virtualAvailable = CountAvailableVirtualOutputsFrom(allPaths, ts);
    st.elevated = IsProcessElevated();
    st.guardOwnedByOther = IsGuardOwnedByOther();

    SessionInfo si = QuerySessionInfo();
    st.remoteSession = si.remoteSession;

    // 判定：以"当前状态"喂给判定器（无时间线），用于在菜单里给出结论摘要。
    // 复用已经拿到的路径列表，不再让 CaptureSnapshot 重新枚举一遍。
    std::vector<DisplayTarget> meaningful;
    meaningful.reserve(allPaths.size());
    for (const DisplayTarget& t : allPaths) {
        if (IsMeaningfulDisplayTarget(t)) meaningful.push_back(t);
    }
    DisplaySnapshot snap = CaptureSnapshotFrom(meaningful, totalPaths, "tray-status");

    VerdictInput vi;
    vi.baseline = &snap;
    vi.latest = &snap;
    vi.session = &si;
    vi.refWidth = refW;
    vi.refHeight = refH;
    Verdict v = EvaluateVerdict(vi);

    st.verdictCode = U82W(v.code);
    st.verdictSummary = U82W(v.summary);
    st.verdictConfidence = U82W(v.confidence);
    st.verdictBlame = U82W(v.blame);
    for (const std::string& s : v.reasons) st.verdictReasons.push_back(U82W(s));
    for (const std::string& s : v.recommendations) {
        st.verdictRecommendations.push_back(U82W(s));
    }

    st.degraded = (v.code == "LINK_LEVEL_FALLBACK" ||
                   v.code == "LINK_LEVEL_FALLBACK_SUSPECTED" ||
                   v.code == "RESOLUTION_FALLBACK_NO_EDID_EVIDENCE" ||
                   v.code == "EDID_UNSTABLE_LINK");

    return st;
}

std::wstring BuildStatusReport(const TrayStatus& st, UINT32 refW, UINT32 refH) {
    std::wstring t;
    t += L"主输出        : " + (st.primaryMode.empty() ? L"(未知)" : st.primaryMode);
    t += L"  " + st.primaryDevice + L"\r\n";
    t += L"活动输出      : " + std::to_wstring(st.totalActive) +
         L" 个（虚拟 " + std::to_wstring(st.activeVirtual) +
         L"，健康物理 " + std::to_wstring(st.healthyPhysical) +
         L"，断链物理 " + std::to_wstring(st.deadPhysical) + L"）\r\n";
    t += L"可用虚拟显示  : " + std::to_wstring(st.virtualAvailable) + L" 个";
    t += (st.virtualAvailable > 0) ? L"   兜底能力就位\r\n"
                                   : L"   无兜底，物理屏断电会回退到 1024x768\r\n";
    t += L"参考分辨率    : " + FormatModeText(refW, refH) + L"\r\n";
    t += L"管理员权限    : " + std::wstring(st.elevated ? L"是" : L"否") + L"\r\n";
    t += L"远程会话      : " + std::wstring(st.remoteSession ? L"是" : L"否") + L"\r\n";
    t += L"守卫          : ";
    if (st.guardOwnedByOther) {
        t += L"由另一个 ResProbe 实例承担（本实例只做界面）\r\n";
    } else {
        t += L"由本实例承担\r\n";
    }
    t += L"\r\n";

    t += L"[判定] code=" + st.verdictCode + L"\r\n";
    t += L"[判定] 置信度=" + st.verdictConfidence + L"  责任方=" + st.verdictBlame + L"\r\n";
    t += L"[判定] 结论: " + st.verdictSummary + L"\r\n";

    if (!st.verdictReasons.empty()) {
        t += L"[判定] 依据:\r\n";
        for (const std::wstring& s : st.verdictReasons) {
            t += L"        - " + s + L"\r\n";
        }
    }
    if (!st.verdictRecommendations.empty()) {
        t += L"[判定] 建议:\r\n";
        for (const std::wstring& s : st.verdictRecommendations) {
            t += L"        - " + s + L"\r\n";
        }
    }
    return t;
}

std::wstring BuildPathsReport(UINT32 refW, UINT32 refH) {
    int totalPaths = 0;
    std::wstring diag;
    std::vector<DisplayTarget> all =
        EnumerateDisplays(refW, refH, &diag, &totalPaths, true);

    int active = 0;
    int meaningful = 0;
    int virtualAvailable = 0;
    for (const DisplayTarget& t : all) {
        if (t.active) ++active;
        if (IsMeaningfulDisplayTarget(t)) ++meaningful;
        if (IsVirtualDisplayTarget(t) && (t.targetAvailable || t.active)) {
            ++virtualAvailable;
        }
    }

    std::wstring t;
    t += L"系统报告输出路径 : " + std::to_wstring(totalPaths) + L" 条\r\n";
    t += L"其中活动         : " + std::to_wstring(active) + L" 条\r\n";
    t += L"带监视器信息     : " + std::to_wstring(meaningful) + L" 条\r\n";
    t += L"可用虚拟显示输出 : " + std::to_wstring(virtualAvailable) + L" 个\r\n";

    // 按适配器聚合，便于识别虚拟/间接显示适配器
    std::map<std::string, int> byAdapter;
    for (const DisplayTarget& d : all) byAdapter[FormatLuid(d.adapterLuid)] += 1;
    t += L"涉及适配器       : " + std::to_wstring(byAdapter.size()) + L" 个\r\n";
    for (const auto& kv : byAdapter) {
        t += L"        " + U82W(kv.first) + L" : " + std::to_wstring(kv.second) +
             L" 条路径\r\n";
    }

    t += L"\r\n--- 有监视器信息或活动的输出 ---\r\n";
    for (const DisplayTarget& d : all) {
        const bool show = d.active || !d.monitorDevicePath.empty() ||
                          d.edid.present || d.monitorDeviceNodePresent;
        if (!show) continue;

        t += (d.active ? L"[ACTIVE] " : L"[idle  ] ");
        t += U82W(FormatLuid(d.adapterLuid));
        t += L"  " + (d.gdiDeviceName.empty() ? std::wstring(L"(无 GDI 名)")
                                            : d.gdiDeviceName);
        t += L"  " + FormatModeText(d.width, d.height);
        t += L"  " + (d.monitorFriendlyName.empty() ? std::wstring(L"(无监视器名)")
                                                   : d.monitorFriendlyName);
        t += L"  EDID=" + std::wstring(d.edid.present ? L"有" : L"无");
        t += L"  可用=" + std::wstring(d.targetAvailable ? L"是" : L"否");
        t += L"\r\n";
    }
    return t;
}

// ===========================================================================
// 单实例保护
// ===========================================================================
namespace {

// 互斥量句柄必须在整个进程生命周期内保持有效：一旦 CloseHandle 就相当于释放了锁。
// 用文件级静态量持有，进程退出时由系统回收。
struct InstanceLocks {
    HANDLE guard = nullptr;
    HANDLE tray = nullptr;
    bool guardOwnedByOther = false;
};
InstanceLocks g_locks;

// 创建（或打开）命名互斥量并尝试取得所有权。
//
// 先试 Global\ 以便跨会话互斥：显示拓扑是**机器级**资源，RDP 多会话下同一个用户
// 可能有两个会话同时在跑。Global\ 需要 SeCreateGlobalPrivilege，提权进程一般都有；
// 万一没有就退回 Local\（同会话互斥，覆盖绝大多数场景）。
// 返回 nullptr 表示连锁都建不出来 —— 调用方应当保守处理（当作"不可用"）。
HANDLE AcquireNamedMutex(const wchar_t* name, bool* ownedByOther) {
    const wchar_t* prefixes[2] = { L"Global\\", L"Local\\" };

    for (const wchar_t* p : prefixes) {
        const std::wstring full = std::wstring(p) + name;

        SetLastError(ERROR_SUCCESS);
        HANDLE h = CreateMutexW(nullptr, TRUE, full.c_str());
        if (!h) continue;

        const DWORD err = GetLastError();
        if (ownedByOther) *ownedByOther = (err == ERROR_ALREADY_EXISTS);
        return h;   // 即使"别人已持有"，句柄也要留着，稍后可查询/释放
    }

    if (ownedByOther) *ownedByOther = true;
    return nullptr;
}

}  // namespace

GuardOwner AcquireGuardLock() {
    if (g_locks.guard) {
        return g_locks.guardOwnedByOther ? GuardOwner::OtherProcess
                                         : GuardOwner::ThisProcess;
    }

    bool other = false;
    g_locks.guard = AcquireNamedMutex(L"ResProbe.Guard", &other);
    g_locks.guardOwnedByOther = other;

    if (!g_locks.guard) return GuardOwner::Unavailable;
    return other ? GuardOwner::OtherProcess : GuardOwner::ThisProcess;
}

bool AcquireTrayLock() {
    if (g_locks.tray) return true;   // 本进程已持有

    bool other = false;
    g_locks.tray = AcquireNamedMutex(L"ResProbe.Tray", &other);
    if (!g_locks.tray) return false;
    return !other;
}

bool IsGuardOwnedByOther() {
    if (g_locks.guard) return g_locks.guardOwnedByOther;

    // 本进程尚未取过锁（例如 selftest、ensure 这类只读路径）：只查询存在性，
    // 绝不索取所有权，否则会把真正的守卫进程"顶掉"。
    const wchar_t* names[2] = { L"Global\\ResProbe.Guard", L"Local\\ResProbe.Guard" };
    for (const wchar_t* n : names) {
        HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, n);
        if (h) {
            CloseHandle(h);
            return true;
        }
    }
    return false;
}

// ===========================================================================
// 杂项
// ===========================================================================
bool OpenFolderInExplorer(const std::wstring& dir) {
    if (dir.empty()) return false;
    EnsureDirectoryExists(dir);
    HINSTANCE rc = ShellExecuteW(nullptr, L"open", L"explorer.exe",
                                 (L"\"" + dir + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    // ShellExecuteW 的返回值语义特殊：> 32 才是成功
    return (reinterpret_cast<INT_PTR>(rc) > 32);
}

bool OpenFileInDefaultApp(const std::wstring& path) {
    if (path.empty()) return false;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    HINSTANCE rc = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr,
                                 SW_SHOWNORMAL);
    return (reinterpret_cast<INT_PTR>(rc) > 32);
}

bool ReadLogTail(const std::wstring& path, size_t maxLines, std::wstring* out) {
    if (out) out->clear();
    if (path.empty() || !out) return false;

    // 先确认文件末尾是完整行。日志很可能正被另一个进程写入（守卫、托盘动作），
    // 此时最后一行是半行；直接显示会让人误以为日志内容本身被截断了。
    bool endsWithNewline = false;
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size;
            ZeroMemory(&size, sizeof(size));
            if (GetFileSizeEx(h, &size) && size.QuadPart > 0) {
                LARGE_INTEGER pos;
                ZeroMemory(&pos, sizeof(pos));
                pos.QuadPart = size.QuadPart - 1;
                if (SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) {
                    char last = 0;
                    DWORD got = 0;
                    if (ReadFile(h, &last, 1, &got, nullptr) && got == 1) {
                        endsWithNewline = (last == '\n' || last == '\r');
                    }
                }
            }
            CloseHandle(h);
        }
    }

    std::string bytes;
    if (!ReadFileBytes(path, &bytes) || bytes.empty()) return false;

    // 注意首选 CP_UTF8：混合内容按 ACP 解一定"成功"，会把 UTF-8 那半毁掉
    std::wstring text = DecodeBestEffort(bytes, CP_UTF8);

    if (!endsWithNewline) {
        const size_t nl = text.find_last_of(L'\n');
        if (nl != std::wstring::npos) {
            text.resize(nl);          // 丢掉最后一个不完整的行
        } else {
            text.clear();             // 整个文件就一行且还没写完
        }
    }

    *out = TailLines(text, maxLines);
    if (out->empty()) {
        *out = endsWithNewline ? std::wstring()
                               : L"(日志正在写入中，暂无可显示的完整行)";
    }
    return true;
}

std::wstring FindNewestLogFile(const std::wstring& dir) {
    std::wstring newest;
    UINT64 newestStamp = 0;

    WIN32_FIND_DATAW fd;
    ZeroMemory(&fd, sizeof(fd));
    HANDLE h = FindFirstFileW((dir + L"\\*.log").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return newest;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULARGE_INTEGER t;
        t.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        if (t.QuadPart > newestStamp) {
            newestStamp = t.QuadPart;
            newest = dir + L"\\" + fd.cFileName;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    return newest;
}

void HideConsoleWindow() {
    // 方案 A：保持控制台子系统，只把控制台窗口藏起来。
    // 之所以不改成 -mwindows：.cmd 脚本依赖 stdout 重定向与退出码契约，
    // 改子系统必须同时保证这两点不退化（见 DEVELOPMENT.md 第 3 节）。
    // 隐藏而非 FreeConsole：保留控制台可继续输出日志，便于诊断。
    HWND con = GetConsoleWindow();
    if (con) ShowWindow(con, SW_HIDE);
}

// ===========================================================================
// 动作层自检
// ===========================================================================
int RunActionSelfTest() {
    int failures = 0;

    auto say = [](const std::wstring& s) { LogLine(W2U8(s)); };
    auto check = [&failures](bool ok, const std::wstring& what) {
        LogLine(std::string(ok ? "  [ OK ] " : "  [FAIL] ") + W2U8(what));
        if (!ok) ++failures;
    };
    auto note = [](const std::wstring& s) { LogLine("         " + W2U8(s)); };

    const std::wstring home = GetHomeDir();

    LogLine("==================================================================");
    LogLine("ResProbe 动作层自检（托盘菜单所依赖的全部函数）");
    LogLine("==================================================================");
    say(L"程序目录 : " + home);
    say(L"输出目录 : " + GetOutDirPath());
    LogLine("");

    // ---- 1) 按角色定位脚本 ------------------------------------------------
    LogLine("[1] 按 RESPROBE-ROLE 标记定位脚本");
    {
        struct RoleCase { const wchar_t* role; const wchar_t* desc; };
        const RoleCase roles[] = {
            { L"once",      L"一次性启动脚本" },
            { L"guard",     L"常驻守卫脚本"   },
            { L"install",   L"安装脚本"       },
            { L"uninstall", L"卸载脚本"       },
            { L"autostart", L"自启动脚本"     },
        };
        for (const RoleCase& rc : roles) {
            std::wstring path;
            const bool found = FindScriptByRole(home, rc.role, &path);
            std::wstring msg = std::wstring(rc.desc) + L"（role=" + rc.role + L"）";
            if (found) {
                msg += L" -> " + path;
            } else {
                msg += L" -> 未找到";
            }
            check(found, msg);
        }
    }
    LogLine("");

    // ---- 2) 脚本执行 + 日志捕获（用 autostart 的 /status，只读）-----------
    LogLine("[2] 脚本执行与日志捕获（autostart /status，只读）");
    {
        ScriptRunResult sr = RunProjectScript(home, L"autostart", L"/status",
                                              L"selftest-autostart.log", 60000);
        check(sr.found, L"定位到脚本" + std::wstring(sr.found ? L" -> " + sr.scriptPath
                                                              : L"（失败）"));
        check(!sr.timedOut, L"未超时（耗时 " + std::to_wstring(sr.elapsedMs) + L" ms）");
        check(sr.exitCode == 0, L"退出码为 0（实际 " + std::to_wstring(sr.exitCode) + L"）");
        check(!sr.tail.empty() && sr.tail != L"(日志为空)",
              L"捕获到日志内容（" + std::to_wstring(sr.tail.size()) + L" 字符）");
        if (!sr.tail.empty()) {
            note(L"日志尾部首行: " + sr.tail.substr(0, sr.tail.find(L'\r')));
        }
    }
    LogLine("");

    // ---- 3) 开机自启状态 --------------------------------------------------
    LogLine("[3] 开机自启动状态查询");
    {
        AutostartState as = QueryAutostart();
        std::wstring msg = L"虚拟屏初始化任务: ";
        msg += as.initRegistered ? L"已注册" : L"未注册";
        msg += L"，常驻守卫任务: ";
        msg += as.guardRegistered ? L"已注册" : L"未注册";
        msg += L"，其它 resprobe 进程: " + std::to_wstring(as.resprobeProcessCount) + L" 个";
        // 未注册不算失败：这是环境选择而非缺陷
        LogLine("  [INFO] " + W2U8(msg));
    }
    LogLine("");

    // ---- 4) 主输出模式列表 ------------------------------------------------
    LogLine("[4] 主输出模式列表枚举");
    {
        std::wstring current, device;
        std::vector<ModeEntry> modes = GetPrimaryModeList(&current, &device);
        check(!device.empty(), L"取到主输出 GDI 设备名 -> " + device);
        check(!current.empty(), L"取到当前分辨率 -> " + current);
        check(modes.size() >= 3, L"模式列表非空（" + std::to_wstring(modes.size()) +
                                     L" 项，已去重/排序）");
        if (!modes.empty()) {
            std::wstring head = L"前三项: ";
            for (size_t i = 0; i < modes.size() && i < 3; ++i) {
                if (i) head += L", ";
                head += modes[i].text;
            }
            note(head);
        }
    }
    LogLine("");

    // ---- 5) 分辨率应用（等值写入，不产生实际变化）-------------------------
    LogLine("[5] 分辨率应用（目标 = 当前分辨率，等值写入）");
    {
        std::wstring current, device;
        GetPrimaryModeList(&current, &device);

        // 就地解析 "WxH"：main.cpp 里的 ParseReference 是文件内函数，取不到
        UINT32 w = 0, h = 0;
        {
            unsigned a = 0, b = 0;
            if (swscanf_s(current.c_str(), L"%ux%u", &a, &b) == 2 && a > 0 && b > 0) {
                w = a;
                h = b;
            }
        }
        if (w > 0 && h > 0) {
            ResolutionResult rr = ApplyResolution(w, h);
            check(rr.applied, L"应用 " + FormatModeText(w, h) + L" 成功（" +
                                  rr.resultName + L"）");
            check(rr.after == rr.before,
                  L"应用前后一致（" + rr.before + L" -> " + rr.after + L"）");
            if (!rr.attempts.empty()) {
                note(L"使用到的变体: " + U82W(rr.attempts.front()));
            }
            if (!rr.message.empty()) note(rr.message.substr(0, 120));
        } else {
            check(false, L"无法从当前模式文本 " + current + L" 解析出宽高");
        }
    }
    LogLine("");

    // ---- 6) 状态摘要 ------------------------------------------------------
    LogLine("[6] 状态摘要（托盘菜单首行 + 状态对话框的数据源）");
    {
        const DWORD t0 = GetTickCount();
        TrayStatus st = QueryTrayStatus(1920, 1080);
        const DWORD dt = GetTickCount() - t0;

        check(!st.primaryMode.empty(), L"主输出分辨率 -> " + st.primaryMode);
        check(st.totalActive > 0, L"活动输出数 -> " + std::to_wstring(st.totalActive));
        check(!st.verdictCode.empty(), L"判定码 -> " + st.verdictCode);
        note(L"可用虚拟显示输出: " + std::to_wstring(st.virtualAvailable) + L" 个");
        note(L"管理员权限: " + std::wstring(st.elevated ? L"是" : L"否"));
        note(L"守卫归属: " + std::wstring(st.guardOwnedByOther
                                             ? L"其它实例" : L"非其它实例"));
        // M6：这里曾经是 3~4 轮全量枚举（EvaluateTopology 两轮、CaptureSnapshot 一轮），
        // 每轮都含 QueryDisplayConfig + SetupAPI 读 EDID。现在收敛为
        // 1 轮 EnumerateDisplays(true) + 1 次 EvaluateTopology。
        note(L"单次状态查询耗时 " + std::to_wstring(dt) + L" ms");
        check(dt < 3000, L"状态查询未出现异常耗时（" + std::to_wstring(dt) + L" ms）");

        const std::wstring report = BuildStatusReport(st, 1920, 1080);
        check(!report.empty(), L"状态报告文本生成（" + std::to_wstring(report.size()) +
                                   L" 字符）");

        const std::wstring paths = BuildPathsReport(1920, 1080);
        check(!paths.empty(), L"路径报告文本生成（" + std::to_wstring(paths.size()) +
                                  L" 字符）");
    }
    LogLine("");

    // ---- 7) 混合编码解码 --------------------------------------------------
    // 捕获到的脚本日志天然是 UTF-8(resprobe) 与 GBK(cmd echo 的中文文件名、
    // schtasks 等系统工具) 拼起来的。这里用固定字节序列做确定性验证。
    LogLine("[7] 混合编码解码（日志里 UTF-8 与 GBK 并存）");
    {
        // "活动输出" 的 UTF-8 字节
        const char utf8Part[] = "\xE6\xB4\xBB\xE5\x8A\xA8\xE8\xBE\x93\xE5\x87\xBA";
        // "中文" 的 GBK 字节
        const char gbkPart[] = "\xD6\xD0\xCE\xC4";

        std::string mixed = "A[";
        mixed += utf8Part;
        mixed += "][";
        mixed += gbkPart;
        mixed += "]B";

        const std::wstring dec = DecodeMixed(mixed);
        check(dec == L"A[活动输出][中文]B", L"混合解码 -> " + dec);

        // 纯 GBK 也必须正确（系统工具输出走这条）
        std::string pureGbk(gbkPart);
        pureGbk += gbkPart;
        const std::wstring decGbk = DecodeBestEffort(pureGbk, CP_ACP);
        check(decGbk == L"中文中文", L"纯 GBK 解码 -> " + decGbk);

        // 纯 UTF-8 走安全路径，不能被启发式影响
        std::string pureUtf8(utf8Part);
        pureUtf8 += utf8Part;
        const std::wstring decUtf8 = DecodeBestEffort(pureUtf8, CP_UTF8);
        check(decUtf8 == L"活动输出活动输出", L"纯 UTF-8 解码 -> " + decUtf8);

        // 真实文件端到端验证：托盘"安装 / 修复"动作留下的日志本身就是混合编码
        // （UTF-8 来自 resprobe.exe，GBK 来自 cmd echo 的中文路径与文件名）。
        // 历史上正是这份日志在弹窗里显示成"娲诲姩杈撳嚭"。
        const std::wstring realLog = GetOutDirPath() + L"\\tray-install.log";
        if (GetFileAttributesW(realLog.c_str()) == INVALID_FILE_ATTRIBUTES) {
            LogLine("  [SKIP] 未找到 OUT\\tray-install.log"
                    "（先在托盘菜单里执行一次\"安装 / 修复\"即可生成）");
        } else {
            std::string bytes;
            if (ReadFileBytes(realLog, &bytes) && !bytes.empty()) {
                const bool pureUtf8File = !TryDecodeWhole(bytes, CP_UTF8, true).empty();
                const std::wstring txt = DecodeBestEffort(bytes, CP_UTF8);
                check(txt.find(L"活动输出") != std::wstring::npos,
                      L"真实托盘安装日志解码后含\"活动输出\"（" +
                          std::to_wstring(bytes.size()) + L" 字节，" +
                          std::wstring(pureUtf8File ? L"纯 UTF-8" : L"确认混合编码") + L"）");
            }
        }

        // 托盘"查看最近的日志"菜单项走的公开接口
        const std::wstring newestLog = FindNewestLogFile(GetOutDirPath());
        if (newestLog.empty()) {
            LogLine("  [SKIP] 输出目录里还没有 .log 文件");
        } else {
            std::wstring tail;
            const bool ok = ReadLogTail(newestLog, 40, &tail);
            check(ok && !tail.empty(),
                  L"ReadLogTail 读取最近日志成功（" + std::to_wstring(tail.size()) +
                      L" 字符）");
        }
    }
    LogLine("");

    // ---- 8) 本轮审查修复项的回归验证 --------------------------------------
    LogLine("[8] 审查修复项回归");

    // H2：失败归因必须先判定目标输出性质，不能再"见 EDID 就断言链路断开"
    {
        std::wstring dev;
        std::wstring current;
        GetPrimaryModeList(&current, &dev);

        std::wstring diag;
        int totalPaths = 0;
        std::vector<DisplayTarget> targets =
            EnumerateDisplays(1920, 1080, &diag, &totalPaths, false);
        const OutputNature nature = ClassifyOutputNature(targets, dev);

        check(nature != OutputNature::Unknown,
              L"主输出 " + dev + L" 的性质可判定 -> " + OutputNatureName(nature));
        note(L"这决定了应用失败时的归因方向（断链物理 / 虚拟屏 / 克隆混合）");

        // 端到端：故意用一个不可能的档位触发真实失败，检查归因**方向**。
        // 16384x16384 不在任何输出的模式表里（已实测返回 DISP_CHANGE_BADMODE），
        // 不会改变显示状态，所以可以安全放在自检里。
        const ResolutionResult rr = ApplyResolution(16384, 16384);
        check(!rr.applied, L"不可能的档位被拒绝（" + rr.resultName + L"）");
        check(!rr.linkDead || rr.nature == OutputNature::DeadPhysical,
              L"未把非断链情形误报为链路断开（性质=" + OutputNatureName(rr.nature) + L"）");
        check(rr.message.find(L"目标输出") != std::wstring::npos,
              L"失败消息给出了目标输出与性质");
        note(L"归因性质=" + OutputNatureName(rr.nature) + L"，该输出支持 " +
             std::to_wstring(rr.supportedModes.size()) + L" 个档位");
    }

    // H3：跨进程守卫互斥
    {
        const GuardOwner owner = AcquireGuardLock();
        const std::wstring desc =
            (owner == GuardOwner::ThisProcess)   ? L"由本进程获得"
            : (owner == GuardOwner::OtherProcess) ? L"已被其它实例持有"
                                                  : L"不可用（无法创建互斥量）";
        check(owner != GuardOwner::Unavailable, L"守卫互斥量可用 -> " + desc);

        // 重复调用必须幂等，且与只读查询一致
        const GuardOwner again = AcquireGuardLock();
        check(again == owner, L"重复获取守卫锁结果一致");

        const bool other = IsGuardOwnedByOther();
        check(other == (owner == GuardOwner::OtherProcess),
              L"IsGuardOwnedByOther 与获取结果一致 -> " +
                  std::wstring(other ? L"true" : L"false"));
    }

    // L6：日志末尾是半行时必须丢掉，否则用户会以为日志内容被截断
    {
        const std::wstring tmp = GetOutDirPath() + L"\\selftest-partial.log";
        bool wrote = false;
        {
            HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                const char body[] = "line1\nline2\nline3-partial-no-newline";
                DWORD wr = 0;
                wrote = (WriteFile(h, body, (DWORD)(sizeof(body) - 1), &wr, nullptr) != FALSE);
                CloseHandle(h);
            }
        }
        if (!wrote) {
            LogLine("  [SKIP] 无法创建临时文件用于 L6 验证");
        } else {
            std::wstring tail;
            const bool ok = ReadLogTail(tmp, 40, &tail);
            check(ok && tail.find(L"line3-partial") == std::wstring::npos &&
                      tail.find(L"line2") != std::wstring::npos,
                  L"未完整结尾的日志丢弃最后半行 -> 「" + tail + L"」");
            DeleteFileW(tmp.c_str());
        }
    }

    // M2：托盘图标创建失败必须能被上层感知
    // （这里只验证接口存在与语义：Create 在图标注册失败时返回 false。
    //   真机上 explorer 正常时必然成功，故只做静态确认，不实际制造失败。）
    LogLine("  [INFO] TrayIcon::Create 现在会在图标注册失败时返回 false，"
            "且 RunTray 会保留控制台并弹窗提示");

    // M5：超时必须真的把子进程停掉，否则会有后台进程继续改系统
    {
        // ping -n 30 大约睡 29 秒；给它 1.5 秒超时
        DWORD rc = 0;
        std::wstring out;
        const DWORD t0 = GetTickCount();
        const bool finished = RunAndCapture(L"cmd.exe /c ping -n 30 127.0.0.1",
                                           GetHomeDir(), 1500, &rc, &out);
        const DWORD elapsed = GetTickCount() - t0;

        check(!finished, L"长命令在 1.5s 超时后返回失败（耗时 " +
                             std::to_wstring(elapsed) + L" ms）");
        check(elapsed < 10000, L"超时后未被长时间阻塞（" +
                                   std::to_wstring(elapsed) + L" ms）");

        PumpSleep(600);   // 给系统回收进程的时间
        const int leftover = CountOtherProcesses(L"PING.EXE");
        check(leftover == 0, L"超时后无残留 ping 进程（发现 " +
                                 std::to_wstring(leftover) + L" 个）");
    }

    LogLine("");

    LogLine("==================================================================");
    if (failures == 0) {
        LogLine("自检通过：全部动作路径可用。");
    } else {
        LogLine(W2U8(L"自检发现 " + std::to_wstring(failures) +
                     L" 项失败，见上面的 [FAIL]。"));
    }
    LogLine("==================================================================");
    return failures;
}

}  // namespace resprobe
