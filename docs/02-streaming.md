# 阶段 2：流式 SSE + 事件系统

## 目标

把"等 LLM 说完整句再显示"改成"LLM 边说边显示"——文字逐字蹦出的效果。

## SSE 协议

SSE = Server-Sent Events。LLM 流式 API 普遍用它。

**非流式响应**（阶段 1，一个大 JSON）：
```json
{"choices":[{"message":{"content":"你好！有什么可以帮你？"}}]}
```

**流式响应**（一连串 `data:` 行）：
```
data: {"choices":[{"delta":{"content":"你"}}]}

data: {"choices":[{"delta":{"content":"好"}}]}

data: {"choices":[{"delta":{"content":"！"}}]}

data: {"choices":[{"finish_reason":"stop"}]}

data: [DONE]
```

每个 `data:` 是一个事件，`delta.content` 是这一小段新增文本。最后 `[DONE]` 表示流结束。

## chunked transfer encoding

"服务器边生成边发"靠 HTTP 的 `Transfer-Encoding: chunked`：

| | 传统 HTTP | chunked HTTP |
|---|---|---|
| 响应头 | `Content-Length: 1234` | `Transfer-Encoding: chunked` |
| 服务器 | 必须算完全部内容才发 | 边产生边发 |
| 客户端 | 读够 1234 字节结束 | 读到"长度 0 的块"结束 |

LLM 服务器不知道要生成多少字，所以用 chunked 一段段发。**SSE 就是建立在 chunked 之上的协议**——用 `data:` 行格式组织 chunked 的内容。

## 核心难点：chunk 边界 ≠ 行边界

`WinHttpReadData` 每次读到的一段数据（chunk），**不一定按 SSE 事件边界切**。一个 chunk 可能含：
- 1 个完整 `data:` 行（最常见）
- 半个 `data:` 行（JSON 被从中间劈开）
- 多个完整 `data:` 行（代理缓冲后批量发）
- 若干完整 + 一个半截

所以 `SseParser` 要维护一个缓冲区 `buf`：**攒到完整的行再解析**，不管 chunk 怎么切。

```cpp
struct SseParser {
    std::string buf;
    void feed(const std::string& chunk, std::function<void(const std::string&)> on_event) {
        buf += chunk;
        // 按 \n 切行，完整一行才调 on_event
        // 没切完的半行留在 buf 等下次
    }
};
```

## 事件系统：解耦 UI 与逻辑

pi 最重要设计：**agent 逻辑不直接 print，而是发事件**。

```
ai.cpp    →  推 AiEvent (Start/TextDelta/Done)
agent.cpp →  转 AgentEvent (MessageStart/Update/End)
main.cpp  →  订阅 AgentEvent，打印
```

阶段 2 的核心改动：
- `http_post` → `http_post_stream`：每读到一段数据调 `on_data` 回调
- `ai_stream`：在 `on_data` 里解析 SSE，每来一个 `delta.content` 推 `TextDelta`
- `main.cpp`：打印从 `MessageEnd`（一次性）挪到 `MessageUpdate`（边收边打）

## 分层架构的回报

```
        阶段1                    阶段2
ai.cpp   非流式(收完再推)  →    流式(边收边推)    【大改】
agent.cpp  转事件            →   一行不改          【没动】
main.cpp   MessageEnd打印   →    MessageUpdate打印 【小改】
```

**`agent.cpp` 一行没改**。阶段 1 立好的事件抽象（`AiEvent` 的 `Start/TextDelta/Done`）天然支持流式——`TextDelta` 来一次推一次，agent 只是透传。这就是 pi 架构"事件解耦"的红利。

对应 pi 的 `packages/agent` + `session.subscribe()` 机制。

## 流式 vs 非流式，一次请求还是多次？

**一次请求，一个响应 body**。流式只是响应的传输方式变了（chunked 分块），不是发多次请求。多轮对话才是多次请求（每次带完整历史）。
