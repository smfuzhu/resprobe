// verify.cpp - 模式可用性验证、回写与双端比对
#include "verify.h"
#include "display.h"
#include "util.h"

#include <fstream>
#include <set>
#include <sstream>

namespace resprobe {

// ---------------------------------------------------------------------------
ModeProbe ProbeModeAvailability(const DisplayTarget& t, UINT32 width, UINT32 height,
                                bool useCdsTest) {
    ModeProbe p;
    p.gdiDeviceName = t.gdiDeviceName;
    p.width = width;
    p.height = height;
    p.deviceActive = t.active;
    p.currentMode = t.gdiCurrentMode;
    p.registryMode = t.gdiRegistryMode;

    std::wstring target = FormatModeText(width, height);

    // 1) 模式表中是否存在
    for (const std::wstring& r : t.modeResolutions) {
        if (r == target) {
            p.listedInModeList = true;
            break;
        }
    }

    // 2) 收集所有“降级级别”的分辨率（<= 1024x768）
    for (const std::wstring& r : t.modeResolutions) {
        UINT32 w = 0, h = 0;
        if (swscanf_s(r.c_str(), L"%ux%u", &w, &h) == 2) {
            if (w <= 1024 && h <= 768) {
                p.degradedModeResolutions.push_back(r);
            }
        }
    }

    if (p.degradedModeResolutions.empty()) {
        for (const std::wstring& r : t.modeResolutions) {
            UINT32 w = 0, h = 0;
            if (swscanf_s(r.c_str(), L"%ux%u", &w, &h) == 2 && w <= 1280 && h <= 800) {
                p.degradedModeResolutions.push_back(r);
            }
        }
    }

    // 3) CDS_TEST 只读探测
    if (useCdsTest && t.active && t.gdiFound && !t.gdiDeviceName.empty()) {
        DEVMODEW dm;
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
        dm.dmPelsWidth = width;
        dm.dmPelsHeight = height;

        p.cdsTestPerformed = true;
        p.cdsTestResult = ChangeDisplaySettingsExW(t.gdiDeviceName.c_str(), &dm,
                                                   nullptr, CDS_TEST, nullptr);
        p.cdsTestResultName = DispHangeResultName(p.cdsTestResult);
        p.cdsTestAccepted = (p.cdsTestResult == DISP_CHANGE_SUCCESSFUL);
    }

    return p;
}

// ---------------------------------------------------------------------------
namespace {

// 在输出的模式表里找出目标分辨率对应的完整 DEVMODE（优先最高刷新率、最高色深）
bool FindFullDevMode(const std::wstring& dev, UINT32 width, UINT32 height,
                     UINT32 refreshHz, DEVMODEW* out) {
    if (dev.empty() || !out) return false;

    bool found = false;
    DEVMODEW best;
    ZeroMemory(&best, sizeof(best));

    for (int i = 0; i < 4000; ++i) {
        DEVMODEW cand;
        ZeroMemory(&cand, sizeof(cand));
        cand.dmSize = sizeof(cand);
        if (!EnumDisplaySettingsExW(dev.c_str(), (DWORD)i, &cand, 0)) break;

        if (cand.dmPelsWidth != width || cand.dmPelsHeight != height) continue;
        if (refreshHz > 0 && cand.dmDisplayFrequency != refreshHz) continue;

        if (!found) {
            best = cand;
            found = true;
            continue;
        }

        // 优先更高的刷新率，其次更高的色深
        bool better = (cand.dmDisplayFrequency > best.dmDisplayFrequency) ||
                      (cand.dmDisplayFrequency == best.dmDisplayFrequency &&
                       cand.dmBitsPerPel > best.dmBitsPerPel);
        if (better) best = cand;
    }

    if (found) {
        *out = best;
        out->dmSize = sizeof(DEVMODEW);
    }
    return found;
}

LONG TryApply(const std::wstring& dev, const DEVMODEW& dm, DWORD flags) {
    DEVMODEW copy = dm;
    copy.dmSize = sizeof(DEVMODEW);
    // 不设置 DM_POSITION：保留当前桌面位置，避免多屏布局被意外改动
    return ChangeDisplaySettingsExW(dev.c_str(), &copy, nullptr, flags, nullptr);
}

}  // namespace

LONG ApplyDisplayMode(const std::wstring& gdiDeviceName, UINT32 width, UINT32 height,
                      UINT32 refreshHz, bool permanent,
                      std::vector<ApplyAttempt>* attempts) {
    DEVMODEW full;
    ZeroMemory(&full, sizeof(full));
    bool haveFull = FindFullDevMode(gdiDeviceName, width, height, refreshHz, &full);

    if (haveFull) {
        // 只保留我们有把握的字段；dmPosition / dmDisplayFlags 不动
        full.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
        if (full.dmBitsPerPel >= 8) {
            full.dmFields |= DM_BITSPERPEL;
        }
        if (full.dmDisplayFrequency > 1) {
            full.dmFields |= DM_DISPLAYFREQUENCY;
        }
    }

    // 最小 DEVMODE（仅宽高）
    DEVMODEW minimal;
    ZeroMemory(&minimal, sizeof(minimal));
    minimal.dmSize = sizeof(minimal);
    minimal.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    minimal.dmPelsWidth = width;
    minimal.dmPelsHeight = height;
    if (refreshHz > 1) {
        minimal.dmFields |= DM_DISPLAYFREQUENCY;
        minimal.dmDisplayFrequency = refreshHz;
    }

    struct Plan {
        const DEVMODEW* dm;
        DWORD flags;
        const wchar_t* desc;
    };

    std::vector<Plan> plans;
    if (haveFull && permanent) {
        plans.push_back({ &full, CDS_UPDATEREGISTRY,
                          L"完整 DEVMODE（含色深/刷新率）+ 写入注册表" });
    }
    if (haveFull) {
        plans.push_back({ &full, 0, L"完整 DEVMODE（含色深/刷新率），不写注册表" });
    }
    plans.push_back({ &minimal, permanent ? (DWORD)CDS_UPDATEREGISTRY : 0U,
                      L"最小 DEVMODE（仅宽高）" });

    LONG lastResult = DISP_CHANGE_FAILED;
    for (const Plan& p : plans) {
        LONG rc = TryApply(gdiDeviceName, *p.dm, p.flags);
        ApplyAttempt a;
        a.description = p.desc;
        a.resultCode = rc;
        a.applied = (rc == DISP_CHANGE_SUCCESSFUL);
        if (attempts) attempts->push_back(a);

        if (a.applied) return rc;
        lastResult = rc;
    }
    return lastResult;
}

// ---------------------------------------------------------------------------
RestoreOutcome RestoreReferenceMode(const DisplaySnapshot& before, UINT32 refW, UINT32 refH) {
    RestoreOutcome r;
    r.before = before.primaryMode;

    const DisplayTarget* target = nullptr;
    for (const DisplayTarget& t : before.targets) {
        if (t.active && t.gdiPrimary) { target = &t; break; }
    }
    if (!target) {
        for (const DisplayTarget& t : before.targets) {
            if (t.active) { target = &t; break; }
        }
    }
    if (!target) {
        r.message = L"未找到活动的显示输出，无法回写";
        return r;
    }

    r.attempted = true;
    r.resultCode = ApplyDisplayMode(target->gdiDeviceName, refW, refH, 0, true, &r.attempts);
    r.resultName = DispHangeResultName(r.resultCode);
    r.succeeded = (r.resultCode == DISP_CHANGE_SUCCESSFUL);

    if (r.succeeded) {
        r.message = L"已请求恢复 " + FormatModeText(refW, refH) +
                    L"（写入注册表，请在下次快照中确认）";
        return r;
    }

    // ---- 失败：按目标输出的**真实性质**归因 ---------------------------------
    // 绝不能只用"EDID 不可读"就断言"显示链路已断开"：虚拟/间接显示输出的 EDID
    // 本来就为空，与链路毫无关系。旧写法会把"虚拟屏不支持该档位"报成"硬件断链、
    // 软件无法绕过"，恰好抵消本项目最核心的精确归因能力
    // （H2 已在 ApplyResolution 上修过同一个坑，这里曾经遗漏）。
    {
        std::wstring diag;
        int totalPaths = 0;
        std::vector<DisplayTarget> targets =
            EnumerateDisplays(refW, refH, &diag, &totalPaths, false);
        r.nature = ClassifyOutputNature(targets, target->gdiDeviceName);
    }

    r.message = L"恢复 " + FormatModeText(refW, refH) + L" 失败: " + r.resultName;
    r.message += L"\n\n目标输出：" + target->gdiDeviceName + L"（" +
                 OutputNatureName(r.nature) + L"）";

    switch (r.nature) {
        case OutputNature::DeadPhysical:
            r.message += L"\n\n该物理输出的 EDID / 监视器设备节点当前不可见，"
                         L"说明显示链路已断开。驱动拒绝在没有接收端的情况下提交非回退模式 ——"
                         L"这是硬件层面的限制，软件回写无法绕过。";
            break;
        case OutputNature::Virtual:
            r.message += L"\n\n这是虚拟/间接显示输出，它的 EDID 本来就为空，与链路无关。"
                         L"失败原因是该虚拟显示器不支持这个档位（或该档位不在其模式表内）。";
            break;
        case OutputNature::Mixed:
            r.message += L"\n\n当前是克隆（镜像）拓扑：同一个 GDI 名下同时挂着虚拟与物理目标，"
                         L"驱动只会接受两者的交集。请先解开镜像，或改选两边都支持的档位。";
            break;
        case OutputNature::HealthyPhysical:
            r.message += L"\n\n该物理输出的链路正常（EDID 与设备节点都在），"
                         L"失败属于驱动层面拒绝该档位。";
            break;
        default:
            r.message += L"\n\n未能判定该输出的性质（可能正处于重配置过程中）。";
            break;
    }
    return r;
}

// ---------------------------------------------------------------------------
namespace {

// 虚拟显示判断已上移到 display.cpp 的 IsVirtualDisplayTarget()，供多个模块共用

}  // namespace

TopologyState EvaluateTopology(UINT32 refW, UINT32 refH) {
    TopologyState st;

    std::wstring diag;
    std::vector<DisplayTarget> targets = EnumerateDisplays(refW, refH, &diag, nullptr, false);

    for (const DisplayTarget& t : targets) {
        if (!t.active) continue;
        ++st.totalActive;

        if (t.gdiPrimary) {
            st.primaryGdiDevice = t.gdiDeviceName;
            st.primaryMode = FormatModeText(t.width, t.height);
        }

        if (IsVirtualDisplayTarget(t)) {
            ++st.activeVirtualCount;
            st.activeVirtualDevices.push_back(t.gdiDeviceName);
            continue;
        }
        // "健康物理"的判据必须与 ClassifyOutputNature 一致（EDID + 设备节点都在），
        // 否则"EDID 可读但设备节点已消失"的中间态会在一处算健康、在另一处算断链。
        if (IsLinkHealthy(t)) {
            ++st.healthyPhysicalCount;
            continue;
        }

        // 无 EDID 且不属于已知虚拟适配器：可能是克隆态的虚拟屏（DisplayConfig 会把它
        // 报在源适配器上，靠适配器识别不出来），也可能是链路断开的物理屏。
        // 以"当前分辨率是否不低于参考分辨率"区分：克隆态虚拟屏处在参考分辨率，
        // 死输出则回退到远低于参考分辨率的驱动默认模式。
        //
        // area == 0 表示模式信息缺失（通常正处于重配置过程中），此时**不能**判为
        // "存在虚拟屏"——否则会虚增 activeVirtualCount，让 ensure 误以为有可用虚拟
        // 输出而给出错误结论。这种未知态一律保守地按死输出处理。
        UINT64 area = (UINT64)t.width * (UINT64)t.height;
        UINT64 refArea = (UINT64)refW * (UINT64)refH;
        if (area > 0 && area >= refArea) {
            ++st.activeVirtualCount;
            st.activeVirtualDevices.push_back(t.gdiDeviceName);
        } else {
            ++st.deadPhysicalCount;
            st.deadPhysicalDevices.push_back(t.gdiDeviceName);
        }
    }

    return st;
}

LONG ApplyCloneTopology() {
    // 注意：SDC_ALLOW_CHANGES 只能与 SDC_USE_SUPPLIED_DISPLAY_CONFIG 组合，
    // 与 SDC_TOPOLOGY_* 同用会返回 ERROR_INVALID_PARAMETER(87)。
    return SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_CLONE);
}

