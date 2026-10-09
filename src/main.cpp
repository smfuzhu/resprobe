// main.cpp - 命令行入口
#include "common.h"

#include "actions.h"
#include "display.h"
#include "edid.h"
#include "report.h"
#include "session.h"
#include "tray.h"
#include "util.h"
#include "verdict.h"
#include "verify.h"
#include "watcher.h"

#include <shellapi.h>

namespace resprobe {

namespace {

void PrintUsage() {
    LogLine("ResProbe " RESPROBE_VERSION
            " - Windows 显示器分辨率变化探测与远程桌面诊断工具");
    LogLine("");
    LogLine("用法:");
    LogLine("  resprobe snapshot [选项]        采集一次完整快照并给出判定");
    LogLine("  resprobe watch    [选项]        持续监听分辨率/设备/电源/会话事件");
    LogLine("  resprobe verify   [选项]        只读验证模式可用性，可与远程客户端比对");
    LogLine("  resprobe paths    [选项]        列出系统报告的全部显示输出路径（含未激活占位路径）");
    LogLine("  resprobe ensure   [选项]        幂等地把拓扑调整到期望状态（供脚本调用，有退出码约定）");
    LogLine("  resprobe vdisplay [选项] --yes  尝试把虚拟/间接显示输出扩展进桌面（会改变系统状态）");
    LogLine("  resprobe restore  [选项] --yes  尝试把主输出恢复到参考分辨率（会改变系统状态）");
    LogLine("  resprobe tray     [选项]        启动系统托盘图标（无参数运行程序即等同于此）");
    LogLine("  resprobe selftest               自检托盘菜单依赖的全部动作路径");
    LogLine("                                  （等值写入：把当前分辨率再写一次，会更新显示模式注册表）");
    LogLine("");

    // 常用操作全部落在 .cmd 脚本里，不在上面的子命令里 —— 不写出来用户找不到。
    LogLine("常用操作（脚本入口，与本程序在同一目录）:");
    LogLine("");
    LogLine("  注意: 除\"其它\"一栏外，其余脚本都【需要以管理员身份运行】");
    LogLine("        （脚本自身会用 fltmc 检查权限，权限不足会直接退出）。");
    LogLine("        PowerShell 下要加 .\\ 前缀，例如  .\\安装.cmd ；cmd 下直接写文件名。");
    LogLine("");
    // 下面这些行的对齐是按**显示宽度**补空格的（一个汉字占 2 列），
    // 不是按字符个数。目标：名称字段宽 26 列，描述从第 34 列开始。
    LogLine("  安装 / 修复");
    LogLine("        .\\安装.cmd                幂等，可重复执行");
    LogLine("                                  驱动包 → 设备节点 → 虚拟显示器 → 显示拓扑");
    LogLine("");
    LogLine("  卸载");
    LogLine("        .\\卸载.cmd                移除虚拟屏与计划任务，保留驱动包");
    LogLine("        .\\卸载.cmd /full          连同 usbmmidd 驱动一起移除");
    LogLine("        .\\卸载.cmd /purge         再删除 OUT 目录与内置驱动包");
    LogLine("");
    LogLine("  一次性启动（没有显示器，或显示器不再接回）");
    LogLine("        .\\一次性启动.cmd          激活虚拟屏并把桌面调成参考分辨率");
    LogLine("        .\\一次性启动.cmd /status  只看状态，不做改动");
    LogLine("");
    LogLine("  守卫模式启动（常驻；默认的开机启动方式）");
    LogLine("        .\\常驻守卫.cmd            监听事件，链路断开时自动接管");
    LogLine("        .\\常驻守卫.cmd /bg        后台启动，立即返回");
    LogLine("        .\\常驻守卫.cmd /json      额外写状态 JSON");
    LogLine("");
    LogLine("  设置开机自启动");
    LogLine("        .\\开机自启.cmd            注册\"虚拟屏初始化\"+\"常驻守卫\"两个登录任务");
    LogLine("        .\\开机自启.cmd /status    查看注册状态");
    LogLine("        .\\开机自启.cmd /mode once 只注册初始化，不注册守卫");
    LogLine("        .\\开机自启.cmd /remove    注销全部");
    LogLine("");
    LogLine("  图形界面");
    LogLine("        resprobe.exe tray         托盘图标；右键菜单含以上全部操作");
    LogLine("                                  另有分辨率调整、状态报告、日志查看");
    LogLine("        resprobe.exe              无参数运行等同 tray");
    LogLine("");
    LogLine("  其它（不需要管理员权限）");
    LogLine("        编译.bat                  重新编译（加 clean 清空重建）");
    LogLine("        脚本编码修复.ps1          .cmd 报奇怪错误时先跑一次");
    LogLine("");
    LogLine("  脚本与命令行的关系");
    LogLine("    脚本只负责编排，真正的动作都由 resprobe.exe 完成；");
    LogLine("    ensure 的退出码(0/20/21)就是脚本判断幂等的依据。");
    LogLine("");
    LogLine("通用选项:");
    LogLine("  --reference WxH     参考健康分辨率，默认 1920x1080");
    LogLine("  --out FILE          写 JSON 报告。默认【不写】任何 JSON；");
    LogLine("                      纯文件名会落到输出目录（默认 <程序目录>\\OUT）");
    LogLine("  --out-dir DIR       覆盖默认输出目录（默认 <程序目录>\\OUT）");
    LogLine("  --no-cds-test       不执行 CDS_TEST 只读模式探测");
    LogLine("  --verbose           输出更详细的信息");
    LogLine("");
    LogLine("ensure 专有:");
    LogLine("  --mode auto|mirror|promote");
    LogLine("                      auto=有健康物理屏则镜像、否则提升虚拟屏（默认）");
    LogLine("  退出码: 0 已就绪 / 20 无可用虚拟显示输出 / 21 主屏未达参考分辨率 / 1 失败");
    LogLine("");
    LogLine("watch 专有:");
    LogLine("  --duration SEC      监听时长上限，0 表示直到 Ctrl+C，默认 0");
    LogLine("  --interval SEC      心跳采样间隔，0 表示关闭，默认 30");
    LogLine("  --settle MS         事件后等待驱动稳定的毫秒数，默认 800");
    LogLine("  --guard             检测到物理链路断开且虚拟屏可用时，自动把虚拟屏提升为主屏");
    LogLine("");
    LogLine("tray 专有:");
    LogLine("  --no-guard          只提供托盘界面，不自动改动桌面拓扑（默认是开守卫的）");
    LogLine("                      注：守卫是跨进程互斥的 —— 若登录任务里的常驻守卫已在运行，");
    LogLine("                      本实例会自动放弃守卫，状态报告里会写明由谁承担");
    LogLine("");
    LogLine("verify 专有:");
    LogLine("  --peer FILE         远程客户端机器生成的 *.peer.json 摘要，用于双端比对");
    LogLine("");
    LogLine("vdisplay 专有:");
    LogLine("  --yes               确认执行拓扑扩展（必需，否则仅做预检）");
    LogLine("  --mirror            把虚拟显示输出与主输出复制（镜像），桌面尺寸保持不变（推荐）");
    LogLine("  --promote           把虚拟显示输出设为参考分辨率主屏，并重排桌面");
    LogLine("");
    LogLine("restore 专有:");
    LogLine("  --yes               确认执行回写（必需，否则仅提示不动作）");
}

bool ParseReference(const std::wstring& s, UINT32* w, UINT32* h) {
    unsigned a = 0, b = 0;
    if (swscanf_s(s.c_str(), L"%ux%u", &a, &b) == 2 && a > 0 && b > 0) {
        *w = a;
        *h = b;
        return true;
    }
    return false;
}

int ToInt(const std::wstring& s, int def) {
    if (s.empty()) return def;
    return _wtoi(s.c_str());
}

// 环境 + 基线快照 + 远程软件痕迹
void CollectEnvironment(Report& r) {
    r.session = QuerySessionInfo();
    r.initial = CaptureSnapshot("initial", r.opt.referenceWidth, r.opt.referenceHeight);
    r.remoteSoftware = DetectRemoteSoftware(r.initial.targets);
}

void CollectProbes(Report& r, const DisplaySnapshot& snap) {
    for (const DisplayTarget& t : snap.targets) {
        if (!t.active || t.gdiDeviceName.empty()) continue;
        ModeProbe p = ProbeModeAvailability(t, r.opt.referenceWidth, r.opt.referenceHeight,
                                            r.opt.cdsTest);
        r.probes.push_back(p);
    }
}

// 统一构造判定输入：判定基于整条事件时间线的最差状态
VerdictInput MakeVerdictInput(const Report& r, const DisplaySnapshot* baseline,
                              const DisplaySnapshot* latest, const PeerSummary* peer) {
    VerdictInput vi;
    vi.baseline = baseline;
    vi.latest = latest;
    vi.events = &r.events;
    vi.session = &r.session;
    vi.software = &r.remoteSoftware;
    vi.peer = peer;
    vi.refWidth = r.opt.referenceWidth;
    vi.refHeight = r.opt.referenceHeight;
    return vi;
}

const DisplaySnapshot* LastOrInitial(const Report& r) {
    if (r.events.empty()) return &r.initial;
    return &r.events.back().snapshot;
}

void PrintHeaderBlock(const Report& r) {
    PrintBanner();
    PrintSession(r.session);
    PrintSnapshotConsole(r.initial, r.opt.verbose);
    PrintRemoteSoftware(r.remoteSoftware);
}

bool WriteOutputs(Report& r) {
    r.finishedUtc = NowIso8601Utc();

    std::wstring outPath = ResolveOutPath(r.opt);
    if (outPath.empty()) {
        // 默认不落盘：只有显式给出 --out 才写报告
        if (r.opt.verbose) {
            LogLine("[输出] 未指定 --out，未生成 JSON 报告（默认不落盘）");
        }
        return true;
    }

    size_t slash = outPath.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        std::wstring dir = outPath.substr(0, slash);
        if (!EnsureDirectoryExists(dir)) {
            LogLine("[输出] 无法创建输出目录: " + W2U8(dir));
            return false;
        }
    }

