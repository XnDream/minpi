// ai.cpp - Provider 实现（OpenAI 兼容，对应 pi packages/ai）
//
// 阶段3：工具系统。
//   - 请求 body 加 tools 数组
//   - 消息支持 user/assistant/tool 三种角色
//   - 流式响应解析 tool_calls 的分片参数
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include "ai.h"
#include "core/json.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <functional>
#include <fstream>
#include <sstream>

// ---------- 调试打印 ----------
// 全局开关。main.cpp 启动时根据配置文件设置。
// 默认关（正常使用模式），配置里 "debug": true 开启详细日志。
bool g_debug = false;

static bool debug_enabled() {
    return g_debug;
}

static void debug(const char* tag, const std::string& content) {
    if (!debug_enabled()) return;
    std::fprintf(stderr, "\n===== %s =====\n", tag);
    std::fprintf(stderr, "%s", content.c_str());
    std::fprintf(stderr, "\n===== /%s =====\n\n", tag);
}

// ---------- 读配置文件 minpi.json ----------

struct Config {
    std::string base_url, api_key, model;
    bool debug = false;
    bool loaded = false;
};

static bool try_load_config(const std::string& path, Config& c) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();
    if (content.size() >= 3 &&
        (unsigned char)content[0] == 0xEF &&
        (unsigned char)content[1] == 0xBB &&
        (unsigned char)content[2] == 0xBF) {
        content = content.substr(3);
    }
    c.loaded = true;
    json_get_string(content, "base_url", c.base_url);
    json_get_string(content, "api_key", c.api_key);
    json_get_string(content, "model", c.model);
    // debug 开关：布尔值
    json_get_bool(content, "debug", c.debug);
    return true;
}

static Config load_config_file() {
    Config c;
    if (try_load_config("minpi.json", c)) return c;
    try_load_config("config/minpi.json", c);
    return c;
}

bool provider_from_config(Provider& p, std::string& err) {
    Config cfg = load_config_file();

    // 设置全局调试开关。环境变量 MINPI_DEBUG=1 可强制开启（优先于配置文件）。
    g_debug = cfg.debug;
    const char* env_dbg = std::getenv("MINPI_DEBUG");
    if (env_dbg && (std::strcmp(env_dbg, "1") == 0 || std::strcmp(env_dbg, "true") == 0)) {
        g_debug = true;
    }

    if (!cfg.base_url.empty()) {
        p.base_url = cfg.base_url;
    } else {
        const char* url = std::getenv("LLM_BASE_URL");
        p.base_url = url ? url : "https://api.openai.com";
    }
    if (!cfg.api_key.empty()) {
        p.api_key = cfg.api_key;
    } else {
        const char* key = std::getenv("LLM_API_KEY");
        if (!key || !*key) {
            err = "No api_key found. Put it in minpi.json or set LLM_API_KEY.";
            return false;
        }
        p.api_key = key;
    }
    if (!cfg.model.empty()) {
        p.model = cfg.model;
    } else {
        const char* model = std::getenv("LLM_MODEL");
        p.model = model ? model : "gpt-4o-mini";
    }
    return true;
}

// ---------- 拼 OpenAI 请求 body ----------
// 阶段3：支持三种角色（user/assistant/tool）和 tools 数组。
// assistant 可能含 tool_calls；tool 消息要带 tool_call_id。

static std::string build_request_body(const Provider& p, const std::vector<Message>& msgs,
                                      const ToolRegistry& tools) {
    std::string body;
    body += "{\"model\":";
    body += json_escape(p.model);
    body += ",\"stream\":true,";

    // tools 数组
    if (!tools.tools.empty()) {
        body += "\"tools\":[";
        for (size_t i = 0; i < tools.tools.size(); ++i) {
            if (i > 0) body += ",";
            body += "{\"type\":\"function\",\"function\":{";
            body += "\"name\":";
            body += json_escape(tools.tools[i].name);
            body += ",\"description\":";
            body += json_escape(tools.tools[i].description);
            body += ",\"parameters\":";
            body += tools.tools[i].parameters_json;
            body += "}}";
        }
        body += "],";
    }

    body += "\"messages\":[";
    for (size_t i = 0; i < msgs.size(); ++i) {
        if (i > 0) body += ",";
        body += "{\"role\":";
        switch (msgs[i].role) {
            case Role::User:
                body += "\"user\"";
                body += ",\"content\":";
                body += json_escape(msgs[i].content);
                break;
            case Role::Assistant:
                body += "\"assistant\"";
                if (!msgs[i].content.empty()) {
                    body += ",\"content\":";
                    body += json_escape(msgs[i].content);
                } else {
                    body += ",\"content\":null";
                }
                if (!msgs[i].tool_calls.empty()) {
                    body += ",\"tool_calls\":[";
                    for (size_t j = 0; j < msgs[i].tool_calls.size(); ++j) {
                        if (j > 0) body += ",";
                        body += "{\"id\":";
                        body += json_escape(msgs[i].tool_calls[j].id);
                        body += ",\"type\":\"function\",\"function\":{";
                        body += "\"name\":";
                        body += json_escape(msgs[i].tool_calls[j].name);
                        body += ",\"arguments\":";
                        body += json_escape(msgs[i].tool_calls[j].arguments);
                        body += "}}";
                    }
                    body += "]";
                }
                break;
            case Role::Tool:
                body += "\"tool\"";
                body += ",\"content\":";
                body += json_escape(msgs[i].content);
                body += ",\"tool_call_id\":";
                body += json_escape(msgs[i].tool_call_id);
                break;
        }
        body += "}";
    }
    body += "]}";
    return body;
}

