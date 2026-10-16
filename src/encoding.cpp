// encoding.cpp - 混合编码解码实现
//
// 从 actions.cpp 的匿名命名空间原样搬出，仅改了可见性（供单元测试链接）。
#include "encoding.h"

namespace resprobe {

namespace {

// 判断 bytes[i] 能否开启一个合法的 UTF-8 序列；成功时给出序列长度。
//
// 这个判据很强：0x80..0xBF 在 UTF-8 里**只能是续字节**，永远不能作为字符起始，
// 所以一个孤立的 0xB7 之类字节已经可以确定"不是 UTF-8"。再加上对过长编码、
// 代理区与码点上限的校验，误判率极低。
bool Utf8SequenceLen(const std::string& s, size_t i, int* lenOut) {
    const unsigned char c = (unsigned char)s[i];
    int need = 0;
    unsigned int cp = 0;

    if (c < 0x80) { *lenOut = 1; return true; }
    if (c >= 0xC2 && c <= 0xDF)      { need = 2; cp = c & 0x1Fu; }
    else if (c >= 0xE0 && c <= 0xEF) { need = 3; cp = c & 0x0Fu; }
    else if (c >= 0xF0 && c <= 0xF4) { need = 4; cp = c & 0x07u; }
    else return false;

    if (i + (size_t)need > s.size()) return false;

    for (int k = 1; k < need; ++k) {
        const unsigned char cc = (unsigned char)s[i + (size_t)k];
        if (cc < 0x80 || cc > 0xBF) return false;
        cp = (cp << 6) | (cc & 0x3Fu);
    }

    if (need == 3 && cp < 0x800u) return false;      // 过长编码
    if (need == 4 && cp < 0x10000u) return false;    // 过长编码
    if (cp > 0x10FFFFu) return false;
    if (cp >= 0xD800u && cp <= 0xDFFFu) return false; // 代理区

    *lenOut = need;
    return true;
}

}  // namespace

// 整体按单一代码页解码。strict=true 时任何非法字节都导致失败（返回空串）。
std::wstring TryDecodeWhole(const std::string& bytes, UINT cp, bool strict) {
    if (bytes.empty()) return std::wstring();
    DWORD flags = strict ? MB_ERR_INVALID_CHARS : 0;

    int need = MultiByteToWideChar(cp, flags, bytes.data(), (int)bytes.size(),
                                   nullptr, 0);
    if (need <= 0) return std::wstring();

    std::wstring out((size_t)need, L'\0');
    int got = MultiByteToWideChar(cp, flags, bytes.data(), (int)bytes.size(),
                                  &out[0], need);
    if (got <= 0) return std::wstring();
    out.resize((size_t)got);
    return out;
}

// 逐字符混合解码。
//
// 为什么必须混合：捕获到的脚本日志天然是两种编码拼起来的 ——
//   * resprobe.exe 输出 UTF-8（它自己 SetConsoleOutputCP(CP_UTF8) 并写 UTF-8 字节）
//   * cmd 的 echo 回显中文文件名、以及 schtasks / pnputil / reg 等系统工具，
//     按 OEM 代码页（本机 936）输出本地化文本
// 整体按任一编码解码都会毁掉另一半：严格 UTF-8 会失败并回落到 GBK，于是 UTF-8 那半
// 变成"娲诲姩杈撳嚭"这种乱码（实测发生过）。
std::wstring DecodeMixed(const std::string& bytes) {
    std::wstring out;
    out.reserve(bytes.size());

    size_t i = 0;
    while (i < bytes.size()) {
        int len = 0;

        // 1) 合法 UTF-8 序列
        if (Utf8SequenceLen(bytes, i, &len) && len > 1) {
            std::wstring one = TryDecodeWhole(bytes.substr(i, (size_t)len),
                                              CP_UTF8, true);
            if (!one.empty()) {
                out += one;
                i += (size_t)len;
                continue;
            }
        }

        const unsigned char c = (unsigned char)bytes[i];

        // 2) ASCII
        if (c < 0x80) {
            out.push_back((wchar_t)c);
            ++i;
            continue;
        }

        // 3) 尝试 GBK 双字节
        if (i + 1 < bytes.size()) {
            std::wstring two = TryDecodeWhole(bytes.substr(i, 2), CP_ACP, true);
            if (!two.empty()) {
                out += two;
                i += 2;
                continue;
            }
        }

        // 4) 兜底：单字节按 ANSI 解（GBK 首字节落单时会走到这里）
        std::wstring one = TryDecodeWhole(bytes.substr(i, 1), CP_ACP, false);
        if (!one.empty()) out += one;
        ++i;
    }
    return out;
}

// 先整体严格按首选编码解码；失败再交给逐字符混合解码器。
//
// 顺序很关键，这里有一个容易写错的地方：
// **绝不能**在首选失败后无条件退回"整体 CP_ACP 严格解码"。GBK 几乎接受任何字节对
// （UTF-8 的字节也算合法 GBK），所以混合内容按 ACP 解一定会"成功"，结果是又把
// UTF-8 那半毁成乱码 —— 等于没修。
//
// 因此：
//   首选 CP_UTF8（日志文件）  -> 纯 UTF-8 走第 1 步；混合/纯 GBK 交给混合解码器
//   首选 CP_ACP（系统工具输出）-> 纯 GBK 走第 1 步这条安全路径；
//                                ACP 都解不通时先试 UTF-8，再交给混合解码器
std::wstring DecodeBestEffort(const std::string& bytes, UINT preferred) {
    if (bytes.empty()) return std::wstring();

    std::wstring whole = TryDecodeWhole(bytes, preferred, true);
    if (!whole.empty()) return whole;

    if (preferred == CP_ACP) {
        std::wstring utf8 = TryDecodeWhole(bytes, CP_UTF8, true);
        if (!utf8.empty()) return utf8;
    }

    return DecodeMixed(bytes);
}

}  // namespace resprobe
