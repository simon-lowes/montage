// Montage — a Model Context Protocol server, so AI agents (Claude and other
// MCP clients) can edit with Montage: probe media, build and change
// projects, add titles, effects, transitions and markers, transcribe and
// search speech, look at frames, render and exchange timelines.
//
// `montage-cli mcp` runs it on stdio (newline-delimited JSON-RPC 2.0). Both
// protocol eras are served: the stateless 2026-07-28 revision (per-request
// _meta, server/discover) and the initialize handshake of 2024-11-05 to
// 2025-11-25. Tools work on project files by path, each edit saved straight
// away with the previous version kept beside it (.bak) for montage_undo.
#pragma once

#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace montage {

class McpServer {
public:
    McpServer();
    ~McpServer();
    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    // Handles one message (one line of JSON). Returns the lines to send back:
    // notifications produced while working (progress), then the response;
    // nothing for notifications from the client.
    std::vector<std::string> handle(const std::string& message);
    // Serves `in` until it closes, writing to `out`. Returns the exit code.
    int run(std::istream& in, std::ostream& out);

    static const std::vector<std::string>& protocolVersions();  // newest first

private:
    struct Impl;
    Impl* d_;
};

}  // namespace montage
