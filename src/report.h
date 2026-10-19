// report.h - 控制台输出与 JSON 报告
#pragma once

#include "common.h"

namespace resprobe {

// --- 输出路径策略 ---------------------------------------------------------
// 默认不落盘任何 JSON。只有在命令行显式给出 --out 时才写报告。
// 不带目录部分的 --out 一律落到输出目录（默认 <程序目录>\OUT）。
std::wstring ExeDirectory();
std::wstring DefaultOutDir();
std::wstring DefaultOutPath(const std::wstring& mode);
std::wstring DerivePeerPath(const std::wstring& outPath);

// 解析出最终的报告路径；返回空字符串表示"不写报告"
std::wstring ResolveOutPath(const Options& opt);
bool EnsureDirectoryExists(const std::wstring& dir);

// --- 精简版事件日志（OUT\events.log）--------------------------------------
// 与详细日志（tray.log / guard.log）分工：详细日志留"现场"（快照全文、路径列表、
// 远程软件痕迹、判定过程），events.log 只留"结论" —— 一次实际变化一行。
//
//   2026-10-19 14:23:11 #12 [电源通知(显示器状态)=ON] 显示器电源状态(电源通知): OFF -> ON | 主输出分辨率: 1024x768 -> 1920x1080
//
// 追加写，不轮转（一行几十字节，量极小）；与 RedirectStdioToLog 一样以共享
// 方式打开，允许常驻守卫与托盘两个进程同时往里写。
std::wstring DefaultEventLogPath();
bool AppendEventLogLine(const std::wstring& path, const std::string& utf8Line);

// 由事件差分结果生成一行精简日志。空串表示该事件没有可记录的变化。
std::wstring FormatEventLogLine(const EventRecord& ev);

// 摘要版：与 FormatEventLogLine 同样的行首，但内容不是逐字段罗列差分，而是
// 聚合后的"到底变了什么"。用于 events.log —— 原始差分保留在详细日志与 JSON
// 报告里，这里只留人看的结论。
//
//   before/after 为前后两份快照；ev 只用来取序号与触发原因。
//   摘要不出任何条目时返回空串，调用方应回退到 FormatEventLogLine。
std::wstring SummarizeEventLogLine(const DisplaySnapshot& before,
                                   const DisplaySnapshot& after,
                                   const EventRecord& ev);

void PrintBanner();
void PrintSnapshotConsole(const DisplaySnapshot& snap, bool verbose);
void PrintSession(const SessionInfo& si);
void PrintRemoteSoftware(const std::vector<RemoteSoftwareInfo>& list);
void PrintProbes(const std::vector<ModeProbe>& probes);
void PrintVerdict(const Verdict& v);
void PrintEventConsole(const EventRecord& ev);

bool WriteJsonReport(const Report& r, const std::wstring& path);
bool WritePeerSummary(const Report& r, const std::wstring& path);

}  // namespace resprobe
