# SSE 与 chunked 传输详解

本文讲清楚流式响应的底层机制：数据怎么分块传输、每块长什么样、怎么知道结束。

## SSE 是什么

SSE = Server-Sent Events。LLM 流式 API 普遍用它。

**非流式响应**（一个大 JSON）：
```json
{"choices":[{"message":{"content":"你好！有什么可以帮你？"}}]}
```

**流式响应**（一连串 `data:` 行）：
```
data: {"choices":[{"delta":{"content":"你"}}]}

data: {"choices":[{"delta":{"content":"好"}}]}

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

LLM 服务器不知道要生成多少字，所以用 chunked 一段段发。**SSE 就是建立在 chunked 之上的应用层协议**——用 `data:` 行格式组织 chunked 的内容。

## 每个包都带长度字段

chunked 编码的规则：**每个 chunk 前面带它自己的长度**。完整格式：

```
<长度1,十六进制>\r\n
<数据1>\r\n
<长度2,十六进制>\r\n
<数据2>\r\n
...
0\r\n
\r\n
```

服务器要发一段数据，就发：
1. 这段的字节数（十六进制）+ `\r\n`
2. 数据本身 + `\r\n`

比如要发 `data: {"delta":{"content":"你"}}\n\n`（35 字节），实际发出去的是：
```
23\r\n                                    ← 0x23 = 35
data: {"delta":{"content":"你"}}\n\n\r\n
```

每段数据自带长度，客户端不需要预先知道总长度——边读长度、边读数据，循环往复。

## 最后一个包长什么样

**长度为 0 的 chunk + 一个空行**：
```
0\r\n
\r\n
```

这是流的结束信号。客户端读到 `0\r\n` 就知道"服务器说完了"。

完整的一次流式响应：
```
1f\r\n
data: {"delta":{"content":"你"}}\n\n\r\n       ← chunk 1
1f\r\n
data: {"delta":{"content":"好"}}\n\n\r\n       ← chunk 2
1f\r\n
data: {"delta":{"content":"！"}}\n\n\r\n       ← chunk 3
10\r\n
data: [DONE]\n\n\r\n                            ← 最后一个有数据的 chunk
0\r\n                                           ← 长度为 0，流结束
\r\n
```

## 连接 vs 响应：两个生命周期

容易混淆的点："连接"和"响应"是两个概念。

```
TCP 连接（长）:   开 ──────────────────────────────── 关
                  │                                    │
响应1（短）:       ├──POST→ ←response1 stream──         │
                  │                                    │
响应2（短）:       │       ├──POST→ ←response2 stream── │
                  │                                    │
                  ↑ 连接保持，可发下一次请求              ↑ 连接才关闭
```

- **连接**：TCP 层的，可以一直保持（HTTP keep-alive），发多个请求复用
- **响应**：HTTP 层的，每个请求对应一个响应，**响应会结束**（哪怕连接不断）

`avail == 0`（WinHTTP 读到 `0\r\n`）表示**"这次响应读完了"**，不是"连接断了"。

### 三个层次再梳理

| 层次 | 生命周期 | 结束信号 | 怎么知道 |
|------|---------|---------|---------|
| TCP 连接 | 长，可复用 | FIN 包（四次挥手）| 句柄关闭 |
| HTTP 响应 | 短，每个请求一个 | chunked 的 `0\r\n` | `avail == 0` |
| SSE 流 | 等于响应 | `data: [DONE]` | `SseParser` 检测 |

`avail == 0` = 这次响应的 chunked body 读完了（服务器发了 `0\r\n`）。连接本身可能还活着，但本项目每次请求结束就关句柄，所以连接也随之关闭。

### 两层结束信号，都要处理

注意区分两个层面的"结束"：

| 层面 | 结束信号 | 谁处理 |
|------|---------|--------|
| HTTP chunked | `0\r\n\r\n` | WinHTTP（自动），代码不用管 |
| SSE 应用层 | `data: [DONE]` | 本项目，在 `SseParser` 里处理 |

服务器先发 `data: [DONE]`（一个普通 chunk），再发 `0\r\n` 关闭 HTTP 流。所以代码里两个都要处理：`[DONE]` 用来知道"LLM 说完了"，`avail == 0` 用来知道"HTTP 响应结束了"。

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

## 一次请求到底收到几块 data

实际数据（问"写一首诗"）：
- 总共 694 个 chunk（`on_data` 调 694 次）
- 总共 ~1268 个 `data:` 行
- **平均每个 chunk 含 1~2 个 data 行**，但分布极不均：
  - 大部分 chunk：~200 字节，1 个 data 行
  - 偶尔：8192 字节，41 个 data 行（代理缓冲批量发）

差距来自三个因素：服务器生成速度、中间代理缓冲、TCP Nagle 算法。

## 流式 vs 非流式，一次请求还是多次

**一次请求，一个响应 body**。流式只是响应的传输方式变了（chunked 分块），不是发多次请求。多轮对话才是多次请求（每次带完整历史）。

## 对应到代码

WinHTTP **帮处理了 chunked 解码**——`WinHttpReadData` 读出来的已经是解码后的纯数据，不需要自己解析 `<长度>\r\n<数据>\r\n` 格式。但"读到 0 字节 = 结束"这个语义保留：

```cpp
for (;;) {
    WinHttpQueryDataAvailable(hRequest, &avail);  // 现在有多少可读
    if (avail == 0) break;                         // 0 = 服务器发完了（对应 0\r\n）
    WinHttpReadData(hRequest, buf, avail, &read);
    if (read == 0) break;                          // 也是 0 = 结束
}
```

`avail == 0` 就是 WinHTTP 识别到那个 `0\r\n` 终止 chunk。

## 对应到 pi

pi 在 TS/Node 里用 `fetch()` 或 SDK 的 stream，chunked 解码由 Node 运行时自动处理。pi 直接拿到解码后的 `data:` 行。本项目用 WinHTTP，同样是自动解码，但读循环要自己写。
