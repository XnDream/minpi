// term.h - 终端工具：折行打印
//
// 为什么需要：stdout 不会自动按终端宽度换行，超长文本一行拉到底很难看。
// 这个工具按终端宽度折行打印，中文字符按 2 列宽算。
//
// 对应 pi：pi 在 TUI 里有专门的 wrapTextWithAnsi() 处理这个，
// 而且要处理 ANSI 颜色码（我们阶段9 TUI 才需要）。
// 现在先做最朴素的：纯文本折行。
#ifndef MINPI_TERM_H
#define MINPI_TERM_H

#include <string>

// 获取终端可见宽度（列数）。失败/重定向时返回 80。
int term_width();

// 按 width 折行打印 text 到 stdout。
// 中日韩字符按 2 列宽，ASCII 按 1 列宽，遇到宽字符不够放就换行。
// 保留原有的 \n（段落分隔）。
void print_wrapped(const std::string& text, int width);

// 便捷版：自动用 term_width()。
void println_wrapped(const std::string& text);

#endif // MINPI_TERM_H
