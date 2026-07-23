// agent.cpp - Agent 层实现（阶段3：agent loop）
//
// 核心循环：
//   do {
//     调 ai_stream（带工具定义）
//     把 assistant 的回复（文本 + tool_calls）加入历史
//     如果有 tool_calls：
//       执行每个工具，结果作为 role=Tool 消息加入历史
//       继续循环（再调 LLM）
//     否则：结束
//   } while (有 tool_calls)
//
// 事件：每个 turn 发 TurnStart/TurnEnd，工具执行发 ToolExecutionStart/End。
#include "agent.h"

#include <cstdio>

bool agent_run(Agent& agent) {
    agent.on_event({AgentEventType::AgentStart});

    bool had_error = false;
    std::string error_msg;

    for (int turn = 0; ; ++turn) {
        agent.on_event({AgentEventType::TurnStart});

        std::string accumulated;       // 这一轮 LLM 的文本回复
        std::vector<ToolCall> tool_calls;  // 这一轮 LLM 要调的工具

        // 调 LLM，订阅流式事件
        bool ok = ai_stream(*agent.provider, *agent.messages, *agent.tools,
            [&](const AiEvent& ev) {
                switch (ev.type) {
                    case AiEventType::Start:
                        accumulated.clear();
                        agent.on_event({AgentEventType::MessageStart});
                        break;
                    case AiEventType::TextDelta:
                        accumulated += ev.delta;
                        agent.on_event({AgentEventType::MessageUpdate, ev.delta, "", false});
                        break;
                    case AiEventType::ThinkingDelta:
                        agent.on_event({AgentEventType::ThinkingUpdate, ev.delta, "", false});
                        break;
                    case AiEventType::ToolCallStart:
                        agent.on_event({AgentEventType::MessageStart});  // 工具调用也算消息开始
                        break;
                    case AiEventType::ToolCallDelta:
                        // 工具参数片段，暂不转发给上层（main 可以显示，但先简化）
                        break;
                    case AiEventType::ToolCallEnd:
                        break;
                    case AiEventType::Done: {
                        // 消息结束。注意：如果这轮有 tool_calls，
                        // accumulated 可能为空（LLM 只调工具不说话）
                        agent.on_event({AgentEventType::MessageEnd, "", accumulated, false});
                        break;
                    }
                    case AiEventType::Error:
                        had_error = true;
                        error_msg = ev.error;
                        break;
                }
            },
            tool_calls);

        if (!ok || had_error) {
            agent.on_event({AgentEventType::AgentEnd, "", "", true, error_msg});
            return false;
        }

        // 把 assistant 回复加入历史（含 tool_calls）
        Message asst_msg;
        asst_msg.role = Role::Assistant;
        asst_msg.content = accumulated;
        asst_msg.tool_calls = tool_calls;
        agent.messages->push_back(std::move(asst_msg));

        // 如果没有工具调用，agent loop 结束
        if (tool_calls.empty()) {
            agent.on_event({AgentEventType::TurnEnd});
            break;
        }

        // 执行每个工具，结果作为 role=Tool 消息加入历史
        for (auto& tc : tool_calls) {
            agent.on_event({AgentEventType::ToolExecutionStart, "", "", false, "", tc.name, tc.id, tc.arguments, "", false});

            ToolResult result = agent.tools->execute(tc);

            agent.on_event({AgentEventType::ToolExecutionEnd, "", "", false, "", tc.name, tc.id, "", result.content, result.is_error});

            // 把工具结果加入历史
            Message tool_msg;
            tool_msg.role = Role::Tool;
            tool_msg.content = result.content;
            tool_msg.tool_call_id = tc.id;
            tool_msg.tool_name = tc.name;
            agent.messages->push_back(std::move(tool_msg));
        }

        agent.on_event({AgentEventType::TurnEnd});
        // 继续下一轮：LLM 看到工具结果后会继续
    }

    agent.on_event({AgentEventType::AgentEnd, "", "", false});
    return true;
}
