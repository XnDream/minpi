// events.h - 事件系统（pi 解耦 UI 与逻辑的核心）
//
// 两层事件：
//   AiEvent    - ai 层产生（LLM 通信原子事件）
//   AgentEvent - agent 层产生（会话级事件）
// agent 订阅 AiEvent，转成 AgentEvent 转发。
//
// 阶段3 加了工具相关事件：
//   AiEvent 加 ToolCallStart/Delta/End（工具调用的参数是分片传来的，要累积）
//   AgentEvent 加 TurnStart/End（多轮工具循环）、ToolExecutionStart/End
#ifndef MINPI_EVENTS_H
#define MINPI_EVENTS_H

#include <functional>
#include <string>

// ============================================================
// AiEvent - ai 层事件
// ============================================================
// 序列: START -> (TEXT_DELTA | TOOLCALL_*)* -> DONE | ERROR
enum class AiEventType {
    Start,              // 流开始
    TextDelta,          // 一段文本 (delta)
    ThinkingDelta,      // 思考过程片段 (delta) —— 阶段3加：GLM/o1/Claude 等推理模型的思考过程
    ToolCallStart,      // 工具调用开始 (tool_call_id, tool_name)
    ToolCallDelta,      // 工具调用参数片段 (tool_call_id, delta)
    ToolCallEnd,        // 工具调用结束 (tool_call_id, tool_name, arguments完整)
    Done,               // 正常结束
    Error,              // 出错 (error)
};

struct AiEvent {
    AiEventType type;
    std::string delta;        // TextDelta / ToolCallDelta
    std::string error;        // Error
    std::string tool_call_id; // ToolCall*
    std::string tool_name;    // ToolCallStart / ToolCallEnd
};

using AiEventCallback = std::function<void(const AiEvent&)>;


// ============================================================
// AgentEvent - agent 层事件
// ============================================================
// agent_run 现在可能含多个 turn（工具循环）：
//   AGENT_START -> TURN_START -> MESSAGE_* -> [TOOL_EXEC_* -> TURN_START -> ...]* -> AGENT_END
enum class AgentEventType {
    AgentStart,
    TurnStart,              // 一轮 LLM 调用开始
    MessageStart,
    MessageUpdate,          // delta (文本)
    ThinkingUpdate,         // delta (思考过程) —— 阶段3加
    MessageEnd,             // text 完整文本
    ToolExecutionStart,     // 工具开始执行 (tool_name, tool_call_id, tool_args)
    ToolExecutionEnd,       // 工具执行完 (tool_name, tool_call_id, tool_result, tool_is_error)
    TurnEnd,                // 一轮结束
    AgentEnd,               // 整个 agent_run 结束 (error, error_msg)
};

struct AgentEvent {
    AgentEventType type;
    std::string delta;          // MessageUpdate
    std::string text;           // MessageEnd
    bool error = false;         // AgentEnd
    std::string error_msg;      // AgentEnd
    std::string tool_name;      // ToolExecution*
    std::string tool_call_id;   // ToolExecution*
    std::string tool_args;      // ToolExecutionStart
    std::string tool_result;    // ToolExecutionEnd
    bool tool_is_error = false; // ToolExecutionEnd
};

using AgentEventCallback = std::function<void(const AgentEvent&)>;

#endif // MINPI_EVENTS_H