LONG ApplyExtendTopology() {
    return SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
}

// ---------------------------------------------------------------------------
int CountAvailableVirtualOutputsFrom(const std::vector<DisplayTarget>& allPaths,
                                     const TopologyState& ts) {
    int count = 0;

    // 信号 a) 全量路径里，虚拟适配器上存在 active 或 targetAvailable 的目标。
    // 调用方必须传入 includePlaceholderPaths=true 的那份列表：未激活但已实例化的
    // 虚拟显示器会被默认过滤器当成"无意义路径"丢掉，而那时 targetAvailable 恰恰为 true。
    for (const DisplayTarget& t : allPaths) {
        if (!IsVirtualDisplayTarget(t)) continue;
        if (t.targetAvailable || t.active) ++count;
    }

    // 信号 b) 拓扑汇总已识别出的活动虚拟输出。
    // 克隆拓扑下 DisplayConfig 会把克隆目标报在源适配器上，信号 a 可能漏检，
    // 因此这里取并集。
    if (ts.activeVirtualCount > count) {
        count = ts.activeVirtualCount;
    }

    return count;
}

int CountAvailableVirtualOutputs(UINT32 refW, UINT32 refH) {
    std::wstring diag;
    std::vector<DisplayTarget> full =
        EnumerateDisplays(refW, refH, &diag, nullptr, true);
    return CountAvailableVirtualOutputsFrom(full, EvaluateTopology(refW, refH));
}

