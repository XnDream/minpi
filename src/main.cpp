// main.cpp - 主程序入口（阶段 3：工具系统）
//
// 阶段3 变化：处理工具相关事件，显示 agent 调用工具的过程。
// 你会看到类似：
//   you> 当前目录有哪些文件？
//   assistant> [tool: bash] ls
//   [tool result] main.cpp
//   ai.cpp
//   ...
//   assistant> 当前目录有 main.cpp、ai.cpp 等文件。
//
// 这就是 agent：不只是问答，而是"调工具→看结果→继续回答"。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ai/ai.h"
#include "agent/agent.h"
#include "core/events.h"
#include "core/term.h"
#include "core/codecvt.h"
#include "core/tools.h"

static void on_agent_event(const AgentEvent& ev) {
    static bool in_thinking = false;   // 是否正在输出思考过程（控制颜色码）
    switch (ev.type) {
        case AgentEventType::AgentStart:
            break;
        case AgentEventType::TurnStart:
            std::printf("assistant> ");
            std::fflush(stdout);
            in_thinking = false;
            break;
        case AgentEventType::MessageStart:
            break;
        case AgentEventType::ThinkingUpdate:
            // 思考过程：灰色（\x1b[90m），首次进入时发一次颜色码
            if (!in_thinking) {
                std::fwrite("\x1b[90m", 1, 5, stdout);
                in_thinking = true;
            }
            std::fwrite(ev.delta.c_str(), 1, ev.delta.size(), stdout);
            std::fflush(stdout);
            break;
        case AgentEventType::MessageUpdate:
            // 正文：如果之前在思考，先重置颜色
            if (in_thinking) {
                std::fwrite("\x1b[0m\n", 1, 6, stdout);
                in_thinking = false;
            }
            std::fwrite(ev.delta.c_str(), 1, ev.delta.size(), stdout);
            std::fflush(stdout);
            break;
        case AgentEventType::MessageEnd:
            if (in_thinking) {
                std::fwrite("\x1b[0m\n", 1, 6, stdout);
                in_thinking = false;
            } else {
                std::fwrite("\x1b[0m", 1, 4, stdout);
            }
            std::printf("\n");
            break;
        case AgentEventType::ToolExecutionStart:
            // 工具开始执行。显示工具名和参数。
            std::printf("\n  [tool: %s] %s\n", ev.tool_name.c_str(), ev.tool_args.c_str());
            break;
        case AgentEventType::ToolExecutionEnd: {
            // 工具执行完。显示结果（截断显示，避免太长）。
            std::string result = ev.tool_result;
            // 缩进显示结果
            std::printf("  [result");
            if (ev.tool_is_error) std::printf(" ERROR");
            std::printf("]\n");
            // 每行加缩进
            size_t pos = 0;
            while (pos < result.size()) {
                size_t nl = result.find('\n', pos);
                std::string line = (nl == std::string::npos) ? result.substr(pos) : result.substr(pos, nl - pos);
                std::printf("    %s\n", line.c_str());
                if (nl == std::string::npos) break;
                pos = nl + 1;
            }
            std::printf("\n");
            std::fflush(stdout);
            break;
        }
        case AgentEventType::TurnEnd:
            break;
        case AgentEventType::AgentEnd:
            if (ev.error) {
                std::fprintf(stderr, "[error] %s\n",
                    ev.error_msg.empty() ? "(unknown)" : ev.error_msg.c_str());
            }
            break;
    }
}

static void usage() {
    std::fprintf(stderr,
        "minpi - minimal pi agent clone (stage 3: tools)\n\n"
        "Config: minpi.json or config/minpi.json\n"
        "  {\n"
        "    \"base_url\": \"...\",\n"
        "    \"api_key\":  \"...\",\n"
        "    \"model\":   \"...\",\n"
        "    \"debug\":   false          (true = 详细日志：URL/请求/响应)\n"
        "  }\n\n"
        "Env override: MINPI_DEBUG=1 forces debug on\n\n"
        "Type /quit to exit.\n");
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    if (argc > 1 && (!std::strcmp(argv[1], "-h") || !std::strcmp(argv[1], "--help"))) {
        usage();
        return 0;
    }

    Provider provider;
    std::string err;
    if (!provider_from_config(provider, err)) {
        std::fprintf(stderr, "Error: %s\n", err.c_str());
        usage();
        return 1;
    }

    // 创建工具注册表（read + bash）
    ToolRegistry tools = make_builtin_tools();

    std::printf("minpi (stage 3) - model: %s @ %s\n", provider.model.c_str(), provider.base_url.c_str());
    std::printf("Debug: %s\n", g_debug ? "ON (详细日志)" : "off");
    std::printf("Tools: ");
    for (size_t i = 0; i < tools.tools.size(); ++i) {
        if (i > 0) std::printf(", ");
        std::printf("%s", tools.tools[i].name.c_str());
    }
    std::printf("\nType /quit to exit.\n\n");

    std::vector<Message> messages;
    char line[4096];
    for (;;) {
        std::printf("you> ");
        std::fflush(stdout);
        if (!std::fgets(line, sizeof(line), stdin)) break;

        size_t len = std::strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        if (len == 0) continue;
        if (!std::strcmp(line, "/quit") || !std::strcmp(line, "/q")) break;

        messages.push_back({Role::User, line});

        Agent agent{&provider, &messages, &tools, on_agent_event};
        bool ok = agent_run(agent);
        if (!ok) {
            messages.pop_back();
        }
        std::printf("\n");
    }

    std::printf("bye.\n");
    return 0;
}
