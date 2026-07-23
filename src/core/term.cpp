// term.cpp - 终端折行打印实现
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>

#include "term.h"

int term_width() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return 80;
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(h, &info)) return 80;
    int w = info.srWindow.Right - info.srWindow.Left + 1;
    return w > 10 ? w : 80;   // 太小说明拿错了，回退 80
}

// 判断 UTF-8 字节序列的显示宽度。
// 返回该字符占几列，并通过 bytes 输出它占几个字节。
// 规则：ASCII 1 列；CJK 及全角符号 2 列；其他(emoji 等)按 1 列近似。
static int char_width(unsigned char c, int* bytes) {
    // 1 字节：ASCII
    if (c < 0x80) { *bytes = 1; return 1; }
    // 计算 UTF-8 序列长度
    int len;
    if ((c & 0xE0) == 0xC0) len = 2;       // 110xxxxx
    else if ((c & 0xF0) == 0xE0) len = 3;  // 1110xxxx (中文在这)
    else if ((c & 0xF8) == 0xF0) len = 4;  // 11110xxxx (emoji 等)
    else { *bytes = 1; return 1; }         // 非法序列，当 1 字节
    *bytes = len;

    // 判断是否 CJK / 全角（占 2 列）
    if (len == 3) {
        // 读出 codepoint 的高字节判断范围
        // CJK 统一表意文字：U+4E00 - U+9FFF（中文常见）
        // 全角标点：U+3000 - U+303F
        // 等。这里简化：3 字节序列大多占 2 列（中日韩）
        return 2;
    }
    return 1;  // 2 字节(拉丁扩展等) 和 4 字节(emoji) 按 1 列近似
}

void print_wrapped(const std::string& text, int width) {
    if (width < 10) width = 80;

    int col = 0;   // 当前列
    size_t i = 0;
    size_t n = text.size();
    while (i < n) {
        // 原有的换行符：直接输出，重置列
        if (text[i] == '\n') {
            std::putchar('\n');
            col = 0;
            ++i;
            continue;
        }

        // 算这个字符的宽度和字节
        int bytes = 1;
        int w = char_width((unsigned char)text[i], &bytes);

        // 当前行的剩余空间不够放这个字符 → 换行
        if (col + w > width) {
            std::putchar('\n');
            col = 0;
        }

        // 输出这个字符的原始字节
        std::fwrite(text.data() + i, 1, bytes, stdout);
        col += w;
        i += bytes;
    }
}

void println_wrapped(const std::string& text) {
    print_wrapped(text, term_width());
    std::putchar('\n');
}
