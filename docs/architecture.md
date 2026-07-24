# 架构设计

本文讲清楚三件事：项目怎么分层、事件系统怎么工作、为什么用回调。

理解这套设计，才能看懂后面阶段（工具循环、重试、多模式）为什么各层互不影响。

## 三层架构

```
┌─────────────────────────────────────────┐
│  main.cpp   ← UI 层（订阅 AgentEvent）   │
├─────────────────────────────────────────┤
│  agent.cpp  ← agent 层（跑循环）         │
├─────────────────────────────────────────┤
│  ai.cpp     ← ai 层（和 LLM 通信）       │
├─────────────────────────────────────────┤
│  WinHTTP    ← HTTP 传输                  │
└─────────────────────────────────────────┘
```

**每层只和相邻层说话，不跨层。** 对应 pi 的 packages/ 结构（ai / agent / coding-agent）。

### 每层干什么

**ai 层**（`ai.cpp`）：
- 负责"和 LLM 通信"这一件事
- 发 HTTP 请求，收 SSE 流
- 把收到的数据**翻译成 AiEvent** 推出去
- 不知道"消息历史"怎么用、不知道"工具"怎么执行、不知道"循环"

**agent 层**（`agent.cpp`）：
- 负责"agent loop"这个循环
- 调 ai 层拿 LLM 的回复
- 如果 LLM 要调工具 → 执行工具 → 把结果塞回消息历史 → 再调 ai 层
- 把 ai 层的 AiEvent **翻译成 AgentEvent** 推出去
- 不知道回复怎么显示

**UI 层**（`main.cpp`）：
- 订阅 AgentEvent，决定怎么显示（打印到终端 / 存文件 / 发给 RPC 客户端）
- 不知道 LLM 怎么调、工具怎么跑

### 依赖方向：单向，绝不反向

```
main.cpp  →  依赖  agent.cpp  →  依赖  ai.cpp  →  依赖  core/
```

- main 知道 agent（调 `agent_run`）
- agent 知道 ai（调 `ai_stream`）
- **ai 不知道 agent**（`ai.cpp` 里没有 `#include "agent.h"`）

为什么重要？**ai 可以独立存在**。把 `ai.cpp` 拿出来单独写个程序调 `ai_stream`，不关心 agent。下层不知道上层，上层可以换。这就是分层。

## ai_stream：ai 层的唯一接口

`ai_stream` 干一件事：**把消息历史发给 LLM，通过回调把 LLM 的回复一段一段推出来。**

```cpp
bool ai_stream(
    Provider& provider,                     // 用哪个 LLM
    const std::vector<Message>& messages,   // 消息历史
    const ToolRegistry& tools,              // 可用工具
    AiEventCallback callback,               // 回调：每来一段数据就调
    std::vector<ToolCall>& out_tool_calls   // 输出：这轮 LLM 要调的工具
);
```

内部流程：
1. 拼 HTTP body（messages + tools）
2. 发请求
3. 读 SSE 流，每读到一段 `delta.content` → 调 `callback(AiEvent{TextDelta, ...})`
4. 解析 tool_calls 分片，攒完整 → 调 `callback(AiEvent{ToolCallEnd, ...})`
5. 流结束 → 调 `callback(AiEvent{Done})`
6. 把完整的 tool_calls 填进 `out_tool_calls` 返回

**关键：ai_stream 不知道"循环"**。它只做一次"发请求→收响应"。要不要循环、什么时候再调一次，是 agent 层的事。

## 事件系统：两套事件，agent 层翻译

**AiEvent** 和 **AgentEvent** 是两套事件，agent 层负责转换。

```
ai 层产生 AiEvent              agent 层转换           UI 层接收 AgentEvent
─────────────────              ────────────          ──────────────────
AiEvent::Start        ──→      MessageStart      ──→  打印 "assistant> "
AiEvent::TextDelta    ──→      MessageUpdate     ──→  打印这段文字
AiEvent::ToolCallEnd  ──→      (内部用，不转发)
AiEvent::Done         ──→      MessageEnd + AgentEnd  ──→  打印换行
```