PromoteOutcome PromoteVirtualDisplay(UINT32 width, UINT32 height, bool detachDeadOutputs) {
    PromoteOutcome out;

    std::wstring diag;
    std::vector<DisplayTarget> targets =
        EnumerateDisplays(width, height, &diag, nullptr, false);

    auto pickVirtual = [](const std::vector<DisplayTarget>& v) -> std::wstring {
        for (const DisplayTarget& t : v) {
            if (t.active && !t.gdiDeviceName.empty() && IsVirtualDisplayTarget(t)) {
                return t.gdiDeviceName;
            }
        }
        return std::wstring();
    };

    out.virtualDevice = pickVirtual(targets);

    if (out.virtualDevice.empty()) {
        // 克隆（镜像）拓扑下多个输出共用同一个 GDI 源名，且 DisplayConfig 会把克隆目标
        // 报在源适配器上，此时识别不出虚拟屏。先切成扩展拓扑，让各输出拿到独立 GDI 名，
        // 再重新识别一次。
        out.log.push_back("克隆态未能识别出虚拟屏，先切换为扩展拓扑后重试");
        LONG erc = ApplyExtendTopology();
        out.log.push_back("        SDC_TOPOLOGY_EXTEND -> " +
                          W2U8(DispHangeResultName(erc)) +
                          " (raw=" + std::to_string((long)erc) + ")");
        Sleep(1500);
        targets = EnumerateDisplays(width, height, &diag, nullptr, false);
        out.virtualDevice = pickVirtual(targets);
    }

    if (out.virtualDevice.empty()) {
        out.message = L"未发现处于活动状态的虚拟显示输出，无需提升";
        return out;
    }

    out.performed = true;
    out.log.push_back(W2U8(L"目标虚拟输出: " + out.virtualDevice));

    std::vector<ApplyAttempt> layoutLog;
    std::wstring msg;
    bool ok = ArrangeDesktopLayout(out.virtualDevice, width, height, 0, &layoutLog, &msg);
    out.log.push_back(W2U8(msg));
    for (const ApplyAttempt& a : layoutLog) {
        out.log.push_back(W2U8(a.description) + " -> " + W2U8(DispHangeResultName(a.resultCode)));
    }
    out.promoted = ok;

    // 摘除断链死输出（顺带清掉多余的空闲虚拟屏），保证桌面只由提升后的虚拟屏构成。
    //
    // 判据说明：显示器断电后监视器设备节点仍然存在（monitorDeviceNodePresent 为 true），
    // 真正的死亡判据是 EDID 读不到（edid.present 为 false）。
    //
    // M-1：摘除是不可逆的桌面改动，而本函数会被**守卫自动调用**（无人复核）。逐屏读
    // EDID 存在瞬时失败的可能（USB/DP 枚举竞争、驱动重载、SetupAPI 短暂不可用），一次
    // 误读就会把正在工作的显示器从桌面上摘掉。因此这里要求"二次确认"：初始枚举与摘除
    // 前的重新枚举都必须判定该输出无 EDID；任一环节读到 EDID 就保留，并把理由写进日志。
    // 判定结果汇总进 out.message，最终进入 actionLog / JSON，便于事后复盘。
    std::wstring detachNote;
    if (ok && detachDeadOutputs) {
        std::vector<DisplayTarget> recheck =
            EnumerateDisplays(width, height, &diag, nullptr, false);

        auto edidPresentIn = [](const std::vector<DisplayTarget>& list,
                                const std::wstring& dev) -> bool {
            for (const DisplayTarget& t : list) {
                if (!t.active || t.gdiDeviceName.empty()) continue;
                if (_wcsicmp(t.gdiDeviceName.c_str(), dev.c_str()) != 0) continue;
                return t.edid.present;
            }
            return false;   // 复核时已不在活动列表里 → 无需摘除
        };

        std::vector<std::wstring> toRemove;
        std::vector<std::wstring> keptAfterRecheck;
        for (const DisplayTarget& t : targets) {
            if (!t.active || t.gdiDeviceName.empty()) continue;
            if (_wcsicmp(t.gdiDeviceName.c_str(), out.virtualDevice.c_str()) == 0) continue;
            if (t.edid.present) continue;   // 有 EDID = 健康输出，保留
            if (edidPresentIn(recheck, t.gdiDeviceName)) {
                // 第二次枚举读到了 EDID → 第一次是瞬时失败，绝不能摘
                keptAfterRecheck.push_back(t.gdiDeviceName);
                continue;
            }

            out.log.push_back(W2U8(L"标记摘除输出 " + t.gdiDeviceName + L"（当前 " +
                                   FormatModeText(t.width, t.height) +
                                   L"，两次枚举均读不到 EDID）"));
            toRemove.push_back(t.gdiDeviceName);
        }

        for (const std::wstring& d : keptAfterRecheck) {
            out.log.push_back(W2U8(L"复核后保留输出 " + d +
                                   L"（首次 EDID 读取失败、二次可读 —— 判为瞬时失败，不摘除）"));
        }
        if (!keptAfterRecheck.empty()) {
            detachNote = L"复核拦下 " + std::to_wstring(keptAfterRecheck.size()) +
                         L" 个疑似瞬时失效的输出（未摘除）";
        }

        if (!toRemove.empty()) {
            LONG rc = DetachOutputs(toRemove);
            out.log.push_back("        DetachOutputs -> " +
                              W2U8(DispHangeResultName(rc)) +
                              " (raw=" + std::to_string((long)rc) + ")");
            const std::wstring note = L"已摘除 " + std::to_wstring(toRemove.size()) +
                                      L" 个断链输出（" + DispHangeResultName(rc) + L"）";
            detachNote = detachNote.empty() ? note : (detachNote + L"；" + note);
        }
    }

    Sleep(1500);

    // 复核结果
    std::vector<DisplayTarget> after =
        EnumerateDisplays(width, height, &diag, nullptr, false);
    int activeCount = 0;
    std::wstring primaryMode;
    std::wstring primaryDev;
    for (const DisplayTarget& t : after) {
        if (!t.active) continue;
        ++activeCount;
        if (t.gdiPrimary) {
            primaryMode = FormatModeText(t.width, t.height);
            primaryDev = t.gdiDeviceName;
        }
    }
    const std::wstring head = ok ? L"已完成虚拟屏提升；"
                                 : L"虚拟屏提升未生效（桌面布局未按预期改变）；";
    out.message = head + L"活动输出 " + std::to_wstring(activeCount) +
                  L" 个，主屏 " + primaryMode + L" (" + primaryDev + L")";
    if (!detachNote.empty()) out.message += L"；" + detachNote;
    return out;
}

