// tools.h - 工具系统（agent 的"手"）
//
// agent 之所以是 agent 而不是 chatbot，就在于 LLM 能调用工具。
// 工具 = 名字 + 描述 + 参数schema + 执行函数。
//
// 对应 pi：pi 的内置工具（read/bash/write/edit/grep/find/ls）在
// packages/coding-agent/src/core/tools/ 下。阶段8 加 extension 时，
// pi.registerTool() 注册的工具和内置工具平起平坐——"一切皆扩展"。
//
// 阶段3 先实现两个最核心的工具：read（读文件）、bash（执行命令）。
#ifndef MINPI_TOOLS_H
#define MINPI_TOOLS_H

#include <string>
#include <functional>
#include <vector>

// 工具调用（LLM 发起的）
struct ToolCall {
    std::string id;         // LLM 给的 ID，回填结果时要对应
    std::string name;        // 工具名，如 "read"
    std::string arguments;   // 参数 JSON 字符串，如 {"path":"test.txt"}
};

// 工具执行结果
struct ToolResult {
    std::string content;     // 文本结果（发回给 LLM）
    bool is_error = false;   // 是否出错
};

// 工具定义
struct Tool {
    std::string name;            // "read", "bash"
    std::string description;     // 给 LLM 看的说明
    std::string parameters_json; // JSON schema（参数定义），直接拼进请求 body
    // 执行函数：传入参数 JSON，返回结果
    std::function<ToolResult(const std::string& args)> execute;
};

// 创建内置工具
Tool make_read_tool();
Tool make_bash_tool();

// 工具表：管理多个工具，按名查找
struct ToolRegistry {
    std::vector<Tool> tools;

    void add(Tool t) { tools.push_back(std::move(t)); }
    Tool* find(const std::string& name) {
        for (auto& t : tools) if (t.name == name) return &t;
        return nullptr;
    }
    // 执行一次工具调用
    ToolResult execute(const ToolCall& tc) {
        Tool* t = find(tc.name);
        if (!t) return {"Unknown tool: " + tc.name, true};
        return t->execute(tc.arguments);
    }
};

// 创建包含所有内置工具的注册表
ToolRegistry make_builtin_tools();

#endif // MINPI_TOOLS_H
