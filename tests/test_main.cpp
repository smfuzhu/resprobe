// test_main.cpp - 不依赖硬件 / 管理员权限的单元测试
//
// 覆盖两条纯逻辑路径，它们此前没有任何回归入口（唯一的 selftest 需要真实
// Windows 主机 + 管理员权限，无法在流水线里跑）：
//   1) 混合编码解码（encoding.cpp）
//   2) 根因判定引擎（verdict.cpp）
//
// 构建并运行:  mingw32-make test   （产物 build/resprobe-tests.exe）
// 退出码:      0 = 全部通过, 1 = 有失败项
//
// 编码约定: 本文件只使用 ASCII 源码 + \uXXXX 转义来构造期望值，避免依赖
// 源文件编码与编译器 -finput-charset 设置；期望的 GBK 字节以原始字节数组
// 给出，不依赖本机代码页。

#include "../src/common.h"
#include "../src/encoding.h"
#include "../src/util.h"
#include "../src/verdict.h"

#include <cstdio>
#include <initializer_list>
#include <string>

using namespace resprobe;

namespace {

int g_checks = 0;
int g_failures = 0;

void CheckStr(const char* what, const std::string& got, const std::string& want) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  [FAIL] %s\n           got : %s\n           want: %s\n",
                    what, got.c_str(), want.c_str());
    }
}

void CheckWStr(const char* what, const std::wstring& got, const std::wstring& want) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  [FAIL] %s\n           got : %s\n           want: %s\n",
                    what, W2U8(got).c_str(), W2U8(want).c_str());
    }
}

void CheckTrue(const char* what, bool cond) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what);
    }
}

std::string Bytes(std::initializer_list<unsigned char> b) {
    return std::string(b.begin(), b.end());
}

// ---------------------------------------------------------------------------
// 编码用例
// ---------------------------------------------------------------------------
void TestEncoding() {
    std::printf("[编码] DecodeMixed / DecodeBestEffort\n");

    // 纯 ASCII：任何代码页下都应原样通过
    CheckWStr("ascii passthrough", DecodeMixed("hello, world"), L"hello, world");

    // 纯 UTF-8 中文（"中文测试"）：UTF-8 段必须完整保留
    const std::string utf8Chinese =
        Bytes({0xE4,0xB8,0xAD, 0xE6,0x96,0x87, 0xE6,0xB5,0x8B, 0xE8,0xAF,0x95});
    const std::wstring wantChinese = L"\u4E2D\u6587\u6D4B\u8BD5";
    CheckWStr("utf8-only via DecodeMixed", DecodeMixed(utf8Chinese), wantChinese);
    CheckWStr("utf8-only via DecodeBestEffort(CP_UTF8)",
              DecodeBestEffort(utf8Chinese, CP_UTF8), wantChinese);

    // 空输入
    CheckWStr("empty DecodeMixed", DecodeMixed(""), L"");
    CheckWStr("empty DecodeBestEffort", DecodeBestEffort("", CP_UTF8), L"");

    // 混合内容：UTF-8 段（"中文"）+ GBK 段（"活动"）。
    // GBK 字节只有在代码页 936（本项目目标环境）下才有确定的期望值；其它 ACP 下
    // 同一串字节会被解成别的字符 —— 因此这类断言仅在 936 上执行。
    const std::string utf8Prefix = Bytes({0xE4,0xB8,0xAD, 0xE6,0x96,0x87}); // 中文(UTF-8)
    const std::string gbkSuffix  = Bytes({0xBB,0xEE, 0xB6,0xAF});            // 活动(GBK)
    const std::string gbkChinese = Bytes({0xD6,0xD0, 0xCE,0xC4});            // 中文(GBK)

    if (GetACP() == 936) {
        // 关键回归：混合流里 UTF-8 那半绝不能被 GBK 解码毁成乱码
        CheckWStr("mixed utf8+gbk keeps utf8 half intact",
                  DecodeMixed(utf8Prefix + gbkSuffix), L"\u4E2D\u6587\u6D3B\u52A8");
        // 纯 GBK 走首选 ACP 的安全路径
        CheckWStr("pure gbk via DecodeBestEffort(CP_ACP)",
                  DecodeBestEffort(gbkChinese, CP_ACP), L"\u4E2D\u6587");
    } else {
        std::printf("        (跳过 GBK 用例：当前 ACP=%u，非 936)\n",
                    (unsigned)GetACP());
    }

    // 孤立/非法字节不得崩溃，且应产出结果（兜底路径生效）
    const std::string stray = Bytes({0x41, 0xB7, 0x42});
    CheckTrue("stray byte handled without crash", !DecodeMixed(stray).empty());
}

