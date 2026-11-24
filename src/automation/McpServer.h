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
// where the file system ignores it): what the call writes (output, report, any *_output or *_report; export, chan,
// ply, usd, save and destinations; the path and file, any *_path or *_file, of an export tool and of any tool that is
// not read-only; and of a tool that is not read-only its folders too: folder, dir, any *_folder or *_dir) and what it
// reads (a read-only tool's path, file and folders; files and paths; lists of them too), in the objects inside the
// arguments as well; a recording (record, any *_record, and a live package's own, beside its folder, when
// montage_render names none) is written. A path it both reads and writes counts as written. Background calls wait
// on these, so two never write one file at once. (project and farm are left out: many calls share them, and a call
// that changes a project runs alone.) montage_production's folder is written when it makes one. A path given after
// protocol names ("file:", "md5:file:"), as FFmpeg takes them, is the file they reach.
struct McpCallPaths {
    QStringList writes, reads;
};
McpCallPaths mcpCallPaths(const QString& tool, const QJsonObject& arguments, bool readOnly);
// Whether two such paths are one file, or one lies inside the other (a file in a folder another call writes).
bool mcpPathsOverlap(const QString& a, const QString& b);
// How a file system tells names apart: exactly (Linux), with letter case and the composition of accented letters
// ignored (macOS), or as Windows does, which also takes a component with a stream name after it ("x:$DATA") or with
// dots or spaces after it as the same file. mcpNameRules: this computer's.
enum class McpNameRules { Exact, Folded, Windows };
McpNameRules mcpNameRules();
// A name as those rules fold it, so that two spellings of one file are the same text.
QString mcpFoldedName(const QString& name, McpNameRules rules);
// Whether `path` is `own` or a file inside it (with `orHolding`, also a folder that holds it): by name, symbolic links
// followed where the path exists and the names folded by `rules`, or by file identity where both exist (a hard link,
// a folder mounted twice, a short or differently spelled name the file system takes as the same file).
bool mcpPathReaches(const QString& path, const QString& own, McpNameRules rules, bool orHolding);
// Why a call is refused for what only Montage writes, and only as the user asks (the roots the user trusts for
// facility lists, the facility list, Montage's DCP certificates and the DKDMs); empty when it names none of them. A
// path the call writes may not be one of them, inside one, or a folder that holds one; and no other text anywhere in
// the arguments, whatever its key, may name one of them or a file inside them, but for montage_make_kdm's DKDM
// (dkdm) and servers' certificates (servers), which it reads. Arguments nested more than 16 deep are refused.
QString mcpOwnFileNamed(const QString& tool, const QJsonObject& arguments, const McpCallPaths& paths);

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
