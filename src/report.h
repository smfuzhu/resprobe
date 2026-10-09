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
