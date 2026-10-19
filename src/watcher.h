// watcher.h - 显示相关事件的持续监听
#pragma once

#include "common.h"

namespace resprobe {

// 请求退出监听（供控制台 Ctrl+C 处理器调用）
void RequestStop();
bool StopRequested();

class Watcher {
public:
    explicit Watcher(Report& report);
    ~Watcher();

    Watcher(const Watcher&) = delete;
    Watcher& operator=(const Watcher&) = delete;

    // 进入消息循环，直到达到时长上限或收到停止请求
    int Run();

    // 供窗口过程回调
    void OnWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    bool CreateHostWindow();
    void DestroyHostWindow();
    void RegisterNotifications();
    void ScheduleSample(const std::string& trigger);
    void TakeSample(const std::string& trigger);
    void OnPowerSetting(WPARAM wp, LPARAM lp);

    Report& report_;
    HWND hwnd_ = nullptr;
    HDEVNOTIFY devNotify_ = nullptr;
    HPOWERNOTIFY powerNotify_ = nullptr;
    bool wtsRegistered_ = false;

    DisplayPowerState powerState_ = DisplayPowerState::Unknown;
    DisplaySnapshot last_;
    bool hasLast_ = false;

    bool pending_ = false;
    std::string pendingTrigger_;
    UINT64 pendingAt_ = 0;
    UINT64 pendingFirstMs_ = 0;   // 本轮事件合并窗口的起点
    UINT64 startMs_ = 0;
    UINT64 lastHeartbeatMs_ = 0;
    bool limitWarned_ = false;
    UINT64 lastGuardMs_ = 0;      // 上次守卫动作时间，用于冷却
    int guardActionCount_ = 0;

    // 精简事件日志（OUT\events.log）路径；空串表示本次运行不写
    std::wstring eventLogPath_;
};

}  // namespace resprobe
