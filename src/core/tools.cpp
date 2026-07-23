// tools.cpp - 工具实现：read（读文件）、bash（执行命令）
//
// 两个最核心的工具，足以让 agent 能"看文件"和"跑命令"。
// 对应 pi 的 packages/coding-agent/src/core/tools/read.ts 和 bash.ts。
//
// 参数 schema 用 JSON Schema 格式，直接拼进请求 body 的 tools 数组。
// LLM 据此知道每个工具要什么参数。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "tools.h"
#include "core/json.h"
#include "core/codecvt.h"

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>

// ---------- read 工具：读文件内容 ----------

static ToolResult read_execute(const std::string& args) {
    // 从 args JSON 里提取 path 参数
    std::string path;
    if (!json_get_string(args, "path", path)) {
        return {"Missing 'path' parameter", true};
    }

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return {"File not found: " + path, true};
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();

    // 截断保护：和 pi 一样，工具输出有上限（50KB / 2000 行）
    // 避免 read 一个大文件把上下文撑爆。阶段7 compaction 会讲为什么。
    const size_t MAX_BYTES = 50 * 1024;
    if (content.size() > MAX_BYTES) {
        content.resize(MAX_BYTES);
        content += "\n\n[... truncated, " + std::to_string(content.size()) + " bytes shown of more]";
    }

    return {content, false};
}

Tool make_read_tool() {
    Tool t;
    t.name = "read";
    t.description = "Read the contents of a file. Use this to examine source code, config files, etc. "
                    "Parameters: path (string, required) - the file path to read.";
    t.parameters_json = R"({
        "type": "object",
        "properties": {
            "path": {
                "type": "string",
                "description": "The file path to read"
            }
        },
        "required": ["path"]
    })";
    t.execute = read_execute;
    return t;
}

// ---------- bash 工具：执行 shell 命令 ----------
//
// 用 CreateProcess 建子进程，能设超时、能强制终止。
// 不能用 _popen——遇到交互式命令（如 Windows 的 date）会永久阻塞。
//
// 对应 pi 的 bash.ts：pi 用 child_process.spawn + AbortSignal。
// 我们的 AbortSignal 就是超时（阶段4 加真正的用户中断）。

static ToolResult bash_execute(const std::string& args) {
    std::string command;
    if (!json_get_string(args, "command", command)) {
        return {"Missing 'command' parameter", true};
    }

    // 用 cmd.exe /c 执行命令。/c 表示执行完就退出。
    // 创建匿名管道读子进程 stdout。
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe = nullptr, hWritePipe = nullptr;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        return {"CreatePipe failed", true};
    }
    // 读端不继承给子进程
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    // 创建空 stdin 句柄：打开 NUL 设备。
    // 子进程读 stdin 立即得到 EOF，交互命令（如 Windows date）不会等键盘。
    // 这是修复 date 卡死的关键——si.hStdInput=nullptr 在继承模式下无效。
    HANDLE hNullInput = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ,
        &sa, OPEN_EXISTING, 0, nullptr);

    // 拼命令行：cmd.exe /c "命令"
    std::string cmdLine = "cmd.exe /c " + command;
    // CreateProcess 可能修改 cmdLine，用可写缓冲
    std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;  // stderr 也重定向到管道
    si.hStdInput = hNullInput;  // NUL：读到就 EOF，交互命令立即退出
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessA(
        nullptr,
        cmdBuf.data(),
        nullptr, nullptr,
        TRUE,               // 继承句柄
        CREATE_NO_WINDOW,   // 不弹黑框
        nullptr, nullptr,
        &si, &pi);

    // 关闭父进程持有的 NUL 句柄副本（子进程已有自己的）
    if (hNullInput && hNullInput != INVALID_HANDLE_VALUE) CloseHandle(hNullInput);

    if (!ok) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return {"CreateProcess failed (err=" + std::to_string(GetLastError()) + ")", true};
    }

    // 关闭写端的父进程副本（子进程已有自己的）
    CloseHandle(hWritePipe);

    // 读输出，带超时
    std::string output;
    const DWORD TIMEOUT_MS = 10000;  // 10 秒超时
    DWORD startTick = GetTickCount();
    bool timed_out = false;

    for (;;) {
        // 检查超时
        if (GetTickCount() - startTick > TIMEOUT_MS) {
            timed_out = true;
            break;
        }

        // 看管道里有没有数据
        DWORD avail = 0;
        if (!PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &avail, nullptr)) break;

        if (avail > 0) {
            char buf[4096];
            DWORD read = 0;
            if (ReadFile(hReadPipe, buf, sizeof(buf), &read, nullptr) && read > 0) {
                output.append(buf, read);
            }
        } else {
            // 没数据，看子进程是否还在跑
            DWORD exitCode;
            if (GetExitCodeProcess(pi.hProcess, &exitCode)) {
                if (exitCode != STILL_ACTIVE) break;  // 进程结束
            }
            Sleep(10);  // 避免空转
        }
    }

    DWORD exitCode = 1;
    if (timed_out) {
        // 超时：杀进程
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
        output += "\n[TIMEOUT: command killed after " + std::to_string(TIMEOUT_MS / 1000) + "s]";
    } else {
        GetExitCodeProcess(pi.hProcess, &exitCode);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);

    // 截断保护
    const size_t MAX_BYTES = 50 * 1024;
    if (output.size() > MAX_BYTES) {
        output.resize(MAX_BYTES);
        output += "\n\n[... truncated]";
    }

    output += "\n[exit code: " + std::to_string(exitCode) + "]";

    // cmd.exe 输出是 GBK（系统代码页），转成 UTF-8 再返回。
    // 否则 LLM 看到乱码，无法理解工具结果，可能反复试错。
    output = gbk_to_utf8(output);
    return {output, false};
}

Tool make_bash_tool() {
    Tool t;
    t.name = "bash";
    t.description = "Execute a shell command and return its output. Use this to run builds, tests, "
                    "git commands, list files, etc. Parameters: command (string, required) - the command to execute.";
    t.parameters_json = R"({
        "type": "object",
        "properties": {
            "command": {
                "type": "string",
                "description": "The shell command to execute"
            }
        },
        "required": ["command"]
    })";
    t.execute = bash_execute;
    return t;
}

// ---------- 工具注册表 ----------

ToolRegistry make_builtin_tools() {
    ToolRegistry reg;
    reg.add(make_read_tool());
    reg.add(make_bash_tool());
    return reg;
}