    bool ok = WriteJsonReport(r, outPath);
    if (ok) {
        LogLine("[输出] JSON 报告: " + W2U8(outPath));
    } else {
        LogLine("[输出] 写入 JSON 报告失败: " + W2U8(outPath));
    }

    std::wstring peerPath = DerivePeerPath(outPath);
    if (WritePeerSummary(r, peerPath)) {
        LogLine("[输出] 对端摘要（可拷贝到另一台机器用于双端比对）: " + W2U8(peerPath));
    }

    return ok;
}

// ---------------------------------------------------------------------------
int RunSnapshot(Report& r) {
    CollectEnvironment(r);
    CollectProbes(r, r.initial);

    r.verdict = EvaluateVerdict(MakeVerdictInput(r, &r.initial, &r.initial, nullptr));

    PrintHeaderBlock(r);
    PrintProbes(r.probes);
    PrintVerdict(r.verdict);
    // M-3：给了 --out 却写不进去（磁盘满 / 只读目录 / 目录建不出来）时不能返回 0，
    // 否则调用脚本会以为报告已经落盘。
    return WriteOutputs(r) ? 0 : 1;
}

// ---------------------------------------------------------------------------
int RunVerify(Report& r) {
    CollectEnvironment(r);

    if (!r.opt.peerPath.empty()) {
        if (!ReadPeerSummary(r.opt.peerPath, &r.peer)) {
            LogLine("[警告] 无法读取对端摘要文件: " + W2U8(r.opt.peerPath));
        } else {
            LogLine("[对端] 已读取 " + W2U8(r.opt.peerPath) +
                    "  对端主分辨率=" + r.peer.primaryMode +
                    "  采集时间=" + r.peer.timeUtc);
        }
    }

    CollectProbes(r, r.initial);

    // H-3：只在真正解析成功（present）时才启用双端比对，与 RunWatch 用同一判据。
    // 否则"文件读得到但不是对端摘要"会被当成有效对端，产出"远程客户端="（空值）
    // 这种无意义证据行，还会把单机时间线判定挡掉。
    const PeerSummary* peer =
        (r.opt.peerPath.empty() || !r.peer.present) ? nullptr : &r.peer;

    r.verdict = EvaluateVerdict(MakeVerdictInput(r, &r.initial, &r.initial, peer));

    PrintHeaderBlock(r);

    // 只读验证结论
    LogLine("[验证] 参考分辨率 " + W2U8(FormatModeText(r.opt.referenceWidth,
                                                        r.opt.referenceHeight)) +
            " 在各活动输出上的可用性:");
    for (const ModeProbe& p : r.probes) {
        std::string verdictText;
        if (p.listedInModeList && (!p.cdsTestPerformed || p.cdsTestAccepted)) {
            verdictText = "模式表内存在，CDS_TEST 预检通过"
                          "（注意：预检只校验模式合法性，不代表当前链路一定能实际应用；"
                          "链路断开时实际应用仍可能返回 DISP_CHANGE_FAILED）";
        } else if (p.listedInModeList && p.cdsTestPerformed && !p.cdsTestAccepted) {
            verdictText = "模式表内存在但 CDS_TEST 即被拒绝 —— 该输出当前无法承载该模式";
        } else if (!p.listedInModeList) {
            verdictText = "模式表内不存在 —— 该输出已丢失该模式的承载能力";
        } else {
            verdictText = "未知";
        }
        LogLine("        " + W2U8(p.gdiDeviceName) + " : " + verdictText);
    }

    PrintProbes(r.probes);
    PrintVerdict(r.verdict);
    return WriteOutputs(r) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// 幂等地把显示拓扑调整到期望状态，供安装 / 启动 / 守卫脚本调用。
//
// 退出码约定（脚本据此判断，无需解析中文输出）：
//   0   已就绪
//   20  没有可用的虚拟显示输出（需先 deviceinstaller64 enableidd 1）
//   21  已调整，但主屏仍未达到参考分辨率
//   1   执行失败
//
// 注意（M-3）：本函数**刻意**不把 WriteOutputs 的失败并入退出码。0/20/21 是脚本做幂等
// 判断的对外契约（DEVELOPMENT.md §6.5），"报告写不进去"不应改变拓扑判定的结论；写盘
// 失败本身仍会打印 "[输出] 写入 JSON 报告失败: …"。诊断类子命令才把失败提升为退出码。
int RunEnsure(Report& r) {
    auto log = [&r](const std::string& s) {
        LogLine(s);
        r.actionLog.push_back(s);
    };

    const UINT32 refW = r.opt.referenceWidth;
    const UINT32 refH = r.opt.referenceHeight;
    const std::wstring want = FormatModeText(refW, refH);

    r.session = QuerySessionInfo();
    PrintBanner();
    PrintSession(r.session);

    // ---- 前置条件：虚拟显示输出是否存在（与当前桌面状态无关）----------------
    // 退出码 0 的含义是"兜底能力已就位"，因此必须先确认驱动确实实例化了虚拟显示器。
    // 只看桌面是否正常是不够的：显示器开着的时候桌面本来就是 1920x1080，
    // 而此刻虚拟屏可能压根不存在，物理屏一旦断电就又掉回 1024x768。
    // 判据见 CountAvailableVirtualOutputs（托盘状态查询共用同一实现）。
    int virtualAvailable = CountAvailableVirtualOutputs(refW, refH);

    TopologyState st = EvaluateTopology(refW, refH);

    log("[ensure] 活动输出 " + std::to_string(st.totalActive) +
        " 个：虚拟 " + std::to_string(st.activeVirtualCount) +
        "，健康物理 " + std::to_string(st.healthyPhysicalCount) +
        "，断链物理 " + std::to_string(st.deadPhysicalCount));
    log("[ensure] 主屏 " + W2U8(st.primaryMode) + " (" + W2U8(st.primaryGdiDevice) + ")");
    log("[ensure] 可用的虚拟显示输出: " + std::to_string(virtualAvailable) + " 个");

    if (virtualAvailable == 0) {
        log("[ensure] 没有可用的虚拟显示输出 —— 物理屏断电后将无法兜底。");
        log("[ensure] 请先执行: deviceinstaller64.exe enableidd 1");
        WriteOutputs(r);
        return 20;
    }

    // ---- 就绪判据 -----------------------------------------------------------
    // 刻意避开"虚拟屏识别"：克隆拓扑下多个输出共用同一个 GDI 源名，DisplayConfig
    // 会把克隆目标报在源适配器上，此时基于适配器/设备描述的识别都会失效。
    // 但只要主屏达到参考分辨率、且桌面上没有挂断链死输出，对使用方来说就是可用的。
    bool primaryOk = (st.primaryMode == want);
    bool noDeadOnDesktop = (st.deadPhysicalCount == 0);

    if (primaryOk && noDeadOnDesktop) {
        log("[ensure] 已就绪，无需调整。");
        WriteOutputs(r);
        return 0;
    }

    bool wantPromote;
    if (r.opt.ensureMode == L"promote") wantPromote = true;
    else if (r.opt.ensureMode == L"mirror") wantPromote = false;
    else wantPromote = (st.healthyPhysicalCount == 0);   // auto

    if (wantPromote) {
        log("[ensure] 模式=提升虚拟屏（无健康物理显示）...");
        PromoteOutcome po = PromoteVirtualDisplay(refW, refH, true);
        for (const std::string& s : po.log) log("        " + s);
        log("[ensure] " + W2U8(po.message));
    } else {
        log("[ensure] 模式=复制（镜像）...");
        LONG rc = ApplyCloneTopology();
        log("        SDC_TOPOLOGY_CLONE -> " + W2U8(DispHangeResultName(rc)));
        if (rc != ERROR_SUCCESS) {
            log("[ensure] 复制拓扑失败，退化为提升虚拟屏以保证主屏正确 ...");
            PromoteOutcome po = PromoteVirtualDisplay(refW, refH, true);
            for (const std::string& s : po.log) log("        " + s);
            log("[ensure] " + W2U8(po.message));
        }
        Sleep(1500);
    }

    TopologyState after = EvaluateTopology(refW, refH);
    log("[ensure] 复核：活动输出 " + std::to_string(after.totalActive) +
        " 个（虚拟 " + std::to_string(after.activeVirtualCount) + "），主屏 " +
        W2U8(after.primaryMode) + " (" + W2U8(after.primaryGdiDevice) + ")");

    WriteOutputs(r);

    if (after.primaryMode == want) {
        log("[ensure] 已就绪。");
        return 0;
    }
    log("[ensure] 主屏未达到 " + W2U8(want) + "，请检查虚拟显示驱动状态。");
    return 21;
}

// ---------------------------------------------------------------------------
// 列出全部显示输出路径，用于排查可被激活的虚拟/间接显示输出
int RunPaths(Report& r) {
    int totalPaths = 0;
    std::wstring diag;
    std::vector<DisplayTarget> all =
        EnumerateDisplays(r.opt.referenceWidth, r.opt.referenceHeight, &diag, &totalPaths, true);

    PrintBanner();
    PrintSession(r.session);
    r.initial.timeUtc = NowIso8601Utc();
    r.initial.uptimeMs = MonotonicMs();
    r.initial.trigger = "paths";
    r.initial.targets = all;
    r.initial.totalPathCount = totalPaths;
    r.initial.primaryMode = PrimaryModeText(r.initial);
    LogLine("[路径] 系统报告 " + std::to_string(totalPaths) + " 条输出路径");

    int active = 0;
    int interesting = 0;
    for (const DisplayTarget& t : all) {
        if (t.active) ++active;
        if (IsMeaningfulDisplayTarget(t)) ++interesting;
    }
    LogLine("[路径] 其中活动 " + std::to_string(active) + " 条，有监视器信息 " +
            std::to_string(interesting) + " 条");

    // 按适配器聚合，便于识别虚拟/间接显示适配器
    std::map<std::string, int> byAdapter;
    for (const DisplayTarget& t : all) {
        byAdapter[FormatLuid(t.adapterLuid)] += 1;
    }
    LogLine("[路径] 涉及 " + std::to_string(byAdapter.size()) + " 个适配器:");
    for (const auto& kv : byAdapter) {
        LogLine("        adapter " + kv.first + " : " + std::to_string(kv.second) + " 条路径");
    }

    LogLine("------------------------------------------------------------------");
    for (const DisplayTarget& t : all) {
        if (!IsMeaningfulDisplayTarget(t) && !r.opt.verbose) {
            // 非活动且无监视器信息：只在 verbose 下逐条打印
            continue;
        }
        std::string line;
        line += t.active ? "  [ACTIVE] " : "  [idle  ] ";
        line += "adapter=" + FormatLuid(t.adapterLuid);
        line += " target=" + std::to_string(t.targetId);
        line += " source=" + std::to_string(t.sourceId);
        line += " tech=" + W2U8(t.outputTechnologyName);
        line += " mode=" + W2U8(FormatModeText(t.width, t.height));
        line += " gdi=" + (t.gdiDeviceName.empty() ? std::string("(无)") : W2U8(t.gdiDeviceName));
        line += " monitor=" +
                (t.monitorFriendlyName.empty() ? std::string("(无)") : W2U8(t.monitorFriendlyName));
        line += " edid=" + std::string(t.edid.present ? "是" : "否");
        line += " available=" + std::string(t.targetAvailable ? "是" : "否");
        LogLine(line);
    }

    PrintRemoteSoftware(DetectRemoteSoftware(all));

    r.verdict.code = "PATHS_LISTED";
    r.verdict.summary = "已列出全部输出路径";
    return WriteOutputs(r) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// 虚拟显示（间接显示驱动 IDD）激活尝试。
// 仅做“拓扑扩展”，不替换已有输出；默认只做 SDC_VALIDATE 预检，--yes 才真正应用。
int RunVirtualDisplay(Report& r) {
    auto log = [&r](const std::string& s) {
        LogLine(s);
        r.actionLog.push_back(s);
    };

    PrintBanner();
    PrintSession(r.session);

    r.initial = CaptureSnapshot("vdisplay-before", r.opt.referenceWidth,
                               r.opt.referenceHeight);
    PrintSnapshotConsole(r.initial, r.opt.verbose);

    // ---- 1. 盘点适配器，找出候选虚拟显示适配器 ----
    int totalPaths = 0;
    std::wstring diag;
    std::vector<DisplayTarget> all = EnumerateDisplays(
        r.opt.referenceWidth, r.opt.referenceHeight, &diag, &totalPaths, true);

    std::map<std::string, int> activeAdapters;
    for (const DisplayTarget& t : all) {
        if (t.active) activeAdapters[FormatLuid(t.adapterLuid)] += 1;
    }

    std::map<std::string, int> idleAdapterPaths;
    for (const DisplayTarget& t : all) {
        std::string luid = FormatLuid(t.adapterLuid);
        if (activeAdapters.count(luid)) continue;
        idleAdapterPaths[luid] += 1;
    }

    log("[虚拟屏] 系统报告 " + std::to_string(totalPaths) + " 条路径，涉及 " +
        std::to_string(activeAdapters.size() + idleAdapterPaths.size()) + " 个适配器");
    for (const auto& kv : activeAdapters) {
        log("        活动适配器 " + kv.first + " : " + std::to_string(kv.second) + " 条活动路径");
    }
    for (const auto& kv : idleAdapterPaths) {
        log("        空闲适配器 " + kv.first + " : " + std::to_string(kv.second) +
            " 条占位路径（潜在虚拟显示承载者）");
    }

    // targetAvailable 是判断“有没有真实显示接收端”的直接依据
    int availableCount = 0;
    int availableOffAdapter = 0;
    for (const DisplayTarget& t : all) {
        if (!t.targetAvailable) continue;
        ++availableCount;
        if (!activeAdapters.count(FormatLuid(t.adapterLuid))) ++availableOffAdapter;
    }
    int activeVirtual = 0;
    for (const DisplayTarget& t : all) {
        if (!t.active || t.gdiDeviceName.empty()) continue;
        if (ContainsInsensitiveW(t.gdiDeviceString, L"USB Mobile Monitor") ||
            ContainsInsensitiveW(t.gdiDeviceString, L"IddDriver") ||
            ContainsInsensitiveW(t.gdiDeviceString, L"Virtual Display") ||
            ContainsInsensitiveW(t.gdiDeviceString, L"Indirect Display")) {
            ++activeVirtual;
            log("[虚拟屏] 已激活的虚拟显示输出: " + W2U8(t.gdiDeviceName) +
                "  适配器=" + W2U8(t.gdiDeviceString) +
                "  当前=" + W2U8(FormatModeText(t.width, t.height)));
        }
    }

    log("[虚拟屏] 可用的显示接收端(targetAvailable=TRUE)共 " + std::to_string(availableCount) +
        " 个，其中已激活的虚拟显示输出 " + std::to_string(activeVirtual) + " 个");
    if (activeVirtual == 0 && availableOffAdapter == 0) {
        log("[虚拟屏] 没有任何已激活的虚拟显示输出 —— 说明虚拟显示驱动尚未创建"
            "虚拟显示器，无法扩展进桌面");
    }

    // ---- 2. 拓扑扩展预检（只读，不改变系统）----
    struct TopologyProbe {
        DWORD flags;
        const char* name;
        LONG result;
    };

    // SDC_VALIDATE 不能与 SDC_ALLOW_CHANGES 组合，否则返回 ERROR_INVALID_PARAMETER(87)
    TopologyProbe probes[] = {
        { SDC_TOPOLOGY_EXTEND, "SDC_TOPOLOGY_EXTEND（扩展桌面到所有可用输出）", 0 },
        { SDC_TOPOLOGY_CLONE,  "SDC_TOPOLOGY_CLONE（复制主输出）", 0 },
    };

    log("[虚拟屏] 拓扑预检（SDC_VALIDATE，不会改动系统）:");
    for (TopologyProbe& p : probes) {
        p.result = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_VALIDATE | p.flags);
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "        %s -> %s (%ld)",
                    p.name,
                    p.result == ERROR_SUCCESS ? "可用" : "不可用",
                    (long)p.result);
        log(buf);
    }

    LONG extendValidate = probes[0].result;

    if (!r.opt.confirmed) {
        log("[虚拟屏] 预检完成，未提供 --yes，未改动系统显示配置。");
        if (extendValidate != ERROR_SUCCESS && availableOffAdapter == 0) {
            log("[虚拟屏] 结论: 当前没有可被激活的额外显示输出 —— 间接显示驱动虽然已安装，"
                "但没有实例化任何虚拟显示器，Windows 无法把它扩展进来");
            log("[虚拟屏] 需要先让虚拟显示驱动创建虚拟显示器"
                "（例如 usbmmidd 的 deviceinstaller64 enableidd 1），再运行本命令接管。");
        }
    } else {
        // ---- 3. 应用拓扑 ----
        // 注意：SDC_ALLOW_CHANGES 只能与 SDC_USE_SUPPLIED_DISPLAY_CONFIG 组合，
        // 与 SDC_TOPOLOGY_* 同用会返回 ERROR_INVALID_PARAMETER(87)。
        if (r.opt.mirror) {
            LONG rc = SetDisplayConfig(0, nullptr, 0, nullptr,
                                       SDC_APPLY | SDC_TOPOLOGY_CLONE);
            char buf[256];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "[虚拟屏] SDC_APPLY | SDC_TOPOLOGY_CLONE（复制/镜像） -> 返回 %ld",
                        (long)rc);
            log(buf);
            if (rc != ERROR_SUCCESS) {
                log("[虚拟屏] 复制拓扑应用失败，未改动系统显示配置。");
            }
        } else if (extendValidate == ERROR_SUCCESS) {
            LONG rc = SetDisplayConfig(0, nullptr, 0, nullptr,
                                       SDC_APPLY | SDC_TOPOLOGY_EXTEND);
            char buf[256];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "[虚拟屏] SDC_APPLY | SDC_TOPOLOGY_EXTEND -> 返回 %ld", (long)rc);
            log(buf);
            if (rc != ERROR_SUCCESS) {
                log("[虚拟屏] 拓扑扩展应用失败，未改动系统显示配置。");
            }
        } else {
            log("[虚拟屏] 拓扑扩展预检未通过，跳过扩展步骤，"
                "直接检查当前是否已有可用的虚拟显示输出。");
        }