// ---------------------------------------------------------------------------
LONG DetachOutputs(const std::vector<std::wstring>& gdiDeviceNames) {
    if (gdiDeviceNames.empty()) return ERROR_SUCCESS;

    std::set<std::wstring> wanted;
    for (const std::wstring& n : gdiDeviceNames) {
        if (!n.empty()) wanted.insert(UpperW(n));
    }
    if (wanted.empty()) return ERROR_SUCCESS;

    // 与 EnumerateDisplays 同理：两步查询之间拓扑可能变化，需对
    // ERROR_INSUFFICIENT_BUFFER 重取尺寸重试，避免摘除操作静默失败。
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    UINT32 numPaths = 0;
    UINT32 numModes = 0;
    LONG rc = ERROR_SUCCESS;
    bool queryOk = false;
    const int kMaxAttempts = 4;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &numPaths, &numModes);
        if (rc != ERROR_SUCCESS) break;
        paths.resize(numPaths);
        modes.resize(numModes);
        rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &numPaths,
                                paths.empty() ? nullptr : paths.data(),
                                &numModes, modes.empty() ? nullptr : modes.data(),
                                nullptr);
        if (rc == ERROR_SUCCESS) {
            paths.resize(numPaths);
            modes.resize(numModes);
            queryOk = true;
            break;
        }
        if (rc != ERROR_INSUFFICIENT_BUFFER) break;   // 非竞态类错误，重试无意义
    }
    if (!queryOk) return rc;

    std::vector<DISPLAYCONFIG_PATH_INFO> kept;
    int removed = 0;

    for (const DISPLAYCONFIG_PATH_INFO& p : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn;
        ZeroMemory(&sn, sizeof(sn));
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size = sizeof(sn);
        sn.header.adapterId = p.sourceInfo.adapterId;
        sn.header.id = p.sourceInfo.id;

        bool drop = false;
        if (DisplayConfigGetDeviceInfo(&sn.header) == ERROR_SUCCESS) {
            drop = (wanted.count(UpperW(sn.viewGdiDeviceName)) > 0);
        }

        if (drop) ++removed;
        else kept.push_back(p);
    }

    if (removed == 0) return ERROR_SUCCESS;
    if (kept.empty()) return ERROR_INVALID_PARAMETER;   // 不允许摘成空桌面

    // 只剔除路径、保留原模式数组，path 里的 modeInfoIdx 依然有效
    DWORD flags = SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES;
    return SetDisplayConfig((UINT32)kept.size(), kept.data(),
                            (UINT32)modes.size(), modes.data(), flags);
}