### 为什么两套事件？关注点不同

| AiEvent | AgentEvent |
|---------|-----------|
| 描述"LLM 此刻在干什么" | 描述"一次完整交互的进度" |
| `TextDelta` = LLM 吐了一个字 | `MessageUpdate` = 一条消息在生长 |
| `ToolCallEnd` = LLM 要调工具了 | `ToolExecutionStart/End` = 工具开始/结束执行 |
| 不知道工具执行结果 | 知道工具执行结果，决定要不要再循环 |

- **ai 层永远看不到 ToolExecutionStart**——工具执行是 agent 的事
- **main 永远看不到 ToolCallDelta**——参数分片是 ai 内部细节，UI 不关心

每层只暴露"上层需要知道的"，藏起"内部细节"。这就是封装。

## 订阅事件：回调机制

### "订阅"是什么意思

订阅 = "我要听这件事的发生，发生了通知我"。

代码里就是：**把一个回调函数登记给对方，对方在事件发生时调用该函数。**

```cpp
// main.cpp 定义回调函数
static void on_agent_event(const AgentEvent& ev) {
    switch (ev.type) {
        case AgentEventType::MessageUpdate:
            std::fwrite(ev.delta.c_str(), ...);  // 收到事件就打印
            break;
        ...
    }
}

// main.cpp 把回调登记给 agent
Agent agent{..., on_agent_event};
agent_run(agent);  // agent 有事件时调 on_agent_event
```

### 为什么用回调，不用返回值

**用返回值（非流式）**：
```cpp
Result ai_stream(...) {
    发请求; 收完所有数据; return {text, tool_calls};
}
// main 拿到完整结果才打印 → 用户要等 LLM 说完整句才看到第一个字
```

**用回调（流式）**：
```cpp
bool ai_stream(..., callback) {
    发请求;
    for (每段数据) { callback(TextDelta, 这段); }  // 来一段推一段
}
// main 在 callback 里边收边打 → 文字逐字蹦出
```

事件回调的本质：**让数据消费者（main）和数据生产者（ai）解耦**。生产者来数据就推，消费者来一个处理一个，不用互相等。

## 回调函数属于上层还是下层

**属于上层，但由下层调用。** 这是理解分层的关键。

```cpp
// main.cpp（上层）定义
static void on_agent_event(const AgentEvent& ev) { /* 打印 */ }

// main.cpp 登记给 agent
Agent agent{..., on_agent_event};

// agent.cpp（下层）调用
bool agent_run(Agent& agent) {
    agent.on_event(...);  // 下层调上层的函数
}
```

| 事实 | 说明 |
|------|------|
| `on_agent_event` 是**上层定义的** | 代码在上层文件，逻辑是上层的（打印） |
| 它**被下层调用** | 执行时机在下层（agent 有事件时调） |

函数属于上层，执行权在下层。

### 这不违反"单向依赖"吗？

不违反。下层不知道这个函数具体是谁：

```cpp
// agent.h（下层只定义接口类型）
using AgentEventCallback = std::function<void(const AgentEvent&)>;

struct Agent {
    AgentEventCallback on_event;  // "任何接受 AgentEvent 的函数都行"
};
```

`agent.h` 只有 `std::function<void(const AgentEvent&)>`，**不知道** main 里那个函数叫 `on_agent_event`。是 main 主动把函数"塞给"agent。

这叫**依赖注入**——上层把具体实现注入给下层，下层只依赖抽象接口。下层调用的是"接口"，不是"上层"。

### 类比：餐厅

- **厨师（下层）**：定义"接到订单就做菜"的流程，不知道菜给谁吃
- **顾客（上层）**：定义"菜好了我要吃"的具体行为
- **服务员（回调）**：顾客把"菜好了叫我"留给厨师

换顾客（换回调），厨师流程不变：

