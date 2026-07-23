// json.cpp - 路径提取式 JSON 解析器
//
// 核心思路：递归下降，但只沿目标 path 走，不建完整树。
// 类似 SAX 但更简单——只关心一个路径。
//
// 一个完整 JSON 解析器要处理 object/array/string/number/bool/null，
// 我们全要能"跳过"（算长度），但只在目标路径上"提取"。
#include "json.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

// 跳过空白
static const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    return p;
}

// 跳过一个完整 JSON 值，返回结束后的位置。
// 最关键的函数：能正确跳过任意嵌套结构。
static const char* skip_value(const char* p) {
    p = skip_ws(p);
    if (*p == '"') {
        ++p;
        while (*p) {
            if (*p == '\\') { p += 2; continue; }   // 跳过转义
            if (*p == '"') { ++p; break; }
            ++p;
        }
        return p;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{' ? '}' : ']');
        int depth = 0;
        for (;;) {
            if (*p == '"') { p = skip_value(p); continue; }  // 跳过字符串里可能出现的括号
            if (*p == open) ++depth;
            else if (*p == close) { --depth; if (depth == 0) { ++p; break; } }
            ++p;
        }
        return p;
    }
    if (*p == 't') return p + 4;   // true
    if (*p == 'f') return p + 5;   // false
    if (*p == 'n') return p + 4;   // null
    // number
    if (*p == '-') ++p;
    while (std::isdigit((unsigned char)*p) || *p=='.'||*p=='e'||*p=='E'||*p=='+'||*p=='-') ++p;
    return p;
}

// 在对象里按键查找。p 指向 '{' 之后。返回 value 起始位置或 nullptr。
static const char* find_key(const char* p, const char* seg, size_t seg_len) {
    p = skip_ws(p);
    if (*p == '}') return nullptr;
    while (*p) {
        p = skip_ws(p);
        if (*p != '"') return nullptr;
        ++p;
        const char* key_start = p;
        while (*p && *p != '"') {
            if (*p == '\\') { p += 2; continue; }
            ++p;
        }
        size_t key_len = size_t(p - key_start);
        ++p;  // 结束引号
        p = skip_ws(p);
        if (*p != ':') return nullptr;
        ++p;
        p = skip_ws(p);
        if (key_len == seg_len && std::strncmp(key_start, seg, key_len) == 0) {
            return p;  // 命中
        }
        p = skip_value(p);  // 跳过不关心的 value
        p = skip_ws(p);
        if (*p == ',') { ++p; continue; }
        return nullptr;
    }
    return nullptr;
}

// 在数组里按下标找。p 指向 '[' 之后。
static const char* find_index(const char* p, int index) {
    p = skip_ws(p);
    if (*p == ']') return nullptr;
    int i = 0;
    while (*p) {
        if (i == index) return p;
        p = skip_value(p);
        p = skip_ws(p);
        if (*p == ',') { ++p; ++i; p = skip_ws(p); continue; }
        return nullptr;
    }
    return nullptr;
}

// 共享：沿 path 查找，返回指向目标值的指针（跳过空白后）。
// 找不到返回 nullptr。string/bool/number 都能用这个定位。
static const char* json_find_value(const std::string& json, const std::string& path) {
    const char* p = skip_ws(json.c_str());
    size_t seg_start = 0;
    while (seg_start <= path.size()) {
        size_t dot = path.find('.', seg_start);
        std::string seg = (dot == std::string::npos)
                          ? path.substr(seg_start)
                          : path.substr(seg_start, dot - seg_start);
        p = skip_ws(p);
        if (*p == '{') {
            const char* found = find_key(p + 1, seg.c_str(), seg.size());
            if (!found) return nullptr;
            p = found;
        } else if (*p == '[') {
            for (char c : seg) if (!std::isdigit((unsigned char)c)) return nullptr;
            int idx = std::atoi(seg.c_str());
            const char* found = find_index(p + 1, idx);
            if (!found) return nullptr;
            p = found;
        } else {
            return nullptr;
        }
        if (dot == std::string::npos) break;
        seg_start = dot + 1;
    }
    return skip_ws(p);
}

bool json_get_string(const std::string& json, const std::string& path, std::string& out) {
    const char* p = json_find_value(json, path);
    if (!p) return false;
    out.clear();

    // p 指向目标值，要是字符串
    if (*p != '"') return false;
    ++p;

    // 拷贝并反转义
    while (*p && *p != '"') {
        if (*p == '\\') {
            ++p;
            switch (*p) {
                case '"':  out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/'); break;
                case 'n':  out.push_back('\n'); break;
                case 't':  out.push_back('\t'); break;
                case 'r':  out.push_back('\r'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'u': {
                    // \uXXXX：阶段1只处理 ASCII，中文等到 TUI 阶段
                    if (p[1] && p[2] && p[3] && p[4]) {
                        char hex[5] = {p[1], p[2], p[3], p[4], 0};
                        unsigned code = (unsigned)std::strtoul(hex, nullptr, 16);
                        out.push_back(code < 0x80 ? (char)code : '?');
                        p += 4;
                    }
                    break;
                }
                default: out.push_back(*p); break;
            }
            ++p;
        } else {
            out.push_back(*p++);
        }
    }
    return true;
}

bool json_get_bool(const std::string& json, const std::string& path, bool& out) {
    const char* p = json_find_value(json, path);
    if (!p) return false;
    if (std::strncmp(p, "true", 4) == 0) { out = true; return true; }
    if (std::strncmp(p, "false", 5) == 0) { out = false; return true; }
    return false;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
    return out;
}

// ---------- JSON 美化打印 ----------
// 扫描 JSON，按结构加换行和缩进。关键是正确跳过字符串字面量
// （字符串里的 { , 等 不算结构字符）。
std::string json_pretty(const std::string& json) {
    std::string out;
    out.reserve(json.size() * 2);
    int indent = 0;
    bool in_string = false;
    size_t i = 0;
    size_t n = json.size();

    auto add_indent = [&]() {
        for (int k = 0; k < indent; ++k) out += "  ";
    };

    while (i < n) {
        char c = json[i];

        if (in_string) {
            // 在字符串里：原样输出，注意转义
            out.push_back(c);
            if (c == '\\' && i + 1 < n) {
                out.push_back(json[++i]);  // 转义符后一个字符原样保留
            } else if (c == '"') {
                in_string = false;
            }
            ++i;
            continue;
        }

        switch (c) {
            case '"':
                out.push_back('"');
                in_string = true;
                ++i;
                break;
            case '{':
            case '[': {
                out.push_back(c);
                ++i;
                // 看下一个非空白是不是 }/ ]（空对象/数组）
                size_t j = i;
                while (j < n && (json[j] == ' ' || json[j] == '\t' || json[j] == '\n' || json[j] == '\r')) ++j;
                if (j < n && (json[j] == '}' || json[j] == ']')) {
                    // 空的，不换行
                } else {
                    out.push_back('\n');
                    ++indent;
                    add_indent();
                }
                break;
            }
            case '}':
            case ']':
                out.push_back('\n');
                --indent;
                add_indent();
                out.push_back(c);
                ++i;
                break;
            case ',':
                out.push_back(',');
                out.push_back('\n');
                ++i;
                add_indent();
                break;
            case ':':
                out.push_back(':');
                out.push_back(' ');  // 冒号后加空格
                ++i;
                break;
            case ' ':
            case '\t':
            case '\n':
            case '\r':
                ++i;  // 跳过原有空白
                break;
            default:
                out.push_back(c);
                ++i;
                break;
        }
    }
    return out;
}
