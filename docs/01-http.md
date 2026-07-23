# 阶段 1：HTTP + Provider 抽象 + 单轮问答

## 目标

从零实现一个能和 LLM 对话的程序。不流式、不存盘、没工具——最朴素的"消息数组 → LLM → 回复"。

## 核心概念：agent 的本质是一个消息数组

```
用户输入 "你好"  →  messages = [{role:"user", content:"你好"}]
                                          ↓ 发给 LLM
LLM 回复 "你好！" →  messages 追加 {role:"assistant", content:"你好！"}
```

agent 不维护复杂状态，核心就是一个不断追加的消息数组。每次调用 LLM，把整个数组发过去；LLM 的回复追加到末尾。

LLM 本身是**无状态**的——不记得对话历史。是 agent 负责记历史，每轮把全部历史发过去，让它"读上文"。这是后面所有功能的基础。

## 核心概念：Provider 抽象

pi 能接 30+ 家 LLM 的关键：**不让 agent 直接接触 provider 的原始协议**。

```
agent 层  ←  只认统一的事件流
    ↓
ai 层     ←  把各家协议（OpenAI/Anthropic/Google）适配成统一接口
    ↓
HTTP 层   ←  WinHTTP 发请求
```

本项目只实现 OpenAI 兼容协议（最通用，DeepSeek/Kimi/ollama 都兼容），但接口设计成"可换 provider"。

对应 pi 的 `packages/ai`。

## 请求格式

```
POST /v1/chat/completions HTTP/1.1
Host: api.openai.com
User-Agent: minpi/1.0
Content-Type: application/json
Authorization: Bearer sk-xxx

{"model":"gpt-4o-mini","messages":[{"role":"user","content":"你好"}]}
```

多轮对话时，**整个消息历史**都发过去：

```json
{"messages":[
  {"role":"user","content":"你好"},
  {"role":"assistant","content":"你好！"},
  {"role":"user","content":"我刚才说了什么"}
]}
```

## 响应格式

```json
{
  "choices": [
    {
      "index": 0,
      "finish_reason": "stop",
      "message": {"role":"assistant", "content":"你好！有什么可以帮你？"}
    }
  ],
  "usage": {"prompt_tokens":10, "completion_tokens":15, "total_tokens":25}
}
```

本项目只关心 `choices[0].message.content`。其他字段后续阶段用到：
- `usage`：token 用量（阶段 7 compaction 算上下文窗口）
- `finish_reason`：`stop` 正常结束，`tool_calls` 要调工具（阶段 3）

## Windows C++ 的代价

TS/Node 里 `fetch()` + `JSON.parse()` 一行搞定的事，C++ 里要手写：

| 事情 | TS | 本项目 C++ |
|------|-----|---------|
| HTTP 请求 | `fetch()` | WinHTTP 三层句柄（Session/Connect/Request）|
| URL 拆解 | `new URL()` | 手写指针算术 |
| 字符编码 | UTF-8 直用 | `MultiByteToWideChar` 转 UTF-16（WinHTTP 要 wchar_t）|
| JSON 解析 | `JSON.parse()` | 手写路径提取式解析器 |
| 资源清理 | GC | 每个句柄手动 `WinHttpCloseHandle` |

这些都是一次性投入，后面阶段复用。

## WinHTTP 的三层句柄模型

```
HINTERNET hSession   ← 会话（进程级，设 User-Agent、代理）
    └─ HINTERNET hConnect   ← 连接（到某个 host:port）
          └─ HINTERNET hRequest   ← 请求（具体的 POST + 路径）
```

每层都是句柄，用完都要 `WinHttpCloseHandle` 关掉，顺序倒着（Request → Connect → Session）。漏一个就句柄泄漏。

## 构建系统

MSVC 的 `cl.exe` 不在 PATH，要先跑 `vcvarsall.bat` 激活环境。本项目用 `build.sh`（bash 找 vcvarsall 路径）+ `build/build.bat`（激活+编译）的组合，避开 cmd 处理带空格路径（`Program Files (x86)`）的问题。

## 分层架构（为后续阶段立地基）

```
main.cpp  (订阅事件，打印)        ← 阶段9换TUI，只改这
   ↓ agent_run()
agent.c   (循环，发 AgentEvent)    ← 阶段3加tool loop，只改这
   ↓ ai_stream()
ai.c      (HTTP通信，发 AiEvent)   ← 阶段2改流式，只改这内部
```

阶段 2 改流式时，**只动 `ai.cpp`**，`agent.cpp` 和 `main.cpp` 一行不改——这就是分层的回报。
