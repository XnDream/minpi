// codecvt.cpp - GBK <-> UTF-8 转换
//
// Windows API 转换两步走：GBK -> UTF-16 -> UTF-8（反之亦然）。
// 因为 Win32 的 MultiByteToWideChar/WideCharToMultiByte 以 UTF-16 为枢纽。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "codecvt.h"

// 通用转换：任意代码页 <-> UTF-16
static std::wstring multi_to_wide(const std::string& s, unsigned codepage) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(codepage, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(len, 0);
    MultiByteToWideChar(codepage, 0, s.c_str(), (int)s.size(), &out[0], len);
    return out;
}

static std::string wide_to_multi(const std::wstring& s, unsigned codepage) {
    if (s.empty()) return std::string();
    int len = WideCharToMultiByte(codepage, 0, s.c_str(), (int)s.size(),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(len, 0);
    WideCharToMultiByte(codepage, 0, s.c_str(), (int)s.size(),
                        &out[0], len, nullptr, nullptr);
    return out;
}

// 936 = GBK 代码页
std::string gbk_to_utf8(const std::string& gbk) {
    std::wstring wide = multi_to_wide(gbk, 936);        // GBK -> UTF-16
    return wide_to_multi(wide, CP_UTF8);                 // UTF-16 -> UTF-8
}

std::string utf8_to_gbk(const std::string& utf8) {
    std::wstring wide = multi_to_wide(utf8, CP_UTF8);    // UTF-8 -> UTF-16
    return wide_to_multi(wide, 936);                     // UTF-16 -> GBK
}