```
顾客A（打印到终端）:  留下"菜好了就打印"
顾客B（存文件）:      留下"菜好了就写文件"
顾客C（发RPC）:       留下"菜好了就发网络包"
```

厨师（agent）一个字不用改，就能服务三种顾客。

### 三层都这么干

每层之间都是"上层定义回调，下层调用回调"：

```
main → 把 on_agent_event 传给 agent    （main 的函数，agent 调）
agent → 把 lambda 传给 ai_stream        （agent 的函数，ai 调）
ai → 把 on_data 传给 http_post_stream   （ai 的函数，http 层调）
```

下层永远只依赖"回调的签名（类型）"，不依赖"回调的具体实现"。

## 对应到 pi

| 本项目 | pi |
|------|-----|
| `ai_stream(callback)` | `Model.stream()` 返回事件流 |
| `AiEventCallback` | `AssistantMessageEvent` 流的订阅 |
| `agent_run` + `on_event` | `Agent` + `session.subscribe(listener)` |
| `AiEvent` | `AssistantMessageEvent` |
| `AgentEvent` | `AgentEvent` |

pi 的事件更多（compaction、retry、queue 等），但架构一模一样：ai 层吐原子事件，agent 层翻译+补充，UI 订阅。这也是为什么 pi 同一套 agent 逻辑能跑交互/print/json/rpc 四种模式——**回调不同，agent 不变**。

## 完整数据流（以"现在几点"为例）

```
用户输入 "现在几点"
    ↓
main: messages.push_back({User, "现在几点"})
    ↓
main: 调 agent_run(agent)
    ↓
agent: 进入 agent loop (turn 0)
    ↓
agent: 调 ai_stream(provider, messages, tools, callback, out_tool_calls)
    ↓
ai: 拼 body, 发请求, 读 SSE 流
    ↓
ai: 每来一个 delta → callback(AiEvent{TextDelta, "The"})
    ↓
agent: 收到 AiEvent{TextDelta} → 翻译成 AgentEvent{MessageUpdate} → 调 main 的回调
    ↓
main: 收到 AgentEvent{MessageUpdate} → 打印 "The"
    ↓
... (LLM 继续吐字)
    ↓
ai: 解析到 tool_calls → callback(AiEvent{ToolCallEnd, name="bash"})
    ↓
agent: 收到，内部记录（不转发给 main）
    ↓
ai: 流结束 → callback(AiEvent{Done})，out_tool_calls 填好
    ↓
ai_stream 返回
    ↓
agent: 拿到 out_tool_calls = [{bash, "date"}]
    ↓
agent: 把 assistant 回复加入 messages
    ↓
agent: 有 tool_calls → 执行工具
    ↓
agent: 调 main 回调 AgentEvent{ToolExecutionStart} → main 打印 "[tool: bash]"
    ↓
agent: 执行 bash("date") → 拿到结果
    ↓
agent: 调 main 回调 AgentEvent{ToolExecutionEnd} → main 打印 "[result]"
    ↓
agent: 把工具结果加入 messages (role=Tool)
    ↓
agent: 回到循环顶部 (turn 1) → 再调 ai_stream
    ↓
... (LLM 看到工具失败，换策略) ...
    ↓
直到某次 out_tool_calls 为空 → agent loop 结束
    ↓
agent: 调 main 回调 AgentEvent{AgentEnd}
```

## 一句话总结

- **ai_stream**：发一次请求，收一次响应，通过回调把 LLM 输出流式推出来。不知道循环。
- **agent_run**：调 ai_stream，有工具就执行+回填+再调，循环到 LLM 不再要工具。
- **AiEvent → AgentEvent**：agent 层翻译，每层只暴露上层该知道的。
- **回调**：上层定义，下层调用。下层只依赖回调签名，不依赖具体实现（依赖注入）。
- **分层红利**：改流式只动 ai.cpp，加工具循环只动 agent.cpp，换 UI 只动 main.cpp。
