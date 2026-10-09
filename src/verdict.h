// verdict.h - 根因判定
#pragma once

#include "common.h"

namespace resprobe {

struct VerdictInput {
    const DisplaySnapshot* baseline = nullptr;
    const DisplaySnapshot* latest = nullptr;
    // 监听过程中的完整快照序列（可为空）。判定基于整条时间线的最差状态，
    // 而不是仅比较首尾快照 —— 否则“降级后又恢复”的故障会被漏报。
    const std::vector<EventRecord>* events = nullptr;
    const SessionInfo* session = nullptr;
    const std::vector<RemoteSoftwareInfo>* software = nullptr;
    const PeerSummary* peer = nullptr;
    UINT32 refWidth = 1920;
    UINT32 refHeight = 1080;
};

Verdict EvaluateVerdict(const VerdictInput& in);

}  // namespace resprobe
