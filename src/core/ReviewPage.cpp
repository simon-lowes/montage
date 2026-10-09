#include "ReviewPage.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <algorithm>
#include <cmath>

#include "History.h"

namespace montage {

namespace {

// Each reviewer's colour, in the order they first appear: Rose, Caribbean, Mango, Violet, Forest, Yellow, Cerulean, Red
// (core/MediaLog.h labels; the page uses the same).
constexpr int kAuthorLabels[] = {7, 3, 8, 1, 6, 9, 5, 11};

std::string htmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    return out;
}

const char* kPage = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="generator" content="Montage">
<title>@@TITLE@@ - Review</title>
<style>
:root{--bg:#141518;--panel:#1d1f24;--line:#2e3138;--text:#e8e8ea;--muted:#9c9ea5;--accent:#5b8def;--on-accent:#fff;color-scheme:dark}
@media (prefers-color-scheme: light){:root{--bg:#f3f3f5;--panel:#fff;--line:#d8d9de;--text:#1b1c1f;--muted:#5b5e66;--accent:#2f63d1;--on-accent:#fff;color-scheme:light}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
header{padding:14px 20px;border-bottom:1px solid var(--line);display:flex;gap:6px 14px;align-items:baseline;flex-wrap:wrap}
header h1{font-size:18px;margin:0;font-weight:600;overflow-wrap:anywhere}
.meta{color:var(--muted);font-size:13px}
.alert{color:#e07a5f;font-size:14px;flex-basis:100%}
main{display:grid;grid-template-columns:minmax(0,1fr) 360px;gap:16px;padding:16px 20px;max-width:1600px;margin:0 auto}
@media (max-width:900px){main{grid-template-columns:minmax(0,1fr);padding:12px 16px}}
.stage{background:#000;border-radius:6px;overflow:hidden}
video{display:block;width:100%;max-height:70vh;background:#000}
.transport{display:flex;align-items:center;gap:6px;flex-wrap:wrap;margin-top:10px}
button{font:inherit;color:var(--text);background:var(--panel);border:1px solid var(--line);border-radius:5px;padding:6px 10px;cursor:pointer;min-height:34px}
button:hover{border-color:var(--accent)}
button.primary{background:var(--accent);border-color:var(--accent);color:var(--on-accent)}
button:focus-visible,textarea:focus-visible,input:focus-visible,#strip:focus-visible,li.note:focus-visible{outline:2px solid var(--accent);outline-offset:1px}
.tc{font:600 18px/1 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;padding:0 8px;font-variant-numeric:tabular-nums}
.range{color:var(--muted);font-size:13px}
#strip{position:relative;height:30px;margin-top:10px;background:var(--panel);border:1px solid var(--line);border-radius:4px;cursor:pointer;touch-action:none;overflow:hidden}
#strip .head{position:absolute;top:0;bottom:0;width:2px;margin-left:-1px;background:var(--accent);pointer-events:none}
#strip .sel{position:absolute;top:0;bottom:0;background:rgba(91,141,239,.25);pointer-events:none}
#strip .tick{position:absolute;top:4px;bottom:4px;min-width:3px;border-radius:2px;pointer-events:none}
#strip .tick.editor{top:auto;height:5px;bottom:2px}
aside{display:flex;flex-direction:column;gap:12px;min-width:0}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:6px;padding:12px}
.panel h2{font-size:13px;text-transform:uppercase;letter-spacing:.05em;color:var(--muted);margin:0 0 8px;font-weight:600}
label{display:block;font-size:13px;color:var(--muted);margin:0 0 4px}
input[type=text],textarea{width:100%;font:inherit;color:var(--text);background:var(--bg);border:1px solid var(--line);border-radius:5px;padding:7px 9px}
textarea{min-height:84px;resize:vertical}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:8px}
.at{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:13px;color:var(--muted)}
ol{list-style:none;margin:0;padding:0;display:flex;flex-direction:column;gap:6px}
li.note{border:1px solid var(--line);border-left:4px solid var(--c,var(--accent));border-radius:5px;padding:7px 9px;cursor:pointer}
li.note.current{background:rgba(91,141,239,.12)}
li.note.done .text{text-decoration:line-through;color:var(--muted)}
li .top{display:flex;gap:8px;align-items:baseline;flex-wrap:wrap;font-size:13px}
li .who{font-weight:600}
li .text{white-space:pre-wrap;overflow-wrap:anywhere;margin-top:2px}
li .tools{display:flex;gap:6px;margin-top:6px}
li .tools button{padding:2px 8px;font-size:12px;min-height:26px}
.empty{color:var(--muted);font-size:14px;margin:0}
.keys{color:var(--muted);font-size:12px;margin:8px 0 0}
.status{color:var(--muted);font-size:13px}
#editorNote{white-space:pre-wrap;margin:0 0 8px}
</style>
</head>
<body>
<header><h1 id="title">@@TITLE@@</h1><span class="meta" id="meta"></span><span class="alert" id="alert" role="alert" hidden></span></header>
<main>
<section aria-label="Player">
<div class="stage"><video id="video" preload="auto" playsinline></video></div>
<div class="transport" role="toolbar" aria-label="Playback">
<button id="start" title="Go to the start (Home)" aria-label="Go to the start">&#x23EE;</button>
<button id="back" title="Back one frame (Left arrow)" aria-label="Back one frame">&#x25C0;</button>
<button id="play" class="primary" title="Play or pause (Space or K)" aria-label="Play">&#x25B6;</button>
<button id="fwd" title="Forward one frame (Right arrow)" aria-label="Forward one frame">&#x25B6;|</button>
<button id="end" title="Go to the end (End)" aria-label="Go to the end">&#x23ED;</button>
<span class="tc" id="tc">00:00:00:00</span>
<button id="markIn" title="Mark in (I)">In</button>
<button id="markOut" title="Mark out (O)">Out</button>
<button id="clearRange" title="Clear in and out (X)">Clear</button>
<span class="range" id="range"></span>
</div>
<div id="strip" role="slider" tabindex="0" aria-label="Position in the cut" aria-valuemin="0"></div>
<p class="keys">Space or K play and pause &middot; J back a second &middot; L play, then faster &middot; &larr; &rarr; one frame &middot; Shift+&larr; &rarr; one second &middot; I and O mark a range &middot; N write a note &middot; Ctrl+Enter add it</p>
</section>
<aside>
<div class="panel" id="editorPanel" hidden><h2>From the editor</h2><p id="editorNote" hidden></p><ol id="editorMarkers"></ol></div>
<div class="panel">
<h2>Add a note</h2>
<label for="author">Your name</label>
<input type="text" id="author" autocomplete="name">
<label for="text" style="margin-top:8px">Note <span class="at" id="noteAt"></span></label>
<textarea id="text" placeholder="What should change here?"></textarea>
)HTML"
    R"HTML(<div class="row"><button class="primary" id="add">Add Note</button><span class="status" id="status" role="status"></span></div>
</div>
<div class="panel">
<h2>Notes <span id="count"></span></h2>
<ol id="notes"></ol>
<p class="empty" id="empty">No notes yet. Pause on a frame and write one.</p>
<div class="row"><button class="primary" id="save">Save Notes File</button><button id="load">Add Notes File&hellip;</button><input type="file" id="file" accept=".json,application/json" multiple hidden></div>
<p class="keys">Send the saved file to the editor: each note opens as a marker at its frame.</p>
</div>
</aside>
</main>
<script type="application/json" id="review-config">@@CONFIG@@</script>
<script>
(() => {
"use strict";
const cfg = JSON.parse(document.getElementById("review-config").textContent);
const $ = (id) => document.getElementById(id);
const video = $("video");
const rate = cfg.fps[0] / cfg.fps[1];
const nominal = Math.round(rate) || 30;
const dropFrame = !!cfg.dropFrame;
const frames = Math.max(1, cfg.frames);
const last = frames - 1;
const palette = ["", "#8a5cc9", "#5b6ed8", "#2a9d8f", "#b28dd8", "#2f8fc9", "#3d8a48", "#d15f8c", "#e0933c", "#d8c23a", "#a88a64", "#c84646"];
const authorLabels = [7, 3, 8, 1, 6, 9, 5, 11];

// SMPTE timecode, as Montage writes it (drop frame skips two or four numbers a minute, except every tenth).
function tc(frame) {
  let f = Math.max(0, Math.round(frame));
  if (dropFrame) {
    const drop = nominal === 60 ? 4 : 2;
    const per10 = Math.round(rate * 600), perMin = nominal * 60 - drop;
    const d = Math.floor(f / per10), m = f % per10;
    f += drop * 9 * d + (m > drop ? drop * Math.floor((m - drop) / perMin) : 0);
  }
  const pad = (n) => String(n).padStart(2, "0");
  const s = Math.floor(f / nominal);
  return pad(Math.floor(s / 3600)) + ":" + pad(Math.floor(s / 60) % 60) + ":" + pad(s % 60) + (dropFrame ? ";" : ":") + pad(f % nominal);
}
const span = (cls, text) => { const e = document.createElement("span"); e.className = cls; e.textContent = text; return e; };
const pct = (f) => (100 * f / frames) + "%";
const status = (text) => { $("status").textContent = text; };

// The frame on screen, in the review copy (0 = its first frame).
let shown = 0;
const frameOf = (t) => Math.min(last, Math.max(0, Math.floor(t * rate + 0.01)));
function seekFrame(f) {
  shown = Math.min(last, Math.max(0, Math.round(f)));
  video.currentTime = (shown + 0.5) / rate;  // the middle of the frame, so rounding never lands on its neighbour
  update();
}
video.style.aspectRatio = cfg.width + " / " + cfg.height;  // the right shape before the copy loads
video.src = encodeURIComponent(cfg.video);
video.addEventListener("error", () => {
  const a = $("alert");
  a.textContent = "The review copy (" + cfg.video + ") could not be played: keep it in the same folder as this page and open the page in Chrome, Edge, Safari or Firefox.";
  a.hidden = false;
});
if ("requestVideoFrameCallback" in HTMLVideoElement.prototype) {
  const onFrame = (now, md) => { shown = frameOf(md.mediaTime); update(); video.requestVideoFrameCallback(onFrame); };
  video.requestVideoFrameCallback(onFrame);
} else {
  const tick = () => { if (!video.paused) { shown = frameOf(video.currentTime); update(); requestAnimationFrame(tick); } };
  video.addEventListener("play", () => requestAnimationFrame(tick));
}
video.addEventListener("seeked", () => { if (video.paused) { shown = frameOf(video.currentTime); update(); } });
video.addEventListener("play", () => { $("play").innerHTML = "&#x275A;&#x275A;"; $("play").setAttribute("aria-label", "Pause"); });
video.addEventListener("pause", () => { $("play").innerHTML = "&#x25B6;"; $("play").setAttribute("aria-label", "Play"); video.playbackRate = 1; });
function toggle() { if (video.paused) video.play(); else video.pause(); }
function faster() {
  if (video.paused) { video.playbackRate = 1; video.play(); }
  else video.playbackRate = Math.min(4, video.playbackRate * 2);
}
function step(n) { video.pause(); seekFrame(shown + n); }

// In and out for a note about a stretch.
let markIn = null, markOut = null;
function range() {
  if (markIn === null || markOut === null) return null;
  return [Math.min(markIn, markOut), Math.max(markIn, markOut)];
}
function showRange() {
  const r = range();
  $("range").textContent = r ? "In " + tc(cfg.offset + r[0]) + "  Out " + tc(cfg.offset + r[1])
    : markIn !== null ? "In " + tc(cfg.offset + markIn) : markOut !== null ? "Out " + tc(cfg.offset + markOut) : "";
  drawStrip();
}

// Notes, kept in this browser until saved to a file.
const key = "montage-review:" + cfg.id;
let notes = [];
try { const saved = JSON.parse(localStorage.getItem(key) || "[]"); if (Array.isArray(saved)) notes = saved; } catch (e) { notes = []; }
try { $("author").value = localStorage.getItem("montage-review:author") || ""; } catch (e) {}
function store() { try { localStorage.setItem(key, JSON.stringify(notes)); } catch (e) {} }
function authors() { return [...new Set(notes.map((n) => n.author))]; }
function colourOf(author, list) { return palette[authorLabels[Math.max(0, list.indexOf(author)) % authorLabels.length]]; }
function newId() { return Date.now().toString(36) + Math.random().toString(36).slice(2, 8); }

function addNote() {
  const text = $("text").value.trim();
  if (!text) { status("Write the note first"); $("text").focus(); return; }
  const author = $("author").value.trim() || "Reviewer";
  try { localStorage.setItem("montage-review:author", author); } catch (e) {}
  const r = range();
  const start = r ? r[0] : shown;
  notes.push({ id: newId(), frame: cfg.offset + start, duration: r ? r[1] - r[0] + 1 : 0, author, text, done: false, created: new Date().toISOString() });
  notes.sort((a, b) => a.frame - b.frame);
  store();
  $("text").value = "";
  render();
  status("Added at " + tc(cfg.offset + start));
}

function render() {
  const list = $("notes");
  list.textContent = "";
  $("empty").hidden = notes.length > 0;
  $("count").textContent = notes.length ? "(" + notes.length + ")" : "";
  const who = authors();
  for (const n of notes) {
    const li = document.createElement("li");
)HTML"
    R"HTML(    li.className = "note" + (n.done ? " done" : "");
    li.style.setProperty("--c", colourOf(n.author, who));
    li.dataset.frame = n.frame;
    li.dataset.duration = n.duration;
    li.tabIndex = 0;
    const top = document.createElement("div");
    top.className = "top";
    top.append(span("at", tc(n.frame) + (n.duration > 1 ? " - " + tc(n.frame + n.duration - 1) : "")), span("who", n.author));
    const text = document.createElement("div");
    text.className = "text";
    text.textContent = n.text;
    const tools = document.createElement("div");
    tools.className = "tools";
    const done = document.createElement("button");
    done.textContent = n.done ? "Reopen" : "Resolve";
    done.addEventListener("click", () => { n.done = !n.done; store(); render(); });
    const del = document.createElement("button");
    del.textContent = "Delete";
    del.addEventListener("click", () => { if (confirm("Delete this note?")) { notes = notes.filter((x) => x !== n); store(); render(); } });
    tools.append(done, del);
    li.append(top, text, tools);
    const go = () => { video.pause(); seekFrame(n.frame - cfg.offset); };
    li.addEventListener("click", (e) => { if (!e.target.closest("button")) go(); });
    li.addEventListener("keydown", (e) => { if (e.key === "Enter" && e.target === li) { e.preventDefault(); go(); } });
    list.append(li);
  }
  drawStrip();
}

let head = null;
function drawStrip() {
  const strip = $("strip");
  strip.textContent = "";
  const r = range();
  if (r) {
    const sel = document.createElement("div");
    sel.className = "sel";
    sel.style.left = pct(r[0]);
    sel.style.width = pct(r[1] - r[0] + 1);
    strip.append(sel);
  }
  const tick = (frame, duration, colour, cls) => {
    const t = document.createElement("div");
    t.className = "tick" + (cls ? " " + cls : "");
    t.style.left = pct(frame);
    if (duration > 1) t.style.width = pct(duration);
    t.style.background = colour;
    strip.append(t);
  };
  for (const m of cfg.markers) tick(m.frame - cfg.offset, m.duration, palette[m.color] || "#9c9ea5", "editor");
  const who = authors();
  for (const n of notes) tick(n.frame - cfg.offset, n.duration, colourOf(n.author, who), n.done ? "" : "");
  head = document.createElement("div");
  head.className = "head";
  strip.append(head);
  update();
}

function update() {
  const at = cfg.offset + shown;
  $("tc").textContent = tc(at);
  if (head) head.style.left = pct(shown + 0.5);
  const strip = $("strip");
  strip.setAttribute("aria-valuemax", String(last));
  strip.setAttribute("aria-valuenow", String(shown));
  strip.setAttribute("aria-valuetext", tc(at));
  const r = range();
  $("noteAt").textContent = r ? "at " + tc(cfg.offset + r[0]) + " - " + tc(cfg.offset + r[1]) : "at " + tc(at);
  for (const li of $("notes").children) {
    const f = Number(li.dataset.frame), d = Math.max(1, Number(li.dataset.duration));
    li.classList.toggle("current", at >= f && at < f + d);
  }
}

function safeName(s) { return s.replace(/[\\/:*?"<>|]/g, "-").trim() || "notes"; }
function saveFile() {
  const data = {
    montageReview: 1, title: cfg.title, id: cfg.id, fps: cfg.fps, dropFrame, saved: new Date().toISOString(),
    notes: notes.map((n) => ({ frame: n.frame, duration: n.duration, timecode: tc(n.frame), author: n.author, text: n.text, done: !!n.done, created: n.created, id: n.id })),
  };
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([JSON.stringify(data, null, 2)], { type: "application/json" }));
  a.download = safeName(cfg.title) + " - " + safeName($("author").value.trim() || "Reviewer") + " notes.json";
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 2000);
  status("Saved " + a.download);
}
async function loadFiles(files) {
  let added = 0;
  for (const file of files) {
    try {
      const data = JSON.parse(await file.text());
      if (!data || !Array.isArray(data.notes)) throw new Error("not a notes file");
      const theirs = Array.isArray(data.fps) && data.fps[1] ? data.fps[0] / data.fps[1] : rate;
      for (const n of data.notes) {
        if (typeof n.frame !== "number" || typeof n.text !== "string" || !n.text.trim()) continue;
        if (n.id && notes.some((x) => x.id === n.id)) continue;
        notes.push({ id: n.id || newId(), frame: Math.round(n.frame * rate / theirs), duration: Math.round((n.duration || 0) * rate / theirs),
          author: String(n.author || "Reviewer"), text: n.text, done: !!n.done, created: n.created || "" });
        ++added;
      }
    } catch (e) {
      status(file.name + " is not a notes file");
    }
  }
  notes.sort((a, b) => a.frame - b.frame);
  store();
  render();
  if (added) status("Added " + added + (added === 1 ? " note" : " notes"));
}

// The editor's markers and message.
if (cfg.note || cfg.markers.length) {
  $("editorPanel").hidden = false;
  if (cfg.note) { $("editorNote").textContent = cfg.note; $("editorNote").hidden = false; }
  for (const m of cfg.markers) {
    const li = document.createElement("li");
    li.className = "note";
    li.tabIndex = 0;
    li.style.setProperty("--c", palette[m.color] || "#9c9ea5");
    const top = document.createElement("div");
    top.className = "top";
    top.append(span("at", tc(m.frame)), span("who", m.name || (m.chapter ? "Chapter" : "Marker")));
    li.append(top);
    if (m.comment) { const t = document.createElement("div"); t.className = "text"; t.textContent = m.comment; li.append(t); }
    const go = () => { video.pause(); seekFrame(m.frame - cfg.offset); };
    li.addEventListener("click", go);
    li.addEventListener("keydown", (e) => { if (e.key === "Enter") { e.preventDefault(); go(); } });
    $("editorMarkers").append(li);
  }
}
$("meta").textContent = cfg.width + " x " + cfg.height + " · " + (Math.round(rate * 1000) / 1000) + " fps · " + tc(frames) + " long"
  + (cfg.offset ? " · from " + tc(cfg.offset) : "");

)HTML"
    R"HTML($("start").addEventListener("click", () => { video.pause(); seekFrame(0); });
$("end").addEventListener("click", () => { video.pause(); seekFrame(last); });
$("back").addEventListener("click", () => step(-1));
$("fwd").addEventListener("click", () => step(1));
$("play").addEventListener("click", toggle);
$("markIn").addEventListener("click", () => { markIn = shown; showRange(); });
$("markOut").addEventListener("click", () => { markOut = shown; showRange(); });
$("clearRange").addEventListener("click", () => { markIn = markOut = null; showRange(); });
$("add").addEventListener("click", addNote);
$("save").addEventListener("click", saveFile);
$("load").addEventListener("click", () => $("file").click());
$("file").addEventListener("change", (e) => { loadFiles([...e.target.files]); e.target.value = ""; });
$("text").addEventListener("focus", () => video.pause());
$("text").addEventListener("input", () => video.pause());
const strip = $("strip");
const scrub = (e) => { const b = strip.getBoundingClientRect(); seekFrame(Math.floor((e.clientX - b.left) / b.width * frames)); };
strip.addEventListener("pointerdown", (e) => { video.pause(); strip.setPointerCapture(e.pointerId); scrub(e); });
strip.addEventListener("pointermove", (e) => { if (strip.hasPointerCapture(e.pointerId)) scrub(e); });
document.addEventListener("keydown", (e) => {
  const t = e.target;
  if (t.closest && t.closest("input, textarea")) {
    if (e.key === "Enter" && (e.ctrlKey || e.metaKey) && t.id === "text") { e.preventDefault(); addNote(); }
    else if (e.key === "Escape") t.blur();
    return;
  }
  if (e.ctrlKey || e.metaKey || e.altKey) return;
  if (t.tagName === "BUTTON" && (e.key === " " || e.key === "Enter")) return;
  switch (e.key) {
    case " ": case "k": case "K": e.preventDefault(); toggle(); break;
    case "j": case "J": step(-nominal); break;
    case "l": case "L": faster(); break;
    case "ArrowLeft": e.preventDefault(); step(e.shiftKey ? -nominal : -1); break;
    case "ArrowRight": e.preventDefault(); step(e.shiftKey ? nominal : 1); break;
    case "Home": e.preventDefault(); step(-shown); break;
    case "End": e.preventDefault(); step(last - shown); break;
    case "i": case "I": markIn = shown; showRange(); break;
    case "o": case "O": markOut = shown; showRange(); break;
    case "x": case "X": markIn = markOut = null; showRange(); break;
    case "n": case "N": e.preventDefault(); video.pause(); $("text").focus(); break;
  }
});
render();
})();
</script>
</body>
</html>
)HTML";

}  // namespace

ReviewPageInfo reviewPageInfo(const Sequence& s, const std::string& videoFile, FrameTime in, FrameTime out, bool withMarkers) {
    ReviewPageInfo info;
    info.title = s.name;
    info.videoFile = videoFile;
    info.fps = s.fps;
    info.dropFrame = isDropFrameRate(s.fps);
    info.offset = std::max<FrameTime>(0, in);
    const FrameTime end = out < 0 ? s.duration() : std::min(out, s.duration());
    info.frames = std::max<FrameTime>(0, end - info.offset);
    info.width = s.width;
    info.height = s.height;
    info.id = QStringLiteral("%1-%2").arg(QString::fromStdString(s.name)).arg(QDateTime::currentMSecsSinceEpoch(), 0, 36).toStdString();
    if (withMarkers)
        for (const Marker& m : s.markers)
            if (m.t >= info.offset && m.t < end) info.markers.push_back(m);
    return info;
}

std::string reviewPageHtml(const ReviewPageInfo& info) {
    QJsonArray markers;
    for (const Marker& m : info.markers)
        markers.append(QJsonObject{{"frame", double(m.t)}, {"duration", double(m.duration)}, {"name", QString::fromStdString(m.name)},
                                   {"comment", QString::fromStdString(m.comment)}, {"color", m.color}, {"chapter", m.chapter}});
    const QJsonObject cfg{{"title", QString::fromStdString(info.title)},
                          {"id", QString::fromStdString(info.id)},
                          {"video", QString::fromStdString(info.videoFile)},
                          {"fps", QJsonArray{double(info.fps.num), double(info.fps.den)}},
                          {"dropFrame", info.dropFrame},
                          {"offset", double(info.offset)},
                          {"frames", double(info.frames)},
                          {"width", info.width},
                          {"height", info.height},
                          {"markers", markers},
                          {"note", QString::fromStdString(info.note)}};
    // '<' only appears inside JSON strings, where < reads the same and cannot close the script element.
    std::string json = QJsonDocument(cfg).toJson(QJsonDocument::Compact).toStdString();
    std::string safe;
    safe.reserve(json.size());
    for (char c : json) {
        if (c == '<') safe += "\\u003c";
        else safe += c;
    }
    std::string page = kPage;
    const std::string title = htmlEscape(info.title.empty() ? std::string("Untitled") : info.title);
    for (size_t at; (at = page.find("@@TITLE@@")) != std::string::npos;) page.replace(at, 9, title);
    page.replace(page.find("@@CONFIG@@"), 10, safe);
    return page;
}

bool isReviewNotes(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    return first != std::string::npos && text[first] == '{' && text.find("\"montageReview\"") != std::string::npos;
}

bool parseReviewNotes(const std::string& json, Rational fps, std::vector<ReviewNote>& out, std::string* title, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    QJsonParseError pe;
    std::string body = json;
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF) body = body.substr(3);  // UTF-8 BOM
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(body), &pe);
    if (!doc.isObject() || !doc.object().contains("montageReview")) return fail("This is not a notes file saved from a Montage review page");
    const QJsonObject o = doc.object();
    if (title) *title = o.value("title").toString().toStdString();
    // Frames in the review copy's rate, brought to `fps`.
    double scale = 1;
    const QJsonArray theirs = o.value("fps").toArray();
    if (theirs.size() == 2 && theirs[0].toDouble() > 0 && theirs[1].toDouble() > 0 && fps.toDouble() > 0)
        scale = fps.toDouble() / (theirs[0].toDouble() / theirs[1].toDouble());
    const Rational noteFps = theirs.size() == 2 && theirs[1].toDouble() > 0 ? Rational{int(theirs[0].toDouble()), int(theirs[1].toDouble())} : fps;
    const size_t before = out.size();
    for (const QJsonValue& v : o.value("notes").toArray()) {
        const QJsonObject n = v.toObject();
        ReviewNote note;
        note.text = n.value("text").toString().trimmed().toStdString();
        if (note.text.empty()) continue;
        FrameTime f = 0;
        if (n.value("frame").isDouble()) f = FrameTime(std::llround(n.value("frame").toDouble()));
        else if (!parseTimecode(n.value("timecode").toString().toStdString(), noteFps, f)) continue;
        note.frame = std::max<FrameTime>(0, FrameTime(std::llround(double(f) * scale)));
        note.duration = std::max<FrameTime>(0, FrameTime(std::llround(n.value("duration").toDouble() * scale)));
        note.author = n.value("author").toString().trimmed().toStdString();
        if (note.author.empty()) note.author = "Reviewer";
        note.done = n.value("done").toBool();
        out.push_back(note);
    }
    if (out.size() == before) return fail("The notes file has no notes");
    std::stable_sort(out.begin() + std::ptrdiff_t(before), out.end(), [](const ReviewNote& a, const ReviewNote& b) { return a.frame < b.frame; });
    return true;
}

std::vector<Marker> reviewNotesToMarkers(const std::vector<ReviewNote>& notes) {
    std::vector<std::string> authors;
    std::vector<Marker> out;
    for (const ReviewNote& n : notes) {
        auto it = std::find(authors.begin(), authors.end(), n.author);
        if (it == authors.end()) it = authors.insert(authors.end(), n.author);
        Marker m;
        m.t = n.frame;
        m.duration = n.duration;
        m.name = (n.done ? "\xE2\x9C\x93 " : "") + n.author;  // ✓
        m.comment = n.text;
        m.color = kAuthorLabels[size_t(it - authors.begin()) % std::size(kAuthorLabels)];
        out.push_back(m);
    }
    return out;
}

}  // namespace montage
