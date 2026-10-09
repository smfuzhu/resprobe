# ResProbe 开发与维护文档

> Windows 显示器分辨率变化探测、远程桌面诊断与虚拟显示器兜底工具
> 版本 1.0.0 ｜ 最后验证：2026-10-09（第 2 轮代码审查修复后，`selftest` 全绿）

**新成员请按这条路线读**（约 40 分钟可上手）：

```
§1 项目概览（为什么做） → §11 术语表（先把词对齐）
→ §3 目录结构与模块划分（代码在哪） → §4 核心功能技术实现（怎么做的）
→ §7.5 新成员快速上手（把环境跑起来） → §13 数据模型（数据结构长什么样）
→ §6 接口与函数约定（改代码前必查） → §12 代码规范（写代码的硬约束）
→ §8 常见问题排查 / §9.7 已知限制（别踩已知坑） → §14 版本更新记录（改动历史）
```

---

## 目录

1. [项目概览](#1-项目概览)
2. [运行环境与依赖](#2-运行环境与依赖)
3. [目录结构与模块划分](#3-目录结构与模块划分)
4. [核心功能技术实现](#4-核心功能技术实现)
5. [关键配置项](#5-关键配置项)
6. [接口与函数约定](#6-接口与函数约定)
7. [部署与启动](#7-部署与启动)
8. [常见问题排查](#8-常见问题排查)
9. [后续扩展注意事项](#9-后续扩展注意事项)
10. [附录：验证方法与证据文件](#10-附录验证方法与证据文件)
11. [术语表](#11-术语表)
12. [代码规范与协作约定](#12-代码规范与协作约定)
13. [数据模型与字段说明](#13-数据模型与字段说明)
14. [版本更新记录](#14-版本更新记录)

---

## 1. 项目概览

### 1.1 要解决的问题

被控电脑**物理关闭显示器**后，桌面分辨率从 1920×1080 掉到 1024×768，此时通过 ToDesk / 向日葵 / RustDesk 远程查看，看到的也是 1024×768。显示器重新开电后分辨率自动恢复。

### 1.2 已确认的根因（本机实测，2026-10-08）

```
物理按显示器电源键
    → HDMI HPD 拉低，链路断开
    → 主机读不到 EDID（DISPLAY1/2/3 的 EDID 全部消失）
    → 监视器设备节点消失、驱动首选模式降级 1920x1080 → 1024x768
    → 模式表从 298 项退化为 32 项「通用回退列表」
    → 系统回退到默认模式 1024x768
    → 远程软件只是被动跟随主机帧缓冲，不是诱因
```

**已排除的三个误判方向**（每个都实际误导过一次排查）：

| 排除项 | 实测结论 |
|---|---|
| 系统省电关屏 | `SC_MONITORPOWER` 关屏**不会**断链，EDID 仍可读，分辨率保持 1920×1080 |
| 显示器电源通知 | 物理关屏时 `GUID_CONSOLE_DISPLAY_STATE` **一次都不触发**，只有拓扑推断能捕获 |
| 软件强行回写 | 链路断开期间 `CDS_TEST` 预检通过，但三种 DEVMODE 变体实际应用全部 `DISP_CHANGE_FAILED` |

### 1.3 解决方案总览

工具分两层：

- **诊断层**：探测拓扑 / EDID / 事件 / 会话 / 远程软件痕迹，对整条时间线做根因判定，输出结构化 JSON 报告。
- **兜底层**：借助**间接显示驱动（IDD）虚拟显示器**，在物理链路断开时把虚拟屏提升为参考分辨率主屏并摘除死输出，使桌面恒为 1920×1080。

### 1.4 目标与非目标

| | 内容 |
|---|---|
| **目标** | ① 能把"关显示器后掉到 1024×768"归因到**主机显示链路**而不是远程软件；② 链路断开期间由虚拟显示器兜底，让远程会话看到稳定的 1920×1080；③ 全程可留证（结构化 JSON + 混合编码日志），可幂等地重跑 |
| **非目标** | 不做远程桌面软件（只诊断与兜底）；不做通用多显示器布局管理器；不提供服务化常驻（当前是登录任务 + 前台进程）；不改写第三方驱动（usbmmidd 原样内置） |

### 1.5 怎么读这份文档

| 你要做的事 | 直接看 |
|---|---|
| 第一次接触项目，先把词对齐 | §11 术语表 |
| 搭环境、跑起来 | §2、§7.1、**§7.5 新成员快速上手** |
| 改代码 | §3、**§12 代码规范与协作约定**（含"改一处必须同步改哪里"） |
| 改某个结构体 / 读懂 JSON 报告 | §13 数据模型与字段说明、§6.9、§6.10 |
| 查某个函数签名或行为契约 | §6 接口与函数约定 |
| 现场排查故障 | §8 常见问题排查 |
| 知道哪些坑还没填 | §9.7 已知限制与未修复项 |
| 看某次改动做了什么 | §14 版本更新记录 |
| 判断"修好了没有" | §10.1 三步复现与验证流程、§7.6 本地回归清单 |

> **改动代码前请先读 §12.8「影响面速查」**：本项目有若干"改一处必须同步改另一处"的强耦合点，
> 漏改会在运行时（而不是编译时）暴露。

---

## 2. 运行环境与依赖

### 2.1 目标平台

| 项 | 要求 |
|---|---|
| 操作系统 | Windows 10 / 11 x64（依赖 WDDM 显示配置 API 与 `IddCx` 间接显示框架） |
| 语言标准 | C++17 |
| 编译器 | MinGW-w64 g++ 15.2.0（本机 `C:\mingw64\bin`）；代码同时兼容 MSVC 2019+ |
| 资源编译器 | `windres`（随 MinGW-w64 提供，用于图标 / manifest / 版本资源） |
| 构建工具 | `mingw32-make`（无 CMake 依赖，刻意保持零第三方构建系统） |
| 权限 | 只读探测：普通用户即可。回写 / 虚拟屏操作：**需管理员** |

### 2.2 链接的系统库

```
user32 gdi32 setupapi wtsapi32 powrprof advapi32 shell32 ole32 oleaut32
```

全部为系统自带库，**无任何第三方运行时依赖**（`-static-libgcc -static-libstdc++`）。

### 2.3 关键编译宏

```makefile
-DWIN32_LEAN_AND_MEAN   # 精简 win32 头；副作用见 §9.1
-DNOMINMAX              # 避免 windows.h 的 min/max 宏破坏 <algorithm>
-DUNICODE -D_UNICODE
-D_WIN32_WINNT=0x0A00   # QDC_ALL_PATHS / QueryDisplayConfig / SetDisplayConfig 需要
-DWINVER=0x0A00
```

### 2.4 外部组件（虚拟屏兜底层）

| 组件 | 说明 |
|---|---|
| **usbmmidd v2** | Amyuni 的 UMDF 间接显示驱动，`usbmmIdd.dll` 经 **WHQL 签名**（Microsoft Windows Hardware Compatibility Publisher） |
| 驱动包位置 | **随项目内置**：`driver\usbmmidd_v2\`（源包来自 `https://www.amyuni.com/downloads/usbmmidd_v2.zip`） |
| 离线部署 | 内置包使整个项目可离线部署。**不要**放回 `C:\Temp`：那是临时目录，随时可能被清理工具或用户删除 |
| 关键工具 | `deviceinstaller64.exe`（官方控制工具，本身未签名，非驱动） |
| 驱动实例 | `ROOT\DISPLAY\0001`，软件键 `Control\Class\{4D36E968-...}\0005` |

> 备选：向日葵自带 `OrayIddDriver`（已安装且 Started），但其虚拟显示器**必须由向日葵用户态程序按需实例化**，外部无法强制创建，因此本项目改用 usbmmidd。

---

## 3. 目录结构与模块划分

```
d:\nodejs\分辨率\
├─ src\
│   ├─ common.h       全局数据结构 + 手工声明的 GUID（不依赖 initguid/devguid）
│   ├─ util.{h,cpp}   编码转换 / 时间 / 枚举名 / JSON 转义 / 日志
│   ├─ edid.{h,cpp}   EDID 读取（SetupAPI）与解析
│   ├─ display.{h,cpp}显示拓扑枚举 / 模式表 / 快照 / 差分
│   ├─ session.{h,cpp}会话环境探测 + 远程软件特征识别
│   ├─ verify.{h,cpp} 模式验证 / 回写 / 布局重排 / 输出摘除 / 虚拟屏提升
│   ├─ verdict.{h,cpp}时间线根因判定
│   ├─ watcher.{h,cpp}隐藏顶层窗口 + 事件监听 + 自动守卫
│   ├─ report.{h,cpp} 控制台输出 + JSON 报告
│   ├─ actions.{h,cpp}托盘动作层：脚本调用 / 自启开关 / 分辨率 / 状态摘要（与 UI 解耦）
│   ├─ tray.{h,cpp}   系统托盘图标 + 右键菜单（自带隐藏宿主窗口，复用 watcher 的消息泵）
│   └─ main.cpp       CLI 解析与各子命令编排
├─ res\
│   ├─ resprobe.rc         资源脚本：图标 + manifest + 版本信息
│   ├─ resprobe.manifest   应用清单（requireAdministrator / DPI / 受支持系统）
│   ├─ resprobe.ico        多尺寸图标（生成物，已入库）
│   ├─ make-icon.ps1       图标生成器（仅改动图案时需要重跑；make icon）
│   └─ check-manifest.ps1  清单校验（构建期自动执行，见 §8.6）
├─ Makefile            构建规则（-MMD -MP 自动依赖；make 默认按此名查找，保留英文）
├─ 编译.bat            【脚本】构建入口（注入 MinGW PATH）
├─ 安装.cmd            【脚本】幂等安装：驱动包 + 设备节点 + 虚拟屏 + 拓扑
├─ 卸载.cmd            【脚本】卸载（/full 移除驱动，/purge 清文件）
├─ 一次性启动.cmd       【脚本】一次性就绪：无显示器 / 不再接回显示器
├─ 常驻守卫.cmd         【脚本】常驻守卫（默认开机启动方式）
├─ 开机自启.cmd         【脚本】开机自启注册 / 注销 / 查询
├─ 脚本编码修复.ps1     【工具】脚本编码归一化 / 纯 ASCII 校验（见 §8.6）
├─ driver\usbmmidd_v2\ 内置的虚拟显示驱动包（离线可用，见 §2.4）
├─ samples\            对端摘要样例（用于演示双端比对）
├─ OUT\                统一输出目录（日志 / 显式指定的 JSON 报告）
├─ build\              目标文件与依赖文件
├─ resprobe.exe        产物（控制台子系统）
└─ DEVELOPMENT.md       本文档
```

> ### 脚本编码模型（务必遵守，这是最容易踩的坑）
>
> 规则只有两条，但都不能破：
>
> 1. **脚本文件名可以是中文**（文件系统按 UTF-16 处理，用户手输也没问题）。
> 2. **脚本内容必须是纯 ASCII**（< 0x80），UTF-8 保存、**不带 BOM**。
>
> **为什么内容是纯 ASCII**：cmd.exe 按**当前 OEM 代码页**（本机 936）解析批处理内容，
> 并且是**按字节偏移**逐行重读文件的。一个多字节字符会让这个偏移错位，于是注释行的
> 后半截被当成新命令执行 —— 典型报错：
>
> ```
> 'l），变量才会继承到调用者。' is not recognized as an internal or external command
> ```
>
> **`chcp 65001` 救不了这个问题**：它只改变已读入字节的解释方式，改变不了「按字节偏移
> 重读」这个机制。实测在 `for` 循环块、`if` 块里都会翻车。唯一稳的做法是内容全 ASCII。
>
> **连带约束**：既然内容是纯 ASCII，就**不能 `call "配置.cmd"`** —— 这一行本身含中文，
> 又变成非 ASCII 了。所以：
>
> - 共享配置块**内联复制**在每个脚本里（搜索 `CONFIG BLOCK` 定位），改动时需同步 6 个文件；
> - 管理员检查内联在每个脚本里（`fltmc` 三行），不再有 `管理员检查.cmd`；
> - `开机自启.cmd` 需要在运行时定位另外两个脚本，改为**按文件内容里的 ASCII 标记行**
>   （`RESPROBE-ROLE:once` / `RESPROBE-ROLE:guard`）扫描发现，匹配锚定行首且跳过自身，
>   否则它自己注释里提到的那串标记就会自匹配。
>
> **禁止事项**：不要在 `.cmd` 里写中文（哪怕是注释或 `echo`）。中文说明一律放本文档。
>
> **检查工具**：
>
> ```
> powershell -NoProfile -ExecutionPolicy Bypass -File 脚本编码修复.ps1
> ```
>
> 它会对目录下所有 `.cmd` / `.bat`：去 BOM →（必要时）GBK 转 UTF-8 → 去掉多余的
> `chcp 65001` 行 → 以 UTF-8 无 BOM 写回 → **逐个报告残留的非 ASCII 字符及其行首位置**。
> 全部干净时退出码 0，发现非 ASCII 时退出码 1。改完脚本建议跑一次。

### 3.1 模块依赖关系

```
main.cpp
  ├─ display ── edid ── util
  ├─ session ── util
  ├─ verify ── display, util
  ├─ watcher ── display, verify, report, util
  ├─ verdict ── util
  └─ report ── util

common.h  ← 所有模块的公共基础（必须作为每个 TU 的第一个头文件）
```

### 3.2 各模块职责边界

| 模块 | 只做 | 不做 |
|---|---|---|
| `edid` | 读注册表 EDID、解析字节 | 不关心显示器是否激活 |
| `display` | 枚举路径、关联 EDID、快照、差分 | 不改动系统状态 |
| `session` | 查询会话、扫描进程/服务/设备特征 | 不判断因果关系 |
| `verify` | 所有**会改变系统状态**的动作集中在此 | 不做判定 |
| `verdict` | 纯函数式判定 | 不调用任何 Win32 状态修改 API |
| `watcher` | 消息循环、事件合并、守卫调度 | 不直接调 Win32 显示 API（走 verify） |
| `report` | 序列化与打印 | 不含业务逻辑 |

> **维护约定**：所有会改变系统状态的调用（`ChangeDisplaySettingsEx` / `SetDisplayConfig` / `DetachOutputs`）必须收敛在 `verify.cpp`，禁止散落到其它模块。这是保证"诊断模式绝不误改系统"的结构性约束。

---

## 4. 核心功能技术实现

### 4.1 显示拓扑探测（`display.cpp`）

**主数据源是 DisplayConfig，不是 GDI。**

```cpp
GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &numPaths, &numModes);
QueryDisplayConfig(QDC_ALL_PATHS, &numPaths, paths, &numModes, modes, nullptr);
```

用 `QDC_ALL_PATHS` 而非 `QDC_ONLY_ACTIVE_PATHS`，才能看到"已连接但未激活"的输出路径 —— 这是识别可激活虚拟显示的前提。

每条路径采集成一个 `DisplayTarget`：

| 字段组 | 来源 |
|---|---|
| `active` / `targetAvailable` / `targetStatusFlags` / `outputTechnology` | `p.flags`、`p.targetInfo.*` |
| `monitorFriendlyName` / `monitorDevicePath` / `edidManufactureId` / `edidProductCodeId` | `DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME` |
| `gdiDeviceName` | `DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME` → `viewGdiDeviceName` |
| `hasPreferred` / `preferredWidth/Height` | `DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_PREFERRED_MODE` |
| 当前分辨率 / 刷新率 | `modes[p.sourceInfo.modeInfoIdx]` 与 `modes[p.targetInfo.modeInfoIdx]` |
| GDI 适配器状态 | `EnumDisplayDevicesW(nullptr, i, ...)` 按 `DeviceName` 匹配 |
| 桌面矩形 | `EnumDisplayMonitors` + `GetMonitorInfoW` |
| 模式表 | `EnumDisplaySettingsExW` 逐索引枚举 |

**重要：`QueryDisplayConfig` 的 `modeInfoArray` 用同一个数组承载 SOURCE 与 TARGET 两类模式信息，靠 `infoType` 区分，索引由 `modeInfoIdx` 给出。**

#### 输出过滤

`QDC_ALL_PATHS` 在本机返回 **146 条路径**，其中绝大多数是占位路径。默认只保留"有意义"的输出：

```cpp
bool meaningful = t.active || !t.monitorDevicePath.empty() ||
                  t.edid.present || t.monitorDeviceNodePresent;
```

`EnumerateDisplays(..., includePlaceholderPaths = true)` 可返回全量（`paths` 子命令用）。

**模式表只对 `active` 输出枚举** —— 否则 100 条占位路径 × 数百模式会让报告爆炸。

#### 快照与差分

`CaptureSnapshot()` 产出 `DisplaySnapshot`（含 `powerState` / `powerStateInferred` / `primaryMode` / `totalPathCount` / `targets`）。

`DiffSnapshots()` 以 **GDI 设备名**（无则用 `适配器LUID:targetId`）为键做配对，产出两份人类可读证据：

- `changedFields`：机器可读的字段列表，如 `\\.\DISPLAY1.edidPresent`
- `transitions`：人类可读的迁移，如 `\\.\DISPLAY1: EDID present -> absent`

### 4.2 EDID 读取与关联（`edid.cpp`）

**读取路径**：`SetupDiGetClassDevsW(GUID_DEVCLASS_MONITOR)` → 逐设备 `SetupDiOpenDevRegKey(DIREG_DEV)` → `RegQueryValueExW(L"EDID")`。

**解析**：检查 8 字节魔数 `00 FF FF FF FF FF FF 00`，提取厂商 PNP 码、产品码、序列号、扩展块数、四个 18 字节描述符（`0xFC` 显示器名、`0xFF` 序列号、描述符 0 为首选时序）。原始字节以十六进制保留作为证据。

**关联策略（关键设计）**：

```cpp
// 1) 优先用规范化后的设备实例 ID 精确匹配
//    \\?\DISPLAY#VSC3C8E#4&1440d7a&0&UID198195#{guid}
//    →  DISPLAY\VSC3C8E\4&1440d7a&0&UID198195
// 2) 只有当 DisplayConfig 完全没给设备路径时，才退化为「厂商+产品码」匹配，
//    且要求候选唯一
```

> **为什么必须这样**：如果无条件用厂商+产品码兜底，"显示器断电导致设备节点消失"会被另一台同型号显示器顶替，**直接掩盖故障信号**。

### 4.3 事件监听（`watcher.cpp`）

#### 4.3.1 窗口类型（曾踩坑）

**必须创建普通顶层窗口，不能创建 message-only 窗口（`HWND_MESSAGE`）。** 广播消息（`WM_DISPLAYCHANGE` / `WM_DEVICECHANGE` / `WM_SETTINGCHANGE`）不会投递给 message-only 窗口。窗口创建后**不调用 `ShowWindow`** 即保持隐藏。

#### 4.3.2 监听的事件源

| 消息 | 说明 |
|---|---|
| `WM_DISPLAYCHANGE` | 分辨率/色深变化，`LOWORD(lp)`×`HIWORD(lp)` 为新分辨率 |
| `WM_DEVICECHANGE` | `DBT_DEVICEARRIVAL` / `DBT_DEVICEREMOVECOMPLETE` / `DBT_DEVNODES_CHANGED` |
| `WM_SETTINGCHANGE` | 工作区/系统设置变化 |
| `WM_POWERBROADCAST` | `PBT_POWERSETTINGCHANGE` → `GUID_CONSOLE_DISPLAY_STATE` 显示器开关 |
| `WM_WTSSESSION_CHANGE` | 远程连接 / 断开 / 锁定（需 `WTSRegisterSessionNotification`） |

注册的三个通知：

```cpp
RegisterPowerSettingNotification(hwnd_, &kGuidConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
RegisterDeviceNotificationW(hwnd_, &filter /*GUID_DEVINTERFACE_MONITOR*/, DEVICE_NOTIFY_WINDOW_HANDLE);
WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_THIS_SESSION);
```

任一注册失败都只打印提示并继续 —— 因为告警路径不能因为某个通知不可用就整体失效。

#### 4.3.3 事件合并（曾踩坑）

变化爆发时（显示器断电会让 HDMI 链路反复协商）事件密集到达。合并窗口若无上限，采样会被**无限推迟**，整个变化过程被完全漏掉。

```cpp
pendingAt_ = min(now + settleMs, pendingFirstMs_ + settleMs * 3);
```

并在消息循环退出时**强制采样一次**（`final-state`），确保终态被记录。

#### 4.3.4 消息循环

```cpp
while (!StopRequested()) {
    DWORD wait = MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);
    if (wait == WAIT_OBJECT_0) { /* PeekMessage / TranslateMessage / DispatchMessage */ }
    // 事件合并窗口到期 → TakeSample
    // 心跳周期到期 → TakeSample("heartbeat")
    // 时长上限到期 → break
}
```

`Ctrl+C` 由 `SetConsoleCtrlHandler` 置标志位，主循环检测后优雅收尾（写报告）。

### 4.4 会话与远程软件识别（`session.cpp`）

- 会话：`GetSystemMetrics(SM_REMOTESESSION)`、`ProcessIdToSessionId`、`WTSGetActiveConsoleSessionId`、`WTSQuerySessionInformationW`（用户名/工作站/域名/客户端名/协议类型/连接状态/客户端地址）。
- 远程软件特征：按 **显示适配器设备、监视器设备、显示适配器 SetupAPI 设备、进程、服务** 五个面扫描，关键词表 `kVendors[]` 覆盖 ToDesk / 向日葵(Oray) / AnyDesk / TeamViewer / RustDesk / Radmin / VNC / Parsec / DisplayLink / Indirect Display / usbmmidd 等。
- 额外判据：`DISPLAY_DEVICE_MIRRORING_DRIVER` 标志、输出技术为 `INDIRECT_WIRED/INDIRECT_VIRTUAL`、SetupAPI 枚举器为 `ROOT`（软件设备）。

> 短关键词（如 `idd`）会误命中 `Middleware`。已将其收紧为 `idddriver` / `idd driver`。**新增关键词时务必确认不会命中常见英文单词。**

### 4.5 模式可用性验证（`verify.cpp`）

```cpp
DEVMODEW dm{}; dm.dmSize = sizeof(dm);
dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
dm.dmPelsWidth = w; dm.dmPelsHeight = h;
LONG rc = ChangeDisplaySettingsExW(dev, &dm, nullptr, CDS_TEST, nullptr);
```

> **`CDS_TEST` 通过 ≠ 实际可应用。** 它只校验模式在列表内合法，不碰硬件。链路断开时 `CDS_TEST` 返回成功而实际应用返回 `DISP_CHANGE_FAILED`。所有面向用户的界面文案都必须带上这个限定。

回写 `ApplyDisplayMode()` 会依次尝试三种 DEVMODE 组合，并记录每次的返回码：

1. 完整 DEVMODE（含色深/刷新率，从模式表反查）+ 写入注册表
2. 完整 DEVMODE，不写注册表
3. 最小 DEVMODE（仅宽高）

**从不设置 `DM_POSITION`**，以免意外打乱多屏布局。

### 4.6 时间线根因判定（`verdict.cpp`）

**判定基于整条时间线的最差状态，而不是首尾快照对比。** 早期实现只比较首尾，导致"掉下去又恢复"的故障被误报为 `NOT_REPRODUCED`。

```cpp
// 去重构建时间线：基线 + 各事件快照 + 最终状态
for (i = 1; i < timeline.size(); ++i)
    ComparePair(*timeline[i-1], *timeline[i], refW, refH, s);
// 再叠加「当前状态证据」，覆盖没有捕获到变化过程的情况
```

`Signals` 聚合的关键量：`edidLost` / `resolutionDrop` / `targetDisappeared` / `referenceLost` / `preferredCollapsed` / `genericFallbackListSeen` / `virtualDisplayActive` / `remoteSession` / 主输出分辨率的 baseline/worst/last 三元组 / `dropTransitions` / `recovered`。

判定优先级：**双端比对 > 单机时间线**。

#### 判定码表

| code | 含义 | blame | 典型置信度 |
|---|---|---|---|
| `LINK_LEVEL_FALLBACK` | EDID/设备节点丢失 + 分辨率下降 | link | high |
| `LINK_LEVEL_FALLBACK_SUSPECTED` | 分辨率下降 + 输出从拓扑消失，但无 EDID 直接证据 | link | medium |
| `EDID_UNSTABLE_LINK` | EDID 丢失但分辨率尚未下降 | link | medium |
| `REFERENCE_MODE_UNAVAILABLE` | 模式表丢失参考分辨率 | link | medium |
| `RESOLUTION_FALLBACK_NO_EDID_EVIDENCE` | 降到默认回退级别，但没抓到 EDID 证据 | unknown | medium |
| `DISPLAY_MODE_CHANGED` | 分辨率下降但未落入默认回退模式 | unknown | low |
| `REMOTE_TAKEOVER_NORMAL` | 主机降级、远端正常 —— 虚拟分辨率接管属预期 | remote-software | medium |
| `REMOTE_SOFTWARE_FAULT` | 主机正常、远端降级 | remote-software | high |
| `REMOTE_MISMATCH` | 双端不一致但无法进一步定位 | remote-software | medium |
| `CONSISTENT` | 双端一致且正常 | none | high |
| `NOT_REPRODUCED` | 未观测到异常 | none | medium |
| `NO_DATA` / `UNDETERMINED` | 数据不足 | unknown | — |
| `PATHS_LISTED` | `paths` 子命令专用，仅表示已列出路径，非判定结论 | unknown | — |

### 4.7 虚拟显示器与自动守卫

#### 4.7.1 激活机制

usbmmidd 的虚拟显示器**只能通过官方工具激活**：

```bat
deviceinstaller64.exe enableidd 1     :: 每次调用添加一块，最多 4 块
deviceinstaller64.exe enableidd 0     :: 每次调用移除一块
```

**实测无效的几种尝试**（不要再重复）：

- 仅用 `devcon install` 创建设备 → 设备 `Started`，但 `targetAvailable` 全为 `false`，零虚拟显示器
- 往 `Enum\ROOT\DISPLAY\0001\Device Parameters` 写 `Parameters\Monitors` → 无效
- 往 `Control\Class\{4D36E968-...}\0005` 写 `Parameters\Monitors` → 无效
- `pnputil /restart-device` 重启设备 → 无效

分辨率表实际位置（**仅在需要改可选分辨率表时使用**）：

```
HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\WUDF\Services\usbmmIdd\Parameters\Monitors
  (默认值) = 默认显示器分辨率，例如 "1920,1080"
  0..9     = 可选分辨率表，例如 0="1024,768" 5="1920,1080"
```

> **虚拟显示器不跨重启保留。** 重启后必须重跑 `enableidd`，见 `一次性启动.cmd` / `常驻守卫.cmd`。

#### 4.7.2 两种拓扑，用途不同

| 拓扑 | 命令 | 适用 |
|---|---|---|
| **复制（镜像）** | `vdisplay --yes --mirror` | 显示器常开。桌面尺寸保持 1920×1080，本地画面无窗口乱跳 |
| **扩展 + 提升** | `vdisplay --yes --promote` | 显示器会断电。虚拟屏设为 1920×1080 主屏，死输出摘除 |

> **镜像不能用于断电场景**：克隆组取两个输出的**共同分辨率**，物理那一份一退化到 1024×768，整个 source 就被拉低，随后 Windows 还会把它拆成扩展模式。**必须先让虚拟屏到 1920×1080 再克隆**，否则桌面会被压到 1024×768。

#### 4.7.3 自动守卫（`watch --guard`）

在每次 `TakeSample` 之后评估，触发条件为**条件驱动 + 冷却窗口**（不是一次性标志）：

```cpp
bool needAction = (!alreadyOk) || deadPhysical;
   // alreadyOk   : 主屏已达到参考分辨率
   // deadPhysical: 存在 active 且 !IsLinkHealthy(t) 的非虚拟输出
if (hasVirtual && needAction && cooldownPassed /*8s*/) {
    PromoteVirtualDisplay(refW, refH, /*detachDeadOutputs=*/true);
}
```

动作序列（`PromoteVirtualDisplay`）：

1. 找出**活动中的虚拟显示输出**（按适配器描述匹配虚拟显示特征）
2. `ArrangeDesktopLayout()`：虚拟屏落 `(0,0)` 并设为参考分辨率，其余输出依次右移
   —— 目标屏那一次 `ChangeDisplaySettingsExW` 失败会返回 `false`，`promoted` 不再虚报成功
3. `DetachOutputs()`：把所有**二次确认**仍读不到 EDID 的非虚拟输出从桌面配置剔除（见 §4.7.4）
4. 复核并输出结果（`po.message` 会写明摘除了几个、以及是否有输出被复核拦下）

**判据演进（务必按当前版本来）：**

| 版本 | 死输出判据 | 问题 |
|---|---|---|
| 早期 | `!monitorDeviceNodePresent` | **错**：显示器断电后监视器设备节点**仍然存在**于注册表中，于是摘除从未触发 |
| 上一版 | `!edid.present` | 方向正确，但"只读一次 EDID"遇到瞬时失败会误摘正常显示器 |
| **当前** | `!IsLinkHealthy(t)`（= 无 EDID **或** 无设备节点）+ 摘除前**二次枚举复核** | 见 §4.7.4 |

> `IsLinkHealthy()` 是**唯一**的链路健康判据（`display.h`），`ClassifyOutputNature`、
> `EvaluateTopology` 的"健康物理"计数、本处守卫判断三处共用它。曾经三处各写一遍，
> 结果是"同一个输出在一处算健康、在另一处算断链"，直接导致摘除/提升决策自相矛盾。

#### 4.7.4 输出摘除必须用 SetDisplayConfig

```cpp
// 错误做法（返回 DISP_CHANGE_SUCCESSFUL，但输出根本没被摘除）：
ChangeDisplaySettingsExW(dev, nullptr, nullptr, CDS_UPDATEREGISTRY, nullptr);

// 正确做法：
QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, ...);          // 取当前活动路径
// 按 GDI 源名剔除目标路径，保留原模式数组（modeInfoIdx 仍有效）
SetDisplayConfig(keptPaths, modes,
                 SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
```

内置保护：`kept` 为空时返回 `ERROR_INVALID_PARAMETER`，**绝不允许摘成空桌面**。

#### 4.7.4.1 摘除前的二次确认

摘除是不可逆的桌面改动，而这条路径会被**守卫自动触发**（没有人复核）。逐屏读 EDID 存在
瞬时失败的可能（USB/DP 枚举竞争、驱动重载、SetupAPI 短暂不可用），一次误读就会把正在
工作的显示器从桌面上摘掉。因此 `PromoteVirtualDisplay()` 在动手之前会：

1. 用**初始枚举**挑出候选（`!edid.present` 且非虚拟屏）；
2. 摘除前**重新枚举一次**（`QDC_ALL_PATHS` + SetupAPI 全量重读）；
3. 只有两次都读不到 EDID 才真正摘除；第二次读到 EDID 的输出会被保留，并向
   `out.log` 写一条
   `复核后保留输出 …（首次 EDID 读取失败、二次可读 —— 判为瞬时失败，不摘除）`；
4. 结论（"已摘除 N 个断链输出（<返回码>）" / "复核拦下 N 个疑似瞬时失效的输出（未摘除）"）
   并入 `PromoteOutcome::message`，最终进入 `report.actionLog` 与 JSON 报告。

效果：把"一次误读就摘掉正常显示器"降为"需要连续两次误读"，并且事后能从日志看出复核发生过。
**不要把这次复核删掉**——它是这条自动路径唯一的安全阀。复核只增加一次枚举（毫秒级），
且只在 `ok && detachDeadOutputs` 且确有候选时才执行。

#### 4.7.5 拓扑分类的一处重要陷阱（`EvaluateTopology`）

判断"哪些输出是虚拟屏、哪些是断链死输出"看起来简单，实际有两个坑：

1. **适配器路径会串味。** 先用 `DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME` 取适配器设备路径，
   再用 `ROOT#DISPLAY` 等特征区分物理/软件适配器。注意该查询的 **`header.id` 必须传 0**
   （传 target id 会让所有适配器都返回同一个路径）。
2. **克隆（镜像）拓扑下适配器路径根本不可用。** 克隆时多个输出共用同一个 GDI 源，
   DisplayConfig 会把克隆目标报在**源适配器**上 —— 此时无论用 `gdiDeviceString` 还是
   `adapterDevicePath`，虚拟屏都长得和物理屏一模一样。

因此 `EvaluateTopology` 在"无 EDID 且不属于已知虚拟适配器"这一支上，改用
**当前分辨率是否低于参考分辨率**来区分：

```cpp
UINT64 area = (UINT64)t.width * (UINT64)t.height;
UINT64 refArea = (UINT64)refW * (UINT64)refH;
if (area > 0 && area < refArea) { ++deadPhysicalCount; }   // 回退了 -> 断链死输出
else                            { ++activeVirtualCount; }  // 仍在参考分辨率 -> 虚拟屏
```

这也解释了为什么 `ensure` 的就绪判据只看「主屏分辨率 + 有无断链死输出」，而**不**

依赖虚拟屏计数 —— 后者在克隆态下不可靠。虚拟屏计数仅用于 `ensure` 判断"要不要提示去
`enableidd`"，以及日志展示。

### 4.8 系统托盘（`tray.cpp` / `actions.cpp`）

#### 4.8.1 集成方式：托盘不侵入 watcher

托盘**不自己跑消息循环**，而是自带一个隐藏宿主窗口，交给监听现成的消息泵派发：

```cpp
// watcher.cpp 的循环（未因托盘改动一行）
MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);
while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {   // hwnd = nullptr
    TranslateMessage(&msg);
    DispatchMessageW(&msg);                               // 本线程所有窗口的消息
}
```

`PeekMessageW` 的 hwnd 传 `nullptr` 表示"取本线程**所有**窗口的消息"，所以托盘窗口的
事件同样会被取出并派发。结论：**`watcher.cpp` / `watcher.h` 一行都不用改**，
两个窗口共享同一个泵，事件管线与守卫动作照常运行。

托盘宿主必须是**顶层窗口**（不能是 `HWND_MESSAGE`）：`TrackPopupMenu` 要求 owner 能被
置为前台，否则点菜单外部不会关闭（经典坑，代码里已按标准做法补 `SetForegroundWindow`
+ 事后 `PostMessage(WM_NULL)`）。

另外两个容易漏的点：
- 监听 `RegisterWindowMessageW(L"TaskbarCreated")`，explorer 重启后要重新 `NIM_ADD`；
- 退出前必须 `NIM_DELETE`，否则留下"幽灵图标"直到鼠标划过。`WM_ENDSESSION` 里也摘一次。

#### 4.8.2 子系统：保持控制台，只隐藏窗口（方案 A）

`.cmd` 脚本依赖两件事：**stdout 可被重定向捕获** + **退出码即契约**（见 §6.5）。
把子系统改成 `-mwindows` 会同时威胁这两点，风险集中在"回归破坏已交付的脚本"。

因此 `Makefile` **不加 `-mwindows`**（保持 g++ 默认的控制台子系统），托盘启动时只做：

```cpp
HWND con = GetConsoleWindow();
if (con) ShowWindow(con, SW_HIDE);     // 隐藏而非 FreeConsole，保留日志能力
```

代价是启动瞬间可能有控制台闪现（自启动用 `start /min` 可基本消除）。
好处是零破坏，而且隐藏的控制台仍能继续输出日志，对诊断工具反而是优点。

> **若将来要改成 `-mwindows`**，必须同时满足：① `AttachConsole(ATTACH_PARENT_PROCESS)`
> 并重开 stdout/stderr；② 回归验证 `安装.cmd` 的 `[6/8] ... (ensure=0)` 这一步
> 仍然拿到正确的子进程退出码。做不到就别改。

#### 4.8.3 提升权限：manifest `requireAdministrator`

需要的动作全部要管理员：`deviceinstaller64`（驱动）、`SetDisplayConfig`（拓扑）、
`schtasks /rl highest`（自启）。脚本侧本来就用 `fltmc` 强制检查管理员，所以清单改成
`requireAdministrator` 不改变能力，只是把"API 失败"换成"干净的 UAC 提示"。

**实测确认不会破坏脚本契约**：已提升的父进程再启动已提升的子进程**不会新建进程**，
所以脚本里的 `> OUT\xxx.log 2>&1` 与 `!ERRORLEVEL!` 都照常工作。
（从**非提升**的控制台直接运行 `resprobe.exe` 会另起一个提升进程，输出与新窗口走、
退出码丢失 —— 这是预期行为，已在清单注释里写明。）

要改回免提升：把 `res/resprobe.manifest` 的 level 改成 `asInvoker` 重新编译，
代价是需要管理员的菜单项会直接失败而不是弹 UAC。

#### 4.8.4 菜单结构与阻塞处理

```
1920x1080   虚拟屏 4            <- 不可点，状态首行（带 8s 缓存）
────────────────────────
完整状态报告...  /  列出显示输出路径...
调整分辨率 ▸  （当前项打勾，来自 EnumDisplaySettingsEx 去重排序）
开机自启动 ▸  启用(初始化+守卫) / 启用(仅初始化) / 停用 / 查看状态...
虚拟显示器 ▸  安装或修复 / 卸载(保留驱动) / 彻底卸载(移除驱动)
────────────────────────
打开输出目录 / 查看最近的守卫日志 / 关于 / 退出
```

`tray` 默认 `guard = true`（与 `常驻守卫.cmd` 同源），意味着**启动托盘即可能改动桌面
拓扑**。只想把托盘当查看面板时用 `--no-guard`：那时它只提供界面，不做任何动作。

**阻塞处理**：动作层里所有"等待外部进程"的地方都走 `PumpSleep()`，它在等待期间持续
派发本线程消息（与 watcher 同一个 `MsgWaitForMultipleObjects(0,nullptr,..)` 模式），
所以跑脚本 / 装驱动期间托盘不会假死。没有引入工作线程，理由是：
- 宿主窗口是隐藏的，即使短暂阻塞也不会被系统标记"无响应"；
- 少一层线程就少一类竞态，`std::thread` 还会引入 MinGW 线程模型的不确定性。

`busy_` 标志覆盖到**弹窗之后**才清除：`MessageBoxW` 有自己的嵌套消息循环，期间用户若
又点托盘，标志能拦住叠菜单。

#### 4.8.5 托盘如何调用中文名脚本

脚本文件名是中文，而脚本内容必须纯 ASCII（§3 编码模型），所以 C++ 侧**不能硬编码
中文文件名**。约定：每个可被调用的脚本在**独占一行**写

```bat
@rem RESPROBE-ROLE:install
```

`FindScriptByRole()` 扫描 `*.cmd`，按**整行精确相等**匹配并跳过自身。
三个坑都踩过：
- 只做子串匹配会命中脚本自己注释里提到的标记（自匹配）；
- 匹配不锚定行首会命中"解释这个约定"的注释行；
- 所以最终用整行相等 + 跳过自身，两者都要。

当前角色：`once` / `guard` / `install` / `uninstall` / `autostart`。

#### 4.8.6 自检入口 `selftest`

托盘菜单**无法自动点击**，所以动作层提供一条脚本化验证入口：

```
resprobe.exe selftest
```

它把菜单依赖的每条函数都跑一遍并逐项判定，尽量选只读路径：按角色定位 5 个脚本、
用 `autostart /status`（只读）跑通脚本执行与日志捕获、查自启状态、枚举模式列表、
**把分辨率"等值写入"当前值**（不产生实际变化）、生成状态与路径报告、
以及混合编码解码的确定性校验。返回失败项数量，0 表示全部可用。
交付后若某条菜单项"点了没反应"，先跑它。

#### 4.8.7 日志是混合编码的（必须按混合方式读）

捕获到的脚本日志**天然是两种编码拼起来的**，这不是 bug 而是来源决定的：

| 来源 | 编码 |
|---|---|
| `resprobe.exe` 的输出 | **UTF-8**（它自己 `SetConsoleOutputCP(CP_UTF8)` 并写 UTF-8 字节） |
| `cmd` 回显中文路径 / 文件名（`%RP_HOME%`、`dir /b *.cmd`） | **OEM 936（GBK）** |
| `schtasks` / `pnputil` / `reg` 等系统工具的本地化输出 | **OEM 936（GBK）** |

后果与对策：

- **绝不能整体按单一编码解码。** 尤其是"先试严格 UTF-8，失败就整体退回 CP_ACP"
  这种写法：GBK 几乎接受任何字节对，所以混合内容按 ACP 解**一定"成功"**，
  结果是又把 UTF-8 那半毁掉 —— 实测出现过，表现是
  `[ensure] 活动输出` 变成 `[ensure] 娲诲姩杈撳嚭`。
- 正确做法是 `DecodeMixed()`：**逐字符**判定。判据基于
  "`0x80..0xBF` 在 UTF-8 里只能是续字节、永远不能作为字符起始"这一强约束，
  再叠加过长编码 / 代理区 / 码点上限校验。纯 UTF-8 与纯 GBK 都走整段严格解码
  这条安全路径，只有确认混合才进入启发式。
- 托盘所有展示日志的地方（安装结果弹窗、**查看最近的日志**）都走
  `ReadLogTail()` → `DecodeBestEffort(..., CP_UTF8)` → `DecodeMixed()`。
  「打开输出目录」仍可用，但**直接用记事本打开 `.log` 会有一半是乱码**，
  这是文件本身的性质，不是程序的问题。

#### 4.8.8 单实例保护（跨进程互斥）

守卫动作会改显示拓扑，而**拓扑是机器级资源**。`kGuardCooldownMs` 只是进程内冷却，
拦不住"登录任务里的常驻守卫 + 用户双击启动的托盘"这种双进程场景。

用**两把独立的锁**，因为两者语义不同：

| 锁 | 名称 | 拿不到时的行为 | 理由 |
|---|---|---|---|
| 守卫锁 | `Global\ResProbe.Guard` | **放弃守卫**，只做界面 | 登录任务的常驻守卫**没有界面**，用户看不到也操作不了它，所以不能因此拒绝用户启动托盘；但两个守卫必须互斥 |
| 托盘锁 | `Global\ResProbe.Tray` | **直接退出**并弹窗 | 两个托盘会出现两个图标 |

实现要点：
- 先试 `Global\`（跨会话互斥，RDP 多会话下同一个用户可能有两个会话在跑）；
  需要 `SeCreateGlobalPrivilege`，提权进程一般都有，失败则退回 `Local\`。
- 互斥量句柄必须**在整个进程生命周期内保持有效**（`g_locks` 静态持有）；
  一旦 `CloseHandle` 就等于释放了锁。
- `IsGuardOwnedByOther()` 在**尚未取锁**时用 `OpenMutexW` 只查询存在性，
  **绝不索取所有权** —— 否则 `selftest` / `ensure` 这类只读路径会把真正的守卫顶掉。
- 状态必须让用户看得见：菜单首行显示 `守卫:本实例 / 守卫:其它实例 / 守卫:关`，
  状态报告里也有一行。否则"我明明启动了程序，为什么没在守卫"无法解释。

#### 4.8.9 失败归因必须按目标输出的性质（不再见到无 EDID 就断言断链）

`ApplyResolution` 失败时**不能**只用"EDID 不可读"就断言"显示链路已断开"：
虚拟/间接显示输出的 EDID **本来就为空**，与链路毫无关系。旧写法会把
"虚拟屏不支持某个档位"报成"链路断了，硬件限制，软件无法绕过"，
把排查引向完全错误的方向 —— 恰好抵消了本项目最核心的精确归因能力。

现在先做性质判定（`ClassifyOutputNature`），再按性质给原因：

| 性质 | 归因 | 处置方向 |
|---|---|---|
| `DeadPhysical` | 链路已断，驱动拒绝在无接收端时提交非回退模式 | 硬件限制，只能靠虚拟屏兜底 |
| `Virtual` | 该虚拟显示器不支持这个模式 | 从它支持的档位里选 |
| `Mixed`（克隆） | 同一 GDI 名下虚拟+物理混合，驱动只取交集 | 先解开镜像，或选两边都支持的档位 |
| `HealthyPhysical` | 链路正常，属驱动层面拒绝 | 换档位或查驱动 |

判定的关键点：**不能用 `gdiDeviceName` 唯一定位输出**。克隆（镜像）拓扑下多个
DisplayConfig 目标共用同一个 GDI 源名，同名下可能同时挂着物理与虚拟目标，
所以要把所有同名活动目标收集起来一起看。判定"链路健康"要求
**EDID 与监视器设备节点都在**（只看 EDID 会在"EDID 可读但设备节点已消失"的中间态误判）。

失败消息里还会列出该输出当前支持的档位，便于直接改选。

**这条规则对三处失败路径一致适用**（曾经只有第一处修好了，另外两处遗漏）：

| 失败路径 | 归因方式 |
|---|---|
| `actions.cpp` `ApplyResolution()`（托盘改分辨率 / `selftest` 第 5 步） | `ClassifyOutputNature` + 四类文案 |
| `verify.cpp` `RestoreReferenceMode()`（`restore --yes`） | 同左，结果存在 `RestoreOutcome::nature` |
| `verify.cpp` `PromoteVirtualDisplay()`（守卫 / `ensure promote`） | 走 `ArrangeDesktopLayout` 的返回码与 §4.7.4.1 的复核结论 |

判定的关键点：**不能用 `gdiDeviceName` 唯一定位输出**。克隆（镜像）拓扑下多个
DisplayConfig 目标共用同一个 GDI 源名，同名下可能同时挂着物理与虚拟目标，
所以要把所有同名活动目标收集起来一起看。判定"链路健康"统一走
`IsLinkHealthy()` = **EDID 与监视器设备节点都在**（只看 EDID 会在"EDID 可读但设备节点
已消失"的中间态误判）。

> 代码位置：`OutputNature` / `OutputNatureName()` / `ClassifyOutputNature()` / `IsLinkHealthy()`
> 都在 **`display.h` / `display.cpp`**（原先放在 `actions.cpp`，导致 `verify.cpp` 要用就得
> 反向依赖 UI 动作层；已下沉）。`ResolutionResult::nature`、`RestoreOutcome::nature`
> 是本层对外暴露的性质字段。

#### 4.8.10 托盘必须有落盘日志

托盘模式会 `ShowWindow(GetConsoleWindow(), SW_HIDE)` 隐藏控制台。若不重定向，
所有 `LogLine` 输出都进了**看不见的隐藏控制台**，进程退出后现场全部丢失 ——
"双击启动出问题"会变成无法事后排查的状态（脚本路径有 `guard.log`，只有双击路径没有）。

因此 `RunTray` 开头就 `RedirectStdioToLog(L"tray.log")`：`_wfreopen` 到
`OUT\tray.log`（追加；**超过 2 MB 则从头写**，防止长期驻留把日志撑爆），
并写入一行会话分隔 `===== ResProbe 托盘会话开始 <UTC> =====`。
`LogLine` 用的是 `fputs(stdout)+fflush`，所以 `freopen` 足以覆盖程序自身输出。

#### 4.8.11 超时必须终止整棵进程树

脚本超时（默认 5 分钟）后**只报告失败是不够的**：`cmd` 派生的
`deviceinstaller64` / `pnputil` / `powershell` 还在继续修改系统状态，
而用户已经在对话框里看到"执行失败"了 —— 这种"报告与现实不一致"比超时本身更危险。

做法：`CreateJobObjectW` + `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`，
`AssignProcessToJobObject(pi.hProcess)`，超时后 `TerminateJobObject`。

- Win8+ 支持嵌套作业，正常环境都能成功；失败则退化为只 `TerminateProcess` 直接子进程。
- 作业句柄在进程结束（或已被终止）后才 `CloseHandle` —— 提前关会误杀仍在运行的进程。
- `RunAndCapture` 与 `RunProjectScript` 都做了同样处理。
- 对话框文案会区分"已强制终止"与"未能终止"，不含糊其辞。

#### 4.8.12 其他已修复的细节

| 项 | 问题 | 修法 |
|---|---|---|
| 角色标记读取方向 | `ReadFileBytes` 默认保留文件**尾部** 64KB（为日志设计），却被 `FindScriptByRole` 用来扫脚本**第 2 行**的标记 → 脚本超过 64KB 会静默定位失败 | 加 `keepHead` 参数，脚本扫描传 `true` |
| 托盘图标注册失败 | `Create()` 无条件返回 true → 进程在跑、控制台被隐藏、托盘却没图标 | 注册失败返回 false；`RunTray` 改为**先建托盘、成功了再隐藏控制台**，失败时弹窗并保留控制台 |
| 悬垂窗口句柄 | `WM_DESTROY` 未置空 `hwnd_`，`Run()` 之后的 `Destroy()` 会对已销毁的 HWND 再操作 | 在 `WM_NCDESTROY` 里 `hwnd_ = nullptr` |
| 菜单静默截断 | 分辨率菜单超过 100 项时 `break`，无任何提示 | 追加灰显条目"… 另有 N 项未显示（菜单上限 100 项）" |
| 状态查询重复枚举 | `QueryTrayStatus` 会做 3~4 轮全量枚举（`EvaluateTopology` 两轮 + `CaptureSnapshot` 一轮），每轮含 `QueryDisplayConfig` + SetupAPI 读 EDID | 新增 `CountAvailableVirtualOutputsFrom` / `CaptureSnapshotFrom` 复用已枚举结果；实测**单次查询 172 ms** |
| `ShellExecute` 静默失败 | `OpenFolderInExplorer` / `OpenFileInDefaultApp` 返回 void | 返回 bool（`> 32` 才是成功），托盘失败时弹窗说明 |
| `--no-guard` 归类与校验 | 用法里列在 `watch 专有` 段落，却在说明里自称 tray 专有；且对所有子命令都生效 | 移到独立的 `tray 专有` 段落；非 tray 模式给警告并忽略 |
| `selftest` 描述不准 | 用法写"只读"，但第 5 步用 `permanent=true` 会更新显示模式注册表 | 用法改为"等值写入：把当前分辨率再写一次，会更新显示模式注册表" |

#### 4.8.13 报告写盘失败必须改变退出码

`WriteOutputs()` 有返回值，但历史上 7 个调用点全部忽略它：`snapshot --out X` 在磁盘满 /
只读目录 / 目录建不出来时会打印 `[输出] 写入 JSON 报告失败`，却仍然 `exit 0`，
调用脚本据此以为报告已经落盘。

现在：

- `snapshot` / `verify` / `paths` / `vdisplay` / `restore`：直接 `return WriteOutputs(r) ? 0 : 1;`
- `watch`：监听正常结束（rc=0）但报告写失败时，把退出码提升为 1

**`ensure` 刻意例外**：它的 `0 / 20 / 21` 是脚本做幂等判断的对外契约（§6.5），
"报告写不进去"不应该改变拓扑判定的结论，因此它仍然只打印错误、不改退出码。

> 新增子命令时按同一原则选择：**契约型退出码不因落盘失败而变，诊断型退出码要变。**

#### 4.8.14 EDID 校验和必须可见

`ParseEdid()` 一直在算第一个 128 字节的校验和，但算完就被一个空 `if` 丢掉，于是
"头部合法（魔数正确）但校验和错误"的 EDID 在控制台仍显示 `EDID=有效` —— 与"原始字节
作为证据保留"的设计目标冲突。

现在 `EdidInfo::checksumOk` 是真实字段：写入 JSON（`targets[].edid.checksumOk`），
控制台分四档显示：

| 条件 | 控制台文案 |
|---|---|
| `!present` | `缺失` |
| `present && !valid` | `存在但无效`（魔数不对 / 长度不足 128） |
| `present && valid && !checksumOk` | `存在但校验和错误` |
| 三者齐备 | `有效` |

`valid` 与 `checksumOk` 分开表达是刻意的：头部合法但校验和错误的数据**仍然保留**（它是有
价值的证据），只是不再被笼统地说成"有效"。

#### 4.8.15 两个容易写错的平台 API

| 位置 | 易错点 | 当前做法 |
|---|---|---|
| 服务枚举（`session.cpp`） | `EnumServicesStatusExW` 在缓冲区装不下剩余项时返回 `FALSE` + `ERROR_MORE_DATA`。只看一次返回值会**整类漏检**远程软件的服务痕迹 | 按 `resume` 续枚举；`needed > buf.size()` 时扩容；64 轮上限防死循环 |
| 客户端地址（`session.cpp`） | `WTS_CLIENT_ADDRESS::Address[]` 中 IPv4 的起始偏移存在两种说法（"地址族 2 字节后紧跟 IP" vs "完整 `sockaddr_in`"），代码与注释曾经互相矛盾 | 偏移集中为唯一常量 `kInetAddrOffset`（当前 = 2），注释与实现一致；全 0 时显示 `(未提供)` 而不是 `0.0.0.0`。**若真实 RDP 会话里看到前三段像端口号，把该常量改成 4**（见 §8.7） |

---

## 5. 关键配置项

### 5.1 命令行选项

| 选项 | 适用 | 默认 | 作用 |
|---|---|---|---|
| `--reference WxH` | 全部 | `1920x1080` | 参考健康分辨率。判定、验证、提升都以它为准 |
| `--out FILE` | 全部 | **不写 JSON** | 显式指定才写报告；纯文件名会落到输出目录，同时派生 `<同名>.peer.json` |
| `--out-dir DIR` | 全部 | `<程序目录>\OUT` | 覆盖输出目录 |
| `--no-cds-test` | 全部 | 关闭（即默认执行） | 跳过 `CDS_TEST` 只读探测 |
| `--verbose` | 全部 | 否 | 输出更详细信息；`paths` 下会列出全部占位路径 |
| `--peer FILE` | verify/watch | — | 对端 `*.peer.json`，启用双端比对 |
| `--mode auto\|mirror\|promote` | ensure | `auto` | `auto`=有健康物理屏则镜像、否则提升虚拟屏 |
| `--duration SEC` | watch | `0`（直到 Ctrl+C） | 监听时长上限 |
| `--interval SEC` | watch | `30` | 心跳采样间隔，`0` 关闭 |
| `--settle MS` | watch | `800` | 事件后等待驱动稳定的毫秒数；合并窗口上限为其 3 倍 |
| `--guard` | watch | 否 | 启用自动守卫（断链自动提升虚拟屏） |
| `--no-guard` | tray | 否 | 托盘只提供界面，**不**自动改动桌面拓扑（默认 tray 是开守卫的） |
| `--yes` | vdisplay/restore | 否 | **必需**，确认执行系统状态变更 |
| `--mirror` | vdisplay | 否 | 复制（镜像）拓扑 |
| `--promote` | vdisplay | 否 | 提升虚拟屏为主屏 + 摘除死输出 |

**短别名**（`main.cpp` 参数解析里同样接受，`--help` 未逐一列出）：

| 别名 | 等价 | 备注 |
|---|---|---|
| `-y` | `--yes` | 确认执行状态变更 |
| `-v` | `--verbose` | |
| `-q` | `--quiet` | ⚠️ **当前无任何效果**（只被解析进 `Options::quiet`，全项目未使用）。保留为未来"抑制常规输出"的位置，见 §9.7 |
| `-h` / `/?` / `help` / `--help` | 打印用法 | 等价子命令，退出码 0 |

参数解析的三条既定行为（改解析逻辑时请保持）：

1. **未知参数只警告并忽略**（`警告: 忽略未知参数 xxx`），不报错退出 —— 保证旧脚本加了新参数
   也不会在新旧版本间互相打断；
2. **缺值参数会被误报成"未知参数"**：`--out` 出现在末尾时落进 `else` 分支，提示为
   `忽略未知参数 --out`。措辞不准确但行为安全（等于不落盘），见 §9.7；
3. `--no-guard` 只对 `tray` 有意义，其它子命令会收到警告并被清理。

### 5.2 需要留意的默认常量

| 常量 | 位置 | 值 | 说明 |
|---|---|---|---|
| `kMaxEvents` | `watcher.cpp` | 1000 | 事件上限；达到后用最新状态覆盖尾条目，保证有界且终态准确 |
| `kMaxDeferFactor` | `watcher.cpp` | 3 | 事件合并窗口相对 `settleMs` 的最大延长倍数 |
| `kGuardCooldownMs` | `watcher.cpp` | 8000 | 守卫动作冷却，防止抖动期反复触发 |
| `kMaxModeEnumeration` | `display.cpp` | 4000 | 单输出模式枚举上限 |
| 模式表保留上限 | `display.cpp` | 64 | 每输出保留的唯一分辨率条数 |
| EDID 原始字节上限 | `edid.cpp` | 256 字节 | 且**仅活动输出**保留 `rawHex`，避免同一显示器重复多份 |
| `kInetAddrOffset` | `session.cpp` | 2 | `WTS_CLIENT_ADDRESS::Address[]` 中 IPv4 的起始偏移；若真实 RDP 会话显示不对就改成 4（§4.8.15） |
| `kStatusTtlMs` | `tray.cpp` | 8000 | 托盘状态摘要缓存有效期（右键路径上避免每次都读 EDID） |
| `kModesTtlMs` | `tray.cpp` | 8000 | 分辨率菜单缓存有效期 |
| 分辨率菜单上限 | `tray.cpp` | 100 项（`kIdResLast - kIdResBase + 1`） | 超出会追加灰显条目明说"另有 N 项未显示" |
| 命令 ID 区间 | `tray.cpp` | `kIdResBase=1100` … `kIdResLast=1199` | 新增菜单项不要占用该区间 |
| 服务枚举轮次上限 | `session.cpp` | 64 | 防 `ERROR_MORE_DATA` 死循环 |
| 托盘日志轮转阈值 | `actions.cpp` | 2 MB | 超过则 `_wfreopen` 从头写 |
| 混合解码缓冲上限 | `actions.cpp` | 64 KB（`kMaxRead`） | 读文件字节；`keepHead` 决定读头还是读尾 |
| 事件尾条目 | 见 `kMaxEvents` | — | 达上限后不再追加，改为覆盖最后一条以保证"有界 + 终态准确" |

### 5.3 注册表 / 系统配置

| 位置 | 作用 |
|---|---|
| `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\WUDF\Services\usbmmIdd\Parameters\Monitors` | usbmmidd 虚拟显示器分辨率表 |
| `Control\Class\{4D36E968-E325-11CE-BFC1-08002BE10318}\000X` | 显示类软件键（`0005` = usbmmidd，`0002` = Oray IDD） |
| `Enum\ROOT\DISPLAY\0000` / `0001` | Oray IDD / usbmmidd 设备实例 |

### 5.4 部署脚本一览

| 脚本 | 角色标记 | 作用 | 关键参数 |
|---|---|---|---|
| `安装.cmd` | `RESPROBE-ROLE:install` | **幂等**安装（详见下方 8 步） | 无 |
| `卸载.cmd` | `RESPROBE-ROLE:uninstall` | 注销任务 → 停守卫 → 移除虚拟屏 | `/full` 移除驱动；`/purge` 再清文件与内置驱动包 |
| `一次性启动.cmd` | `RESPROBE-ROLE:once` | 一次性就绪（无显示器 / 不再接回显示器） | `/status` 只看状态 |
| `常驻守卫.cmd` | `RESPROBE-ROLE:guard` | 常驻守卫（**默认开机启动方式**） | `/bg` 后台；`/json` 额外写状态 JSON |
| `开机自启.cmd` | `RESPROBE-ROLE:autostart` | 注册 / 注销 / 查询开机自启 | `/mode guard`（默认）`\|once`；`/status`；`/remove` |
| `编译.bat` | — | 构建入口 | `clean` 清理 |
| `脚本编码修复.ps1` | — | 脚本编码归一化 + 纯 ASCII 校验（见 §8.6） | 无 |
| `res\check-manifest.ps1` | — | manifest 校验，**构建期自动执行**（见 §8.6） | 无 |
| `res\make-icon.ps1` | — | 重新生成多尺寸图标（仅改图案时需要，`make icon`） | 无 |

> **怎么调用这些脚本**
>
> | 项 | 说明 |
> |---|---|
> | 权限 | 除 `编译.bat` / `脚本编码修复.ps1` 外，其余都要**以管理员身份运行**（脚本自身用 `fltmc` 检查，权限不足直接退出） |
> | PowerShell | 必须加 `.\` 前缀，例如 `.\安装.cmd`；PowerShell 不会搜索当前目录 |
> | cmd | 直接写文件名即可 |
> | 图形入口 | 以上操作在**托盘的右键菜单**里都有对应项，效果完全相同（托盘走的是同一批脚本，见 §4.8.5） |
>
> 脚本只是编排层，真正的动作都由 `resprobe.exe` 完成；`ensure` 的退出码（0/20/21）
> 就是脚本判断幂等的依据（§6.5）。**`resprobe.exe --help` 里有一份面向使用者的
> 同一份说明**，改动脚本入参时记得同步 `PrintUsage()`。
>
> **角色标记**（第二列）是托盘在运行时定位脚本的依据，必须**独占一行**写在 `@echo off`
> 之后。新增可被托盘调用的脚本时务必补上，否则托盘会报"未找到所需的脚本"。
> 原理与三个已踩的匹配坑见 §4.8.5。

> 共享配置（路径 / 参考分辨率 / 驱动包位置 / 任务名 / 退出码）**内联在每个脚本里**，
> 搜索 `CONFIG BLOCK` 即可定位。改动配置需要同步 6 个 `.cmd` / `.bat` —— 这是
> 「内容必须纯 ASCII，因而无法 `call` 中文名配置文件」换来的代价，见 §3 编码模型。

#### `安装.cmd` 的 8 个步骤与幂等判据

新机上驱动、设备节点、虚拟屏**都不存在**，本脚本负责从零装出来。
每一步都先查状态再动作，因此可重复执行：

| 步骤 | 动作 | 幂等判据 |
|---|---|---|
| 1 | 确保 `resprobe.exe` | 已存在则跳过；缺失则调 `mingw32-make` 现场编译 |
| 2 | 确保 `OUT\` | `mkdir` 失败即已存在 |
| 3 | **定位驱动包**（三级回退） | ①内置 `driver\usbmmidd_v2\` ②迁移旧位置 `C:\Temp\...` ③联网下载 |
| 4 | **创建设备节点** | `pnputil /enum-devices /class Display` 里出现 `USB Mobile Monitor` 则跳过 |
| 5 | 写虚拟显示器分辨率表 | `reg add /f` 覆盖写，天然幂等 |
| 6 | **激活恰好一块虚拟显示器** | **依赖 `resprobe ensure` 返回 20**；已有虚拟屏就不再 `enableidd 1` |
| 7 | 应用显示拓扑 | `resprobe ensure` 内部先判"已就绪"再决定动作 |
| 8 | 汇总结果与列出同目录脚本 | — |

> **第 4 步是全新机器上的关键**：`pnputil /add-driver` 只把驱动包放进 DriverStore，
> **不会创建 ROOT 枚举的设备节点**。必须用驱动包自带的
> `deviceinstaller64.exe install usbmmIdd.inf usbmmIdd` 才行。
> 缺了这一步，第 6 步的 `enableidd 1` 会静默失效（设备在跑但一个虚拟显示器都没有）。

> **第 3 步为什么优先用内置包**：`C:\Temp` 是临时目录，随时可能被清理工具或用户删除。
> 驱动包已随项目内置在 `driver\usbmmidd_v2\`，**离线机器直接可用**；只有在既没有内置包、
> 也没有旧位置副本时才联网下载。要做纯离线部署，把整个项目目录拷过去即可。

#### `开机自启.cmd` 注册的两个登录任务（默认 `guard` 模式）

| 任务名 | 目标 | 延迟 | 原因 |
|---|---|---|---|
| `ResProbe VDisplay Init` | `一次性启动.cmd` | 20 秒 | usbmmidd 虚拟屏**不跨重启保留**，必须先重新激活 |
| `ResProbe Guard` | `常驻守卫.cmd` | 60 秒 | 常驻守卫，需等虚拟屏就绪后再启动 |

两者均为「登录时触发 + 最高权限」（改显示拓扑需要管理员）。
注册前会用 `Get-ScheduledTask | Where-Object { $_.TaskName -like 'ResProbe*' }` 清理
**所有** `ResProbe` 前缀任务，因此历史上任何命名（中文或英文）的残留都会被清掉。

> 任务名保持英文：中文任务名必须写进脚本内容，违反纯 ASCII 约束。
> 脚本路径由内容里的 ASCII 标记行发现（见 §3），而不是硬编码中文文件名。

### 5.5 进程退出码汇总

| 退出码 | 出现位置 | 含义 | 调用方应采取的动作 |
|---|---|---|---|
| `0` | 全部 | 成功（`ensure` 特指"兜底能力已就位"）| — |
| `1` | 诊断类子命令 | 参数或执行失败；**含"给了 `--out` 但报告写不进去"**（§4.8.13）| 看输出定位 |
| `1` | 参数解析 | 未知子命令 / `CommandLineToArgvW` 失败 | 修正命令行 |
| `3` | `watch` / `tray` | 无法创建监听窗口（事件监听不可用）| 多为 GUI 环境异常 |
| `4` | `tray` | 托盘图标创建失败（常见：`explorer.exe` 未运行）| 保留控制台查看原因 |
| `5` | `tray` | 已有托盘在运行（`Global\ResProbe.Tray` 被占用）| 去通知区域找已有图标，或先退出它 |
| `20` | `ensure` | 没有可用的虚拟显示输出 | `deviceinstaller64.exe enableidd 1` 后重试 |
| `21` | `ensure` | 已调整，但主屏仍未达到参考分辨率 | 人工排查驱动 / 拓扑 |
| `N` | `selftest` | **失败项数量**（`0` = 全部通过）| 查输出里的 `[FAIL]` 行 |

> `ensure` 的 `0 / 20 / 21` 是脚本幂等判断的对外契约，**不要因为落盘失败或日志问题改动它**（§6.5）。

---

## 6. 接口与函数约定

以下均为 `namespace resprobe` 内的自由函数，头文件见 `src/`。

### 6.1 `edid.h`

```cpp
// 规范化 DisplayConfig 监视器路径为设备实例 ID
std::wstring NormalizeMonitorDevicePath(const std::wstring& path);

// 键为大写化的设备实例 ID
std::map<std::wstring, EdidInfo> EnumerateMonitorEdid(std::wstring* diagOut = nullptr);

// 解析原始 EDID 字节。valid 只表示"头部（魔数）合法"，与校验和分开表达：
// checksumOk 独立成字段（写入 JSON），"头部合法但校验和错误"在控制台显示为
// "存在但校验和错误"而不是笼统的"有效"（见 §4.8.14）
EdidInfo ParseEdid(const std::vector<BYTE>& bytes);
```

### 6.2 `display.h`

```cpp
// includePlaceholderPaths=false 时只返回"有意义"的输出
std::vector<DisplayTarget> EnumerateDisplays(UINT32 refW, UINT32 refH,
                                             std::wstring* diagOut = nullptr,
                                             int* totalPathsOut = nullptr,
                                             bool includePlaceholderPaths = false);

// 用**已经枚举好**的输出构建快照（托盘热路径复用，避免重复 QueryDisplayConfig + 读 EDID）
DisplaySnapshot CaptureSnapshotFrom(const std::vector<DisplayTarget>& targets,
                                    int totalPathCount, const std::string& trigger);
DisplaySnapshot CaptureSnapshot(const std::string& trigger, UINT32 refW, UINT32 refH);
DisplayPowerState InferPowerStateFromTopology(const std::vector<DisplayTarget>& targets);
std::wstring PrimaryModeText(const DisplaySnapshot& snap);

// "有意义的输出"：活动，或已连接监视器（有设备路径 / EDID / 设备节点）
bool IsMeaningfulDisplayTarget(const DisplayTarget& t);

// 虚拟显示判断：优先看 adapterDevicePath，退化到 gdiDeviceString，再看镜像驱动标志
bool IsVirtualDisplayTarget(const DisplayTarget& t);

// 链路健康：EDID 与监视器设备节点**都**可见。
// 输出性质判定 / 拓扑计数 / 守卫判断三处必须共用它（详见 §4.7.3 与 §4.8.9）
bool IsLinkHealthy(const DisplayTarget& t);

// 输出性质：失败归因的唯一依据（四类文案见 §4.8.9）
enum class OutputNature { Unknown, Virtual, HealthyPhysical, DeadPhysical, Mixed };
std::wstring OutputNatureName(OutputNature n);
OutputNature ClassifyOutputNature(const std::vector<DisplayTarget>& targets,
                                  const std::wstring& gdiDevice);

// 输出：changedFields=机器可读字段，transitions=人类可读迁移
void DiffSnapshots(const DisplaySnapshot& before, const DisplaySnapshot& after,
                   std::vector<std::string>* changedFields,
                   std::vector<std::string>* transitions);
```

**关键行为约定（改这几个函数前必读）**

| 函数 | 约定 |
|---|---|
| `EnumerateDisplays` | `includePlaceholderPaths = false`（默认）只返回"有意义"的输出；`true` 返回全部路径。**`paths` 子命令与托盘的"可用虚拟屏计数"必须用 `true`** —— 未激活但已实例化的虚拟显示器会被默认过滤器丢掉，而它的 `targetAvailable` 恰恰是 `true` |
| `IsVirtualDisplayTarget` | **必须优先看 `adapterDevicePath`**：复制（镜像）拓扑下 `gdiDeviceString` 会串成物理适配器的描述，只看它认不出虚拟屏 |
| `ClassifyOutputNature` | **不能用 `gdiDeviceName` 唯一定位输出**：同名下可能同时挂着物理与虚拟目标（→ `Mixed`）。判定"链路健康"用 `IsLinkHealthy()` |
| `IsLinkHealthy` | = `edid.present && monitorDeviceNodePresent`。**不要退化成只看 EDID**（"EDID 可读但设备节点已消失"的中间态会被误判为健康） |
| `CaptureSnapshotFrom` | 传入的 `targets` 必须是 `EnumerateDisplays` 的结果（理想是 `includePlaceholderPaths = true` 那一份，它是超集），否则主屏识别可能漏 |
| `DiffSnapshots` | 键为 `gdiDeviceName`（空则 `T:<LUID>:<targetId>`）；两个出参指针必须非空（内部已判空，但不要依赖） |

### 6.3 `session.h`

```cpp
SessionInfo QuerySessionInfo();
std::vector<RemoteSoftwareInfo> DetectRemoteSoftware(const std::vector<DisplayTarget>& targets);
bool IsProcessElevated();
```

### 6.4 `verify.h`

```cpp
ModeProbe ProbeModeAvailability(const DisplayTarget& t, UINT32 width, UINT32 height,
                                bool useCdsTest);

// 依次尝试 3 种 DEVMODE 组合；attempts 可空
LONG ApplyDisplayMode(const std::wstring& gdiDeviceName, UINT32 width, UINT32 height,
                      UINT32 refreshHz, bool permanent,
                      std::vector<ApplyAttempt>* attempts = nullptr);

// 失败时按目标输出的**性质**归因（结果存在 RestoreOutcome::nature），
// 不再"见到无 EDID 就断言链路已断开"（虚拟屏 EDID 天然为空，见 §4.8.9）
RestoreOutcome RestoreReferenceMode(const DisplaySnapshot& before, UINT32 refW, UINT32 refH);

// 目标屏落 (0,0) 并设为指定分辨率，其余输出依次右移。
// **返回值 = 目标主屏那一次 ChangeDisplaySettingsExW 是否成功**（不再是"只要找到设备就 true"）；
// 失败时 message 会带失败码。调用方（ensure/vdisplay/守卫）据此判断"是否真的提升了"
bool ArrangeDesktopLayout(const std::wstring& primaryGdi, UINT32 width, UINT32 height,
                          UINT32 refreshHz, std::vector<ApplyAttempt>* log,
                          std::wstring* message);

// 返回 ERROR_SUCCESS / Win32 错误码；不允许摘成空桌面
LONG DetachOutputs(const std::vector<std::wstring>& gdiDeviceNames);

// 提升虚拟屏为参考分辨率主屏并摘除死输出。
// 摘除前会**二次枚举复核**（两次都读不到 EDID 才摘，见 §4.7.4.1）；
// promoted 现在真实反映 ArrangeDesktopLayout 的返回码；message 汇总摘除/拦下数量
PromoteOutcome PromoteVirtualDisplay(UINT32 width, UINT32 height, bool detachDeadOutputs);

// 拓扑状态汇总（供 ensure 做幂等决策）
struct TopologyState {
    int totalActive = 0;
    int activeVirtualCount = 0;     // 活动中的虚拟显示输出数量
    int healthyPhysicalCount = 0;   // IsLinkHealthy 的非虚拟活动输出（EDID 与设备节点都在）
    int deadPhysicalCount = 0;      // 非虚拟、非健康，且当前分辨率低于参考值的活动输出
    std::vector<std::wstring> activeVirtualDevices;
    std::vector<std::wstring> deadPhysicalDevices;
    std::wstring primaryGdiDevice;
    std::wstring primaryMode;
};
TopologyState EvaluateTopology(UINT32 refW, UINT32 refH);

LONG ApplyCloneTopology();    // SDC_APPLY | SDC_TOPOLOGY_CLONE
LONG ApplyExtendTopology();   // SDC_APPLY | SDC_TOPOLOGY_EXTEND

// 只有真正解析出 primaryMode 才返回 true 并置 out->present；解析失败时 out 会被清空。
// 调用方必须用 (peerPath 非空 && out->present) 作为"是否启用双端比对"的判据 ——
// 曾经只在解析前就置 present=true，于是"文件读得到但不是对端摘要"会被当成有效对端，
// 产出"远程客户端="（空值）这种无意义证据行，还会盖掉单机时间线判定（见 §6.10）
bool ReadPeerSummary(const std::wstring& path, PeerSummary* out);
```

### 6.5 `ensure` 子命令的契约

`ensure` 是给脚本调用的**幂等**入口，退出码是主要接口（脚本无需解析中文输出）：

| 退出码 | 含义 | 调用方应采取的动作 |
|---|---|---|
| `0` | **兜底能力已就位**（有虚拟显示器，且主屏达到参考分辨率、桌面无断链死输出） | 无需动作 |
| `20` | 没有可用的虚拟显示输出 | 执行 `deviceinstaller64.exe enableidd 1` 后重试 |
| `21` | 已调整，但主屏仍未达到参考分辨率 | 人工介入排查（驱动/拓扑异常） |
| `1` | 参数或执行失败 | 人工介入 |

**退出码 0 的语义是「兜底能力就位」，不是「当前桌面看着正常」。** 这一点很关键：
显示器开着的时候桌面本来就是 1920×1080，而此刻虚拟屏可能压根不存在，
物理屏一旦断电又会掉回 1024×768。因此虚拟屏的存在性是返回 0 的**前置条件**。

行为：

1. **前置检查：虚拟显示输出是否存在**（与当前桌面状态无关）。
   判据取两个信号的并集：全量路径里虚拟适配器上存在 `active` / `targetAvailable`
   的目标，或拓扑汇总里已识别出活动的虚拟输出。
   若为 0 → 打印提示并返回 **20**；
2. 采集拓扑，打印状态（活动/健康物理/断链物理计数、主屏、可用虚拟输出数）；
3. **就绪判据**：`主屏分辨率 == 参考分辨率` 且 `deadPhysicalCount == 0`
   → 直接返回 0，**不做任何改动**（这是幂等的关键）；
4. 否则按 `--mode` 决策：`promote` → 提升虚拟屏；`mirror` → 应用复制拓扑；
   `auto` → 有健康物理屏则镜像，否则提升（镜像失败会退化为提升，保证主屏正确）；
5. 复核并返回 0 / 21。

> 关于判据为什么刻意不依赖"虚拟屏识别"：克隆（镜像）拓扑下多个输出共用同一个 GDI
> 源名，DisplayConfig 会把克隆目标报在**源适配器**上，此时基于适配器/设备描述的识别
> 全部失效。所以第 3 步只用「主屏分辨率 + 有无断链死输出」，虚拟屏识别仅用于第 1 步
> 的存在性判断（那时它能工作，因为拓扑是扩展态或虚拟屏独立存在）。

> **退出码不受落盘影响**：即使 `--out` 指定的路径写不进去，`ensure` 仍然只返回
> `0 / 20 / 21`（只打印 `[输出] 写入 JSON 报告失败`）。把落盘失败提升为退出码 `1` 的
> 只有诊断类子命令（`snapshot` / `verify` / `paths` / `vdisplay` / `restore` / `watch`），
> 理由见 §4.8.13。改这条契约前请先看 §12.8 的影响面清单。

### 6.6 `verdict.h`

```cpp
struct VerdictInput {
    const DisplaySnapshot* baseline = nullptr;
    const DisplaySnapshot* latest   = nullptr;
    const std::vector<EventRecord>* events = nullptr;   // 整条时间线，可空
    const SessionInfo* session      = nullptr;
    const std::vector<RemoteSoftwareInfo>* software = nullptr;
    const PeerSummary* peer         = nullptr;
    UINT32 refWidth = 1920, refHeight = 1080;
};

Verdict EvaluateVerdict(const VerdictInput& in);   // 纯函数，不改系统状态
```

### 6.7 `watcher.h`

```cpp
void RequestStop();       // 供控制台 Ctrl+C 处理器调用
bool StopRequested();

class Watcher {
public:
    explicit Watcher(Report& report);
    int Run();                                     // 0=正常结束
    void OnWindowMessage(HWND, UINT, WPARAM, LPARAM);
};
```

### 6.8 `report.h`

```cpp
std::wstring DefaultOutPath(const std::wstring& mode);
std::wstring DerivePeerPath(const std::wstring& outPath);

void PrintBanner();
void PrintSession(const SessionInfo&);
void PrintSnapshotConsole(const DisplaySnapshot&, bool verbose);
void PrintRemoteSoftware(const std::vector<RemoteSoftwareInfo>&);
void PrintProbes(const std::vector<ModeProbe>&);
void PrintEventConsole(const EventRecord&);
void PrintVerdict(const Verdict&);

bool WriteJsonReport(const Report&, const std::wstring& path);
bool WritePeerSummary(const Report&, const std::wstring& path);
```

### 6.9 JSON 报告结构

```jsonc
{
  "tool": "ResProbe", "version": "1.0.0", "mode": "watch",
  "startedUtc": "...", "finishedUtc": "...", "generatedUtc": "...",
  "diagnostics": "DisplayConfig: 146 条路径 / ...",
  "options": { "referenceWidth": 1920, "referenceHeight": 1080, "durationSec": 0,
               "intervalSec": 10, "settleMs": 800, "cdsTest": true, "writeConfirmed": false },
  "session": { "remoteSession": false, "protocolType": "Console", /* ... */ },
  "remoteSoftware": [ { "name": "...", "kind": "process|service|display-adapter|monitor-device",
                        "detail": "...", "isVirtualDisplay": false } ],
  "initialSnapshot": { /* DisplaySnapshot */ },
  "displayPowerNotifications": { "notificationCount": 1, "transitions": [ "..." ] },
  "events": [ { "index": 2, "timeUtc": "...", "uptimeMs": 12345,
                "trigger": "WM_DISPLAYCHANGE(1024x768 32bpp)",
                "changed": true, "changedFields": [ "..." ], "transitions": [ "..." ],
                "snapshot": { /* DisplaySnapshot */ } } ],
  "verification": [ { "gdiDeviceName": "\\\\.\\DISPLAY1", "listedInModeList": true,
                      "cdsTestPerformed": true, "cdsTestAccepted": true,
                      "degradedModeResolutions": [ "..." ] } ],
  "peer": { "present": false, "primaryMode": "", /* ... */ },
  "actionLog": [ "guard#1: 已完成虚拟屏提升；活动输出 1 个，主屏 1920x1080 (\\\\.\\DISPLAY46)" ],
  "restore": { "attempted": true, "succeeded": true, "resultName": "DISP_CHANGE_SUCCESSFUL",
               "attempts": [ "完整 DEVMODE（含色深/刷新率）+ 写入注册表 -> DISP_CHANGE_SUCCESSFUL" ] },
  "verdict": { "code": "LINK_LEVEL_FALLBACK", "blame": "link", "confidence": "high",
               "summary": "...", "reasons": [ "..." ], "recommendations": [ "..." ] }
}
```

`DisplaySnapshot` 关键字段：`timeUtc` / `uptimeMs` / `trigger` / `displayPowerState` /
`displayPowerStateInferred` / `remoteSession` / `sessionId` / `primaryGdiDevice` / `primaryMode` /
`totalPathCount` / `reportedTargetCount` / `targets[]`。

`DisplayTarget` 关键字段：`active` / `targetAvailable` / `targetStatusFlags` / `gdiDeviceName` /
`outputTechnology` / `monitorFriendlyName` / `monitorDevicePath` / `monitorInstanceId` /
`monitorDeviceNodePresent` / `currentMode` / `preferredMode` / `gdi{...}` / `modeList{...}` / `edid{...}`。

`edid{...}` 含 `present` / `valid` / `manufacturer` / `pnpIdRaw` / `productCode` /
`monitorName` / `preferredTiming` / `rawHex`（仅活动输出）。

### 6.10 对端摘要（`*.peer.json`）

扁平的键值结构，便于用最简解析器读取（`ReadPeerSummary` 只做按行键值提取，不做通用 JSON 解析）：

```jsonc
{ "tool": "ResProbe", "timeUtc": "...", "hostName": "...",
  "remoteSession": false, "primaryMode": "1920x1080", "primaryGdiDevice": "\\\\.\\DISPLAY1",
  "degraded": false, "activeTargets": [ { "gdiDeviceName": "...", "mode": "1920x1080",
                                          "edidPresent": true } ] }
```

**双端比对流程**：在被控端与远程客户端各跑一次 `snapshot`，把客户端产出的
`*.peer.json` 拷到被控端，执行 `resprobe verify --peer <该文件>`。

> **`present` 语义**：只有解析出 `primaryMode` 才算读取成功（`present = true`）。
> 调用方判据统一为 `peerPath 非空 && peer.present`。指向一个"能读但不是对端摘要"的文件时，
> 只会得到一句警告，判定器不会走双端分支 —— 这是刻意的：宁可不比对，也不要基于空值出结论。

### 6.11 `actions.h` / `tray.h`（动作层与托盘）

动作层与 UI **完全解耦**：不引用任何窗口/菜单类型，只做动作并返回结果结构；托盘只负责把它接到
菜单上、并把结果弹给用户。这样这些动作将来也能直接暴露成 CLI 子命令或单元测试。

```cpp
// ---- 通用：带消息泵的等待 / 命令执行 ----
void PumpSleep(DWORD ms);                          // 等待期间持续派发本线程消息（托盘不假死的关键）
bool RunAndCapture(const std::wstring& commandLine, const std::wstring& workDir,
                   DWORD timeoutMs, DWORD* exitCodeOut, std::wstring* outputOut);

// ---- 项目脚本调用 ----
bool FindScriptByRole(const std::wstring& homeDir, const std::wstring& marker,
                      std::wstring* pathOut);      // 按 "@rem RESPROBE-ROLE:<marker>" 整行精确匹配
ScriptRunResult RunProjectScript(const std::wstring& homeDir, const std::wstring& marker,
                                 const std::wstring& args, const std::wstring& logName,
                                 DWORD timeoutMs = 300000);

// ---- 输出目录与日志 ----
std::wstring GetHomeDir();  std::wstring GetOutDirPath();  bool EnsureOutDir();
bool RedirectStdioToLog(const std::wstring& logName, std::wstring* pathOut);
bool ReadLogTail(const std::wstring& path, size_t maxLines, std::wstring* out);
std::wstring FindNewestLogFile(const std::wstring& dir);

// ---- 单实例保护（设计见 §4.8.8）----
GuardOwner AcquireGuardLock();   // ThisProcess / OtherProcess / Unavailable
bool AcquireTrayLock();
bool IsGuardOwnedByOther();

// ---- 状态摘要（托盘菜单首行 + 状态对话框的数据源）----
TrayStatus QueryTrayStatus(UINT32 refW, UINT32 refH);
std::wstring BuildStatusReport(const TrayStatus& st, UINT32 refW, UINT32 refH);
std::wstring BuildPathsReport(UINT32 refW, UINT32 refH);

// ---- 分辨率与开机自启 ----
std::vector<ModeEntry> GetModeListOfDevice(const std::wstring& device);
std::vector<ModeEntry> GetPrimaryModeList(std::wstring* currentOut, std::wstring* deviceOut);
ResolutionResult ApplyResolution(UINT32 width, UINT32 height);
AutostartState QueryAutostart();
bool SetAutostart(bool enable, bool guardMode, std::wstring* message);

// ---- 杂项 ----
bool OpenFolderInExplorer(const std::wstring& dir);
bool OpenFileInDefaultApp(const std::wstring& path);
void HideConsoleWindow();
int  RunActionSelfTest();        // selftest 子命令的实现
```

`class TrayIcon`（`tray.h`）：`Create(refW, refH)` / `Destroy()` / `Window()` / `SetGuardEnabled(bool)`。
宿主窗口是隐藏的**顶层**窗口（`TrackPopupMenu` 要求 owner 能被置前台），其消息由 `Watcher`
现成的消息泵代为派发，所以 `watcher.cpp` 一行都不用改。**`Create()` 在图标注册失败时返回
`false`**，调用方必须据此保留控制台并提示 —— 否则会出现"进程在跑、控制台被隐藏、托盘没图标"
的假死观感（详见 §4.8.2 / §4.8.10）。

**关键行为约定**

| 函数 | 约定 |
|---|---|
| `FindScriptByRole` | 只匹配**独占一行且整行相等**的 `@rem RESPROBE-ROLE:<marker>`；读取用 `keepHead = true`（脚本超过 64 KB 时"读尾部"会静默定位失败）|
| `RunProjectScript` | 超时（默认 300000 ms）用 JobObject 终止**整棵进程树**；日志写 `<OUT>\<logName>`（`CREATE_ALWAYS`，每次覆盖）；`tail` 为混合编码解码后的尾部 24 行 |
| `RunAndCapture` | 合并捕获 stdout+stderr；返回 `false` = 未正常结束（启动失败或被超时杀掉），此时 `exitCodeOut` 为 `0xFFFFFFFF` |
| `ReadLogTail` | 首选 `CP_UTF8` 严格解码，失败才走 `DecodeMixed`；**丢弃末尾未写完的半行** |
| `AcquireGuardLock` | 幂等；返回 `OtherProcess` 时调用方必须**放弃守卫动作**（而不是退出进程）|
| `IsGuardOwnedByOther` | 本进程未取锁时只 `OpenMutexW` 查询存在性，**绝不索取所有权**（否则 `selftest` / `ensure` 这类只读路径会把真守卫顶掉）|
| `ApplyResolution` | 作用于**主输出**（`FindPrimaryGdiDevice()`）；失败按 `OutputNature` 归因（§4.8.9）|
| `QueryTrayStatus` | 单轮 `EnumerateDisplays(true)` + 一次 `EvaluateTopology` 并复用结果，实测约 140 ms（不要退回"多轮全量枚举"）|

---

## 7. 部署与启动

### 7.1 构建

```bat
cd /d d:\nodejs\分辨率
编译.bat                 :: 等价于 mingw32-make
编译.bat clean           :: 清理
```

`编译.bat` 会注入 `MINGW_BIN`（默认 `C:\mingw64\bin`）到 PATH。若 MinGW 装在别处：

```bat
set MINGW_BIN=D:\mingw64\bin && 编译.bat
```

产物：`resprobe.exe`（控制台子系统，务必**不要**加 `-mwindows`）。

### 7.2 推荐流程（脚本方式，优先用这套）

```bat
:: 首次部署（幂等，可重复执行）
安装.cmd

:: 注册开机自启（默认：常驻守卫）
开机自启.cmd

:: 立刻启动常驻守卫（不等下次登录）
常驻守卫.cmd /bg

:: 无显示器 / 显示器不再接回：一次性把虚拟屏定为唯一主屏
一次性启动.cmd

:: 卸载
卸载.cmd            :: 保留驱动，可重装
卸载.cmd /full      :: 连驱动一起移除
```

### 7.3 底层命令（排查与手工操作用）

```bat
:: 诊断（只读，无副作用）
resprobe.exe snapshot
resprobe.exe paths --verbose
resprobe.exe verify --peer samples\peer-degraded-1024x768.json

:: 幂等拓扑调整（脚本内部用的就是它，有退出码约定）
resprobe.exe ensure
resprobe.exe ensure --mode promote
resprobe.exe ensure --mode mirror

:: 监听与自动守卫
resprobe.exe watch --duration 0 --interval 10 --guard

:: 手动修复（会改变系统状态，需 --yes）
resprobe.exe vdisplay --yes --mirror      :: 显示器常开：镜像，桌面恒 1920x1080
resprobe.exe vdisplay --yes --promote     :: 显示器会断电：提升虚拟屏 + 摘除死输出
resprobe.exe restore --reference 1920x1080 --yes

:: 需要留证据时才写 JSON（默认不落盘）
resprobe.exe watch --duration 120 --interval 10 --guard --out repro.json

:: 直接操作虚拟屏（脚本已封装，一般不需要手工执行）
driver\usbmmidd_v2\deviceinstaller64.exe enableidd 1
driver\usbmmidd_v2\deviceinstaller64.exe enableidd 0
```

### 7.4 输出策略

- **默认不写任何 JSON。** 只有显式给出 `--out` 才落盘，避免污染项目目录。
- 所有落盘文件统一进 **`OUT\`**（可用 `--out-dir` 覆盖，或用带目录的 `--out` 指定绝对路径）。
- 脚本的日志也统一进 `OUT\`：`guard.log`（守卫）、`tray-install.log` 等。
- `resprobe.exe` 自己的控制台输出是 **UTF-8**，但**脚本日志文件不是单一编码**：
  `cmd` 回显中文路径/文件名、以及 `schtasks` 等系统工具会按 OEM 936 写入 GBK。
  所以 `Get-Content xxx.log -Encoding UTF8` 会把 GBK 那半解坏，去掉 `-Encoding`
  则把 UTF-8 那半解坏 —— **两种单编码读法都不可能全对**。
  正确读法是托盘的「查看最近的日志」（内部走 `DecodeMixed()`），
  或代码里用 `ReadLogTail()`。详见 §4.8.7。

### 7.5 新成员快速上手（从零到跑通，约 15 分钟）

**前置**：Windows 10/11 x64、**管理员**权限、MinGW-w64（默认 `C:\mingw64\bin`）。

本工具是"管理员工具"：清单里是 `requireAdministrator`，所以**所有**命令都应在**已提升**的
控制台里执行。从非提升控制台启动会另起一个提升进程 —— stdout 与你关心的退出码都会丢
（详见 §4.8.3），这是预期行为。

```bat
:: 0) 确认在提升的控制台里（fltmc 能成功即已提升）
fltmc

:: 1) 编译（产物 resprobe.exe，控制台子系统）
cd /d d:\nodejs\分辨率
编译.bat
::    退出码现在真实反映 make 的结果（0=成功），可安全放进脚本/CI

:: 2) 只读体检：先看清当前状态，不做任何改动
resprobe.exe snapshot
resprobe.exe paths --verbose

:: 3) 动作层自检（托盘菜单无法自动点击，这是它唯一的脚本化回归入口）
resprobe.exe selftest
::    退出码 = 失败项数量，期望 0

:: 4) 幂等拓扑：期望 0（"兜底能力已就位"）
resprobe.exe ensure --reference 1920x1080

:: 5) 双端比对冒烟（用仓库自带样例）
resprobe.exe verify --peer samples\peer-degraded-1024x768.json
```

跑通后应看到的关键行：`[判定] code=…`、`可用虚拟显示输出: N 个`、
`[ensure] 已就绪，无需调整。`、`自检通过：全部动作路径可用。`

**必须知道的 4 件事**（不知道就很容易"看不懂现象"）：

| 事项 | 说明 |
|---|---|
| 守卫会锁住 exe | 有守卫/托盘在跑时 `mingw32-make` 会报 `cannot open output file resprobe.exe: Permission denied`。**先** `taskkill /im resprobe.exe /f` 再编译 |
| 只有 `--yes` 才改系统 | `vdisplay` / `restore` 不带 `--yes` 只做预检；`tray` **默认开守卫**（会改拓扑），只想拿它当查看面板要加 `--no-guard` |
| 日志是混合编码的 | `OUT\*.log` = UTF-8(resprobe) + GBK(cmd / schtasks)，必须用托盘「查看最近的日志」或 `ReadLogTail()` 读（§4.8.7）|
| 本机大概率处于复制（镜像）拓扑 | 此时"输出性质"是 `Mixed`、多个目标共用同一个 `gdiDeviceName`，**所有基于名字的判断都会失真**（§4.7.5 / §4.8.9）|

### 7.6 本地回归清单（改完代码必跑，约 10 分钟）

| # | 命令 | 期望 | 覆盖什么 |
|---|---|---|---|
| 1 | `taskkill /im resprobe.exe /f`，然后 `mingw32-make` | `Build OK: resprobe.exe`，**零新增告警** | 编译门禁（`-Wall -Wextra`）|
| 2 | `resprobe.exe selftest` | 退出码 `0` | 脚本定位 / 脚本执行+日志捕获 / 自启查询 / 模式列表 / 等值写入 / 状态与路径报告 / 混合解码 / 单实例锁 / 归因方向 / 超时杀进程树 / 半行日志丢弃 |
| 3 | `resprobe.exe snapshot` | 退出码 `0`，`EDID=` 与实机一致 | 拓扑 / EDID / 模式表探测 |
| 4 | `resprobe.exe ensure` | 退出码 `0`（已就绪）| 幂等判据（**不要改动 0 的语义**）|
| 5 | `resprobe.exe verify --peer samples\peer-degraded-1024x768.json` | 出现 `双端比对` 证据行 | 双端比对正向路径 |
| 6 | `resprobe.exe verify --peer <任意非对端 JSON>` | 只有 `[警告] 无法读取对端摘要文件`，**没有** `双端比对` 行 | `present` 语义（§6.10）|
| 7 | `resprobe.exe snapshot --out 'Z:\nope\x.json'` | 退出码 `1` | 落盘失败的退出码语义（§4.8.13）|
| 8 | `resprobe.exe restore --reference 16384x16384 --yes` | 失败，且消息含 `目标输出：…（性质）`、**不含**"链路已断开" | 归因方向（§4.8.9）。该档位不在任何模式表内，安全 |
| 9 | 双击 `resprobe.exe`（托盘）后从菜单退出 | 日志尾部有 `ResProbe 托盘已退出。`，且**不残留进程** | 托盘创建/回收、图标摘除 |
| 10 | `powershell -NoProfile -ExecutionPolicy Bypass -File 脚本编码修复.ps1` | 退出码 `0` | `.cmd` / `.bat` 纯 ASCII |
| 11 | `powershell -NoProfile -ExecutionPolicy Bypass -File res\check-manifest.ps1` | 退出码 `0` | manifest 合法性（改过 manifest 必跑）|

> 第 7、8 条是**反向用例**（专门验证失败路径的语义），建议保留：正向用例通过并不代表失败
> 路径正确 —— 本轮修的 `WriteOutputs` 退出码与 `RestoreReferenceMode` 归因这两个缺陷，
> 都恰好藏在失败路径里。

---

## 8. 常见问题排查

### 8.1 编译相关

| 现象 | 原因 | 处理 |
|---|---|---|
| `cannot open output file resprobe.exe: Permission denied` | 有 `resprobe.exe` 进程在运行（守卫） | `Get-Process resprobe \| Stop-Process -Force` 后重编 |
| `'DISPLAYCONFIG_RATIONAL' has no member named 'numerator'` | 字段名大小写 | 用 `Numerator` / `Denominator` |
| `'AF_INET' was not declared` | `winsock2.h` 必须早于 `windows.h`，顺序冲突 | 已改用数值常量（2 / 23），不要引入 winsock2.h |
| `'DBT_DEVICEARRIVAL' was not declared` | `WIN32_LEAN_AND_MEAN` 下 `dbt.h` 不自动包含 | `common.h` 已显式 `#include <dbt.h>` |
| `'EnumerateDisplays' was not declared` | 漏包含 `display.h` | 在 `verify.cpp` 中已补 |

### 8.2 运行时 — 探测类

| 现象 | 排查 |
|---|---|
| `snapshot` 显示 `EDID=缺失` 但显示器明明开着 | ①确认显示器走的是 HDMI/DP 且链路正常；②用 `--verbose` 看 `monitorInstanceId` 是否为空；③检查 EDID 匹配是否退化为厂商+产品码 |
| `输出数` 极大（如 146） | 正常。`QDC_ALL_PATHS` 会返回大量占位路径；默认已过滤，`totalPathCount` 才是真实总数 |
| 电源状态始终 `UNKNOWN` | 未收到电源通知，正常回退到拓扑推断值（`displayPowerStateInferred`） |
| 远程软件识别出无关条目 | 关键词过宽（如旧版 `idd` 命中 `Middleware`）。收紧 `kVendors[]` 中的关键词 |

### 8.3 运行时 — 事件类

| 现象 | 排查 |
|---|---|
| 监听期间关闭显示器完全没有事件 | ①确认关的是**物理电源键**，不是系统省电关屏（后者不断链）；②确认窗口是顶层窗口而非 message-only；③看是否打印了 `[电源] 共收到 N 条...` |
| 变化发生了但报告里 `events` 为空 | 合并窗口逻辑问题：检查 `kMaxDeferFactor` 是否被改小/去掉 |
| 分辨率掉下去又恢复被判 `NOT_REPRODUCED` | 判定退化成首尾对比了。必须用整条时间线（`VerdictInput::events`） |

### 8.4 运行时 — 回写与虚拟屏

| 现象 | 原因 | 处理 |
|---|---|---|
| `CDS_TEST=预检通过` 但回写 `DISP_CHANGE_FAILED` | 链路已断，驱动拒绝在无接收端时提交模式 | **硬件限制，软件绕过不了**，只能靠虚拟屏兜底 |
| `SetDisplayConfig` 返回 `87` (`ERROR_INVALID_PARAMETER`) | `SDC_ALLOW_CHANGES` 与 `SDC_TOPOLOGY_*` 同用；`SDC_VALIDATE` 同理 | 用 `SDC_TOPOLOGY_*` 时**不要**带 `SDC_ALLOW_CHANGES`；只有配 `SDC_USE_SUPPLIED_DISPLAY_CONFIG` 时才加 |
| 守卫报 `DISP_CHANGE_SUCCESSFUL` 但死输出仍在 | 用了 `ChangeDisplaySettingsEx(NULL)` 摘除 | 必须走 `DetachOutputs()`（`SetDisplayConfig`） |
| 守卫反复空转 | 摘除未生效导致 `deadPhysical` 恒为真 | 同上；已加 8 秒冷却 |
| `vdisplay` 报「没有可用显示接收端」 | 虚拟显示器尚未创建（`targetAvailable` 全为 `false`） | 先跑 `deviceinstaller64 enableidd 1` |
| 镜像后桌面被压到 1024×768 | 克隆取共同分辨率，虚拟屏当时还是 1024×768 | 先把虚拟屏设到 1920×1080 再克隆 |
| 桌面上多出好几块屏 | `enableidd 1` 跑多次，最多 4 块 | `deviceinstaller64 enableidd 0` 逐块移除；或让守卫自动摘除 |
| 重启后虚拟屏消失 | usbmmidd 不跨重启保留 | 执行 `开机自启.cmd` 注册登录任务 |
| `ensure` 报 `虚拟 0，断链物理 2` 但实际上虚拟屏在跑 | 克隆拓扑下 DisplayConfig 把克隆目标报在**源适配器**上，`gdiDeviceString` / `adapterDevicePath` 都识别不出虚拟屏 | 已用「无 EDID 且分辨率不低于参考值」作为兜底判据（见 §4.7.5） |
| `adapterDevicePath` 对多个不同适配器返回同一个路径 | `DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME` 的 **`header.id` 必须为 0**，传 target id 会拿到错误结果 | 已修正，见 §4.1 |

### 8.5 环境相关经验

- **向日葵自带 OrayIddDriver 无法由外部激活**。驱动 `Started`、`enable_idd=1`、暴露 100 条占位路径，但 `targetAvailable` 全为 `false`。虚拟显示器必须由向日葵用户态程序（主控端远控工具栏「虚拟屏/多屏」）按需实例化。因此本项目改用 usbmmidd。
- 本机同时装有 VMware Workstation，设备树里会出现 `PCI\VEN_15AD&DEV_0405`（VMware SVGA，显示为「Microsoft 基本显示适配器 / Disconnected」）的残留设备，**与本工具无关**。
- **本机当前处于复制（镜像）拓扑**：同一个 GDI 源名下同时挂着虚拟目标与物理目标，
  因此输出性质判定结果是 `Mixed`，且 `gdiDeviceName` **不能**再当唯一标识键。
  调试判定相关代码时请以这个状态为基准 —— 它正是最容易触发误报的状态。

### 8.6 脚本相关

| 现象 | 原因 | 处理 |
|---|---|---|
| 运行 `.cmd` 报 `锘緻echo off 不是内部或外部命令` | 文件带 UTF-8 **BOM**，首行被破坏 | 跑 `脚本编码修复.ps1`（去 BOM） |
| 运行 `.cmd` 报 `'xxx' 不是内部或外部命令`，且 `xxx` 是某条 `rem` 注释的后半截 | 文件里有**非 ASCII 字符**，cmd 按 OEM 代码页(936)解析时字节偏移错位 | 跑 `脚本编码修复.ps1`，它会报出具体字符与位置；改成纯 ASCII |
| 运行 `.cmd` 报 `) was unexpected at this time` | `if (...)` / `for ... do (...)` 块**空**了，或有未配对的括号 | 检查块体是否被编辑误删；括号内的字面括号要写 `^(` `^)` |
| 脚本输出的中文变乱码 | 脚本里写了中文（内容必须纯 ASCII） | 中文放文档，脚本用英文 |
| 日志里脚本输出乱码、但 `resprobe.exe` 的输出正常 | 内容仍含非 ASCII，两种编码混进同一个日志 | 同上；修好后日志统一为 UTF-8 |
| `call :Label` 找不到标签 | 批处理的 `call :label` **不能跨文件**调用 | 用独立脚本，或把逻辑内联 |
| `timeout /t N` 报 `Input redirection is not supported` | `timeout` 要求 stdin 是控制台；脚本输出被重定向时必然失败 | 改用 `powershell -NoProfile -Command "Start-Sleep -Seconds N" >nul 2>&1` |
| 链式命令里 `echo %ERRORLEVEL%` 永远打印 0 | `%VAR%` 在**整行解析时**就展开了，读到的是执行前的旧值 | 用 `cmd /v:on /c "... & echo !ERRORLEVEL!"`，或把命令拆成多行分别读 |
| 脚本自己匹配到了自己的标记 | `findstr` 未锚定行首，命中了注释里提到的标记文字 | 加 `/b` 锚定行首，并 `if /i not "!CAND!"=="%~f0"` 跳过自身 |
| `.ps1` 里中文乱码 | Windows PowerShell 5.1 对无 BOM 的 `.ps1` 按 ANSI 读取 | 本项目的 `.ps1` 刻意保持纯 ASCII |
| 进程**启动时**报 `The application has failed to start because its side by side configuration is incorrect` | manifest 是非法 XML。最常见原因是 **XML 注释里出现两个连续连字符**（XML 规范禁止；连在注释里"说明不要这么写"都会触发），其次是文件带 BOM | 跑 `res\check-manifest.ps1` 定位；构建已自动执行该校验。注意这个错误只在运行时暴露，构建不会失败 |
| `make icon` 生成的 `.ico` 只有一百多字节，`System.Drawing.Icon` 加载失败 | PowerShell 的 `BinaryWriter.Write()` 把 `byte[]` 表达式解析成**单字节**重载，只写了 1 个字节 | 用三参数重载 `Write(bytes, 0, bytes.Length)`；`make-icon.ps1` 末尾的加载校验就是为抓这个 |
| 用 `%ERRORLEVEL%` 在链式命令里读子进程退出码总是 0 | `%VAR%` 在**整行解析时**展开，读到的是执行前的旧值 | 用 `cmd /v:on` + `!ERRORLEVEL!`，或拆成多行分别读 |
| 日志中文乱码，如 `娲诲姩杈撳嚭`（其实是「活动输出」） | 日志是 **UTF-8 与 GBK 混合**的（§4.8.7）。`-Encoding UTF8` 会解坏 GBK 那半，默认编码会解坏 UTF-8 那半 | 用托盘「查看最近的日志」（走 `DecodeMixed`）或代码里的 `ReadLogTail()`；单编码读取不可能全对 |
| 自己写的解码逻辑仍然部分乱码 | "严格 UTF-8 失败就整体退回 CP_ACP"。GBK 几乎接受任何字节对，混合内容按 ACP 解**一定"成功"**，UTF-8 那半被毁 | 用 `DecodeMixed()` **逐字符**判定，不要整体切换编码 |
| `File Timeout`/`Input redirection` 类报错 | `timeout` 要求 stdin 是控制台；已被替换为 `powershell Start-Sleep` | 见上文既有条目 |
| 双击启动托盘后"什么都没有" | 控制台被隐藏、托盘图标又没建起来 | 看 `OUT\tray.log`（托盘模式已落盘，见 §4.8.10）；若日志里有"无法创建托盘图标"，说明 `explorer.exe` 未运行 |
| 启动托盘时弹出"已经有一个 ResProbe 托盘在运行" | 托盘锁 `Global\ResProbe.Tray` 已被占用（§4.8.8） | 在任务栏通知区域找已有图标，或 `taskkill /im resprobe.exe /f` |
| 菜单首行显示"守卫:其它实例" | 守卫锁已被登录任务里的常驻守卫持有，本实例只做界面 | 这是**设计行为**，保护仍在生效（由那个进程承担）；想让它归本实例就先停掉常驻守卫 |
| 托盘能不能和登录任务守卫同时跑 | 不会同时改拓扑 —— 守卫锁保证只有一个进程执行守卫动作 | 见 §4.8.8 |
| 改分辨率失败，提示"显示链路已断开" | 只有当目标输出**确实是断链物理输出**时才会这样提示（§4.8.9） | 若提示与直觉不符，看消息里的"目标输出：…（性质）"一行；克隆拓扑会显示为"克隆拓扑（虚拟+物理混合）" |
| 脚本执行超时后系统状态奇怪 | 超时会强制终止整棵进程树（§4.8.11），可能停在中间态 | 重跑一次托盘里的"安装 / 修复" |

> 一条命令检查 / 修复全部脚本：
> `powershell -NoProfile -ExecutionPolicy Bypass -File 脚本编码修复.ps1`
> 对目录下所有 `.cmd` / `.bat`：去 BOM →（必要时）GBK 转 UTF-8 → 去掉多余的
> `chcp 65001` 行 → 以 UTF-8 无 BOM 写回 → 报告残留的非 ASCII 字符。
> 全部干净退出 0，发现非 ASCII 退出 1。

### 8.7 本轮新增 / 最易混淆的几个问题

| 现象 | 原因 | 处理 |
|---|---|---|
| 运行 `常驻守卫.cmd` 时连续打印 `The process cannot access the file because it is being used by another process`（约 5 行），之后仍提示 `Resident guard started in background` | **已经有一个守卫在运行**。守卫的 stdout 由 `start /min cmd /c "… >> guard.log 2>&1"` 持有，`cmd` 以"只允许读共享"打开 `guard.log`，于是脚本里所有 `echo >>%RP_LOG%` 追加都被拒 | 正常，不是故障。用 `Get-CimInstance Win32_Process -Filter "Name='resprobe.exe'" \| Select ProcessId,CommandLine` 确认只有一个实例。要重写该日志需先 `taskkill /im resprobe.exe /f` |
| `Get-Content OUT\guard.log -Encoding UTF8` 中文乱码 | `guard.log` 是 **UTF-8（resprobe 输出）+ GBK（cmd 的 `echo %date% %time%`）** 混合的 | 用托盘「查看最近的日志」或 `ReadLogTail()`；单编码读法不可能全对（§4.8.7）。注意 `常驻守卫.cmd` 头部注释仍自称 UTF-8，那是**过时描述** |
| `mingw32-make` 报 `cannot open output file resprobe.exe: Permission denied` | 守卫/托盘正占用 exe | `taskkill /im resprobe.exe /f` 后重编（§8.1）|
| `snapshot --out …` 打印 `[输出] 无法创建输出目录` / `写入 JSON 报告失败`，且**退出码为 1** | 目标目录不可建 / 只读 / 磁盘满 | 这是 §4.8.13 引入的语义：诊断类子命令落盘失败会返回 `1`。只想看结论时去掉 `--out` 即可 |
| `verify --peer` 报 `[警告] 无法读取对端摘要文件` | ①文件不可读；②文件**不含 `primaryMode`**（即不是本工具生成的 `*.peer.json`）| 在被控端用 `resprobe.exe snapshot --out peer.json` 生成后再拷过来（§6.10）|
| `selftest` 显示 `守卫归属: 其它实例` / `其它 resprobe 进程: 1 个` | 常驻守卫在跑，本实例只做界面 | 设计行为（§4.8.8），保护仍然生效 |
| `编译.bat` 返回非 0 | make 真的失败了 | 看 make 输出；必要时 `编译.bat clean` 后重试。退出码语义见 §14（M-6）|
| 托盘「关于」里列出的 `record` 子命令不存在 | 已知文案缺陷（§9.7 的 L-3）| 有效子命令看 `resprobe.exe --help` |

---

## 9. 后续扩展注意事项

### 9.1 结构性约束（不要破坏）

1. **`common.h` 必须是每个翻译单元的第一个头文件**。它定义了 `WIN32_LEAN_AND_MEAN` 与 `_WIN32_WINNT`，晚包含会导致 `QueryDisplayConfig` 等原型缺失。
2. **所有改系统状态的调用必须收敛在 `verify.cpp`**。诊断子命令（`snapshot` / `paths` / `verify`）宁可少报，也不能有副作用。
3. **`verdict.cpp` 保持纯函数**。不要在其中调用 Win32 状态修改 API，也不要引入 I/O。
4. **不要引入第三方依赖或 CMake**。当前设计刻意保持"拷走源码 + 有 g++ 就能编译"。
5. **`.cmd` / `.bat` 的文件名可以中文，但内容必须是纯 ASCII**（UTF-8 无 BOM）。
   理由见 §3 编码模型与 §8.6。任何编辑器保存时若加了 BOM、或写进了中文（哪怕注释），
   脚本立刻失效。改完脚本必须跑一次 `脚本编码修复.ps1` 校验。
   由此推导出的两条硬约束：**不能有中文名的被 `call` 的公共脚本**（配置内联）、
   **不能在脚本里硬编码中文文件名**（用 ASCII 标记行在运行时发现，见 §3）。
6. **`ensure` 的退出码是对外契约**，脚本依赖它做幂等判断。改退出码必须同步改每个脚本里的
   `CONFIG BLOCK` 与所有调用方；尤其是 `0 = 兜底能力就位` 的语义（要求虚拟屏存在），
   不要退回成"桌面当前看着正常就返回 0"，否则 `安装.cmd` 第 6 步会漏激活虚拟屏。
7. **`ensure` 的就绪判据（`primaryMode == 参考` 且 `deadPhysicalCount == 0`）不要放宽成"只要分辨率对就行"**，否则断链死输出会长期挂在桌面上污染桌面尺寸。

### 9.2 修改判定逻辑时

- 任何新增信号都要同时处理两种情况：**有历史变化过程** 和 **只有当前状态**（后者对应首次 `snapshot`）。
- 新增 `code` 时同步更新 §4.6 的判定码表，并考虑是否需要在 `recommendations` 中给出对应建议。
- 判定优先级是"双端比对 > 单机时间线"。改动时确认没有破坏这个顺序。
- 涉及 `blame` 字段时，注意它会被上游用于自动决策，取值限定在 `link` / `remote-software` / `none` / `unknown`。

### 9.3 修改事件监听时

- **新注册通知必须容错**：任一注册失败只打印提示，不能中断监听。
- **改动合并窗口要保留上限**，否则密集事件下会漏掉整个变化过程。
- **`TakeSample` 中的守卫逻辑依赖 `last_` 更新**，如果调整采样顺序，注意 `last_ = fixed` 的赋值不能丢。
- `kMaxEvents` 的"覆盖尾条目"策略保证了有界 + 终态准确，调大上限时注意内存与报告体积。

### 9.4 修改虚拟屏相关逻辑时

- 虚拟显示输出识别依赖适配器描述字符串匹配（`LooksLikeVirtualDisplayAdapter`）。**若要支持新的 IDD 厂商，把关键词加进这个函数**，同时同步 `session.cpp` 的 `kVendors[]`。
- **死输出判据必须用 `edid.present == false`**，不要用 `monitorDeviceNodePresent`。
- `DetachOutputs` 的"不允许摘成空桌面"保护不要移除。
- 守卫的冷却窗口（8 秒）是防止抖动期反复动作的关键，调小会增加系统扰动。

### 9.5 可选的后续增强

| 方向 | 状态 | 说明 |
|---|---|---|
| 幂等安装 / 卸载 / 启动 / 自启脚本 | ✅ 已实现 | `安装.cmd` / `卸载.cmd` / `一次性启动.cmd` / `常驻守卫.cmd` / `开机自启.cmd` |
| 输出统一到 `OUT\` 且默认不落 JSON | ✅ 已实现 | `--out` 显式开启，`--out-dir` 覆盖 |
| 幂等拓扑接口 | ✅ 已实现 | `resprobe ensure`，退出码 0 / 20 / 21 / 1 |
| 常驻服务化 | 未做 | 目前是前台进程 + 登录任务。改 Windows 服务需注意**服务会话无法操作桌面显示配置**，仍须运行在交互式会话 |
| 自动切回镜像 | 未做 | 显示器重新上电后目前依赖 Windows 自愈（实测能自动恢复镜像 1920×1080）。可加「物理屏恢复健康 → 主动切回镜像」 |
| 报告离线分析 | 未做 | 可加 `resprobe analyze <report.json>` 与轻量 JSON 解析，用于复盘历史故障 |
| 多显示器场景 | 未做 | 当前假设"单物理屏 + 虚拟屏"。多物理屏时 `PromoteVirtualDisplay` 的目标挑选策略需重新设计 |
| 无 usbmmidd 时降级 | 未做 | 可检测并提示改用向日葵虚拟屏（需主控端操作）或 RDP（`RdpIdd_IndirectDisplay` 系统自带，分辨率跟随客户端） |
| 清理多余虚拟屏 | 部分 | `ensure` 目前只在 promote 路径顺带摘除；克隆态下无法安全识别。彻底清理靠 `卸载.cmd` 的 `enableidd 0` ×4 |

### 9.6 交接清单

- [ ] `编译.bat` 能产出 `resprobe.exe`
- [ ] `resprobe.exe snapshot` 输出的 `EDID` / `模式数` / `首选模式` 与实机一致
- [ ] `resprobe.exe ensure` 返回 0（已就绪）
- [ ] `安装.cmd` 重复执行两次，第二次全部走 "skip" 分支（幂等验证）
- [ ] `driver\usbmmidd_v2\` 存在且含 `deviceinstaller64.exe` + `usbmmIdd.inf` + `x64\usbmmIdd.dll`
      （**不需要**依赖 `C:\Temp`，那里随时可能被清掉）
- [ ] 两个计划任务已注册（或明确不注册）：`开机自启.cmd /status`
- [ ] **知道如何停掉守卫进程**：`taskkill /im resprobe.exe /f`
      —— 它锁着 `resprobe.exe`，不停掉就无法重新编译
- [ ] 知道脚本编码模型：文件名可中文，**内容必须纯 ASCII**（§3 / §8.6）；
      改完脚本跑一次 `脚本编码修复.ps1`，退出码应为 0
- [ ] 知道共享配置块内联在 6 个脚本里（搜 `CONFIG BLOCK`），改动需同步
- [ ] 所有落盘产物都在 `OUT\`；日志是 **UTF-8 与 GBK 混合**的，用托盘
      「查看最近的日志」或 `ReadLogTail()` 读，**不要**用单编码方式读（§4.8.7）
- [ ] 全新机器验证过一遍 `安装.cmd` 的第 4 步（创建设备节点）确实生效
- [ ] 托盘可启动且能干净退出：`resprobe.exe tray --duration 10`，日志尾部应有
      "ResProbe 托盘已退出。"，且**不残留 resprobe 进程**（残留说明图标没摘）
- [ ] 托盘动作层自检通过：`resprobe.exe selftest` 退出码 0
- [ ] `res\check-manifest.ps1` 通过（构建已自动执行）；手改过 manifest 后务必单独再跑一次
- [ ] 若改动过 `.cmd` 的角色标记，确认托盘的"安装 / 卸载 / 自启动"菜单项仍能定位到脚本
      （`selftest` 第 1 节会逐项报告）
- [ ] 知道托盘模式会落盘到 `OUT\tray.log`（隐藏控制台不代表没有日志，§4.8.10）
- [ ] 知道守卫是**跨进程互斥**的：托盘与常驻守卫不会同时改拓扑，菜单首行会写明归属（§4.8.8）
- [ ] 知道分辨率失败的归因按目标输出性质分四类，克隆拓扑下会显示为"混合"（§4.8.9）
- [ ] `selftest` 第 8 节通过（覆盖单实例锁、归因方向、超时终止、半行日志丢弃）
- [ ] 知道"给了 `--out` 但写不进去"会让诊断类子命令返回 `1`，而 `ensure` 的 `0/20/21` 不受影响（§4.8.13）
- [ ] 知道 `常驻守卫.cmd` 在"已有守卫在跑"时打印的那 5 行文件占用报错**不是故障**（§8.7）
- [ ] `git status --short` 里看不到 `build/`、`OUT/`、`*.exe`、`分辨率V*.zip`（`.gitignore` 生效）
- [ ] 读一遍 §9.7 的已知限制表，知道哪些坑是"已知未修"

### 9.7 已知限制与未修复项

状态图例：✅ 已修（本轮）｜ ⚠️ 已知未修（不阻塞主流程）｜ 🚧 发布阻塞 / 待验证

| 编号 | 现象 | 影响 | 状态 / 建议 |
|---|---|---|---|
| L-1 | `--quiet` / `-q` 被解析但全项目**未使用** | 期望"安静输出"的用户会失望 | ⚠️ 保留参数位；要么实现，要么从用法里删掉 |
| L-2 | `Report::diagnostics` 从未赋值，JSON 里恒为空串 | 报告少一项诊断信息（`EnumerateDisplays` 其实已产出 `diag` 文本）| ⚠️ 接上或删除字段 |
| L-3 | 托盘「关于」列出不存在的子命令 `record`，且漏了 `vdisplay` / `selftest` | 使用者被误导 | ⚠️ 改 `tray.cpp` 的关于文案 |
| L-4 | 死代码：`DefaultOutPath()`、`OpenFileInDefaultApp()`、`EnsureOutDir()` 定义后无调用 | 维护噪音 | ⚠️ 删除或投入使用 |
| L-5 | `SessionInfo::consoleDisplayConnected` 只写不读 | 维护噪音 | ⚠️ 同 L-4 |
| L-6 | ~~守卫的死输出判据只看 `edid.present`~~ | — | ✅ 本轮已统一为 `IsLinkHealthy()`（§4.7.3）|
| L-7 | `ArrangeDesktopLayout` 逐屏 `ChangeDisplaySettingsExW` 未带 `CDS_NORESET`，每块屏立即生效；末尾的 `(nullptr, nullptr)` 调用并非"一次性提交"语义 | 多屏重排时可能看到闪烁/中间态 | ⚠️ 若要消除，改成"全部 `CDS_NORESET` + 最后统一提交" |
| L-8 | `RunAndCapture` 收尾用**阻塞** `ReadFile` 抽干管道，而作业对象是在进程创建**之后**才分配的 | 存在"孙子进程已继承写端但未入作业"的窗口，极端情况下收尾读取长时间挂住 | ⚠️ 改成 `PeekNamedPipe` + 超时风格（前面的抽取循环已是该风格）|
| L-9 | `EnumerateDisplays` 遇到 `ERROR_INSUFFICIENT_BUFFER` 不重试，直接返回空列表 | 热插拔瞬间可能瞬时误报"没有显示输出" | ⚠️ 加 1~2 次重试 |
| L-10 | `常驻守卫.cmd` 头部注释称 `guard.log` 是 UTF-8，实际是 **UTF-8 + GBK 混合** | 按注释用 `-Encoding UTF8` 读会有一半乱码 | ⚠️ 更正注释（`echo %date%` 会写入本地化文本，单编码读法不可能全对，§4.8.7）|
| L-11 | 互斥量句柄全生命周期不释放，`IsGuardOwnedByOther` 见到对象存在即判"他人持有" | 真守卫退出后，只要还有实例持有该句柄，状态显示会短暂滞后 | ⚠️ 影响很小，可接受 |
| L-12 | `--out` 缺值时提示为 `忽略未知参数 --out` | 措辞不准确（行为安全：等于不落盘）| ⚠️ 改为"`--out` 缺少参数值" |
| L-13 | 全项目无 C++ `try/catch` / SEH | `std::bad_alloc` 会直接 `terminate`（无日志、无退出码语义）| ⚠️ 可在 `main()` 包一层，记日志后返回非 0 |
| V-1 | IPv4 客户端地址的字节偏移（`kInetAddrOffset`）**未经真实 RDP 会话验证** | 极端情况会把端口字节当成 IP 的前两段 | 🚧 验证：真实远程连接后看 `snapshot` 的 `[环境] 地址=`；不对就把常量改成 `4`（§4.8.15）|
| V-2 | 摘除死输出的"瞬时 EDID 失败"分支**未被触发验证**（需拔线/驱动重载）| 复核逻辑目前只有代码审查保证 | ⚠️ 有硬件条件时按 §10.1 流程复现一次 |
| V-3 | 服务枚举的 `ERROR_MORE_DATA` 分支未在"服务超过 64 KB"的机器上验证 | 只验证了不回归 | ⚠️ 可选：在服务较多的机器上跑 `snapshot`，看 `[远程软件]` 的 service 条目是否齐全 |
| P-1 | 项目**未声明许可证**，无 `LICENSE` 文件 | 无法对外分发、无法接受外部贡献 | 🚧 见 README §11.1 |
| P-2 | 内置 usbmmidd v2 会弹广告页，且要求在**产品文档中致谢** | 对外分发前需确认授权 / 致谢 / 广告是否可接受 | 🚧 见 README §11.2 |
| P-3 | 原先无 `.gitignore`、仓库无任何提交 | 首次提交会把构建产物与发布压缩包一起入库 | ✅ 已提供 `.gitignore`，并验证 `build/`、`OUT/`、`*.exe`、`分辨率V*.zip` 均被忽略 |
| P-4 | 清单为 `requireAdministrator`，只读命令也要 UAC | 普通用户门槛高；从非提升控制台启动会丢 stdout 与退出码 | ⚠️ 若要免提升只读入口，应拆第二个可执行文件（改 `asInvoker`）|
| P-5 | 无自动化测试、无 CI | 唯一回归入口是人工 `selftest`（需管理员 + 真实硬件）| ⚠️ 至少把 §7.6 的回归清单脚本化 |

**维护约定**：修掉某项后把状态改成 ✅ 并注明"（哪一轮修）"；新增已知问题必须登记到本表，
同时在 §14 的版本更新记录里记一笔。**不要**在正文里静默留下"已知但没写在这张表里"的缺陷。

---

## 10. 附录：验证方法与证据文件

### 10.1 三步复现与验证流程

```
① 关显示器前
   resprobe.exe snapshot

② 开着监听，物理关闭显示器，等 10 秒，再开回来
   resprobe.exe watch --duration 120 --interval 10 --guard

③ 关显示器后（保持关闭状态）
   resprobe.exe snapshot
```

**判定标准**：

| 观测项 | 正常（已修复） | 异常（未修复） |
|---|---|---|
| 主屏 | `1920×1080 @ (0,0)` | `1024×768` |
| 活动输出数 | **1**（仅虚拟屏） | 2~3（含死输出） |
| 桌面尺寸 | 恰为 `1920×1080` | `2048×768` / `3968×1080` 等 |
| `edid.present` | 虚拟屏为 `false`，物理屏已从桌面移除 | 死输出仍 `ACTIVE` |
| 守卫日志 | `DetachOutputs -> DISP_CHANGE_SUCCESSFUL (raw=0)` 且随后活动输出数=1 | 报成功但输出仍在 |

### 10.2 证据文件

> ⚠️ **2026-10-08 备注**：排查期间累积在 `OUT\evidence\` 下的那批历史报告已被清理
> （原因未查明，本机装有 360 安全软件，不排除外部清理工具所为）。这些文件都是
> `resprobe.exe` 的输出，**随时可以按 §10.1 的流程重新采集**，不影响功能。
>
> 如果后续需要长期留证，建议把证据目录移出本项目目录（例如放到 D 盘专门的数据盘），
> 避免被项目内的清理动作或安全软件波及。

**建议保留的证据**（按 §10.1 流程生成，命名后放入 `OUT\evidence\`）：

| 建议文件名 | 采集方式 | 内容 |
|---|---|---|
| `复现-首次.json` | 物理关显示器期间 `snapshot --out` | 完整根因证据链（EDID 消失、模式数 298→32、首选模式降级） |
| `守卫-接管.json` | `watch --duration 120 --interval 10 --guard --out` | `actionLog` 中守卫接管记录，判定为 `LINK_LEVEL_FALLBACK` |
| `守卫-日志.txt` | 守卫运行时的 `OUT\guard.log` | 虚拟屏提升 + 死输出摘除的逐条日志 |
| `链路断开回写.json` | 关显示器时 `restore --yes --out` | 三种 DEVMODE 变体全部 `DISP_CHANGE_FAILED` 的证据 |
| `路径盘点.json` | `paths --verbose --out` | 全量路径，含 Oray IDD / usbmmidd 适配器盘点 |

`samples\peer-*.json` 是双端比对样例夹具，**不属于证据**，保留在 `samples\`。

> 判断"修复是否生效"的判据见 §10.1 表；其中最关键的是**活动输出数必须为 1**。

### 10.3 关键 API 速查

| 用途 | API |
|---|---|
| 完整显示路径枚举 | `GetDisplayConfigBufferSizes` + `QueryDisplayConfig(QDC_ALL_PATHS, ...)` |
| 路径详情 | `DisplayConfigGetDeviceInfo`（`GET_TARGET_NAME` / `GET_SOURCE_NAME` / `GET_TARGET_PREFERRED_MODE`） |
| 顶层拓扑变更 | `SetDisplayConfig(SDC_APPLY \| SDC_TOPOLOGY_EXTEND/CLONE)` |
| 自定义配置 | `SetDisplayConfig(SDC_APPLY \| SDC_USE_SUPPLIED_DISPLAY_CONFIG \| SDC_ALLOW_CHANGES)` |
| 模式列表 / 当前模式 | `EnumDisplaySettingsExW` |
| 模式合法性预检 | `ChangeDisplaySettingsExW(..., CDS_TEST, ...)` |
| 单输出模式与位置 | `ChangeDisplaySettingsExW(..., DM_POSITION \| DM_PELSWIDTH \| DM_PELSHEIGHT, ...)` |
| EDID 读取 | `SetupDiGetClassDevsW(GUID_DEVCLASS_MONITOR)` + `SetupDiOpenDevRegKey` + `RegQueryValueExW(L"EDID")` |
| 显示器电源状态 | `RegisterPowerSettingNotification(hwnd, &GUID_CONSOLE_DISPLAY_STATE, ...)` |
| 监视器热插拔 | `RegisterDeviceNotificationW(hwnd, &filter /*GUID_DEVINTERFACE_MONITOR*/, ...)` |
| 会话信息 | `WTSQuerySessionInformationW` 系列 |

### 10.4 回归记录（第 2 轮修复，2026-10-09 实测）

环境：MSI B560M PRO-E / Intel UHD 630 / ViewSonic VA2702-H（HDMI），当前处于**复制（镜像）拓扑**。
构建：`mingw32-make`（g++ 15.2.0，`-Wall -Wextra`）→ `Build OK: resprobe.exe`，**零告警**。

| # | 用例 | 期望 | 实测 |
|---|---|---|---|
| 1 | 编译 | 通过、零告警 | ✅ 通过 |
| 2 | `selftest`（守卫停 / 在跑各一次）| 退出码 0 | ✅ `0`，第 8 节全 `[ OK ]` |
| 3 | `restore --reference 16384x16384 --yes` | 失败，且给出目标输出性质、**不得**说"链路已断开" | ✅ `DISP_CHANGE_BADMODE` + `目标输出：\\.\DISPLAY1（克隆拓扑（虚拟 + 物理混合））`，无"链路已断开" |
| 4 | `restore --reference 1920x1080 --yes` | 等值写入成功 | ✅ 回写前后均为 `1920x1080` |
| 5 | `verify --peer <可读但不含 primaryMode 的 JSON>` | 只有告警，**没有**"双端比对"证据行 | ✅ 告警有、双端比对无、判定回落 `NOT_REPRODUCED` |
| 6 | `verify --peer samples\peer-degraded-1024x768.json` | 出现双端比对 | ✅ `code=REMOTE_SOFTWARE_FAULT`（正向对照）|
| 7 | `snapshot --out <正常路径>` | 退出码 0 且文件生成 | ✅ |
| 8 | `snapshot --out 'Z:\nope\x.json'` | 退出码 `1` | ✅ `1`（修复前为 `0`）|
| 9 | `ensure --reference 1920x1080` | 退出码 0 | ✅ `[ensure] 已就绪，无需调整。` |
| 10 | EDID 校验和可见性 | 物理屏 `EDID=有效` 且 JSON `"checksumOk": true`；虚拟屏 `缺失` / `false` | ✅ |
| 11 | `编译.bat` / `编译.bat nosuchtarget` | `0` / 非 `0` | ✅ `0` / `2`（修复前构建失败也返回 `0`）|
| 12 | 守卫在跑时的互斥归属 | `已被其它实例持有`、`IsGuardOwnedByOther -> true` | ✅ |
| 13 | `.gitignore` 生效性 | 忽略 `build/`、`OUT/`、`*.exe`、`分辨率V*.zip`；保留 `samples/`、`driver/` | ✅ `git check-ignore` 逐项确认 |

**未能离线验证的三项**：V-1（需真实 RDP 会话）、V-2（需拔线/驱动重载）、V-3（需服务数量超过
64 KB 缓冲区的机器），详见 §9.7。

---

## 11. 术语表

本项目有多组近义词。**正文、代码注释、提交信息请统一使用"推荐用词"**，避免同一概念出现三种叫法。

| 推荐用词 | 代码标识 | 定义 | 易混淆 / 别名 |
|---|---|---|---|
| 显示输出（输出）| `DisplayTarget` | 一条 DisplayConfig 路径 = 一个 source + 一个 target 的组合 | 别名"路径"；**不要**与下一条"目标输出"混用 |
| 目标输出 | 动作函数参数 `gdiDevice` / `device` | 某个动作**实际作用**的那一个输出 | 与"显示输出"区分：后者泛指集合 |
| source（源）| `DISPLAYCONFIG_PATH_SOURCE_INFO` | 桌面侧的 GDI 表面（`\\.\DISPLAYn`），决定桌面尺寸 | — |
| target（目标）| `DISPLAYCONFIG_PATH_TARGET_INFO` | 真正的显示接收端（物理接口 / 虚拟屏）| — |
| 适配器 | `adapterLuid` / `adapterDevicePath` | 显卡，或软件（ROOT 枚举）显示适配器 | — |
| GDI 设备名 | `gdiDeviceName`，形如 `\\.\DISPLAY1` | `EnumDisplaySettingsEx` / `ChangeDisplaySettingsEx` 的键 | **复制拓扑下会重名**，不能当唯一键 |
| 复制（镜像）| `SDC_TOPOLOGY_CLONE`、`DISPLAY_DEVICE_MIRRORING_DRIVER` | 多个输出显示同一画面 | 别名"克隆"、"镜像"；**统一写"复制（镜像）"** |
| 扩展 | `SDC_TOPOLOGY_EXTEND` | 多输出拼成一块大桌面 | — |
| 断链死输出 | `deadPhysicalCount` / `deadPhysicalDevices` | 仍处于活动状态、但链路已断的物理输出 | 简写"死输出" |
| 虚拟显示输出 | `IsVirtualDisplayTarget()`、IDD | 由间接显示驱动（usbmmidd 等）合成的输出 | 别名"虚拟屏" |
| 链路健康 | `IsLinkHealthy()` | `edid.present && monitorDeviceNodePresent` | **唯一**判据，不要再写第二份 |
| 输出性质 | `OutputNature` | `Virtual` / `HealthyPhysical` / `DeadPhysical` / `Mixed` / `Unknown` | 失败归因的唯一依据（§4.8.9）|
| 参考分辨率 | `--reference`（默认 `1920x1080`）| 期望的健康分辨率 | 判定、验证、提升都以它为准 |
| 兜底（能力）| `ensure` 返回 `0` | 虚拟屏已就绪，物理屏断电后桌面仍为参考分辨率 | **不等于**"桌面当前看着正常" |
| 就绪判据 | `primaryMode == 参考 && deadPhysicalCount == 0` | `ensure` 判断是否免动作 | 刻意不依赖"虚拟屏识别"（§6.5）|
| 守卫 | `--guard`、`AcquireGuardLock()` | 检测到链路断开时自动提升虚拟屏的常驻逻辑 | 与"托盘"是两个关注点，各有互斥锁（§4.8.8）|
| 角色标记 | `@rem RESPROBE-ROLE:<marker>` | 脚本里独占一行的 ASCII 标记，供托盘定位脚本 | 它**不是**普通注释，删了托盘就找不到脚本（§4.8.5）|
| 混合编码 | `DecodeMixed()` | UTF-8 与 GBK 逐字符判定解码 | 日志文件的**固有**性质，不是 bug（§4.8.7）|
| 输出目录 | `<程序目录>\OUT`（`--out-dir` 可覆盖）| 所有日志与报告的落盘位置 | — |
| 事件合并窗口 | `--settle`（延长上限 3 倍）| 密集事件先合并再采样，避免漏掉整个变化过程 | §4.3.3 |
| 判定码 / 责任方 | `Verdict::code` / `blame` | 结论标识与归因方（`link` / `remote-software` / `none` / `unknown`）| 码表见 §4.6 |

---

## 12. 代码规范与协作约定

> 协作流程（分支策略、提交信息规范、PR 流程与检查清单）见 **README §10**；本节只规定
> "代码怎么写"与"改一处必须同步改哪里"。两者不重复。

### 12.1 语言与编译约定

| 项 | 约定 |
|---|---|
| 语言标准 | C++17；不使用 C++20 及以上特性（MSVC 2019 兼容是硬要求）|
| 编译选项 | 必须通过 `-Wall -Wextra`：**零告警**。唯一豁免是 `-Wno-unused-parameter` |
| 依赖 | **禁止**引入第三方库、包管理器或 CMake。只能用 Windows 系统库（§2.2）|
| 链接 | 保持 `-static-libgcc -static-libstdc++` |
| 子系统 | **不要加 `-mwindows`**：`.cmd` 脚本依赖"stdout 可重定向 + 退出码即契约"（§4.8.2）|
| 单元粒度 | 一个 `.cpp` 对应一个 `.h`（`main.cpp` 除外）；跨模块只通过头文件暴露 |

### 12.2 命名规范

| 对象 | 规则 | 示例 |
|---|---|---|
| 文件名 | 全小写，无下划线 | `display.cpp`、`verify.h` |
| 类型 / 结构体 / 类 | PascalCase | `DisplayTarget`、`Watcher`、`TrayIcon`、`ModeProbe` |
| 函数（含自由函数）| PascalCase（Win32 风格）| `EnumerateDisplays()`、`EvaluateVerdict()`、`LogLine()` |
| 纯数据结构的字段 | 小驼峰 | `DisplayTarget::gdiDeviceName`、`EdidInfo::checksumOk` |
| 有行为类的成员 | 小驼峰 + **尾下划线** | `Watcher::hwnd_`、`TrayIcon::status_` |
| 局部变量 / 参数 | 小驼峰 | `refW`、`totalPaths`、`snap` |
| 编译期常量 | `k` + PascalCase | `kMaxEvents`、`kRolePrefix`、`kInetAddrOffset` |
| 文件级全局量 | `g_` 前缀 | `g_locks`、`g_stopRequested` |
| 枚举值 | PascalCase | `DisplayPowerState::Off`、`OutputNature::DeadPhysical` |
| 宏 | 全大写 | `RESPROBE_VERSION`、`RESPROBE_NAME` |
| 匿名命名空间辅助函数 | PascalCase（明确"内部实现"）| `LooksLikeVirtualAdapterText()`、`TryDecodeWhole()`、`TailLines()` |

### 12.3 文件与头文件规范

1. `#pragma once`（不用传统 include guard）。
2. **`common.h` 必须是每个翻译单元的第一个 `#include`**（见 §9.1 第 1 条）。
3. 头文件必须自洽：只包含自己用到的头（当前风格是 `#include "common.h"` + 必要的模块头）。
4. 声明与实现一一对应；**新增公开函数必须写进对应头文件**（不要靠"同文件内先定义后使用"绕过）。
5. 实现细节放**匿名命名空间**，不要用 `static` 污染文件作用域语义。
6. 资源脚本 / manifest 保持纯 ASCII（`windres` 对代码页敏感）。

### 12.4 错误处理与日志规范

| 项 | 约定 |
|---|---|
| 返回值检查 | **所有** Win32 调用必须检查返回值。失败要么降级并记日志，要么向上返回错误码 |
| 不使用异常 | 项目风格是"返回码 + 日志"（无 `try/catch`；`bad_alloc` 的兜底见 §9.7 L-13）|
| 成对释放 | 每个 `CreateXxx` 都要有对应的 `CloseHandle` / `RegCloseKey` / `SetupDiDestroyDeviceInfoList`；失败分支同样要释放（这是历史上**没有**出现泄漏的原因，请保持）|
| 对象资源 | 类自己管自己的资源：析构函数负责清理（`Watcher` / `TrayIcon` 已如此），并注意"销毁后把句柄置空"防悬垂 |
| 输出统一走 `LogLine()` | 不要直接 `printf` / `std::cout`：托盘模式依赖 `_wfreopen` 重定向 stdout 才能留下 `tray.log` |
| 日志前缀 | 用 `[模块]` 标识来源，例如 `[ensure]` `[守卫]` `[输出]` `[回写]` `[路径]` `[虚拟屏]` `[判定]` `[电源]` `[事件]` |
| 用户可见文案 | **中文**，且必须包含"做了什么 + 结果 + 下一步"；失败消息必须给出目标对象与原因（例如"目标输出：…（性质）"）|
| 失败归因 | 一律基于 `OutputNature`，**禁止**用"看到某个字段是空值"直接下因果结论（§4.8.9 的教训）|
| 退出码 | 遵守 §5.5 与 §6.5；契约型退出码不因落盘失败而变（§4.8.13）|

### 12.5 注释规范

- **解释"为什么"，而不是"是什么"**。`// i++` 这类注释一律不写。
- **凡是被踩过的坑，必须把坑写进注释**：现象、根因、为什么当前写法是安全的。这是本项目注释密度的来源，也是它最有价值的部分，不要删。
- 反直觉的常量（`kInetAddrOffset`、`kMaxDeferFactor`、`8 秒冷却`、`64 轮上限`）必须有注释说明取值理由。
- 引用本文档时写明小节号（如 `见 §4.8.9`），便于跳转。
- 中文注释 + 中文文案；代码标识符全英文。

### 12.6 编码与文案规范

| 文件类型 | 编码要求 | 理由 |
|---|---|---|
| `*.cpp` / `*.h` | UTF-8（**不带 BOM**），允许中文注释与文案 | 编辑器与 MSVC/MinGW 都能正确处理 |
| `*.cmd` / `*.bat` | **纯 ASCII**（UTF-8 无 BOM），文件名可中文 | `cmd.exe` 按 OEM 代码页**按字节偏移**重读批处理，多字节字符会让偏移错位（§3 编码模型）|
| `*.ps1` | **纯 ASCII** | Windows PowerShell 5.1 对无 BOM 的 `.ps1` 按 ANSI 读取 |
| `*.md` | UTF-8 | 中文文档 |
| `*.rc` / `*.manifest` | 纯 ASCII | `windres` / SxS 加载器对代码页敏感 |
| 日志文件 | **混合编码是正常现象**，读取走 `ReadLogTail()` / 托盘「查看最近的日志」 | §4.8.7 |

### 12.7 结构性约束检查表

改代码后逐条自查（与 §9.1 同源，这里做成可勾选形式）：

- [ ] `common.h` 仍是每个 TU 的第一个 include
- [ ] **没有**在 `verify.cpp` 之外调用状态修改 API（`ChangeDisplaySettingsEx(W)` / `SetDisplayConfig` / `DetachOutputs` 等）
- [ ] `verdict.cpp` 仍然是纯函数（无 Win32 状态修改、无 I/O）
- [ ] 没有引入第三方依赖 / CMake / `-mwindows`
- [ ] 没有新增"第三份判据"：链路健康走 `IsLinkHealthy()`，性质走 `ClassifyOutputNature()`
- [ ] 改了 `.cmd` / `.ps1` 内容 → 已跑 `脚本编码修复.ps1`（退出码 0）
- [ ] 改了 manifest → 已跑 `res\check-manifest.ps1`（退出码 0）
- [ ] 失败路径给了反向用例（本项目的缺陷多数藏在失败路径里）

### 12.8 影响面速查：改一处必须同步改哪里

| 改动点 | 必须同步的位置 |
|---|---|
| `RESPROBE_VERSION` | `src/common.h` ＋ `res/resprobe.rc`（`FILEVERSION`、`VERSIONINFO` 的 `FileVersion`/`ProductVersion`，共 3 处）＋ `res/resprobe.manifest` 的 `assemblyIdentity version` ＋ README §13 |
| `ensure` 的退出码语义 | 6 个 `.cmd`/`.bat` 的 `CONFIG BLOCK` ＋ `PrintUsage()` ＋ 本文档 §5.5/§6.5 ＋ README §5.4 ＋ 三个调用点脚本（`安装.cmd` 第 6/7 步、`一次性启动.cmd`、`常驻守卫.cmd`）|
| 新增 / 改子命令、选项 | `main.cpp` 的参数解析与 `PrintUsage()` ＋ 本文档 §5.1 ＋ README §5.2/§5.3 ＋ 用到它的 `.cmd` |
| 新增托盘菜单项 | `tray.cpp` 的 `MenuId`（**避开** `kIdResBase..kIdResLast` 区间）＋ `ShowContextMenu()` ＋ `HandleCommand()` ＋ `RunActionSelfTest()`（§6.11）|
| 新增可被托盘调用的脚本 | 脚本内**独占一行** `@rem RESPROBE-ROLE:<marker>` ＋ `RunActionSelfTest()` 的 roles 数组 ＋ README §9 的表 ＋ 跑 `脚本编码修复.ps1` |
| 改脚本共享配置（路径/参考分辨率/任务名/退出码）| 6 个 `.cmd`/`.bat` 的 `CONFIG BLOCK`（内容必须纯 ASCII，因此**不能**用中文名的公共配置文件，只能内联复制）|
| 新增 IDD / 远程软件厂商关键词 | `display.cpp` 的 `LooksLikeVirtualAdapterText()` ＋ `session.cpp` 的 `kVendors[]`（**注意**：短关键词会误命中，如 `idd` 命中 `Middleware`）|
| 改判定逻辑 / 新增判定码 | 本文档 §4.6 判定码表 ＋ `recommendations` ＋ §9.2 |
| 改结构体字段 | §13 数据模型 ＋ §6.9 / §6.10 的 JSON 结构 ＋ `report.cpp` 的对应序列化/打印函数 |
| 改 manifest | 跑 `res\check-manifest.ps1` 并重新编译（SxS 错误只在**进程启动时**暴露，编译不会失败）|
| 改图标 | `mingw32-make icon` 重新生成 `.ico`（脚本末尾有加载校验）＋ 重新编译 |
| 改输出目录 / 落盘策略 | `report.cpp` 的 `ResolveOutPath()` ＋ `actions.cpp` 的 `GetOutDirPath()` ＋ §7.4 ＋ README §8 |
| 修掉 §9.7 的某一项 | 状态改 ✅（注明轮次）＋ §14 记一笔 ＋ 若影响使用者则同步 README §12 |
| 任何行为变更 | §14 版本更新记录 ＋ README §13 版本记录 |

### 12.9 提交前自检

1. `taskkill /im resprobe.exe /f` → `编译.bat`（零告警）
2. `resprobe.exe selftest` → 退出码 `0`
3. 跑 §7.6 的**本地回归清单**（改动触及哪条就跑哪条；失败路径务必跑第 7、8 条）
4. 对照 §12.7 的检查表与 §12.8 的影响面清单
5. `git status --short` 确认没有 `build/`、`OUT/`、`*.exe`、发布压缩包

---

## 13. 数据模型与字段说明

所有结构体都定义在 **`src/common.h`**（唯一的公共数据头），字段名与 JSON 报告里的键一一对应
（映射关系见 §6.9 / §6.10）。改字段时按 §12.8 同步 §6、§13 与 `report.cpp`。

### 13.1 数据流总览

```
命令行选项 ─▶ Options ─┐
                       ├─▶ EnumerateDisplays() ─▶ DisplayTarget[] ─▶ DisplaySnapshot ─┐
SetupAPI EDID ─────────┘                                                             │
WTS / 进程 / 服务 ─────▶ SessionInfo + RemoteSoftwareInfo[] ─────────────────────────┼─▶ VerdictInput ─▶ Verdict
窗口消息事件 ──────────▶ EventRecord[]（每条形如 {trigger, changedFields, transitions, snapshot}）┘
对端摘要文件 ──────────▶ PeerSummary ────────────────────────────────────────────────┘
动作（ensure/restore/vdisplay/守卫）─▶ TopologyState / ResolutionResult / RestoreOutcome / PromoteOutcome
全部汇入 ─▶ Report ─▶ WriteJsonReport() → OUT\*.json ；WritePeerSummary() → *.peer.json
```

### 13.2 `Options`（命令行选项）

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `mode` | `wstring` | — | 子命令：`snapshot` / `watch` / `verify` / `paths` / `ensure` / `vdisplay` / `restore` / `tray` / `selftest` |
| `outPath` | `wstring` | 空 | 空 = **不落盘**；纯文件名会落到输出目录，含目录/盘符则原样使用 |
| `outDir` | `wstring` | 空 = `<程序目录>\OUT` | 输出目录覆盖 |
| `peerPath` | `wstring` | 空 | 对端 `*.peer.json` |
| `ensureMode` | `wstring` | `auto` | `auto` / `mirror` / `promote`（非法值会警告并回落 `auto`）|
| `referenceWidth` / `referenceHeight` | `UINT32` | `1920` / `1080` | 参考健康分辨率 |
| `durationSec` | `int` | `0` | `watch` 时长上限，`0` = 直到 Ctrl+C |
| `intervalSec` | `int` | `30` | 心跳采样间隔，`0` = 关闭 |
| `settleMs` | `int` | `800` | 事件后等待驱动稳定的毫秒数（合并窗口上限 = ×3）|
| `cdsTest` | `bool` | `true` | 是否执行 `CDS_TEST` 只读探测 |
| `confirmed` | `bool` | `false` | `--yes`：允许回写 / 改拓扑 |
| `promote` | `bool` | `false` | `vdisplay --promote` |
| `mirror` | `bool` | `false` | `vdisplay --mirror` |
| `guard` | `bool` | `false` | `watch --guard`；`tray` 默认被置为 true（除非 `--no-guard` 或守卫权已被他人持有）|
| `noGuard` | `bool` | `false` | `tray` 专有：只提供界面。**其它子命令会收到警告并被忽略** |
| `quiet` | `bool` | `false` | ⚠️ 解析后**全项目未使用**（§9.7 L-1）|
| `verbose` | `bool` | `false` | 更详细输出；`paths` 下会打印占位路径 |

### 13.3 `SessionInfo`（会话环境）

| 字段 | 说明 |
|---|---|
| `remoteSession` | `GetSystemMetrics(SM_REMOTESESSION)` |
| `sessionId` / `activeConsoleSessionId` | 当前进程会话 ID / 活动控制台会话 ID（`0xFFFFFFFF` = 无）|
| `isConsoleSession` | 当前进程是否运行在活动控制台会话上 |
| `protocolType` / `protocolName` | `0` Console / `1` ICA / `2` RDP（查询失败时按 `remoteSession` 猜 `2` 或 `0`）|
| `clientName` / `clientAddress` / `connectState` / `connectStateName` | 客户端名、客户端地址（IPv4 偏移见 §4.8.15）、`WTS_CONNECTSTATE_CLASS` 名称 |
| `userName` / `domainName` / `winStationName` | 用户 / 域 / 工作站名 |
| `consoleDisplayConnected` | `WTSGetActiveConsoleSessionId()` 有效。⚠️ 目前只写不读（§9.7 L-5）|
| `elevated` | 令牌是否提升（`TokenElevation`）|

### 13.4 `EdidInfo`（EDID 证据）

| 字段 | 说明 |
|---|---|
| `present` | 注册表里是否读到 `EDID` 值 |
| `valid` | **头部**（8 字节魔数 `00 FF FF FF FF FF FF 00`）与长度是否合法 |
| `checksumOk` | 第一个 128 字节校验和是否为 0（与 `valid` **独立**，见 §4.8.14）|
| `registryPath` | 读取来源（设备实例 ID），也是 JSON 里的 `deviceInstanceId` |
| `manufacturer` | 三字母厂商代码（由 `pnpIdRaw` 解出）|
| `pnpIdRaw` / `productCodeRaw` | EDID[8..9] / EDID[10..11]，与 DisplayConfig 的 `edidManufactureId`/`edidProductCodeId` 同源 |
| `serialNumber` | EDID[12..15]（32 位）|
| `monitorName` / `serialText` | 描述符 `0xFC` / `0xFF` 的文本 |
| `preferredWidth` / `preferredHeight` / `preferredPixelClock10kHz` | 描述符 0 的首选时序 |
| `extensionCount` | EDID[126] 扩展块数量 |
| `byteCount` / `rawHex` | 原始字节数 / 十六进制原文（**仅活动输出**保留，最多 256 字节）|

### 13.5 `DisplayTarget`（一条输出路径 —— 核心模型）

**路径与适配器**

| 字段 | 说明 |
|---|---|
| `active` | `DISPLAYCONFIG_PATH_ACTIVE` |
| `targetAvailable` | 该输出当前是否有可用显示接收端。**这是判断"能否把这块屏扩展进桌面"的直接依据** |
| `targetStatusFlags` / `outputTechnology` / `outputTechnologyName` | 目标状态标志 / 输出技术（HDMI、`INDIRECT_VIRTUAL`…）/ 其名称 |
| `adapterDevicePath` | 适配器设备路径（`GET_ADAPTER_NAME`，查询时 `header.id` **必须为 0**）。**复制拓扑下识别虚拟屏的可靠依据** |
| `adapterLuid` / `targetId` / `sourceId` | 适配器 LUID / 目标 ID / 源 ID |

**监视器与 EDID 关联**

| 字段 | 说明 |
|---|---|
| `monitorFriendlyName` / `monitorDevicePath` / `monitorInstanceId` | 监视器名 / 原始设备路径 / 规范化实例 ID |
| `edidManufactureId` / `edidProductCodeId` / `connectorInstance` / `friendlyNameFromEdid` | 来自 `GET_TARGET_NAME` |
| `edid` | `EdidInfo` 副本（按 `monitorInstanceId` 精确匹配，见 §4.2）|
| `monitorDeviceNodePresent` | 监视器设备节点是否仍存在于系统（与 `edid.present` 一起构成 `IsLinkHealthy()`）|

**当前模式 / 首选模式 / GDI 侧**

| 字段 | 说明 |
|---|---|
| `hasSourceMode` / `width` / `height` / `pixelFormat` | 源模式（决定桌面尺寸）|
| `hasTargetMode` / `refreshNumerator` / `refreshDenominator` | 目标模式刷新率（分数形式）|
| `hasPreferred` / `preferredWidth` / `preferredHeight` / `preferredRefresh*` | 驱动认为该输出"应当"使用的模式 |
| `gdiDeviceName` | `\\.\DISPLAYn`（**复制拓扑下会重名**）|
| `gdiFound` / `gdiAttached` / `gdiPrimary` / `gdiMirroringDriver` / `gdiRemote` | GDI 适配器状态标志 |
| `gdiDeviceString` / `gdiDeviceId` | 驱动描述（如 `NVIDIA GeForce …`）/ 硬件 ID |
| `gdiDesktopRectValid` / `gdiDesktopRect` | 桌面矩形是否有效 / 矩形 |

**模式表**

| 字段 | 说明 |
|---|---|
| `modeCount` / `modeListTruncated` | 模式数量 / 是否因上限被截断 |
| `referenceModeAvailable` / `fallbackModeAvailable` | 模式表里是否仍有参考分辨率 / 是否存在 1024×768、800×600、640×480 |
| `modeResolutions` | 去重后的分辨率文本（上限 64 项）|
| `gdiCurrentMode` / `gdiRegistryMode` / `gdiCurrentRefreshHz` / `gdiCurrentBitsPerPel` | GDI 侧当前模式 / 注册表模式 / 刷新率 / 色深 |
| `modeListIsGenericFallback` | 模式表是否为"无 EDID 时的通用回退列表"（**该列表通常仍含 1920×1080，因此不能只看"表里有没有参考分辨率"来判断链路退化**）|

### 13.6 `DisplaySnapshot`（一次完整快照）

| 字段 | 说明 |
|---|---|
| `timeUtc` / `uptimeMs` / `trigger` | 采集时间（UTC ISO8601）/ 开机毫秒 / 触发原因（如 `WM_DISPLAYCHANGE(1024x768 32bpp)`）|
| `powerState` | 电源通知给出的状态（`GUID_CONSOLE_DISPLAY_STATE`）|
| `powerStateInferred` | 由拓扑（EDID 可见性）推断的状态 |
| `remoteSession` / `sessionId` | 采集时的远程会话标志 / 会话 ID |
| `primaryGdiDevice` / `primaryMode` | 主输出设备名 / 其模式文本（如 `1920x1080`）|
| `totalPathCount` | 系统报告的**路径总数**（含占位路径）|
| `targets` | 有意义的输出列表（活动 / 已连接监视器）|

### 13.7 `ModeProbe`（模式可用性探测结果）

`gdiDeviceName`、`width`/`height`（被探测的目标档位）、`deviceActive`、`listedInModeList`、
`cdsTestPerformed`、`cdsTestResult`、`cdsTestResultName`、`cdsTestAccepted`、
`currentMode`、`registryMode`、`degradedModeResolutions`（模式表中所有 ≤1024×768 的档位；
为空时放宽到 ≤1280×800）。

> `cdsTestAccepted == true` **只**说明该模式在列表内合法，**不代表**当前链路实际能应用（§4.5）。

### 13.8 `EventRecord` / `Report`

`EventRecord`：`index`、`timeUtc`、`trigger`、`uptimeMs`、`changed`、
`changedFields`（机器可读，如 `\.\DISPLAY1.edidPresent`）、
`transitions`（人类可读，如 `\.\DISPLAY1: EDID present -> absent`）、`snapshot`（该时刻完整快照）。

`Report`：`opt`（本次选项）、`session`、`remoteSoftware`、`initial`（基线快照）、`events`（时间线）、
`probes`、`verdict`、`peer`、`startedUtc`/`finishedUtc`、`diagnostics`（⚠️ 恒为空，§9.7 L-2）、
`powerNotificationCount`/`powerTransitions`（电源通知原始证据，用于与分辨率变化做时间关联）、
`restoreAttempted`/`restoreSucceeded`/`restoreResultName`/`restoreMessage`/`restoreAttempts`（回写记录）、
`actionLog`（显示配置动作记录，如 `guard#1: …`）。

### 13.9 `Verdict` / `VerdictInput`

`VerdictInput`：`baseline`、`latest`、`events`（整条时间线，可为空）、`session`、`software`、
`peer`、`refWidth`/`refHeight`。**判定基于整条时间线的最差状态**，不是首尾对比（§4.6）。

`Verdict`：`code`（判定码，见 §4.6 码表）、`summary`（中文结论）、
`confidence`（`high`/`medium`/`low`）、`blame`（`link`/`remote-software`/`none`/`unknown`）、
`reasons`（依据）、`recommendations`（建议）。`blame` 取值被上游用于自动决策，**不要新增取值**。

### 13.10 动作结果结构

| 结构 | 字段 | 说明 |
|---|---|---|
| `TopologyState` | `totalActive`、`activeVirtualCount`、`healthyPhysicalCount`（`IsLinkHealthy` 的非虚拟活动输出）、`deadPhysicalCount`、`activeVirtualDevices`、`deadPhysicalDevices`、`primaryGdiDevice`、`primaryMode` | 供 `ensure` 做幂等决策（§6.5）|
| `ApplyAttempt` | `description`、`resultCode`、`applied` | 一次 DEVMODE 变体的尝试记录 |
| `ResolutionResult` | `applied`、`resultCode`、`resultName`、`device`、`before`、`after`、`nature`、`linkDead`（**仅 `nature == DeadPhysical` 时才为 true**）、`supportedModes`、`attempts`、`message` | `ApplyResolution()` 的结果 |
| `RestoreOutcome` | `attempted`、`succeeded`、`resultCode`、`resultName`、`before`、`after`、`message`、`attempts`、`nature` | `RestoreReferenceMode()` 的结果 |
| `PromoteOutcome` | `performed`（是否真做了动作）、`promoted`（虚拟屏是否真的成为主屏并通过 `ArrangeDesktopLayout`）、`virtualDevice`、`message`（含摘除/拦下数量）、`log` | `PromoteVirtualDisplay()` 的结果 |
| `ModeEntry` | `width`、`height`、`text` | 托盘分辨率菜单的一项（已按面积降序、去重、过滤 <640×480 与 <16bpp）|

### 13.11 托盘与脚本结构

| 结构 | 字段 | 说明 |
|---|---|---|
| `TrayStatus` | `primaryMode`、`primaryDevice`、`totalActive`、`activeVirtual`、`healthyPhysical`、`deadPhysical`、`virtualAvailable`、`elevated`、`remoteSession`、`guardOwnedByOther`、`verdictCode`/`verdictSummary`/`verdictConfidence`/`verdictBlame`/`verdictReasons`/`verdictRecommendations`、`degraded` | 托盘菜单首行与状态对话框的数据源。`guardOwnedByOther == true` 时必须让用户看得见（否则"我启动了却没在守卫"无法解释）|
| `ScriptRunResult` | `found`（是否定位到脚本）、`ok`（退出码是否为 0）、`exitCode`、`scriptPath`、`logPath`、`tail`（日志尾部 ≤24 行，混合编码解码后）、`timedOut`、`killed`（超时后是否成功杀树）、`elapsedMs` | `RunProjectScript()` 的结果 |
| `AutostartState` | `initRegistered`、`guardRegistered`、`initState`、`guardState`、`guardRunning`、`resprobeProcessCount` | `QueryAutostart()` 的结果（**未注册不算失败**，是环境选择）|
| `RemoteSoftwareInfo` | `name`（厂商友好名）、`kind`（`display-adapter`/`monitor-device`/`process`/`service`）、`detail`（具体证据串）、`isVirtualDisplay` | 远程 / 虚拟显示痕迹 |

### 13.12 `PeerSummary`（对端摘要）

`present`（**只有解析出 `primaryMode` 才为 true**，§6.10）、`timeUtc`、`primaryMode`、
`remoteSession`、`degraded`（对端主分辨率是否 ≤1024×768）、`sourcePath`、`rawText`（原文）。

JSON 字段（见 `samples/peer-*.json`）：`tool`、`version`、`generatedUtc`、`mode`、`hostName`、
`userName`、`remoteSession`、`sessionId`、`protocolType`、`timeUtc`、`primaryMode`、
`primaryGdiDevice`、`degraded`、`activeTargets[]`（`gdiDeviceName`、`mode`、`outputTechnology`、
`active`、`primary`、`edidPresent`、`monitorName`）。

### 13.13 枚举取值

| 枚举 | 取值 | 备注 |
|---|---|---|
| `DisplayPowerState` | `Unknown=0`、`On=1`、`Off=2`、`Dimmed=3` | ⚠️ **本枚举的数值与电源通知的数值不同**：通知里 `0=Off`、`1=On`、`2=Dimmed`（映射在 `Watcher::OnPowerSetting()` 里，别把两套数值混用）|
| `OutputNature` | `Unknown`、`Virtual`、`HealthyPhysical`、`DeadPhysical`、`Mixed` | 中文名见 `OutputNatureName()`；失败归因依据（§4.8.9）|
| `GuardOwner` | `ThisProcess`、`OtherProcess`、`Unavailable` | `Unavailable` 表示连互斥量都建不出来 → **保守按"不得守卫"处理**（§4.8.8）|
| `DISPLAYCONFIG_PIXELFORMAT` | `0=8bpp`、`1=16bpp`、`2=32bpp`、`3=NA` | `PixelFormatName()` 转名 |

### 13.14 与 JSON 报告的对应关系

- 快照 / 目标 / EDID 的字段 → §6.9 的 `initialSnapshot`、`events[].snapshot`、`targets[]`、`edid{}`。
- 判定 → `verdict{ code, blame, confidence, summary, reasons[], recommendations[] }`。
- 动作记录 → `actionLog[]`（`ensure` / `guard` / `vdisplay` 的日志）与 `restore{ … }`。
- 对端摘要 → 单独的 `*.peer.json`（结构见 §6.10，比完整报告扁平得多，便于用最简解析器读取）。

> **JSON 里的键名与 C++ 字段名不完全一致**（例如 `EdidInfo::registryPath` → `deviceInstanceId`、
> `DisplaySnapshot::targets` → 同时给出 `reportedTargetCount`）。以 §6.9 的结构图为准。

---

## 14. 版本更新记录

### 14.1 记录约定

- 版本号定义在 `src/common.h` 的 `RESPROBE_VERSION`；**同步位置共 5 处**（§12.8），改版本号必查。
- 分类用：**新增 / 修复 / 变更 / 移除 / 文档 / 构建**。
- 未发布的改动累积在 §14.2 的"未发布"里；正式发布时整理成带版本号的条目，并按 `vX.Y.Z` 打 tag。
- **每条记录必须写清**：日期、类型、涉及模块、（修复类）现象与根因、（功能类）动机、
  验证方式、以及**是否改变对外契约**（退出码 / 接口语义 / JSON 字段 / 显示文案）。
- 修掉 §9.7 里的已知问题时，把该表状态改为 ✅ 并在这里记一笔。

### 14.2 未发布（`1.0.0` 开发中）

| 日期 | 类型 | 范围 | 摘要 |
|---|---|---|---|
| 2026-10-08 | 新增 | 全部模块 | 初版功能：诊断层（拓扑 / EDID / 事件 / 会话 / 远程软件痕迹 / 双端比对 / 时间线判定）+ 兜底层（usbmmidd 虚拟屏、`ensure` 幂等拓扑、`watch --guard` 自动守卫）+ 托盘 + 5 个部署脚本 + `selftest`。详见 §4 |
| 2026-10-09 | 修复 | 见 §4.8.7–§4.8.12 | **第 1 轮代码审查修复（15 项）**：失败归因四分类（H2）／跨进程守卫与托盘双互锁（H3）／托盘落盘日志（M1）／角色标记读取方向 `keepHead`（M4）／超时终止整棵进程树（M5）／状态查询收敛为单轮枚举（M6）／更名与校验类 L 项若干。详见 §4.8.7–§4.8.12 与 §14.4 |
| 2026-10-09 | 修复 | verify / display / edid / session / main / watcher | **第 2 轮代码审查修复（10 项：H-1~H-4、M-1~M-6）**，实测证据见 §10.4，逐项说明见 §14.3 |
| 2026-10-09 | 文档 / 构建 | 仓库根 | 新增 `README.md`（使用者 + 贡献者入口）与 `.gitignore`（首次提交前必需）；本文档补齐 §7.5 / §7.6 / §9.7 / §10.4 / §11 术语表 / §12 代码规范 / §13 数据模型 / §14 版本更新记录，并把 §8.5 与 §8.6 的错序调整为正序 |

### 14.3 详细记录：第 2 轮（2026-10-09）

| 编号 | 类型 | 模块 | 变更 | 对外契约 |
|---|---|---|---|---|
| H-1 | 修复 | `verify` | `RestoreReferenceMode()` 失败时按 `OutputNature` 归因（新增 `RestoreOutcome::nature`），不再"见无 EDID 就断言链路断开" | 消息文案变化；退出码不变 |
| H-2 | 修复 | `verify` | `ArrangeDesktopLayout()` 改为返回**目标主屏那一次是否成功**，失败时 `message` 带失败码 | ⚠️ `PromoteOutcome::promoted` 可能由"虚报 true"变为真实 `false` |
| H-3 | 修复 | `verify` / `main` | `ReadPeerSummary()` 仅在解析出 `primaryMode` 时置 `present`（失败清空 `out`）；`RunVerify` 改用与 `RunWatch` 相同的判据 | ⚠️ 行为变化：指向非对端摘要时不再走双端分支 |
| H-4 | 修复 | `edid` / `report` | `EdidInfo::checksumOk` 真正落库：JSON 新增字段、控制台区分"有效/存在但校验和错误/存在但无效/缺失" | 新增 JSON 字段（向后兼容）|
| M-1 | 修复 | `verify` | 摘除死输出前**二次枚举复核**（两次都读不到 EDID 才摘），结论并入 `PromoteOutcome::message` | 行为变化：更少摘除（更安全）|
| M-2 | 修复 | `display` / `verify` / `watcher` | 新增唯一判据 `IsLinkHealthy()`，三处判定统一；`OutputNature` / `OutputNatureName` / `ClassifyOutputNature` 由 `actions` 下沉到 `display` | 头文件归属变化（编译期影响）|
| M-3 | 修复 | `main` | `snapshot` / `verify` / `paths` / `vdisplay` / `restore` 落盘失败返回 `1`；`watch` 提升为 `1`；**`ensure` 刻意不变** | ⚠️ **退出码变化**（仅诊断类子命令）|
| M-4 | 变更 | `session` | IPv4 客户端地址偏移集中为常量 `kInetAddrOffset`（保持取值 2）、注释与实现一致、全 0 显示"(未提供)" | 显示文案变化；**待真实 RDP 复核**（§9.7 V-1）|
| M-5 | 修复 | `session` | 服务枚举支持 `ERROR_MORE_DATA` 续枚举 + 按需扩容 + 轮次上限 | 无（修漏检）|
| M-6 | 修复 | `编译.bat` | 退出码改用 `!ERRORLEVEL!`（延迟展开），构建失败不再返回 `0` | ⚠️ 脚本退出码变化（可据此判断失败）|

### 14.4 详细记录：第 1 轮（2026-10-09）

第一轮的 15 项修复已在 §4.8.7–§4.8.12 逐条记录（含"问题 → 修法"表），此处只做索引：

| 分组 | 内容 | 详见 |
|---|---|---|
| 归因正确性 | `ApplyResolution` 失败不再"见无 EDID 就断言链路断开"，改为 `ClassifyOutputNature` 四分类（**克隆拓扑下为必然误报**）| §4.8.9 |
| 并发正确性 | 守卫锁与托盘锁分离（`Global\ResProbe.Guard` / `.Tray`），`IsGuardOwnedByOther` 只查询不索取所有权 | §4.8.8 |
| 可观测性 | 托盘模式把 stdout/stderr 重定向到 `OUT\tray.log`（超 2 MB 重头写）；超时提示区分"已终止/未终止" | §4.8.10、§4.8.11 |
| 进程与资源 | 超时用 JobObject 终止**整棵进程树**；`WM_NCDESTROY` 置空 `hwnd_` 防悬垂；`Create()` 图标注册失败返回 `false` | §4.8.11、§4.8.12 |
| 脚本定位与日志 | 角色标记改用"读头部"（`keepHead`）；日志读取走混合编码解码并丢弃未写完的半行 | §4.8.5、§4.8.7、§4.8.12 |
| 性能与体验 | 状态查询由 3~4 轮全量枚举收敛为单轮复用（实测 172 ms → 141 ms）；分辨率菜单超 100 项时明示截断；`ShellExecute` 返回值检查 | §4.8.12 |
| 参数与文案 | `--no-guard` 归入 `tray 专有` 并在其它模式警告忽略；`selftest` 的描述改为"等值写入" | §4.8.12 |

### 14.5 新条目模板

```markdown
| <编号> | 修复/新增/变更/移除 | <模块> | <现象与根因 → 改法> | 无 / ⚠️ <契约变化说明> |
```

修复类条目请附上实测命令与结果（可引用 §10.4 的表格行）。**只写"已修复"而不写怎么验证的，
视为记录不完整**。
