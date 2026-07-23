// agent.h - Agent 层（对应 pi packages/agent）
//
// 阶段3：agent_run 变成 while 循环——agent loop。
//   while (LLM 返回 tool_calls) {
//     执行每个工具
//     把结果作为 role=Tool 消息加入历史
//     再调 LLM
//   }
// 直到 LLM 不再要工具（finish_reason=stop），返回最终文本。
//
// 这就是 agent 之所以是 agent：不只是问→答，而是问→[调工具→看结果→继续]→答。
#ifndef MINPI_AGENT_H
#define MINPI_AGENT_H

#include <vector>
#include <string>
#include "ai/ai.h"
#include "core/events.h"
#include "core/tools.h"

struct Agent {
    Provider*                provider = nullptr;
    std::vector<Message>*    messages = nullptr;
    ToolRegistry*            tools = nullptr;       // 可用工具
    AgentEventCallback       on_event;
};

// 跑一轮 agent：从最后一条 user 消息开始，循环调用 LLM + 执行工具，
// 直到 LLM 给出最终文本回复。
bool agent_run(Agent& agent);

#endif // MINPI_AGENT_H