// ---------- WinHTTP 发流式请求 ----------
// 每读到一段响应数据，调 on_data(chunk)。调用方在 on_data 里解析 SSE。

using DataCallback = std::function<void(const std::string& chunk)>;

static bool http_post_stream(const Provider& p, const char* path, const std::string& body,
                             DataCallback on_data, std::string& out_resp, std::string& out_err) {
    const char* scheme_end = std::strstr(p.base_url.c_str(), "://");
    if (!scheme_end) { out_err = "base_url must start with http:// or https://"; return false; }
    bool is_https = (p.base_url.rfind("https://", 0) == 0);
    const char* host_start = scheme_end + 3;

    char host[256] = {0};
    const char* path_after_host = std::strchr(host_start, '/');
    size_t host_len = path_after_host ? size_t(path_after_host - host_start) : std::strlen(host_start);
    const char* colon = (const char*)std::memchr(host_start, ':', host_len);
    if (colon) host_len = size_t(colon - host_start);
    if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
    std::memcpy(host, host_start, host_len);

    INTERNET_PORT port = is_https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    if (colon) port = (INTERNET_PORT)std::atoi(colon + 1);

    {
        std::string info;
        info += "base_url : "; info += p.base_url; info += "\n";
        info += "scheme   : "; info += (is_https ? "https" : "http"); info += "\n";
        info += "host     : "; info += host; info += "\n";
        info += "port     : "; info += std::to_string(port); info += "\n";
        info += "path     : "; info += path;
        if (path_after_host) { info += "  (base_url prefix: "; info += path_after_host; info += ")"; }
        info += "\n";
        debug("URL PARSED", info);
    }

    HINTERNET hSession = WinHttpOpen(L"minpi/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { out_err = "WinHttpOpen failed"; return false; }

    wchar_t whost[256];
    MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, 256);

    HINTERNET hConnect = WinHttpConnect(hSession, whost, port, 0);
    if (!hConnect) { out_err = "WinHttpConnect failed"; WinHttpCloseHandle(hSession); return false; }

    std::string full_path = path_after_host ? std::string(path_after_host) : std::string();
    full_path += path;
    wchar_t wpath[1024];
    MultiByteToWideChar(CP_UTF8, 0, full_path.c_str(), -1, wpath, 1024);

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wpath,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        is_https ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) { out_err = "WinHttpOpenRequest failed"; WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    std::string headers = "Content-Type: application/json\r\nAccept: text/event-stream\r\nAuthorization: Bearer " + p.api_key + "\r\n";

    {
        std::string info = "POST " + full_path + " HTTP/1.1\n";
        info += "Host: "; info += host; info += "\n";
        info += "User-Agent: minpi/1.0\n";
        info += "Content-Length: "; info += std::to_string(body.size()); info += "\n";
        info += headers;
        info += "\n";
        info += json_pretty(body);
        debug(">>> REQUEST", info);
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, headers.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wheaders(wlen);
    MultiByteToWideChar(CP_UTF8, 0, headers.c_str(), -1, wheaders.data(), wlen);
    WinHttpAddRequestHeaders(hRequest, wheaders.data(), (DWORD)-1,
        WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    bool ok = false;
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            (LPVOID)body.c_str(), (DWORD)body.size(), (DWORD)body.size(), 0)) {
        out_err = "WinHttpSendRequest failed (err=" + std::to_string(GetLastError()) + ")";
    } else if (!WinHttpReceiveResponse(hRequest, NULL)) {
        out_err = "WinHttpReceiveResponse failed (err=" + std::to_string(GetLastError()) + ")";
    } else {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);

        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &avail)) break;
            if (avail == 0) break;
            std::vector<char> buf(avail + 1);
            DWORD read = 0;
            if (WinHttpReadData(hRequest, buf.data(), avail, &read) && read > 0) {
                std::string chunk(buf.data(), read);
                out_resp.append(chunk);
                if (status == 200) on_data(chunk);
            }
            if (read == 0) break;
        }

        if (status != 200) {
            out_err = "HTTP " + std::to_string(status) + ": " + out_resp;
        } else {
            ok = true;
        }

        {
            std::string info = "HTTP "; info += std::to_string(status); info += "\n";
            DWORD hdr_size = 0;
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                WINHTTP_HEADER_NAME_BY_INDEX, NULL, &hdr_size, WINHTTP_NO_HEADER_INDEX);
            if (hdr_size > 0) {
                std::vector<wchar_t> whdr(hdr_size / sizeof(wchar_t) + 1);
                if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                        WINHTTP_HEADER_NAME_BY_INDEX, whdr.data(), &hdr_size, WINHTTP_NO_HEADER_INDEX)) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, whdr.data(), -1, nullptr, 0, nullptr, nullptr);
                    std::string hdr(len > 0 ? len - 1 : 0, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, whdr.data(), -1, &hdr[0], len, nullptr, nullptr);
                    info += hdr;
                }
            }
            info += "\n";
            info += out_resp;
            debug("<<< RESPONSE (raw stream)", info);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