        {
            Sleep(1500);
            DisplaySnapshot after = CaptureSnapshot("vdisplay-after", r.opt.referenceWidth,
                                                    r.opt.referenceHeight);
            PrintSnapshotConsole(after, true);

            EventRecord ev;
            ev.index = (int)r.events.size() + 1;
            ev.timeUtc = after.timeUtc;
            ev.uptimeMs = after.uptimeMs;
            ev.trigger = "vdisplay-extend";
            ev.snapshot = after;
            DiffSnapshots(r.initial, after, &ev.changedFields, &ev.transitions);
            ev.changed = !ev.changedFields.empty();
            r.events.push_back(ev);
            PrintEventConsole(ev);

            int activeAfter = 0;
            for (const DisplayTarget& t : after.targets) {
                if (t.active) ++activeAfter;
            }
            log("[虚拟屏] 扩展后活动输出数: " + std::to_string(activeAfter));

            // 识别虚拟显示输出：适配器描述里带虚拟/间接显示特征，或带镜像驱动标志
            auto isVirtualTarget = [](const DisplayTarget& t) {
                return ContainsInsensitiveW(t.gdiDeviceString, L"USB Mobile Monitor") ||
                       ContainsInsensitiveW(t.gdiDeviceString, L"IddDriver") ||
                       ContainsInsensitiveW(t.gdiDeviceString, L"Virtual Display") ||
                       ContainsInsensitiveW(t.gdiDeviceString, L"Indirect Display") ||
                       t.gdiMirroringDriver;
            };

            std::map<std::string, bool> beforeActive;
            for (const DisplayTarget& t : r.initial.targets) {
                if (t.active) beforeActive[UpperAscii(W2U8(t.gdiDeviceName))] = true;
            }

            std::wstring newDevice;
            // 1) 优先选择本次新出现的活动输出
            for (const DisplayTarget& t : after.targets) {
                if (!t.active || t.gdiDeviceName.empty()) continue;
                if (!beforeActive.count(UpperAscii(W2U8(t.gdiDeviceName)))) {
                    newDevice = t.gdiDeviceName;
                    break;
                }
            }
            // 2) 否则选择已处于活动状态的虚拟显示输出
            if (newDevice.empty()) {
                for (const DisplayTarget& t : after.targets) {
                    if (!t.active || t.gdiDeviceName.empty()) continue;
                    if (isVirtualTarget(t)) {
                        newDevice = t.gdiDeviceName;
                        log("[虚拟屏] 检测到已激活的虚拟显示输出 " + W2U8(t.gdiDeviceName) +
                            "（适配器: " + W2U8(t.gdiDeviceString) + "）");
                        break;
                    }
                }
            }

            if (r.opt.mirror) {
                log("[虚拟屏] 已使用复制（镜像）拓扑，不做主屏重排。");
                newDevice.clear();
            } else if (!r.opt.promote && !newDevice.empty()) {
                log("[虚拟屏] 已找到虚拟显示输出 " + W2U8(newDevice) +
                    "，未提供 --promote，保持当前桌面布局不变。");
                newDevice.clear();
            }

            if (activeAfter > 1 && !newDevice.empty()) {
                log("[虚拟屏] 将虚拟输出 " + W2U8(newDevice) +
                    " 设为参考分辨率主屏，并摘除断链死输出 ...");

                PromoteOutcome po = PromoteVirtualDisplay(r.opt.referenceWidth,
                                                          r.opt.referenceHeight, true);
                for (const std::string& s : po.log) {
                    log("[虚拟屏] " + s);
                }
                log("[虚拟屏] " + W2U8(po.message));

                DisplaySnapshot final = CaptureSnapshot("vdisplay-primary",
                                                        r.opt.referenceWidth,
                                                        r.opt.referenceHeight);
                PrintSnapshotConsole(final, true);
                log("[虚拟屏] 主屏现在应为 " + W2U8(final.primaryMode) + " (" +
                    W2U8(final.primaryGdiDevice) + ")");
            } else if (activeAfter <= 1) {
                log("[虚拟屏] 当前仍只有 1 个活动输出，未发现虚拟显示器。");
            }
        }
    }

    CollectProbes(r, *LastOrInitial(r));

    r.verdict = EvaluateVerdict(MakeVerdictInput(r, &r.initial, LastOrInitial(r), nullptr));
    PrintVerdict(r.verdict);
    return WriteOutputs(r) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// 托盘模式：常驻的图形入口（无主窗口，只有托盘图标 + 右键菜单）
