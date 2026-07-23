// codecvt.h - Windows 控制台编码转换（GBK <-> UTF-8）
//
// 为什么需要：Windows 中文控制台默认代码页 936 (GBK)，
// 但 LLM API、JSON、我们的字符串处理都用 UTF-8。
// 所以从 stdin 读进来的中文是 GBK 字节，必须转 UTF-8 才能发出去。
//
// 对应 pi：pi 在 TS/Node 里全程 UTF-8，没这问题。
// 这是 Windows C/C++ 开发的特有税。
#ifndef MINPI_CODECVT_H
#define MINPI_CODECVT_H

#include <string>

// GBK 字节串 -> UTF-8 字符串。输入是控制台读进来的原生字节。
std::string gbk_to_utf8(const std::string& gbk);

// UTF-8 字符串 -> GBK 字节串。输出给控制台打印用（如果需要）。
std::string utf8_to_gbk(const std::string& utf8);

#endif // MINPI_CODECVT_H
