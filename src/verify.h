// verify.h - 模式可用性验证、回写与双端比对
#pragma once

#include "common.h"
// OutputNature / ClassifyOutputNature / IsLinkHealthy：回写失败的归因、拓扑"健康物理"
// 计数、以及摘除死输出的白名单判断，必须与 display 层共用同一套判据。
#include "display.h"

namespace resprobe {

// 只读验证：目标分辨率是否存在于该输出的模式表中，以及驱动是否接受该模式。
// 注意：CDS_TEST 通过只说明该模式在列表内合法，并不保证当前链路实际可应用。
ModeProbe ProbeModeAvailability(const DisplayTarget& t, UINT32 width, UINT32 height,
                                bool useCdsTest);

// 一次回写尝试
struct ApplyAttempt {
    std::wstring description;
    LONG resultCode = 0;
    bool applied = false;
};

// 应用显示模式。会依次尝试多种 DEVMODE 组合，返回首个成功的返回码。
// 链路已断开（无 EDID / 无监视器设备节点）时，驱动通常会拒绝，此时返回最后一次失败码。
LONG ApplyDisplayMode(const std::wstring& gdiDeviceName, UINT32 width, UINT32 height,
                      UINT32 refreshHz, bool permanent,
                      std::vector<ApplyAttempt>* attempts = nullptr);

// 回写验证：尝试把主输出恢复到参考分辨率，返回执行结果描述
struct RestoreOutcome {
    bool attempted = false;
    bool succeeded = false;
    LONG resultCode = 0;
    std::wstring resultName;
    std::wstring before;
    std::wstring after;
    std::wstring message;
    std::vector<ApplyAttempt> attempts;
    // 目标输出的性质。失败原因的归因**只能**基于它：虚拟屏的 EDID 天然为空，
    // 若看到"没有 EDID"就断言"链路已断开"，会把"虚拟屏不支持该档位"误报成
    // 硬件断链（H2 在 ApplyResolution 上修过同一个坑，这里必须同样处理）。
    OutputNature nature = OutputNature::Unknown;
};
RestoreOutcome RestoreReferenceMode(const DisplaySnapshot& before, UINT32 refW, UINT32 refH);

// 多显示器布局：把 primaryGdi 放到桌面原点 (0,0) 并设为指定分辨率，其余输出依次右移。
// 用于虚拟显示器出现后，让远程软件捕捉到 1920x1080 的主画面。
bool ArrangeDesktopLayout(const std::wstring& primaryGdi, UINT32 width, UINT32 height,
                          UINT32 refreshHz, std::vector<ApplyAttempt>* log,
                          std::wstring* message);

// 把若干输出从桌面配置中摘除（用于链路已断、只剩回退分辨率的死输出）。
//
// 注意：ChangeDisplaySettingsEx(dev, NULL, ..., CDS_UPDATEREGISTRY, ...) 虽然会返回
// DISP_CHANGE_SUCCESSFUL，但实测并不会真正摘除输出，因此这里改用受支持的
// SetDisplayConfig + SDC_USE_SUPPLIED_DISPLAY_CONFIG，从活动配置里剔除对应路径。
// 至少会保留一个输出，避免摘成空桌面。
LONG DetachOutputs(const std::vector<std::wstring>& gdiDeviceNames);

// 把活动中的虚拟显示输出提升为参考分辨率主屏，并把断链死输出从桌面摘除。
// 用于物理显示器断电后，让桌面回到 1920x1080 且远程软件只看到这一块屏。
struct PromoteOutcome {
    bool performed = false;        // 是否真正执行了动作
    bool promoted = false;         // 虚拟屏是否成功设为主屏并达到目标分辨率
    std::wstring virtualDevice;
    std::wstring message;
    std::vector<std::string> log;
};

PromoteOutcome PromoteVirtualDisplay(UINT32 width, UINT32 height,
                                     bool detachDeadOutputs);

// 拓扑状态汇总（供 ensure 子命令做幂等决策）
struct TopologyState {
    int totalActive = 0;
    int activeVirtualCount = 0;     // 活动中的虚拟显示输出数量
    int healthyPhysicalCount = 0;   // 有 EDID 的非虚拟活动输出
    int deadPhysicalCount = 0;      // 无 EDID 的非虚拟活动输出
    std::vector<std::wstring> activeVirtualDevices;
    std::vector<std::wstring> deadPhysicalDevices;
    std::wstring primaryGdiDevice;
    std::wstring primaryMode;
};
TopologyState EvaluateTopology(UINT32 refW, UINT32 refH);

// 顶层拓扑切换
LONG ApplyCloneTopology();    // 复制（镜像）
LONG ApplyExtendTopology();   // 扩展

// 统计当前“可用”的虚拟显示输出数量（与桌面当前状态无关）。
// 语义：虚拟适配器上存在 active 或 targetAvailable 的 DisplayConfig 目标，
// 与该信号与拓扑汇总的 activeVirtualCount 取并集，以降低克隆（镜像）拓扑下的漏检风险。
// 返回 0 表示驱动没有实例化任何虚拟显示器 —— 此时物理屏一旦断电就没有兜底。
// 该判据同时被 ensure 子命令和托盘状态查询使用，集中在此避免两处逻辑漂移。
//
// 调用方若已经拿到了全量路径列表与拓扑汇总，应当用 From 版本，避免重复枚举：
// 一次 QueryDisplayConfig + SetupAPI EDID 读取是毫秒级开销，托盘右键路径上可感知。
int CountAvailableVirtualOutputsFrom(const std::vector<DisplayTarget>& allPaths,
                                     const TopologyState& ts);

// 便捷包装：自行完成两次探测。适合不想管缓存的调用方（如 ensure）。
int CountAvailableVirtualOutputs(UINT32 refW, UINT32 refH);

// 读取对端机器生成的 *.peer.json 摘要
bool ReadPeerSummary(const std::wstring& path, PeerSummary* out);

}  // namespace resprobe