// ---------- SSE 解析器 ----------
// SSE 格式：每行 "data: <payload>"，事件之间用空行分隔。
// 难点：chunk 边界不等于行边界。维护缓冲区，攒够一行再处理。
struct SseParser {
    std::string buf;

    void feed(const std::string& chunk, std::function<void(const std::string&)> on_event) {
        buf += chunk;
        size_t pos = 0;
        for (;;) {
            size_t nl = buf.find('\n', pos);
            if (nl == std::string::npos) break;
            std::string line = buf.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pos = nl + 1;

            if (line.empty()) continue;
            if (line[0] == ':') continue;
            if (line.rfind("data:", 0) == 0) {
                std::string payload = line.substr(5);
                if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
                on_event(payload);
            }
        }
        buf.erase(0, pos);
    }
};

// ---------- 对外 API：ai_stream ----------
//
// 阶段3核心：解析 tool_calls 的分片。
// 流式响应里，工具调用的参数 JSON 被切成多个片段，要累积。

struct PendingToolCall {
    std::string id;
    std::string name;
    std::string arguments;
};

bool ai_stream(Provider& provider, const std::vector<Message>& messages,
               const ToolRegistry& tools,
               AiEventCallback callback,
               std::vector<ToolCall>& out_tool_calls) {
    std::string body = build_request_body(provider, messages, tools);

    std::string resp, err;
    SseParser sse;
    bool started = false;
    std::vector<PendingToolCall> pending;

    bool ok = http_post_stream(provider, "/v1/chat/completions", body,
        [&](const std::string& chunk) {
            sse.feed(chunk, [&](const std::string& payload) {
                if (payload == "[DONE]") return;

                std::string finish_reason;
                json_get_string(payload, "choices.0.finish_reason", finish_reason);

                // 1. 文本增量
                std::string text_delta;
                if (json_get_string(payload, "choices.0.delta.content", text_delta) && !text_delta.empty()) {
                    if (!started) { callback({AiEventType::Start}); started = true; }
                    callback({AiEventType::TextDelta, text_delta});
                }

                // 1b. 思考过程增量（GLM/o1/Claude 等推理模型的 reasoning_content）
                std::string think_delta;
                if (json_get_string(payload, "choices.0.delta.reasoning_content", think_delta) && !think_delta.empty()) {
                    if (!started) { callback({AiEventType::Start}); started = true; }
                    callback({AiEventType::ThinkingDelta, think_delta});
                }

                // 2. 工具调用：首次带 name，后续只带 arguments 片段
                std::string tc_name;
                if (json_get_string(payload, "choices.0.delta.tool_calls.0.function.name", tc_name) && !tc_name.empty()) {
                    std::string tc_id;
                    json_get_string(payload, "choices.0.delta.tool_calls.0.id", tc_id);
                    if (pending.empty()) pending.push_back({tc_id, tc_name, ""});
                    else { pending[0].id = tc_id; pending[0].name = tc_name; }
                    if (!started) { callback({AiEventType::Start}); started = true; }
                    callback({AiEventType::ToolCallStart, "", "", tc_id, tc_name});
                }
                std::string tc_args;
                if (json_get_string(payload, "choices.0.delta.tool_calls.0.function.arguments", tc_args) && !tc_args.empty()) {
                    if (pending.empty()) pending.push_back({"", "", ""});
                    pending[0].arguments += tc_args;
                    if (!started) { callback({AiEventType::Start}); started = true; }
                    callback({AiEventType::ToolCallDelta, tc_args, "", pending[0].id, pending[0].name});
                }

                // 3. finish_reason=tool_calls：工具调用结束
                if (finish_reason == "tool_calls" && !pending.empty()) {
                    for (auto& p : pending) {
                        callback({AiEventType::ToolCallEnd, "", "", p.id, p.name});
                    }
                }
            });
        },
        resp, err);

    if (!ok) {
        callback({AiEventType::Error, "", err});
        return false;
    }

    for (auto& p : pending) {
        out_tool_calls.push_back({p.id, p.name, p.arguments});
    }

    if (!started) callback({AiEventType::Start});
    callback({AiEventType::Done});
    return true;
}