// ---------------------------------------------------------------------------
bool ArrangeDesktopLayout(const std::wstring& primaryGdi, UINT32 width, UINT32 height,
                          UINT32 refreshHz, std::vector<ApplyAttempt>* log,
                          std::wstring* message) {
    // 收集所有已挂到桌面的输出
    struct DispRec {
        std::wstring name;
        UINT32 w = 0;
        UINT32 h = 0;
        bool primary = false;
    };
    std::vector<DispRec> displays;

    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW dd;
        ZeroMemory(&dd, sizeof(dd));
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(nullptr, i, &dd, 0)) break;

        if (dd.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) continue;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;

        DEVMODEW dm;
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsExW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm, 0)) continue;

        DispRec r;
        r.name = dd.DeviceName;
        r.w = dm.dmPelsWidth;
        r.h = dm.dmPelsHeight;
        r.primary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
        displays.push_back(r);
    }

    if (displays.empty()) {
        if (message) *message = L"未找到已挂到桌面的显示输出";
        return false;
    }

    // 排序：目标主屏排第一，其余依次跟随
    bool foundPrimary = false;
    for (const DispRec& d : displays) {
        if (_wcsicmp(d.name.c_str(), primaryGdi.c_str()) == 0) foundPrimary = true;
    }
    if (!foundPrimary) {
        if (message) *message = L"目标输出 " + primaryGdi + L" 不在桌面配置中，无法设为主屏";
        return false;
    }

    std::vector<DispRec> ordered;
    for (const DispRec& d : displays) {
        if (_wcsicmp(d.name.c_str(), primaryGdi.c_str()) == 0) ordered.push_back(d);
    }
    for (const DispRec& d : displays) {
        if (_wcsicmp(d.name.c_str(), primaryGdi.c_str()) != 0) ordered.push_back(d);
    }

    // 从左到右重新排布，目标主屏落在 (0,0)
    LONG x = 0;
    bool targetApplied = false;
    LONG targetResult = DISP_CHANGE_SUCCESSFUL;
    int failedCount = 0;

    for (size_t i = 0; i < ordered.size(); ++i) {
        const DispRec& d = ordered[i];
        bool isTarget = (i == 0);

        DEVMODEW dm;
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
        dm.dmPosition.x = x;
        dm.dmPosition.y = 0;
        dm.dmPelsWidth = isTarget ? width : d.w;
        dm.dmPelsHeight = isTarget ? height : d.h;
        if (isTarget && refreshHz > 1) {
            dm.dmFields |= DM_DISPLAYFREQUENCY;
            dm.dmDisplayFrequency = refreshHz;
        }
        x += (LONG)dm.dmPelsWidth;

        LONG rc = ChangeDisplaySettingsExW(d.name.c_str(), &dm, nullptr,
                                           CDS_UPDATEREGISTRY, nullptr);

        ApplyAttempt a;
        a.description = U82W(W2U8(d.name) + (isTarget ? "（目标主屏）" : "") +
                             " -> " + std::to_string(dm.dmPelsWidth) + "x" +
                             std::to_string(dm.dmPelsHeight) + " @" +
                             std::to_string(dm.dmPosition.x) + ",0");
        a.resultCode = rc;
        a.applied = (rc == DISP_CHANGE_SUCCESSFUL);
        if (log) log->push_back(a);

        if (a.applied) {
            if (isTarget) targetApplied = true;
        } else {
            ++failedCount;
            if (isTarget) targetResult = rc;
        }
    }

    // 一次性提交多显示器改动
    ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);

    // 目标主屏没生效就必须如实返回 false：调用方（ensure / vdisplay / 守卫）用返回值
    // 判定"虚拟屏是否已提升为主屏"。无条件返回 true 会产出"报告成功、实际没动"的假象
    // —— 这正是本项目最忌讳的"报告与现实不一致"，也让 ensure 的退出码失真。
    if (!targetApplied) {
        if (message) {
            *message = L"重新排布桌面失败：目标主屏 " + primaryGdi + L" 返回 " +
                       DispHangeResultName(targetResult) + L"，桌面布局未按预期改变";
        }
        return false;
    }

    if (message) {
        *message = L"已重新排布桌面，目标主屏 " + primaryGdi + L" 位于原点";
        if (failedCount > 0) {
            *message += L"；另有 " + std::to_wstring(failedCount) +
                        L" 个输出未接受新位置（见尝试明细）";
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
namespace {

// 从一行 JSON 中提取字符串值： "key": "value"
bool ExtractJsonString(const std::string& text, const std::string& key, std::string* out) {
    std::string needle = "\"" + key + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) return false;
    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    size_t q1 = text.find('"', pos + 1);
    if (q1 == std::string::npos) return false;
    size_t q2 = text.find('"', q1 + 1);
    if (q2 == std::string::npos) return false;
    if (out) *out = text.substr(q1 + 1, q2 - q1 - 1);
    return true;
}

bool ExtractJsonBool(const std::string& text, const std::string& key, bool* out) {
    std::string needle = "\"" + key + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) return false;
    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    size_t p = pos + 1;
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
    if (text.compare(p, 4, "true") == 0) { if (out) *out = true; return true; }
    if (text.compare(p, 5, "false") == 0) { if (out) *out = false; return true; }
    return false;
}

}  // namespace

bool ReadPeerSummary(const std::wstring& path, PeerSummary* out) {
    if (!out) return false;
    *out = PeerSummary();   // 解析失败时不允许残留上一次的内容

    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return false;

    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    if (text.empty()) return false;

    PeerSummary parsed;
    parsed.sourcePath = path;
    parsed.rawText = text;

    ExtractJsonString(text, "timeUtc", &parsed.timeUtc);
    ExtractJsonString(text, "primaryMode", &parsed.primaryMode);
    ExtractJsonBool(text, "remoteSession", &parsed.remoteSession);
    ExtractJsonBool(text, "degraded", &parsed.degraded);

    // 只有真正解析成功才算 present。曾经的写法在解析之前就置 present = true，
    // 于是"文件读得到、但不是对端摘要"会被当成有效的对端，判定器据此走进双端分支，
    // 产出"双端比对: … 远程客户端="（空值）这种无意义证据，还会盖掉单机时间线判定。
    if (parsed.primaryMode.empty()) return false;

    parsed.present = true;
    *out = parsed;
    return true;
}

}  // namespace resprobe