//
// 关键点：托盘**不自己跑消息循环**，而是复用监听的消息循环。
//   - TrayIcon 在本线程另建一个隐藏的顶层窗口
//   - Watcher::Run 用 MsgWaitForMultipleObjects(0,nullptr,..) + PeekMessageW(hwnd=nullptr)
//     取出本线程**所有**窗口的消息再 DispatchMessage
//   - 于是两个窗口共享同一个泵，watcher.cpp 一行都不用改
//
// 方案 A（子系统）：保持控制台子系统，这里只把控制台窗口藏起来。
// 不改成 -mwindows 的原因见 DEVELOPMENT.md 第 3 节：.cmd 脚本依赖
// stdout 可重定向 + 退出码即契约，改子系统必须同时保证这两点不退化。
int RunTray(Report& r) {
    // 先把输出落到文件：稍后控制台窗口会被隐藏，不重定向就等于把现场丢进黑洞
    LogLine("==== ResProbe tray ====");
    if (!RedirectStdioToLog(L"tray.log", nullptr)) {
        LogLine("提示: 无法把输出重定向到 OUT\\tray.log，本次运行不落日志。");
    }

    // ---- 单实例保护 --------------------------------------------------------
    // 托盘：重复启动会出现两个图标，所以拿不到锁就直接退出。
    if (!AcquireTrayLock()) {
        LogLine("已有 ResProbe 托盘在运行，本次启动退出。"
                "（请在任务栏通知区域找到已有图标，或先结束该进程）");
        MessageBoxW(nullptr,
                    L"已经有一个 ResProbe 托盘在运行。\n\n"
                    L"请在任务栏通知区域找到它的图标；若要重启，先在那里选择\"退出\"。",
                    L"ResProbe", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return 5;
    }

    // 守卫：跨进程互斥资源。登录任务里跑的常驻守卫可能已经在守卫了 ——
    // 那个进程没有界面，所以不能因此拒绝用户启动托盘（否则用户没界面可用），
    // 但本实例必须放弃守卫，避免两个进程同时改显示拓扑。
    const GuardOwner guardOwner = AcquireGuardLock();
    r.opt.guard = (!r.opt.noGuard) && (guardOwner == GuardOwner::ThisProcess);

    CollectEnvironment(r);

    TrayIcon tray;
    // 先建托盘，成功了再隐藏控制台。反过来的话，图标创建失败时用户
    // 既看不到图标、也看不到控制台，会以为程序根本没启动。
    if (!tray.Create(r.opt.referenceWidth, r.opt.referenceHeight)) {
        LogLine("错误: 无法创建托盘图标（控制台保持可见以便查看原因）");
        MessageBoxW(nullptr,
                    L"无法创建系统托盘图标。\n\n"
                    L"常见原因：explorer.exe（任务栏）未运行。\n"
                    L"详细输出保留在控制台窗口中。",
                    L"ResProbe 启动失败", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return 4;
    }

    tray.SetGuardEnabled(r.opt.guard);
    HideConsoleWindow();

    LogLine("ResProbe 托盘已启动。右键托盘图标打开菜单；菜单里的\"退出\"可结束进程。");
    LogLine("托盘与事件监听共用同一个消息循环。");
    if (r.opt.noGuard) {
        LogLine("守卫：已按 --no-guard 关闭，本实例不会自动改动桌面拓扑。");
    } else if (guardOwner == GuardOwner::ThisProcess) {
        LogLine("守卫：由本实例承担。");
    } else {
        LogLine("守卫：另一个 ResProbe 实例已持有守卫权，本实例只提供界面"
                "（避免两个进程同时改显示拓扑）。");
    }

    Watcher watcher(r);
    int rc = watcher.Run();

    tray.Destroy();
    LogLine("ResProbe 托盘已退出。");
    return rc;
}

// ---------------------------------------------------------------------------
int RunWatch(Report& r) {
    // 守卫是跨进程互斥资源：显示拓扑是机器级资源，两个进程同时改会互相打架
    // （kGuardCooldownMs 只是进程内冷却，拦不住跨进程）。
    // 登录任务里的常驻守卫会一直占着这把锁，此时本进程降级为"只监听不守卫"。
    if (r.opt.guard) {
        const GuardOwner owner = AcquireGuardLock();
        if (owner != GuardOwner::ThisProcess) {
            r.opt.guard = false;
            LogLine("[守卫] 另一个 ResProbe 实例已持有守卫权，本次监听不执行守卫动作。");
        }
    }

    // 先采集环境信息，供报告使用
    CollectEnvironment(r);

    PrintBanner();
    PrintSession(r.session);
    PrintSnapshotConsole(r.initial, r.opt.verbose);
    PrintRemoteSoftware(r.remoteSoftware);
    LogLine("");

    Watcher w(r);
    int rc = w.Run();

    CollectProbes(r, *LastOrInitial(r));

    const DisplaySnapshot* baseline =
        r.events.empty() ? &r.initial : &r.events.front().snapshot;
    const PeerSummary* peer =
        (r.opt.peerPath.empty() || !r.peer.present) ? nullptr : &r.peer;

    r.verdict = EvaluateVerdict(MakeVerdictInput(r, baseline, LastOrInitial(r), peer));

    PrintProbes(r.probes);
    PrintVerdict(r.verdict);

    // 会话信息可能已经变化（远程连接/断开），刷新一次
    r.session = QuerySessionInfo();
    r.remoteSoftware = DetectRemoteSoftware(LastOrInitial(r)->targets);

    // M-3：报告写入失败时把退出码提升为非 0（监听本身已经是 0 才算正常结束）
    const bool reportWritten = WriteOutputs(r);
    if (!reportWritten && rc == 0) rc = 1;
    return rc;
}

// ---------------------------------------------------------------------------
int RunRestore(Report& r) {
    CollectEnvironment(r);

    PrintBanner();
    PrintSession(r.session);
    PrintSnapshotConsole(r.initial, r.opt.verbose);
    PrintRemoteSoftware(r.remoteSoftware);

    if (!r.opt.confirmed) {
        LogLine("");
        LogLine("[回写] 未提供 --yes，仅做只读分析，不修改系统显示状态。");
        CollectProbes(r, r.initial);
        r.verdict = EvaluateVerdict(MakeVerdictInput(r, &r.initial, &r.initial, nullptr));
        PrintProbes(r.probes);
        PrintVerdict(r.verdict);
        WriteOutputs(r);
        return 0;
    }

    LogLine("");
    LogLine("[回写] 开始尝试恢复 " +
            W2U8(FormatModeText(r.opt.referenceWidth, r.opt.referenceHeight)) + " ...");

    RestoreOutcome ro = RestoreReferenceMode(r.initial, r.opt.referenceWidth,
                                             r.opt.referenceHeight);
    LogLine("[回写] " + W2U8(ro.message));

    r.restoreAttempted = ro.attempted;
    r.restoreSucceeded = ro.succeeded;
    r.restoreResultName = W2U8(ro.resultName);
    r.restoreMessage = W2U8(ro.message);
    for (const ApplyAttempt& a : ro.attempts) {
        r.restoreAttempts.push_back(W2U8(a.description) + " -> " +
                                    W2U8(DispHangeResultName(a.resultCode)));
    }
    if (!ro.attempts.empty() && !ro.succeeded) {
        LogLine("[回写] 尝试明细:");
        for (const std::string& s : r.restoreAttempts) {
            LogLine("        - " + s);
        }
    }

    if (ro.attempted && ro.succeeded) {
        Sleep(1500);
    }

    DisplaySnapshot after = CaptureSnapshot("after-restore", r.opt.referenceWidth,
                                            r.opt.referenceHeight);
    ro.after = after.primaryMode;

    EventRecord ev;
    ev.index = 1;
    ev.timeUtc = after.timeUtc;
    ev.uptimeMs = after.uptimeMs;
    ev.trigger = "restore";
    ev.snapshot = after;
    DiffSnapshots(r.initial, after, &ev.changedFields, &ev.transitions);
    ev.changed = !ev.changedFields.empty();
    r.events.push_back(ev);
    PrintEventConsole(ev);

    LogLine("[回写] 回写前主分辨率=" + W2U8(ro.before) +
            "  回写后主分辨率=" + W2U8(ro.after));

    CollectProbes(r, after);

    r.verdict = EvaluateVerdict(MakeVerdictInput(r, &r.initial, &after, nullptr));

    PrintProbes(r.probes);
    PrintVerdict(r.verdict);
    return WriteOutputs(r) ? 0 : 1;
}

}  // namespace

// ---------------------------------------------------------------------------
// 动作层自检（托盘菜单无法自动点击，这是它唯一可脚本化验证的入口）
int RunSelfTest(Report& r) {
    (void)r;
    PrintBanner();
    return (RunActionSelfTest() == 0) ? 0 : 1;
}

}  // namespace resprobe