// ---------------------------------------------------------------------------
// 判定用例辅助
// ---------------------------------------------------------------------------
DisplayTarget MakePhysical(const wchar_t* gdi, UINT32 w, UINT32 h,
                           bool edid, bool node) {
    DisplayTarget t;
    t.active = true;
    t.hasSourceMode = true;
    t.gdiDeviceName = gdi;
    t.width = w;
    t.height = h;
    t.edid.present = edid;
    t.monitorDeviceNodePresent = node;
    if (edid) t.monitorDevicePath = L"\\\\?\\DISPLAY#TEST#1";
    return t;
}

DisplayTarget MakeVirtual(const wchar_t* gdi, UINT32 w, UINT32 h) {
    DisplayTarget t;
    t.active = true;
    t.hasSourceMode = true;
    t.gdiDeviceName = gdi;
    t.width = w;
    t.height = h;
    t.gdiMirroringDriver = true;   // 虚拟/间接显示驱动的强特征
    t.monitorDeviceNodePresent = false;   // 虚拟输出本就没有 EDID/设备节点
    return t;
}

DisplaySnapshot MakeSnap(const wchar_t* primary, std::vector<DisplayTarget> targets) {
    DisplaySnapshot s;
    s.primaryMode = primary;
    if (!targets.empty()) s.primaryGdiDevice = targets.front().gdiDeviceName;
    s.targets = std::move(targets);
    return s;
}

// A. 链路级回退：EDID/设备节点丢失 + 分辨率下降
void TestVerdictLinkFallback() {
    std::printf("[判定] 链路级回退\n");

    DisplayTarget b = MakePhysical(L"\\\\.\\DISPLAY1", 1920, 1080, true, true);
    b.referenceModeAvailable = true;
    b.hasPreferred = true;
    b.preferredWidth = 1920;
    b.preferredHeight = 1080;

    DisplayTarget a = MakePhysical(L"\\\\.\\DISPLAY1", 1024, 768, false, false);
    a.referenceModeAvailable = false;
    a.hasPreferred = true;
    a.preferredWidth = 1024;
    a.preferredHeight = 768;
    a.modeListIsGenericFallback = true;
    a.modeCount = 32;

    DisplaySnapshot baseline = MakeSnap(L"1920x1080", {b});
    DisplaySnapshot latest = MakeSnap(L"1024x768", {a});

    VerdictInput in;
    in.baseline = &baseline;
    in.latest = &latest;
    in.refWidth = 1920;
    in.refHeight = 1080;

    Verdict v = EvaluateVerdict(in);
    CheckStr("code", v.code, "LINK_LEVEL_FALLBACK");
    CheckStr("blame", v.blame, "link");
}

// B. 一切正常：不应误报
void TestVerdictNotReproduced() {
    std::printf("[判定] 正常未复现\n");

    DisplayTarget h = MakePhysical(L"\\\\.\\DISPLAY1", 1920, 1080, true, true);
    h.referenceModeAvailable = true;

    DisplaySnapshot baseline = MakeSnap(L"1920x1080", {h});
    DisplaySnapshot latest = MakeSnap(L"1920x1080", {h});

    VerdictInput in;
    in.baseline = &baseline;
    in.latest = &latest;

    Verdict v = EvaluateVerdict(in);
    CheckStr("code", v.code, "NOT_REPRODUCED");
    CheckStr("blame", v.blame, "none");
}

// C. 健康物理屏 + 兜底虚拟屏：虚拟屏的"无 EDID"不得被当成故障证据
void TestVerdictVirtualNoFalseAlarm() {
    std::printf("[判定] 虚拟屏兜底不误报\n");

    DisplayTarget phys = MakePhysical(L"\\\\.\\DISPLAY1", 1920, 1080, true, true);
    phys.referenceModeAvailable = true;
    DisplayTarget virt = MakeVirtual(L"\\\\.\\DISPLAY2", 1920, 1080);

    DisplaySnapshot baseline = MakeSnap(L"1920x1080", {phys, virt});
    DisplaySnapshot latest = MakeSnap(L"1920x1080", {phys, virt});

    VerdictInput in;
    in.baseline = &baseline;
    in.latest = &latest;

    Verdict v = EvaluateVerdict(in);
    CheckStr("code", v.code, "NOT_REPRODUCED");
}

