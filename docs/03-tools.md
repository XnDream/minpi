# 阶段 3：工具系统 + agent loop

## 目标

让 minpi 从"chatbot"变成"agent"——LLM 能调用工具（read/bash），能多轮试错。

## agent 之魂：agent loop

chatbot 是问→答。agent 是**循环**：

```
用户: "当前目录有哪些文件？"
    ↓
LLM 返回: "我要调 bash 工具，参数 ls"     ← 不直接回答，而是要调工具
    ↓
agent 执行 bash("ls")，得到结果
    ↓
agent 把结果塞回消息历史，再次调 LLM
    ↓
LLM 看到结果，回复: "当前目录有 main.cpp、ai.cpp..."
```

代码：

```cpp
for (int turn = 0; ; ++turn) {
    调 ai_stream（带工具定义）
    把 assistant 回复（文本 + tool_calls）加入历史
    if (没有 tool_calls) break;          // LLM 不再要工具，结束
    执行每个工具，结果作为 role:Tool 消息加入历史
    // 继续下一轮：LLM 看到工具结果后继续
}
```

这就是 pi 的 `Agent` 类的核心，对应 `packages/agent`。

## 工具定义

工具 = 名字 + 描述 + 参数 schema + 执行函数：

```cpp
struct Tool {
    std::string name;            // "read", "bash"
    std::string description;     // 给 LLM 看的说明
    std::string parameters_json; // JSON schema，拼进请求 body
    std::function<ToolResult(const std::string&)> execute;
};
```

请求 body 里加 `tools` 数组，LLM 据此知道有哪些工具可调：

```json
{"tools":[{"type":"function","function":{
  "name":"bash",
  "description":"Execute a shell command...",
  "parameters":{"type":"object","properties":{"command":{"type":"string"}}}
}}]}
```

对应 pi 的内置工具（read/bash/write/edit/grep/find/ls）+ `pi.registerTool()` 注册的扩展工具。

## 流式 tool_calls 的难点：参数分片

LLM 调工具时，参数 JSON **不是一次性给全**，而是分片传来：

```
data: {"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"read","arguments":""}}]}}
data: {"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"path\""}}]}}
data: {"delta":{"tool_calls":[{"index":0,"function":{"arguments":":\"test.txt\"}"}}]}}
```

要按 `index` 累积 `arguments` 片段，直到这个 tool_call 结束（`finish_reason: "tool_calls"`）。最后拼成完整的 `{"path":"test.txt"}`。

## 三种消息角色

```
user       用户说的话
assistant  LLM 的回复（可能含 tool_calls）
tool       工具执行结果（带 tool_call_id 对应是哪个调用）
```

工具结果**作为消息加入历史**，不是函数返回值。LLM 下一轮能看到所有工具结果。这是 agent 能"多步推理"的基础。

## agent 的试错学习

LLM 不一定一次调对工具。典型场景：在 Windows 上查时间，LLM 先按 Linux 经验调 `date "+%Y..."` 失败，看到错误后换 `date`，又发现进交互模式，最后换 `echo %TIME% %DATE%` 成功。

每次失败的结果都作为 `role:Tool` 消息进历史，LLM 下一轮能看到前面的失败，所以不会重试同样的命令。**这就是 agent 比 chatbot 强的地方**：能从工具结果里学习、调整策略。

## bash 工具的交互命令卡死问题

**问题**：Windows 的 `date` 等命令是交互式的——显示后等键盘输入。如果子进程继承父进程（终端）的 stdin，会永久阻塞。

**错误修复**：`si.hStdInput = nullptr` —— **无效**。在 `bInheritHandles=TRUE` 时，子进程还是继承父进程 stdin。

**正确修复**：打开 **NUL 设备**当子进程 stdin。NUL 是 Windows 的空设备，读到立即返回 EOF。交互命令一读就 EOF，立即退出。

```cpp
HANDLE hNullInput = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ,
    &sa, OPEN_EXISTING, 0, nullptr);
si.hStdInput = hNullInput;  // NUL：读到就 EOF，交互命令立即退出
```

NUL 没让命令"成功"——它还是失败（exit code 1）。但 NUL 让它**快速失败而不是卡死**，agent loop 能继续，LLM 能看到失败、换策略。

**这就是 abort/超时机制的本质价值**：不是让命令成功，而是让 agent 不被卡住。本项目还加了 10 秒超时强杀作为兜底。

## 输出编码

cmd.exe 输出是 GBK（系统代码页），但 LLM 要 UTF-8。bash 工具返回前用 `gbk_to_utf8()` 转换，否则 LLM 看到乱码无法理解工具结果。

## 思考过程（reasoning_content）

GLM / o1 / Claude 等推理模型会返回 `reasoning_content`（思考过程）。本项目加了 `ThinkingDelta` 事件，用灰色 ANSI 显示，和正文区分：

```
assistant> The user is greeting me... I should respond in kind.  ← 灰色（思考）
 你好！有什么可以帮你？😊                                       ← 正常色（正文）
```

对应 pi 的 `thinking_delta` 事件。
