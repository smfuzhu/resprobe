// display.h - 显示输出拓扑 / 模式表 / EDID 关联探测
#pragma once

#include "common.h"

namespace resprobe {

// 枚举显示输出路径。
// 返回的列表中默认只包含“有意义”的输出：活动路径，或已连接监视器（有设备路径/EDID）的路径。
// includePlaceholderPaths=true 时返回全部路径（含未激活且未连接监视器的占位路径，
// 用于排查可被激活的虚拟/间接显示输出）。
// refW / refH 为“参考健康分辨率”，用于判断模式表中是否仍存在该模式。
// totalPathsOut 返回系统报告的路径总数。
std::vector<DisplayTarget> EnumerateDisplays(UINT32 refW, UINT32 refH,
                                             std::wstring* diagOut = nullptr,
                                             int* totalPathsOut = nullptr,
                                             bool includePlaceholderPaths = false);

// 判断某个输出是否由虚拟/间接显示适配器承载。
// 优先使用 adapterDevicePath：镜像拓扑下多个输出共用同一个 GDI 源名，
// 此时 gdiDeviceString 会串成物理适配器的描述，无法用于区分。
bool IsVirtualDisplayTarget(const DisplayTarget& t);

// 链路是否健康：EDID 与监视器设备节点**都**可见。
//
// 为什么必须集中成一个函数：这个判据出现在三处 —— ClassifyOutputNature 的分类、
// EvaluateTopology 的"健康物理"计数、watcher 守卫的死输出判断。三处各写一遍必然漂移，
// 而漂移的后果是"同一个输出在一处算健康、在另一处算断链"，直接导致摘除/提升决策矛盾。
// 只看 EDID 会在"EDID 可读但设备节点已消失"的中间态上误判为健康。
bool IsLinkHealthy(const DisplayTarget& t);

// ---------------------------------------------------------------------------
// 目标输出的性质。**失败原因的归因完全取决于它**：
//   断链物理 -> 驱动拒绝，硬件限制，软件无法绕过
//   虚拟屏   -> 该虚拟显示器不支持这个模式（EDID 天然缺失，与链路无关）
//   健康物理 -> 其它原因（模式不被驱动接受 / 带宽等）
// 不做这个区分就会出现"虚拟屏不支持 1680x1050"被报成"显示链路已断开"的误判。
// ---------------------------------------------------------------------------
enum class OutputNature {
    Unknown = 0,
    Virtual,
    HealthyPhysical,
    DeadPhysical,
    Mixed,          // 克隆（镜像）拓扑：同一 GDI 名下既有虚拟又有物理目标
};

std::wstring OutputNatureName(OutputNature n);

// 判定某个 GDI 设备对应的输出性质。
// 关键点：**不能用 gdiDeviceName 唯一定位**。克隆（镜像）拓扑下多个 DisplayConfig
// 目标共用同一个 GDI 源名，此时同一个名字下既可能挂着物理目标、也可能挂着虚拟目标。
// 所以要把所有同名的活动目标收集起来一起看，据此才敢下结论。
OutputNature ClassifyOutputNature(const std::vector<DisplayTarget>& targets,
                                  const std::wstring& gdiDevice);

// “有意义的输出”判据：活动路径，或已连接监视器（有设备路径 / EDID / 设备节点）的路径。
// 其余是"占位路径"（系统给未实例化的 IDD 预留的、mode 为 0x0 的条目）。
// 集中在此是因为该判据出现在三处：EnumerateDisplays 的过滤、paths 子命令、
// 以及托盘的路径报告。三份各写一遍必然漂移。
bool IsMeaningfulDisplayTarget(const DisplayTarget& t);

// 生成一次完整快照
DisplaySnapshot CaptureSnapshot(const std::string& trigger, UINT32 refW, UINT32 refH);

// 用**已经枚举好**的输出构建快照。
// 存在的意义是避免重复枚举：托盘状态查询这类热路径上，调用方往往已经拿到了
// 全量路径列表（includePlaceholderPaths=true），再让 CaptureSnapshot 枚举一次
// 就多出一整轮 QueryDisplayConfig + SetupAPI EDID 读取（毫秒级，右键可感知）。
// targets 必须是 EnumerateDisplays 的结果（理想情况是 includePlaceholderPaths=true
// 的那一份，它是超集）；totalPathCount 传系统报告的路径总数。
DisplaySnapshot CaptureSnapshotFrom(const std::vector<DisplayTarget>& targets,
                                    int totalPathCount,
                                    const std::string& trigger);

// 依据拓扑推断显示器电源状态（无电源事件时的兜底）
DisplayPowerState InferPowerStateFromTopology(const std::vector<DisplayTarget>& targets);

// 取当前主显示输出的模式文本
std::wstring PrimaryModeText(const DisplaySnapshot& snap);

// 差分：比较两个快照的差异
void DiffSnapshots(const DisplaySnapshot& before, const DisplaySnapshot& after,
                   std::vector<std::string>* changedFields,
                   std::vector<std::string>* transitions);

}  // namespace resprobe