// D. 降级发生在虚拟输出上：判为远程软件接管
void TestVerdictRemoteTakeover() {
    std::printf("[判定] 远程软件接管\n");

    DisplayTarget vb = MakeVirtual(L"\\\\.\\DISPLAY2", 1920, 1080);
    DisplayTarget va = MakeVirtual(L"\\\\.\\DISPLAY2", 1024, 768);

    DisplaySnapshot baseline = MakeSnap(L"1920x1080", {vb});
    DisplaySnapshot latest = MakeSnap(L"1024x768", {va});

    VerdictInput in;
    in.baseline = &baseline;
    in.latest = &latest;

    Verdict v = EvaluateVerdict(in);
    CheckStr("code", v.code, "REMOTE_TAKEOVER_NORMAL");
    CheckStr("blame", v.blame, "remote-software");
}

// E. 参考分辨率从模式表消失
void TestVerdictReferenceLost() {
    std::printf("[判定] 参考分辨率不可用\n");

    DisplayTarget b = MakePhysical(L"\\\\.\\DISPLAY1", 1920, 1080, true, true);
    b.referenceModeAvailable = true;
    DisplayTarget a = MakePhysical(L"\\\\.\\DISPLAY1", 1920, 1080, true, true);
    a.referenceModeAvailable = false;

    DisplaySnapshot baseline = MakeSnap(L"1920x1080", {b});
    DisplaySnapshot latest = MakeSnap(L"1920x1080", {a});

    VerdictInput in;
    in.baseline = &baseline;
    in.latest = &latest;

    Verdict v = EvaluateVerdict(in);
    CheckStr("code", v.code, "REFERENCE_MODE_UNAVAILABLE");
}

// F. 缺少快照
void TestVerdictNoData() {
    std::printf("[判定] 数据缺失\n");

    DisplayTarget h = MakePhysical(L"\\\\.\\DISPLAY1", 1920, 1080, true, true);
    DisplaySnapshot latest = MakeSnap(L"1920x1080", {h});

    VerdictInput in;
    in.baseline = nullptr;
    in.latest = &latest;

    Verdict v = EvaluateVerdict(in);
    CheckStr("code", v.code, "NO_DATA");
}

// G. 双端一致且均降级 + 主机观测到 EDID 丢失 -> 链路级回退
void TestVerdictPeerFallback() {
    std::printf("[判定] 双端一致且降级\n");

    DisplayTarget b = MakePhysical(L"\\\\.\\DISPLAY1", 1920, 1080, true, true);
    DisplayTarget a = MakePhysical(L"\\\\.\\DISPLAY1", 1024, 768, false, false);

    DisplaySnapshot baseline = MakeSnap(L"1920x1080", {b});
    DisplaySnapshot latest = MakeSnap(L"1024x768", {a});

    PeerSummary peer;
    peer.present = true;
    peer.primaryMode = "1024x768";

    VerdictInput in;
    in.baseline = &baseline;
    in.latest = &latest;
    in.peer = &peer;

    Verdict v = EvaluateVerdict(in);
    CheckStr("code", v.code, "LINK_LEVEL_FALLBACK");
    CheckStr("confidence", v.confidence, "high");
}

// H. 主机正常、客户端降级 -> 远程软件故障
void TestVerdictPeerRemoteFault() {
    std::printf("[判定] 客户端单端降级\n");

    DisplayTarget h = MakePhysical(L"\\\\.\\DISPLAY1", 1920, 1080, true, true);
    h.referenceModeAvailable = true;

    DisplaySnapshot baseline = MakeSnap(L"1920x1080", {h});
    DisplaySnapshot latest = MakeSnap(L"1920x1080", {h});

    PeerSummary peer;
    peer.present = true;
    peer.primaryMode = "1024x768";
    peer.degraded = true;

    VerdictInput in;
    in.baseline = &baseline;
    in.latest = &latest;
    in.peer = &peer;

    Verdict v = EvaluateVerdict(in);
    CheckStr("code", v.code, "REMOTE_SOFTWARE_FAULT");
    CheckStr("blame", v.blame, "remote-software");
}

}  // namespace

int main() {
    std::printf("ResProbe 单元测试\n");
    std::printf("=================\n");

    TestEncoding();
    TestVerdictLinkFallback();
    TestVerdictNotReproduced();
    TestVerdictVirtualNoFalseAlarm();
    TestVerdictRemoteTakeover();
    TestVerdictReferenceLost();
    TestVerdictNoData();
    TestVerdictPeerFallback();
    TestVerdictPeerRemoteFault();

    std::printf("\n共 %d 项检查, %d 项失败\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("全部通过。\n");
        return 0;
    }
    std::printf("存在失败项。\n");
    return 1;
}
