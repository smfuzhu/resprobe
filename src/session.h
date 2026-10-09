// session.h - 会话环境与远程软件特征探测
#pragma once

#include "common.h"

namespace resprobe {

SessionInfo QuerySessionInfo();

// 识别远程控制 / 虚拟显示相关的适配器、监视器设备、进程与服务
std::vector<RemoteSoftwareInfo> DetectRemoteSoftware(
    const std::vector<DisplayTarget>& targets);

bool IsProcessElevated();

}  // namespace resprobe
