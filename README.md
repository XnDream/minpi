# minpi — 用 C++ + Win32 从零实现 LLM Agent

一个**教学项目**：模仿 [pi](https://github.com/earendil-works/pi-mono)（一个 TypeScript 写的终端 coding agent）的核心架构，用 C++14 + Win32 API 从零实现，目的是**搞懂一个 agent harness 到底由哪些核心机制组成**。

> 适合：写过 C/C++、想理解 LLM agent 内部原理、不想被一堆 SDK 黑盒糊弄的人。

## 为什么有这个项目

市面上的 agent 工具（Claude Code、Cursor、pi、Aider...）要么是闭源，要么是 TS/Python 写的、被各种 SDK 和抽象层包着，看不清内核。

这个项目反其道而行：**零第三方依赖**（纯 Win32 + STL），每个机制都手写，每个文件对应 pi 的一个模块。读完代码你能说清楚：

- agent loop 到底怎么转的
- 流式 SSE 怎么解析的
- LLM 调工具的参数怎么分片传过来的
- 事件系统为什么能解耦 UI 和逻辑
- ...

## 当前进度

分 10+1 个阶段逐步实现，每个阶段都能独立编译运行：

| 阶段 | 主题 | 状态 | 笔记 |
|------|------|------|------|
| - | 架构设计 | ✅ | [docs/architecture.md](docs/architecture.md) |
| - | SSE 与 chunked 详解 | ✅ | [docs/sse.md](docs/sse.md) |
| 1 | HTTP + Provider 抽象 + 单轮问答 | ✅ | [docs/01-http.md](docs/01-http.md) |
| 2 | 流式 SSE + 事件系统 | ✅ | [docs/02-streaming.md](docs/02-streaming.md) |
| 3 | 工具系统 + agent loop | ✅ | [docs/03-tools.md](docs/03-tools.md) |
| 4 | Abort + Retry | 🔜 | |
| 5 | 会话持久化（JSONL） | 🔜 | |
| 6 | 会话分支（树结构） | 🔜 | |
| 7 | Compaction（上下文压缩） | 🔜 | |
| 8 | Extensions（插件系统） | 🔜 | |
| 9 | 多模式（print/json/rpc） | 🔜 | |
| 10 | 消息队列（steering/follow-up） | 🔜 | |
| +1 | Skills / AGENTS.md / Prompt 模板 | 🔜 | |

## 编译运行

**依赖**：Visual Studio 2022 Build Tools（MSVC）+ Windows 10+

```bash
# 1. 配置
cp config/minpi.json.example config/minpi.json
#   填入你的 base_url / api_key / model（任何 OpenAI 兼容的 LLM 都行）

# 2. 编译
bash build.sh

# 3. 运行
./build/minpi.exe
```

**支持的 LLM**：任何 OpenAI 兼容 API —— OpenAI / DeepSeek / Kimi / 智谱 GLM / 本地 ollama 等。

## 项目结构

```
src/
├── ai/          LLM provider 抽象   (对应 pi packages/ai)
│   ├── ai.h
│   └── ai.cpp        WinHTTP + SSE 解析 + tool_calls 分片
├── agent/       agent loop           (对应 pi packages/agent)
│   ├── agent.h
│   └── agent.cpp     while(有tool_calls){调LLM→执行工具→回填→再调}
├── core/        跨层共享
│   ├── events.h      AiEvent / AgentEvent 两层事件
│   ├── tools.h/cpp   工具接口 + read/bash 内置工具
│   ├── json.h/cpp    极简 JSON 路径解析（零依赖）
│   ├── term.h/cpp    终端折行打印
│   └── codecvt.h/cpp GBK↔UTF-8（Windows 控制台编码）
└── main.cpp     入口（事件订阅者）   (对应 pi packages/coding-agent CLI)
```

**分层纪律**（看 `#include` 就知道）：
```
main.cpp → agent/ → ai/ → core/
```
绝不反向。改流式只动 `ai/`，加工具循环只动 `agent/`，换 UI 只动 `main.cpp`。

## 核心机制速览

- **agent loop**：`while (LLM 返回 tool_calls) { 执行工具 → 结果作为 role:Tool 消息回填 → 再调 LLM }`
- **流式 SSE**：响应是一连串 `data: {...}` 行，`SseParser` 处理 chunk 边界 ≠ 行边界
- **tool_calls 分片**：工具参数 JSON 被切成片段传输，按 index 累积
- **事件系统**：ai 层推 `AiEvent`，agent 层转 `AgentEvent`，UI 订阅——同一套逻辑能跑交互/print/json/rpc 多种模式
- **bash 工具不卡死**：子进程 stdin 指向 NUL 设备（交互命令读到 EOF 立即退出）+ 10 秒超时强杀

## 配置说明

`config/minpi.json`：

```json
{
  "base_url": "https://api.openai.com",
  "api_key": "sk-...",
  "model": "gpt-4o-mini",
  "debug": false
}
```

- `debug: true` 开启详细日志（打印每次请求/响应的完整内容），学习调试时打开
- 环境变量 `MINPI_DEBUG=1` 可强制开启（优先级高于配置文件）

## 致谢

这个项目的架构设计全部来自 [pi](https://github.com/earendil-works/pi-mono)。pi 是一个设计精良的极简 agent harness，强烈推荐读它的源码和文档。

## License

MIT