int main(int, char**) {
    using namespace resprobe;

    SetConsoleOutputCP(CP_UTF8);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        PrintUsage();
        return 1;
    }

    if (argc < 2) {
        // 无参数 = 直接启动托盘 GUI，这是双击 exe 的预期行为。
        // 控制台用户要看用法请用 help / --help / -h / /?
        LocalFree(argv);
        Report trayReport;
        trayReport.opt.mode = L"tray";
        return RunTray(trayReport);
    }

    Report report;
    report.opt.mode = argv[1];

    if (report.opt.mode == L"help" || report.opt.mode == L"--help" ||
        report.opt.mode == L"-h" || report.opt.mode == L"/?") {
        PrintUsage();
        LocalFree(argv);
        return 0;
    }

    if (report.opt.mode != L"snapshot" && report.opt.mode != L"watch" &&
        report.opt.mode != L"verify" && report.opt.mode != L"restore" &&
        report.opt.mode != L"paths" && report.opt.mode != L"vdisplay" &&
        report.opt.mode != L"ensure" && report.opt.mode != L"tray" &&
        report.opt.mode != L"selftest") {
        LogLine("错误: 未知子命令 " + W2U8(report.opt.mode));
        PrintUsage();
        LocalFree(argv);
        return 1;
    }

    for (int i = 2; i < argc; ++i) {
        std::wstring a = argv[i];

        if (a == L"--out" && i + 1 < argc) {
            report.opt.outPath = argv[++i];
        } else if (a == L"--out-dir" && i + 1 < argc) {
            report.opt.outDir = argv[++i];
        } else if (a == L"--mode" && i + 1 < argc) {
            std::wstring m = argv[++i];
            if (m == L"auto" || m == L"mirror" || m == L"promote") {
                report.opt.ensureMode = m;
            } else {
                LogLine("警告: --mode 仅支持 auto|mirror|promote，已使用 auto");
            }
        } else if (a == L"--peer" && i + 1 < argc) {
            report.opt.peerPath = argv[++i];
        } else if (a == L"--reference" && i + 1 < argc) {
            UINT32 w = 0, h = 0;
            if (ParseReference(argv[++i], &w, &h)) {
                report.opt.referenceWidth = w;
                report.opt.referenceHeight = h;
            } else {
                LogLine("警告: --reference 参数格式应为 WxH，已使用默认 1920x1080");
            }
        } else if (a == L"--duration" && i + 1 < argc) {
            report.opt.durationSec = ToInt(argv[++i], 0);
        } else if (a == L"--interval" && i + 1 < argc) {
            report.opt.intervalSec = ToInt(argv[++i], 30);
        } else if (a == L"--settle" && i + 1 < argc) {
            report.opt.settleMs = ToInt(argv[++i], 800);
        } else if (a == L"--no-cds-test") {
            report.opt.cdsTest = false;
        } else if (a == L"--yes" || a == L"-y") {
            report.opt.confirmed = true;
        } else if (a == L"--promote") {
            report.opt.promote = true;
        } else if (a == L"--mirror") {
            report.opt.mirror = true;
        } else if (a == L"--guard") {
            report.opt.guard = true;
        } else if (a == L"--no-guard") {
            report.opt.noGuard = true;
        } else if (a == L"--verbose" || a == L"-v") {
            report.opt.verbose = true;
        } else if (a == L"--quiet" || a == L"-q") {
            report.opt.quiet = true;
        } else {
            LogLine("警告: 忽略未知参数 " + W2U8(a));
        }
    }

    LocalFree(argv);

    // --no-guard 只对 tray 有意义。其它模式下静默接受会让"我明明加了参数却没生效"
    // 变成无从查起的困惑，所以明确警告并清掉。
    if (report.opt.noGuard && report.opt.mode != L"tray") {
        LogLine("警告: --no-guard 仅对 tray 子命令有意义，已忽略。");
        report.opt.noGuard = false;
    }

    report.startedUtc = NowIso8601Utc();

    int rc = 0;
    if (report.opt.mode == L"snapshot")      rc = RunSnapshot(report);
    else if (report.opt.mode == L"watch")    rc = RunWatch(report);
    else if (report.opt.mode == L"verify")   rc = RunVerify(report);
    else if (report.opt.mode == L"paths")    rc = RunPaths(report);
    else if (report.opt.mode == L"vdisplay") rc = RunVirtualDisplay(report);
    else if (report.opt.mode == L"ensure")   rc = RunEnsure(report);
    else if (report.opt.mode == L"restore")  rc = RunRestore(report);
    else if (report.opt.mode == L"tray")     rc = RunTray(report);
    else if (report.opt.mode == L"selftest") rc = RunSelfTest(report);

    return rc;
}
