#include "ReviewPage.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <algorithm>
#include <cmath>
#include <set>

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
.stage{background:#000;border-radius:6px;overflow:hidden;position:relative}
.stage.side{display:grid;grid-template-columns:1fr 1fr;gap:2px}
.pane{position:relative;min-width:0}
.stage.wipe #prevPane{position:absolute;inset:0;clip-path:inset(0 calc(100% - var(--wipe,50%)) 0 0);z-index:1}
.stage.wipe #wipeLine{position:absolute;top:0;bottom:0;left:var(--wipe,50%);width:2px;margin-left:-1px;background:#fff;z-index:2;pointer-events:none}
video{display:block;width:100%;max-height:70vh;background:#000}
#ink{position:absolute;left:0;top:0;touch-action:none;pointer-events:none;z-index:3}
#ink.drawing{pointer-events:auto;cursor:crosshair}
.vlabel{position:absolute;top:6px;left:6px;padding:1px 6px;background:rgba(0,0,0,.65);color:#fff;font-size:12px;border-radius:3px;pointer-events:none;z-index:4}
#prevPane .vlabel{left:auto;right:6px}
.stage.wipe #prevPane .vlabel{right:auto;left:6px}
.stage.wipe #curLabel{left:auto;right:6px}
.gone{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;color:#bbb;font-size:14px;background:#000;pointer-events:none}
.drawbar{display:flex;gap:6px;align-items:center;flex-wrap:wrap;margin-top:8px}
.swatch{width:28px;min-width:28px;height:28px;min-height:28px;padding:0;border-radius:50%;border:2px solid var(--line)}
button[aria-pressed=true]{border-color:var(--accent);background:rgba(91,141,239,.2)}
.swatch[aria-pressed=true]{border-color:var(--text)}
select{font:inherit;color:var(--text);background:var(--panel);border:1px solid var(--line);border-radius:5px;padding:4px 6px;min-height:34px}
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
li .tools{display:flex;gap:6px;margin-top:6px;flex-wrap:wrap;align-items:center}
li .tools button,li .tools select{padding:2px 8px;font-size:12px;min-height:26px}
li.progress{border-left-style:dashed}
li .replies{margin:6px 0 0;padding-left:9px;border-left:2px solid var(--line);display:flex;flex-direction:column;gap:4px}
li .reply{font-size:14px;white-space:pre-wrap;overflow-wrap:anywhere}
li .reply b{font-weight:600}
li .answer{display:flex;gap:6px;margin-top:6px}
li .answer input{flex:1;min-width:0}
.badge{font-size:11px;border:1px solid var(--line);border-radius:3px;padding:0 4px;color:var(--muted)}
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
<div class="stage" id="stage">
<div class="pane" id="curPane"><video id="video" preload="auto" playsinline></video><canvas id="ink" aria-label="Drawing over the picture"></canvas><span class="vlabel" id="curLabel" hidden></span></div>
<div class="pane" id="prevPane" hidden><video id="prev" preload="auto" playsinline muted></video><span class="vlabel" id="prevLabel"></span><div class="gone" id="prevGone" hidden></div></div>
<div id="wipeLine" hidden></div>
</div>
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
<button id="draw" aria-pressed="false" title="Draw over the frame (D)">Draw</button>
<span id="compareTools" hidden><button id="compare" aria-pressed="false" title="Compare with the version before (C)"></button>
<select id="compareMode" aria-label="How to compare"><option value="side">Side by side</option><option value="wipe">Wipe</option></select>
<input type="range" id="wipe" min="0" max="100" value="50" aria-label="Wipe position" hidden></span>
<span class="range" id="range"></span>
</div>
<div class="drawbar" id="drawbar" role="toolbar" aria-label="Drawing" hidden>
<button data-tool="pen" aria-pressed="true" title="Pen">Pen</button><button data-tool="arrow" aria-pressed="false" title="Arrow">Arrow</button><button data-tool="box" aria-pressed="false" title="Box">Box</button><button data-tool="ellipse" aria-pressed="false" title="Ellipse">Ellipse</button>
<button class="swatch" data-color="#ff3b30" aria-pressed="true" title="Red" aria-label="Red" style="background:#ff3b30"></button><button class="swatch" data-color="#ffcc00" aria-pressed="false" title="Yellow" aria-label="Yellow" style="background:#ffcc00"></button><button class="swatch" data-color="#34c759" aria-pressed="false" title="Green" aria-label="Green" style="background:#34c759"></button><button class="swatch" data-color="#0a84ff" aria-pressed="false" title="Blue" aria-label="Blue" style="background:#0a84ff"></button><button class="swatch" data-color="#ffffff" aria-pressed="false" title="White" aria-label="White" style="background:#ffffff"></button>
<button id="undoStroke" title="Take back the last stroke (Ctrl+Z)">Undo</button><button id="clearInk" title="Clear the drawing">Clear Drawing</button>
<span class="status" id="drawStatus"></span>
</div>
<div id="strip" role="slider" tabindex="0" aria-label="Position in the cut" aria-valuemin="0"></div>
<p class="keys">Space or K play and pause &middot; J back a second &middot; L play, then faster &middot; &larr; &rarr; one frame &middot; Shift+&larr; &rarr; one second &middot; I and O mark a range &middot; D draw &middot; N write a note &middot; Ctrl+Enter add it</p>
</section>
<aside>
<div class="panel" id="editorPanel" hidden><h2>From the editor</h2><p id="editorNote" hidden></p><ol id="editorMarkers"></ol></div>
<div class="panel">
<h2>Add a note</h2>
<label for="author">Your name</label>
<input type="text" id="author" autocomplete="name">
<label for="text" style="margin-top:8px">Note <span class="at" id="noteAt"></span></label>
<textarea id="text" placeholder="What should change here? (A drawing alone is a note too.)"></textarea>
<div class="row"><button class="primary" id="add">Add Note</button><span class="status" id="status" role="status"></span></div>
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
)HTML"
    R"HTML(const span = (cls, text) => { const e = document.createElement("span"); e.className = cls; e.textContent = text; return e; };
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
  // (While paused and seeking, a frame from an earlier seek is not where the playhead is.)
  const onFrame = (now, md) => { if (!video.paused || !video.seeking) { shown = frameOf(md.mediaTime); update(); } video.requestVideoFrameCallback(onFrame); };
  video.requestVideoFrameCallback(onFrame);
} else {
  const tick = () => { if (!video.paused) { shown = frameOf(video.currentTime); update(); requestAnimationFrame(tick); } };
  video.addEventListener("play", () => requestAnimationFrame(tick));
}
video.addEventListener("seeked", () => { if (video.paused && !video.seeking) { shown = frameOf(video.currentTime); update(); } });
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
const statuses = ["open", "progress", "done"];
function fix(n) {
  n.status = statuses.includes(n.status) ? n.status : (n.done ? "done" : "open");
  n.done = n.status === "done";
  n.replies = Array.isArray(n.replies) ? n.replies.filter((r) => r && typeof r.text === "string" && r.text.trim()) : [];
  n.drawing = Array.isArray(n.drawing) ? n.drawing.filter(okStroke) : [];
  n.text = typeof n.text === "string" ? n.text : "";
  return n;
}
function okStroke(st) {
  return st && ["pen", "arrow", "box", "ellipse"].includes(st.tool) && Array.isArray(st.points) && st.points.length > 0
    && st.points.every((p) => Array.isArray(p) && p.length >= 2 && isFinite(p[0]) && isFinite(p[1]));
}
const now = () => new Date().toISOString();
try { const saved = JSON.parse(localStorage.getItem(key) || "[]"); if (Array.isArray(saved)) notes = saved.filter((n) => n && typeof n.frame === "number").map(fix); } catch (e) { notes = []; }
try { $("author").value = localStorage.getItem("montage-review:author") || ""; } catch (e) {}
function store() { try { localStorage.setItem(key, JSON.stringify(notes)); } catch (e) {} }
function authors() { return [...new Set(notes.map((n) => n.author))]; }
function colourOf(author, list) { return palette[authorLabels[Math.max(0, list.indexOf(author)) % authorLabels.length]]; }
function newId() { return Date.now().toString(36) + Math.random().toString(36).slice(2, 8); }

function addNote() {
  const text = $("text").value.trim();
  if (!text && !pending.length) { status("Write the note first, or draw on the frame"); $("text").focus(); return; }
  const author = $("author").value.trim() || "Reviewer";
  try { localStorage.setItem("montage-review:author", author); } catch (e) {}
  const r = range();
  let start = r ? r[0] : shown, duration = r ? r[1] - r[0] + 1 : 0;
  // A drawing belongs to the frame it was drawn on (or a range holding it).
  if (pending.length && !(r && r[0] <= inkFrame && inkFrame <= r[1])) { start = inkFrame; duration = 0; }
  notes.push({ id: newId(), frame: cfg.offset + start, duration, author, text, status: "open", done: false, version: cfg.version,
    replies: [], drawing: pending.slice(), created: now(), updated: now() });
  notes.sort((a, b) => a.frame - b.frame);
  pending = [];
  inkFrame = null;
  store();
  $("text").value = "";
  render();
  status("Added at " + tc(cfg.offset + start));
}
function author() { return $("author").value.trim() || "Reviewer"; }

function render() {
  const list = $("notes");
  list.textContent = "";
  $("empty").hidden = notes.length > 0;
  $("count").textContent = notes.length ? "(" + notes.length + ")" : "";
  const who = authors();
  for (const n of notes) {
    const li = document.createElement("li");
    li.className = "note" + (n.done ? " done" : "") + (n.status === "progress" ? " progress" : "");
    li.style.setProperty("--c", colourOf(n.author, who));
    li.dataset.frame = n.frame;
    li.dataset.duration = n.duration;
    li.tabIndex = 0;
    const top = document.createElement("div");
    top.className = "top";
    top.append(span("at", tc(n.frame) + (n.duration > 1 ? " - " + tc(n.frame + n.duration - 1) : "")), span("who", n.author));
    if (n.drawing.length) top.append(span("badge", "drawing"));
    if (n.version && n.version !== cfg.version) top.append(span("badge", "v" + n.version));
    const text = document.createElement("div");
    text.className = "text";
    text.textContent = n.text;
    const replies = document.createElement("div");
    replies.className = "replies";
    replies.hidden = !n.replies.length;
    for (const r of n.replies) {
      const d = document.createElement("div");
      d.className = "reply";
      const b = document.createElement("b");
      b.textContent = r.author + ": ";
      d.append(b, document.createTextNode(r.text));
      replies.append(d);
    }
    const tools = document.createElement("div");
    tools.className = "tools";
    const state = document.createElement("select");
    state.setAttribute("aria-label", "Status");
    for (const [v, label] of [["open", "Open"], ["progress", "In progress"], ["done", "Done"]]) state.append(new Option(label, v, false, n.status === v));
    state.addEventListener("change", () => { n.status = state.value; n.done = n.status === "done"; n.updated = now(); store(); render(); });
    const reply = document.createElement("button");
    reply.textContent = "Reply";
    const del = document.createElement("button");
    del.textContent = "Delete";
    del.addEventListener("click", () => { if (confirm("Delete this note?")) { notes = notes.filter((x) => x !== n); store(); render(); } });
    tools.append(state, reply, del);
    const answer = document.createElement("div");
    answer.className = "answer";
    answer.hidden = true;
    const input = document.createElement("input");
    input.type = "text";
    input.placeholder = "Reply as " + author();
    input.setAttribute("aria-label", "Reply");
    const send = document.createElement("button");
    send.textContent = "Send";
    const post = () => {
      const t = input.value.trim();
      if (!t) return;
      n.replies.push({ author: author(), text: t, created: now() });
      n.updated = now();
      store();
      render();
    };
    send.addEventListener("click", post);
    input.addEventListener("keydown", (e) => { if (e.key === "Enter") { e.preventDefault(); post(); } });
    reply.addEventListener("click", () => { answer.hidden = !answer.hidden; if (!answer.hidden) { video.pause(); input.focus(); } });
    answer.append(input, send);
    li.append(top, text, replies, tools, answer);
    const go = () => { video.pause(); seekFrame(n.frame - cfg.offset); };
    li.addEventListener("click", (e) => { if (!e.target.closest("button, select, input")) go(); });
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
  $("noteAt").textContent = (pending.length && !(r && r[0] <= inkFrame && inkFrame <= r[1])) ? "with the drawing at " + tc(cfg.offset + inkFrame)
    : r ? "at " + tc(cfg.offset + r[0]) + " - " + tc(cfg.offset + r[1]) : "at " + tc(at);
  paintInk();
  syncPrev();
  for (const li of $("notes").children) {
    const f = Number(li.dataset.frame), d = Math.max(1, Number(li.dataset.duration));
    li.classList.toggle("current", at >= f && at < f + d);
  }
}

function safeName(s) { return s.replace(/[\\/:*?"<>|]/g, "-").trim() || "notes"; }
function saveFile() {
  const data = {
    montageReview: 2, title: cfg.title, id: cfg.id, fps: cfg.fps, dropFrame, version: cfg.version, saved: now(),
    notes: notes.map((n) => ({ frame: n.frame, duration: n.duration, timecode: tc(n.frame), author: n.author, text: n.text, status: n.status,
      done: n.status === "done", version: n.version || cfg.version, replies: n.replies, drawing: n.drawing, created: n.created, updated: n.updated, id: n.id })),
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
  let added = 0, changed = 0;
  for (const file of files) {
    try {
      const data = JSON.parse(await file.text());
      if (!data || !Array.isArray(data.notes)) throw new Error("not a notes file");
      const theirs = Array.isArray(data.fps) && data.fps[1] ? data.fps[0] / data.fps[1] : rate;
      for (const raw of data.notes) {
        if (!raw || typeof raw.frame !== "number") continue;
        const n = fix(Object.assign({}, raw));
)HTML"
    R"HTML(        if (!n.text.trim() && !n.drawing.length) continue;
        const mine = n.id ? notes.find((x) => x.id === n.id) : null;
        if (mine) {
          // The same note from another copy: its new replies, and its status if changed later.
          const seen = new Set(mine.replies.map((r) => r.author + "\n" + r.text + "\n" + r.created));
          const more = n.replies.filter((r) => !seen.has(r.author + "\n" + r.text + "\n" + r.created));
          const later = (n.updated || "") > (mine.updated || "");
          if (more.length || (later && n.status !== mine.status)) ++changed;
          mine.replies.push(...more);
          mine.replies.sort((a, b) => String(a.created).localeCompare(String(b.created)));
          if (later) { mine.status = n.status; mine.done = n.done; mine.updated = n.updated; }
          continue;
        }
        notes.push({ id: n.id || newId(), frame: Math.round(n.frame * rate / theirs), duration: Math.round((n.duration || 0) * rate / theirs),
          author: String(n.author || "Reviewer"), text: n.text, status: n.status, done: n.done, version: Number(n.version || data.version) || 0,
          replies: n.replies, drawing: n.drawing, created: n.created || "", updated: n.updated || "" });
        ++added;
      }
    } catch (e) {
      status(file.name + " is not a notes file");
    }
  }
  notes.sort((a, b) => a.frame - b.frame);
  store();
  render();
  if (added || changed) status([added ? "Added " + added + (added === 1 ? " note" : " notes") : "", changed ? changed + " brought up to date" : ""].filter(Boolean).join(", "));
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
$("meta").textContent = (cfg.version > 1 || cfg.previous ? "Version " + cfg.version + " · " : "") + cfg.width + " x " + cfg.height + " · " + (Math.round(rate * 1000) / 1000) + " fps · " + tc(frames) + " long"
  + (cfg.offset ? " · from " + tc(cfg.offset) : "");


// Drawing over the frame: strokes in 0..1 of the picture, waiting for the note they go with.
const canvas = $("ink");
let pending = [], inkFrame = null, drawMode = false, tool = "pen", colour = "#ff3b30", live = null;
function contentRect(v) {
  const bw = v.clientWidth, bh = v.clientHeight, a = cfg.width / cfg.height;
  let w = bw, h = bw / a;
  if (h > bh) { h = bh; w = bh * a; }
  return { x: v.offsetLeft + (bw - w) / 2, y: v.offsetTop + (bh - h) / 2, w, h };
}
function layoutInk() {
  const r = contentRect(video), dpr = window.devicePixelRatio || 1;
  canvas.style.left = r.x + "px";
  canvas.style.top = r.y + "px";
  canvas.style.width = r.w + "px";
  canvas.style.height = r.h + "px";
  canvas.width = Math.max(1, Math.round(r.w * dpr));
  canvas.height = Math.max(1, Math.round(r.h * dpr));
  paintInk();
}
function strokePath(g, st, w, h) {
  g.strokeStyle = st.color || "#ff3b30";
  g.lineWidth = Math.max(1.5, (st.width || 0.005) * w);
  g.lineCap = "round";
  g.lineJoin = "round";
  const P = st.points.map((p) => [p[0] * w, p[1] * h]);
  const a = P[0], b = P[P.length - 1];
  g.beginPath();
  if (st.tool === "pen") {
    g.moveTo(a[0], a[1]);
    if (P.length === 1) g.lineTo(a[0] + 0.01, a[1]);
    for (let i = 1; i < P.length; ++i) g.lineTo(P[i][0], P[i][1]);
  } else if (st.tool === "box") {
    g.rect(Math.min(a[0], b[0]), Math.min(a[1], b[1]), Math.abs(b[0] - a[0]), Math.abs(b[1] - a[1]));
  } else if (st.tool === "ellipse") {
    g.ellipse((a[0] + b[0]) / 2, (a[1] + b[1]) / 2, Math.abs(b[0] - a[0]) / 2, Math.abs(b[1] - a[1]) / 2, 0, 0, 2 * Math.PI);
  } else {
    const ang = Math.atan2(b[1] - a[1], b[0] - a[0]), len = Math.max(10, g.lineWidth * 4);
    g.moveTo(a[0], a[1]);
    g.lineTo(b[0], b[1]);
    g.moveTo(b[0] - len * Math.cos(ang - 0.45), b[1] - len * Math.sin(ang - 0.45));
    g.lineTo(b[0], b[1]);
    g.lineTo(b[0] - len * Math.cos(ang + 0.45), b[1] - len * Math.sin(ang + 0.45));
  }
  g.stroke();
}
const covers = (frame, duration, at) => duration > 1 ? at >= frame && at < frame + duration : at === frame;
function paintInk() {
  const g = canvas.getContext("2d");
  if (!g) return;
  g.setTransform(1, 0, 0, 1, 0, 0);
  g.clearRect(0, 0, canvas.width, canvas.height);
  const at = cfg.offset + shown, w = canvas.width, h = canvas.height;
  const shownStrokes = [];
  if (video.paused || drawMode) {
    for (const m of cfg.markers) if (Array.isArray(m.drawing) && covers(m.frame, m.duration, at)) shownStrokes.push(...m.drawing);
    for (const n of notes) if (n.drawing.length && covers(n.frame, n.duration, at)) shownStrokes.push(...n.drawing);
  }
  if (pending.length && inkFrame === shown) shownStrokes.push(...pending);
  g.shadowColor = "rgba(0,0,0,.6)";
  g.shadowBlur = 2;
  for (const st of shownStrokes) strokePath(g, st, w, h);
}
function pointOf(e) {
  const r = canvas.getBoundingClientRect();
  const c = (v) => Math.round(Math.min(1, Math.max(0, v)) * 10000) / 10000;
  return [c((e.clientX - r.left) / r.width), c((e.clientY - r.top) / r.height)];
}
function setDraw(on) {
  drawMode = on;
  if (on) { video.pause(); setCompare(false); }
  $("draw").setAttribute("aria-pressed", String(on));
  $("drawbar").hidden = !on;
  canvas.classList.toggle("drawing", on);
  paintInk();
}
canvas.addEventListener("pointerdown", (e) => {
  if (!drawMode) return;
  e.preventDefault();
  video.pause();
  if (pending.length && inkFrame !== shown) {
    $("drawStatus").textContent = "Add the note for the drawing at " + tc(cfg.offset + inkFrame) + " first, or clear it";
    return;
  }
  $("drawStatus").textContent = "";
  inkFrame = shown;
  canvas.setPointerCapture(e.pointerId);
  live = { tool, color: colour, width: tool === "pen" ? 0.004 : 0.003, points: [pointOf(e)] };
  pending.push(live);
  update();
});
canvas.addEventListener("pointermove", (e) => {
  if (!live || !canvas.hasPointerCapture(e.pointerId)) return;
  const p = pointOf(e);
  if (live.tool === "pen") {
    const q = live.points[live.points.length - 1];
    if (Math.hypot(p[0] - q[0], p[1] - q[1]) >= 0.002 && live.points.length < 4000) live.points.push(p);
  } else live.points[1] = p;
  paintInk();
});
const endStroke = () => {
  if (!live) return;
  if (live.tool !== "pen" && live.points.length < 2) pending.pop();
  live = null;
  if (!pending.length) inkFrame = null;
  update();
};
canvas.addEventListener("pointerup", endStroke);
canvas.addEventListener("pointercancel", endStroke);
function undoStroke() { pending.pop(); if (!pending.length) inkFrame = null; update(); }
if ("ResizeObserver" in window) new ResizeObserver(layoutInk).observe(video);
window.addEventListener("resize", layoutInk);
video.addEventListener("loadedmetadata", layoutInk);

// The version before, side by side or under a wipe, kept on the same frame of the cut.
const P = cfg.previous;
const prev = $("prev");
let comparing = false;
function setCompare(on) {
  if (!P) return;
  comparing = on;
  if (on && drawMode) setDraw(false);
  $("compare").setAttribute("aria-pressed", String(on));
  $("prevPane").hidden = !on;
  $("curLabel").hidden = !on;
  const mode = $("compareMode").value;
  $("stage").classList.toggle("side", on && mode === "side");
  $("stage").classList.toggle("wipe", on && mode === "wipe");
  $("wipeLine").hidden = !(on && mode === "wipe");
  $("wipe").hidden = !(on && mode === "wipe");
  if (!on) prev.pause();
  layoutInk();
  syncPrev();
}
function syncPrev() {
  if (!P || !comparing) return;
  const prate = P.fps[0] / P.fps[1];
  const seqTime = cfg.offset / rate + (video.paused ? (shown + 0.5) / rate : video.currentTime);
  const t = seqTime - P.offset / prate;
  const inside = t >= 0 && t < P.frames / prate;
  $("prevGone").hidden = inside;
  $("prevGone").textContent = "Not in version " + P.version + " here";
  if (!inside) { prev.pause(); return; }
  if (video.paused) {
    if (!prev.paused) prev.pause();
    if (Math.abs(prev.currentTime - t) > 0.4 / prate) prev.currentTime = t;
  } else {
    prev.playbackRate = video.playbackRate;
    if (prev.paused) prev.play().catch(() => {});
    if (Math.abs(prev.currentTime - t) > 0.12) prev.currentTime = t;
  }
}
if (P) {
  $("compareTools").hidden = false;
  $("compare").textContent = "Compare v" + P.version;
  $("prevLabel").textContent = "v" + P.version;
  $("curLabel").textContent = "v" + cfg.version;
  prev.style.aspectRatio = cfg.width + " / " + cfg.height;
  prev.src = encodeURIComponent(P.video);
  $("compare").addEventListener("click", () => setCompare(!comparing));
  $("compareMode").addEventListener("change", () => setCompare(comparing));
  $("wipe").addEventListener("input", () => $("stage").style.setProperty("--wipe", $("wipe").value + "%"));
  video.addEventListener("pause", syncPrev);
  video.addEventListener("play", syncPrev);
}
$("draw").addEventListener("click", () => setDraw(!drawMode));
for (const b of document.querySelectorAll("#drawbar [data-tool]"))
  b.addEventListener("click", () => { tool = b.dataset.tool; for (const x of document.querySelectorAll("#drawbar [data-tool]")) x.setAttribute("aria-pressed", String(x === b)); });
for (const b of document.querySelectorAll("#drawbar [data-color]"))
  b.addEventListener("click", () => { colour = b.dataset.color; for (const x of document.querySelectorAll("#drawbar [data-color]")) x.setAttribute("aria-pressed", String(x === b)); });
$("undoStroke").addEventListener("click", undoStroke);
$("clearInk").addEventListener("click", () => { pending = []; inkFrame = null; $("drawStatus").textContent = ""; update(); });
video.addEventListener("pause", paintInk);
video.addEventListener("play", paintInk);
$("start").addEventListener("click", () => { video.pause(); seekFrame(0); });
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
  if (t.closest && t.closest("input, textarea, select")) {
    if (e.key === "Enter" && (e.ctrlKey || e.metaKey) && t.id === "text") { e.preventDefault(); addNote(); }
)HTML"
    R"HTML(    else if (e.key === "Escape") t.blur();
    return;
  }
  if ((e.ctrlKey || e.metaKey) && !e.altKey && (e.key === "z" || e.key === "Z") && drawMode) { e.preventDefault(); undoStroke(); return; }
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
    case "d": case "D": setDraw(!drawMode); break;
    case "c": case "C": setCompare(!comparing); break;
    case "Escape": if (drawMode) setDraw(false); break;
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

namespace {

QJsonArray drawingArray(const std::vector<ReviewStroke>& strokes) {
    QJsonArray out;
    for (const ReviewStroke& s : strokes) {
        QJsonArray pts;
        for (const auto& p : s.points) pts.append(QJsonArray{std::round(p[0] * 1e4) / 1e4, std::round(p[1] * 1e4) / 1e4});
        out.append(QJsonObject{{"tool", QString::fromStdString(s.tool)}, {"color", QString::fromStdString(s.color)},
                               {"width", std::round(s.width * 1e5) / 1e5}, {"points", pts}});
    }
    return out;
}

std::vector<ReviewStroke> strokesFrom(const QJsonArray& list) {
    static const QRegularExpression hex(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    static const std::set<QString> tools = {"pen", "arrow", "box", "ellipse"};
    std::vector<ReviewStroke> out;
    for (const QJsonValue& v : list) {
        if (out.size() >= 200) break;
        const QJsonObject o = v.toObject();
        ReviewStroke s;
        const QString tool = o.value("tool").toString(QStringLiteral("pen"));
        if (!tools.count(tool)) continue;
        s.tool = tool.toStdString();
        const QString colour = o.value("color").toString();
        if (hex.match(colour).hasMatch()) s.color = colour.toLower().toStdString();
        const double w = o.value("width").toDouble(0.005);
        s.width = std::isfinite(w) ? std::clamp(w, 0.001, 0.05) : 0.005;
        for (const QJsonValue& pv : o.value("points").toArray()) {
            if (s.points.size() >= 4000) break;
            const QJsonArray p = pv.toArray();
            if (p.size() < 2 || !p[0].isDouble() || !p[1].isDouble()) continue;
            const double x = p[0].toDouble(), y = p[1].toDouble();
            if (!std::isfinite(x) || !std::isfinite(y)) continue;
            s.points.push_back({std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0)});
        }
        if (s.points.empty() || (s.tool != "pen" && s.points.size() < 2)) continue;
        out.push_back(std::move(s));
    }
    return out;
}

}  // namespace

std::string reviewDrawingJson(const std::vector<ReviewStroke>& strokes) {
    if (strokes.empty()) return {};
    return QJsonDocument(drawingArray(strokes)).toJson(QJsonDocument::Compact).toStdString();
}

std::vector<ReviewStroke> parseReviewDrawing(const std::string& json) {
    if (json.empty()) return {};
    return strokesFrom(QJsonDocument::fromJson(QByteArray::fromStdString(json)).array());
}

std::string reviewPageHtml(const ReviewPageInfo& info) {
    QJsonArray markers;
    for (const Marker& m : info.markers) {
        QJsonObject o{{"frame", double(m.t)}, {"duration", double(m.duration)}, {"name", QString::fromStdString(m.name)},
                      {"comment", QString::fromStdString(m.comment)}, {"color", m.color}, {"chapter", m.chapter}};
        if (const auto strokes = parseReviewDrawing(m.drawing); !strokes.empty()) o["drawing"] = drawingArray(strokes);
        markers.append(o);
    }
    QJsonValue previous;
    if (info.previous)
        previous = QJsonObject{{"version", info.previous->version},
                               {"video", QString::fromStdString(info.previous->videoFile)},
                               {"fps", QJsonArray{double(info.previous->fps.num), double(info.previous->fps.den)}},
                               {"offset", double(info.previous->offset)},
                               {"frames", double(info.previous->frames)}};
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
                          {"note", QString::fromStdString(info.note)},
                          {"version", std::max(1, info.version)},
                          {"previous", previous}};
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

std::optional<ReviewPageInfo> readReviewPage(const std::string& html) {
    const std::string tag = "<script type=\"application/json\" id=\"review-config\">";
    const size_t a = html.find(tag);
    if (a == std::string::npos || html.find("<meta name=\"generator\" content=\"Montage\">") == std::string::npos) return std::nullopt;
    const size_t from = a + tag.size(), to = html.find("</script>", from);
    if (to == std::string::npos) return std::nullopt;
    const QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(html.substr(from, to - from))).object();
    const QJsonArray fps = o.value("fps").toArray();
    if (o.isEmpty() || !o.value("video").isString() || fps.size() != 2 || fps[0].toInt() <= 0 || fps[1].toInt() <= 0) return std::nullopt;
    ReviewPageInfo info;
    info.title = o.value("title").toString().toStdString();
    info.id = o.value("id").toString().toStdString();
    info.videoFile = o.value("video").toString().toStdString();
    info.fps = {fps[0].toInt(), fps[1].toInt()};
    info.dropFrame = o.value("dropFrame").toBool();
    info.offset = FrameTime(o.value("offset").toDouble());
    info.frames = FrameTime(o.value("frames").toDouble());
    info.width = o.value("width").toInt(1920);
    info.height = o.value("height").toInt(1080);
    info.note = o.value("note").toString().toStdString();
    info.version = std::max(1, o.value("version").toInt(1));
    return info;
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
        note.drawing = reviewDrawingJson(strokesFrom(n.value("drawing").toArray()));
        if (note.text.empty() && note.drawing.empty()) continue;
        FrameTime f = 0;
        if (n.value("frame").isDouble()) f = FrameTime(std::llround(n.value("frame").toDouble()));
        else if (!parseTimecode(n.value("timecode").toString().toStdString(), noteFps, f)) continue;
        note.frame = std::max<FrameTime>(0, FrameTime(std::llround(double(f) * scale)));
        note.duration = std::max<FrameTime>(0, FrameTime(std::llround(n.value("duration").toDouble() * scale)));
        note.author = n.value("author").toString().trimmed().toStdString();
        if (note.author.empty()) note.author = "Reviewer";
        const QString status = n.value("status").toString();
        note.status = status == QLatin1String("progress") || status == QLatin1String("done") || status == QLatin1String("open")
                          ? status.toStdString()
                          : (n.value("done").toBool() ? "done" : "open");
        note.done = note.status == "done";
        note.version = std::max(0, n.value("version").toInt(o.value("version").toInt(0)));
        note.id = n.value("id").toString().left(64).toStdString();
        for (const QJsonValue& rv : n.value("replies").toArray()) {
            const QJsonObject r = rv.toObject();
            ReviewReply reply{r.value("author").toString().trimmed().toStdString(), r.value("text").toString().trimmed().toStdString(),
                              r.value("created").toString().toStdString()};
            if (reply.text.empty()) continue;
            if (reply.author.empty()) reply.author = "Reviewer";
            note.replies.push_back(reply);
        }
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
        m.name = (n.done ? "\xE2\x9C\x93 " : n.status == "progress" ? "\xE2\x97\x90 " : "") + n.author;  // ✓, ◐
        if (n.version > 1) m.name += " \xC2\xB7 v" + std::to_string(n.version);  // · v2
        m.comment = n.text;
        for (const ReviewReply& r : n.replies) m.comment += (m.comment.empty() ? "" : "\n") + ("\xE2\x86\xB3 " + r.author + ": " + r.text);  // ↳
        if (m.comment.empty() && !n.drawing.empty()) m.comment = "(a drawing)";
        m.color = kAuthorLabels[size_t(it - authors.begin()) % std::size(kAuthorLabels)];
        m.drawing = n.drawing;
        m.reviewId = n.id;
        out.push_back(m);
    }
    return out;
}

}  // namespace montage
