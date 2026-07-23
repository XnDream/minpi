// json.h - 极简 JSON 路径提取（专为 LLM 响应设计）
//
// 为什么不引第三方（nlohmann/json 等）？
//   学习项目零依赖最干净，且 LLM 响应结构固定，只需取几个字段。
//   全量建树浪费内存且代码量大。路径匹配最小最直白。
//
// 设计：给 JSON 文本 + 路径（如 "choices.0.message.content"），
//       返回该路径的字符串值或原始片段。
#ifndef MINPI_JSON_H
#define MINPI_JSON_H

#include <string>

// 从 json 提取路径对应的字符串值（已反转义）。
// path 点分隔，数组下标用数字。找到返回 true 并写入 out。
// 只对 string 类型有效；number/bool/null 不支持。
bool json_get_string(const std::string& json, const std::string& path,
                     std::string& out);

// 从 json 提取路径对应的布尔值。
// 值为 true 返回 true 并写 out=true；false 返回 true 并写 out=false；
// 找不到或不是 bool 返回 false。
bool json_get_bool(const std::string& json, const std::string& path, bool& out);

// 把 C++ string 转义成 JSON 字符串字面量（含双引号）。
// 拼 HTTP body 用。
std::string json_escape(const std::string& s);

// 美化打印 JSON：加缩进和换行，便于人读。
// 输入是紧凑的 JSON 字符串，输出是带 2 空格缩进的格式化字符串。
// 用于调试输出。不修改语义，只加空白。
std::string json_pretty(const std::string& json);

#endif // MINPI_JSON_H
