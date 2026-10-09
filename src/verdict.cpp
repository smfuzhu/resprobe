// verdict.cpp - 根因判定
#include "verdict.h"
#include "display.h"   // IsVirtualDisplayTarget：区分物理输出与虚拟/间接显示输出
#include "util.h"

#include <set>

namespace resprobe {

namespace {

std::string TargetKey(const DisplayTarget& t) {
    if (!t.gdiDeviceName.empty()) return UpperAscii(W2U8(t.gdiDeviceName));
    char buf[64];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "T:%08X%08X:%u",
                (unsigned)t.adapterLuid.HighPart, (unsigned)t.adapterLuid.LowPart,
                (unsigned)t.targetId);
    return std::string(buf);
}

bool IsDegradedResolution(UINT32 w, UINT32 h) {
    return (w > 0 && h > 0 && w <= 1024 && h <= 768);
}

UINT64 Area(UINT32 w, UINT32 h) {
    return (UINT64)w * (UINT64)h;
}

bool ParseMode(const std::string& s, UINT32* w, UINT32* h) {
    unsigned a = 0, b = 0;
    if (sscanf_s(s.c_str(), "%ux%u", &a, &b) == 2 && a > 0 && b > 0) {
        if (w) *w = a;
        if (h) *h = b;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
struct Signals {
    bool edidLost = false;
    bool resolutionDrop = false;
    bool targetDisappeared = false;
    bool referenceLost = false;
    bool virtualDisplayActive = false;
    bool remoteSession = false;
    bool anyActiveEdidAbsentEver = false;
    bool preferredCollapsed = false;
    bool genericFallbackListSeen = false;

    UINT32 baselineW = 0, baselineH = 0;
    UINT32 lastW = 0, lastH = 0;
    UINT32 worstW = 0, worstH = 0;
    int dropTransitions = 0;
    bool recovered = false;
    bool haveAnyMode = false;

    bool baselineDegraded() const { return IsDegradedResolution(baselineW, baselineH); }
    bool worstDegraded() const { return IsDegradedResolution(worstW, worstH); }
    bool lastDegraded() const { return IsDegradedResolution(lastW, lastH); }

    std::vector<std::string> details;
};

void AddDetail(Signals& s, const std::string& text) {
    for (const std::string& e : s.details) {
        if (e == text) return;
    }
    if (s.details.size() < 40) s.details.push_back(text);
}

// 比较相邻两个快照
void ComparePair(const DisplaySnapshot& base, const DisplaySnapshot& last,
                 UINT32 refW, UINT32 refH, Signals& s) {
    std::map<std::string, const DisplayTarget*> baseMap;
    for (const DisplayTarget& t : base.targets) baseMap[TargetKey(t)] = &t;

    std::map<std::string, const DisplayTarget*> lastMap;
    for (const DisplayTarget& t : last.targets) lastMap[TargetKey(t)] = &t;

    for (const auto& kv : baseMap) {
        const DisplayTarget& b = *kv.second;
        auto it = lastMap.find(kv.first);
        std::wstring name = b.gdiDeviceName.empty() ? U82W(kv.first) : b.gdiDeviceName;

        if (it == lastMap.end()) {
            if (b.active) {
                s.targetDisappeared = true;
                AddDetail(s, W2U8(L"输出 " + name + L" 从拓扑中消失（原 " +
                                  FormatModeText(b.width, b.height) + L"）"));
                if (b.edid.present || b.monitorDeviceNodePresent) {
                    s.edidLost = true;
                    AddDetail(s, W2U8(L"输出 " + name + L" 消失前持有有效 EDID/设备节点"));
                }
            }
            continue;
        }

        const DisplayTarget& a = *it->second;

        if (b.monitorDeviceNodePresent && !a.monitorDeviceNodePresent) {
            s.edidLost = true;
            AddDetail(s, W2U8(L"输出 " + name + L" 的监视器设备节点消失（链路断开）"));
        }
        if (b.edid.present && !a.edid.present) {
            s.edidLost = true;
            AddDetail(s, W2U8(L"输出 " + name + L" 的 EDID 由可读变为不可读"));
        }
        if (b.active && a.active && Area(a.width, a.height) < Area(b.width, b.height) &&
            a.width > 0 && a.height > 0) {
            s.resolutionDrop = true;
            ++s.dropTransitions;
            AddDetail(s, W2U8(L"输出 " + name + L" 分辨率下降：" +
                              FormatModeText(b.width, b.height) + L" -> " +
                              FormatModeText(a.width, a.height)));
        }
        if (b.referenceModeAvailable && !a.referenceModeAvailable) {
            s.referenceLost = true;
            AddDetail(s, W2U8(L"输出 " + name + L" 的模式表丢失参考分辨率 " +
                              FormatModeText(refW, refH)));
        }
        if (b.active && a.active && b.hasPreferred && a.hasPreferred &&
            Area(a.preferredWidth, a.preferredHeight) <
                Area(b.preferredWidth, b.preferredHeight)) {
            s.preferredCollapsed = true;
            AddDetail(s, W2U8(L"输出 " + name + L" 的驱动首选模式降级：" +
                              FormatModeText(b.preferredWidth, b.preferredHeight) + L" -> " +
                              FormatModeText(a.preferredWidth, a.preferredHeight)));
        }
        if (b.active && !a.modeListIsGenericFallback && a.modeListIsGenericFallback) {
            s.genericFallbackListSeen = true;
            AddDetail(s, W2U8(L"输出 " + name + L" 的模式表已退化为通用回退列表（无 EDID 支撑），"
                              L"即使其中仍列有 1920x1080 也不代表显示器支持"));
        }
        if (a.active && (!a.edid.present || !a.monitorDeviceNodePresent)) {
            s.anyActiveEdidAbsentEver = true;
        }
        if (a.active && (a.gdiMirroringDriver ||
                         a.outputTechnology == 0x80000001 ||
                         a.outputTechnology == 0x80000002)) {
            s.virtualDisplayActive = true;
        }
    }
}

Signals Analyze(const VerdictInput& in) {
    Signals s;

    // 构建时间线：基线 + 各事件快照 + 最终状态（去重，保持顺序）
    std::vector<const DisplaySnapshot*> timeline;
    std::set<const DisplaySnapshot*> seen;
    auto push = [&](const DisplaySnapshot* p) {
        if (!p) return;
        if (seen.insert(p).second) timeline.push_back(p);
    };
    push(in.baseline);
    if (in.events) {
        for (const EventRecord& e : *in.events) push(&e.snapshot);
    }
    push(in.latest);

    for (size_t i = 1; i < timeline.size(); ++i) {
        ComparePair(*timeline[i - 1], *timeline[i], in.refWidth, in.refHeight, s);
    }

    // 当前状态证据：即使没有捕获到变化过程，也要能说清“现在为什么异常”。
    // 注意必须排除虚拟/间接显示输出 —— 它们本来就没有 EDID、模式表也是驱动合成的，
    // 把这些当异常证据会把“一切正常 + 有虚拟屏兜底”误报成故障。
    if (in.latest) {
        for (const DisplayTarget& t : in.latest->targets) {
            if (!t.active) continue;
            if (IsVirtualDisplayTarget(t)) continue;
            std::wstring name = t.gdiDeviceName.empty() ? L"(未知输出)" : t.gdiDeviceName;

            // 只在输出确实低于参考分辨率时才产出“异常证据”。
            // 克隆（镜像）拓扑下虚拟屏会被报在源适配器上，靠适配器识别不出来；
            // 但它的当前分辨率就是参考值，本身没有任何异常需要解释。
            {
                UINT64 area = (UINT64)t.width * (UINT64)t.height;
                UINT64 refArea = (UINT64)in.refWidth * (UINT64)in.refHeight;
                if (area == 0 || area >= refArea) continue;
            }

            if (!t.edid.present) {
                const wchar_t* why = t.monitorDevicePath.empty()
                                         ? L"未报告监视器设备路径"
                                         : L"监视器设备节点不存在";
                AddDetail(s, W2U8(L"当前输出 " + name + L" 的 EDID 不可读（" + why + L"）"));
            }
            if (t.modeCount > 0 && !t.edid.present) {
                AddDetail(s, W2U8(L"当前输出 " + name + L" 的模式表只剩 " +
                                  std::to_wstring(t.modeCount) +
                                  L" 项的通用回退列表，不代表显示器的真实能力"));
            }
            if (t.hasPreferred && !t.edid.present && t.preferredWidth > 0) {
                AddDetail(s, W2U8(L"当前输出 " + name + L" 的驱动首选模式已降级为 " +
                                  FormatModeText(t.preferredWidth, t.preferredHeight)));
            }
        }
    }

    // 主输出分辨率轨迹
    bool first = true;
    for (const DisplaySnapshot* p : timeline) {
        UINT32 w = 0, h = 0;
        if (!ParseMode(W2U8(p->primaryMode), &w, &h)) continue;

        if (first) {
            s.baselineW = w;
            s.baselineH = h;
            s.worstW = w;
            s.worstH = h;
            first = false;
        }
        if (Area(w, h) < Area(s.worstW, s.worstH)) {
            s.worstW = w;
            s.worstH = h;
        }
        s.lastW = w;
        s.lastH = h;
        s.haveAnyMode = true;
    }

    if (s.haveAnyMode && Area(s.lastW, s.lastH) >= Area(s.baselineW, s.baselineH)) {
        s.recovered = (Area(s.worstW, s.worstH) < Area(s.baselineW, s.baselineH));
    }

    if (in.session) {
        s.remoteSession = in.session->remoteSession || in.session->protocolType != 0;
    }

    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
Verdict EvaluateVerdict(const VerdictInput& in) {
    Verdict v;

    if (!in.baseline || !in.latest) {
        v.code = "NO_DATA";
        v.summary = "缺少快照数据，无法判定";
        return v;
    }

    Signals s = Analyze(in);

    for (const std::string& d : s.details) v.reasons.push_back(d);

    std::string hostMode = W2U8(in.latest->primaryMode);
    std::string worstMode = s.haveAnyMode
                                ? W2U8(FormatModeText(s.worstW, s.worstH))
                                : std::string("n/a");
    std::string baseMode = s.haveAnyMode
                               ? W2U8(FormatModeText(s.baselineW, s.baselineH))
                               : std::string("n/a");

    if (s.remoteSession && in.session) {
        v.reasons.push_back("当前处于远程会话（协议=" + W2U8(in.session->protocolName) + "）");
    }
    if (s.virtualDisplayActive) {
        v.reasons.push_back("检测到镜像/间接显示驱动承载着活动输出");
    }
    if (s.recovered) {
        v.reasons.push_back("监听结束时主输出已恢复到 " + hostMode + "（故障为暂时性）");
    }

    auto mentionRecovery = [&](std::string& text) {
        if (s.recovered) {
            text += "。监听结束时已恢复为 " + hostMode + "，但期间确实发生过降级";
        }
    };

    // ---------------- 双端比对优先 ----------------
    bool peerCompared = false;
    if (in.peer && in.peer->present) {
        UINT32 hw = 0, hh = 0, pw = 0, ph = 0;
        bool hostOk = ParseMode(hostMode, &hw, &hh);
        bool peerOk = ParseMode(in.peer->primaryMode, &pw, &ph);
        bool peerDegraded = in.peer->degraded || (peerOk && IsDegradedResolution(pw, ph));
        bool hostDegraded = s.lastDegraded() || s.worstDegraded();

        v.reasons.push_back("双端比对: 主机=" + hostMode + "（历史最差 " + worstMode +
                            "）, 远程客户端=" + in.peer->primaryMode);
        peerCompared = true;

        if (hostOk && peerOk && hostMode == in.peer->primaryMode) {
            if (hostDegraded) {
                v.code = s.edidLost ? "LINK_LEVEL_FALLBACK" : "LINK_LEVEL_FALLBACK_SUSPECTED";
                v.blame = "link";
                v.confidence = s.edidLost ? "high" : "medium";
                v.summary = s.edidLost
                                ? "双端分辨率一致且均降级，且主机侧观测到 EDID/设备节点丢失 —— 根因在主机显示链路（显示器关闭导致链路断开，系统回退到默认模式）"
                                : "双端分辨率一致且均降级，但未直接观测到 EDID 丢失 —— 高度怀疑主机显示链路回退";
            } else {
                v.code = "CONSISTENT";
                v.blame = "none";
                v.confidence = "high";
                v.summary = "双端分辨率一致且为正常分辨率，未复现异常";
            }
        } else if (hostOk && peerOk && hostMode != in.peer->primaryMode) {
            if (hostDegraded && !peerDegraded) {
                v.code = "REMOTE_TAKEOVER_NORMAL";
                v.blame = "remote-software";
                v.confidence = "medium";
                v.summary = "主机分辨率降级但远程客户端保持正常分辨率 —— 远程软件以虚拟显示驱动接管了分辨率，属预期行为";
            } else if (!hostDegraded && peerDegraded) {
                v.code = "REMOTE_SOFTWARE_FAULT";
                v.blame = "remote-software";
                v.confidence = "high";
                v.summary = "主机分辨率正常但远程客户端降级 —— 故障在远程软件的虚拟显示驱动或采集/编码链路";
            } else {
                v.code = "REMOTE_MISMATCH";
                v.blame = "remote-software";
                v.confidence = "medium";
                v.summary = "双端分辨率不一致（主机 " + hostMode + " / 客户端 " +
                            in.peer->primaryMode + "），需结合会话与远程软件配置进一步定位";
            }
        }
    }

    // ---------------- 单机时间线判定 ----------------
    if (!peerCompared || v.code == "UNDETERMINED") {
        if (s.edidLost && (s.resolutionDrop || s.targetDisappeared || s.worstDegraded())) {
            v.code = "LINK_LEVEL_FALLBACK";
            v.blame = "link";
            v.confidence = "high";
            v.summary = "观测到 EDID/监视器设备节点丢失并伴随分辨率下降 —— 根因在主机显示链路："
                        "显示器关闭后链路断开，系统读不到 EDID，于是回退到默认模式（常见为 1024x768）";
            if (s.preferredCollapsed) {
                v.summary += "；驱动首选模式同步降级、模式表退化为通用回退列表，"
                             "与“链路断开导致 EDID 丢失”完全吻合";
            }
            if (s.remoteSession) {
                v.summary += "。远端看到的是主机的回退分辨率，远程软件只是跟随，不是诱因";
            }
            mentionRecovery(v.summary);
        } else if (s.resolutionDrop && s.targetDisappeared) {
            v.code = "LINK_LEVEL_FALLBACK_SUSPECTED";
            v.blame = "link";
            v.confidence = "medium";
            v.summary = "显示输出在分辨率下降的同时从拓扑中消失 —— 高度怀疑主机显示链路回退";
            mentionRecovery(v.summary);
        } else if (s.resolutionDrop && s.worstDegraded() && s.virtualDisplayActive) {
            v.code = "REMOTE_TAKEOVER_NORMAL";
            v.blame = "remote-software";
            v.confidence = "medium";
            v.summary = "降级发生在虚拟/间接显示驱动的输出上 —— 属于远程软件的虚拟分辨率接管";
            mentionRecovery(v.summary);
        } else if (s.resolutionDrop && s.worstDegraded() &&
                   (s.baselineDegraded() == false)) {
            v.code = "RESOLUTION_FALLBACK_NO_EDID_EVIDENCE";
            v.blame = "unknown";
            v.confidence = "medium";
            v.summary = "分辨率由 " + baseMode + " 降级到默认回退级别 " + worstMode +
                        "（共 " + std::to_string(s.dropTransitions) + " 次），"
                        "但本次未捕获到 EDID/设备节点层面的证据";
            if (s.remoteSession) {
                v.summary += "；期间处于远程会话，建议用 verify --peer 做双端比对";
            }
            mentionRecovery(v.summary);
        } else if (s.resolutionDrop) {
            v.code = "DISPLAY_MODE_CHANGED";
            v.blame = "unknown";
            v.confidence = "low";
            v.summary = "发生 " + std::to_string(s.dropTransitions) +
                        " 次分辨率下降（最差 " + worstMode + "），但未落入默认回退模式，"
                        "也缺少 EDID 层面的确证证据";
            mentionRecovery(v.summary);
        } else if (s.edidLost) {
            v.code = "EDID_UNSTABLE_LINK";
            v.blame = "link";
            v.confidence = "medium";
            v.summary = "观测到 EDID/监视器设备节点丢失，但分辨率尚未下降 —— 链路不稳定，异常随时可能发生";
        } else if (s.referenceLost) {
            v.code = "REFERENCE_MODE_UNAVAILABLE";
            v.blame = "link";
            v.confidence = "medium";
            v.summary = "参考分辨率已从模式表中消失 —— 驱动/链路上已无该模式可选";
        } else if (s.baselineDegraded() && s.anyActiveEdidAbsentEver) {
            v.code = "LINK_LEVEL_FALLBACK";
            v.blame = "link";
            v.confidence = "medium";
            v.summary = "当前主输出已处于降级分辨率 " + baseMode +
                        "，且活动输出的 EDID 或监视器设备节点不可见 —— 状态与"
                        "“显示器关闭导致链路回退”一致（本次未观测到变化过程）";
        } else if (s.virtualDisplayActive && s.baselineDegraded()) {
            v.code = "REMOTE_TAKEOVER_NORMAL";
            v.blame = "remote-software";
            v.confidence = "low";
            v.summary = "当前分辨率降级且由虚拟/间接显示驱动承载 —— 更像远程软件的虚拟分辨率接管";
        } else {
            v.code = "NOT_REPRODUCED";
            v.blame = "none";
            v.confidence = "medium";
            v.summary = "监听期间未观测到分辨率或 EDID 异常";
        }
    }

    // ---------------- 建议 ----------------
    if (v.code == "LINK_LEVEL_FALLBACK" || v.code == "LINK_LEVEL_FALLBACK_SUSPECTED" ||
        v.code == "EDID_UNSTABLE_LINK" || v.code == "REFERENCE_MODE_UNAVAILABLE") {
        v.recommendations.push_back("改用 HDMI/DVI 连接，或在显卡与显示器之间串接 EDID 模拟器/保持器，避免显示器断电后链路断开");
        v.recommendations.push_back("DisplayPort 在显示器省电时会断开 AUX 通道导致 EDID 丢失，优先排查线材与显示器省电设置");
        v.recommendations.push_back("更新显卡驱动，并在显卡控制面板中固定输出模式");
        v.recommendations.push_back("需要自动恢复时，可用 resprobe restore --yes 在事件后回写分辨率（需管理员）");
    } else if (v.code == "RESOLUTION_FALLBACK_NO_EDID_EVIDENCE") {
        v.recommendations.push_back("延长监听时长并复现，尽量捕获 EDID/设备节点丢失的直接证据");
        v.recommendations.push_back("若复现时正在使用远程软件，请在远程客户端机器同时运行本工具并用 verify --peer 比对");
        v.recommendations.push_back("物理关闭显示器（按显示器电源键）通常比系统省电关屏更容易触发链路断开，建议用这种方式复现");
    } else if (v.code == "REMOTE_SOFTWARE_FAULT" || v.code == "REMOTE_MISMATCH") {
        v.recommendations.push_back("在被控端远程软件设置中锁定分辨率，或关闭“自适应分辨率/虚拟显示器”功能");
        v.recommendations.push_back("对比 ToDesk / 向日葵 / RDP 下的表现，定位具体软件");
        v.recommendations.push_back("检查远程软件的虚拟显示驱动（如 OrayIddDriver）版本，必要时重装或升级");
    } else if (v.code == "REMOTE_TAKEOVER_NORMAL") {
        v.recommendations.push_back("这是远程软件的预期行为；若希望主机保持 1920x1080，应先解决主机侧 EDID 丢失问题");
    } else if (v.code == "DISPLAY_MODE_CHANGED") {
        v.recommendations.push_back("延长监听时长并复现，以捕获 EDID 层面的直接证据");
        v.recommendations.push_back("在被控端与客户端同时运行本工具，用 verify --peer 做双端比对");
    } else if (v.code == "NOT_REPRODUCED") {
        v.recommendations.push_back("请在监听运行期间复现问题（物理关闭显示器，或让远程软件连接/断开）");
    }

    if (v.reasons.empty()) v.reasons.push_back("无异常信号");
    return v;
}

}  // namespace resprobe
