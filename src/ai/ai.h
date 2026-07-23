// ai.h - LLM provider 抽象层（对应 pi packages/ai）
//
// 阶段3 变化：
//   - Message 加 ToolCall 和 toolResult 角色
//   - ai_stream 接受 tools 列表，把工具定义拼进请求 body
//   - 流式响应解析 tool_calls 的分片参数
#ifndef MINPI_AI_H
#define MINPI_AI_H

#include <string>
#include <vector>
#include "core/events.h"
#include "core/tools.h"

// 消息角色（pi AgentMessage 子集）
enum class Role { User, Assistant, Tool };

struct Message {
    Role role;
    std::string content;             // 文本内容
    std::vector<ToolCall> tool_calls; // assistant 调用工具时填
    std::string tool_call_id;        // role=Tool 时：对应的调用 ID
    std::string tool_name;           // role=Tool 时：工具名
};

// Provider 配置
struct Provider {
    std::string base_url;
    std::string api_key;
    std::string model;
};

bool provider_from_config(Provider& p, std::string& err);

// 全局调试开关。main.cpp 启动时根据配置设置。
// true = 打印 URL/请求/响应等详细日志；false = 正常使用模式。
extern bool g_debug;

// 核心 API：ai_stream（对应 pi Model.stream / streamSimple）。
// 阶段3：接受 tools 列表，LLM 可能返回 tool_calls。
// 通过 callback 推 AiEvent：Start -> (TextDelta | ToolCall*)* -> Done|Error
// 返回值：true=成功。out_tool_calls 填入这轮产生的工具调用（可能为空）。
bool ai_stream(Provider& provider, const std::vector<Message>& messages,
               const ToolRegistry& tools,
               AiEventCallback callback,
               std::vector<ToolCall>& out_tool_calls);

#endif // MINPI_AI_H
