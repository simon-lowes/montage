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
// Tools run one at a time; any called with "async": true runs in the
// background, on a thread of its own, and is read or cancelled with
// montage_job (answered at once while a job runs).
#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace montage {

// The paths a tool call's arguments name, each as one name for its file (symbolic links followed, letter case folded
// where the file system ignores it): what the call writes (output, report, any *_output or *_report; an export
// tool's path and file; and of a tool that changes the project its folders too: folder, dir, any *_folder or *_dir)
// and what it reads (path, file, files, paths, any *_path or *_file, and a read-only tool's folders; lists of them
// too). A path it both reads and writes counts as written. Background calls wait on these, so two never write one
// file at once.
struct McpCallPaths {
    QStringList writes, reads;
};
McpCallPaths mcpCallPaths(const QString& tool, const QJsonObject& arguments, bool readOnly);
// Whether two such paths are one file, or one lies inside the other (a file in a folder another call writes).
bool mcpPathsOverlap(const QString& a, const QString& b);

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
    // Cancels the call with request id `id` (running, or when it comes): the work it started stops where it can, and
    // the call answers with an error. Any thread. (notifications/cancelled does the same.)
    void cancel(const QJsonValue& requestId);
    // Notifications (progress) as they happen, from the thread the tool runs on, rather than among handle()'s lines.
    void setNotificationSink(std::function<void(const std::string&)> sink);
    // Serves `in` until it closes, writing to `out` (answers in the order calls finish: tool calls run in turn on a
    // thread of their own, while montage_job, cancellations and the rest are answered as they come). Returns the
    // exit code.
    int run(std::istream& in, std::ostream& out);
    // Takes up again the background calls a server before this one left when its process stopped or crashed (those of
    // tools that leave the project as it is: renders, exports, analyses), as `montage-cli mcp` does when it starts.
    // Returns how many.
    int resumeStoredJobs();

    static const std::vector<std::string>& protocolVersions();  // newest first

private:
    struct Impl;
    Impl* d_;
};

}  // namespace montage
