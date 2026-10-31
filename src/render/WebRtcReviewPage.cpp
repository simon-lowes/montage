// Montage — the page a reviewer opens for live review in a browser (render/WebRtcReview.h): asks for their name and the
// passphrase, plays the Program over WebRTC with the timecode of the frame shown, and sends notes, drawings and a
// pointer back over a data channel. No scripts, styles or fonts from anywhere else.
#include "render/WebRtcReview.h"

namespace montage::output {

const char* browserReviewPage() {
    return R"html(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Montage live review</title>
<style>
:root { color-scheme: dark; --fg: #e8e8ea; --dim: #9a9aa2; --bg: #121214; --panel: #1d1d21; --line: #34343a; --accent: #0a84ff; }
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--fg); font: 15px/1.4 system-ui, -apple-system, "Segoe UI", sans-serif; }
[hidden] { display: none !important; }
#join { max-width: 22rem; margin: 12vh auto; padding: 1.5rem; background: var(--panel); border: 1px solid var(--line); border-radius: 10px; }
#join h1 { margin: 0 0 1rem; font-size: 1.25rem; font-weight: 600; }
label { display: block; margin: .75rem 0 .25rem; color: var(--dim); font-size: .85rem; }
input { width: 100%; padding: .55rem .6rem; border-radius: 6px; border: 1px solid var(--line); background: var(--bg); color: var(--fg); font: inherit; }
button { padding: .5rem .8rem; border-radius: 6px; border: 1px solid var(--line); background: #2a2a30; color: var(--fg); font: inherit; cursor: pointer; }
button.on, #join button { background: var(--accent); border-color: var(--accent); color: #fff; }
#join button { width: 100%; margin-top: 1.25rem; }
#joinError { color: #ff6961; min-height: 1.2em; margin: .75rem 0 0; }
#noteError { color: #ff6961; margin: 0; padding: 0 .75rem; background: var(--panel); font-size: .9rem; }
#noteError:empty { display: none; }
main { display: grid; grid-template-rows: 1fr auto auto; height: 100vh; }
#stage { position: relative; background: #000; min-height: 0; }
#video { width: 100%; height: 100%; object-fit: contain; display: block; }
#ink { position: absolute; touch-action: none; }
#pointers { position: absolute; inset: 0; pointer-events: none; }
.pointer { position: absolute; width: 14px; height: 14px; margin: -7px 0 0 -7px; border-radius: 50%; border: 2px solid #fff; }
.pointer span { position: absolute; left: 16px; top: -4px; white-space: nowrap; font-size: 12px; text-shadow: 0 0 3px #000; }
#bar { display: flex; flex-wrap: wrap; gap: .5rem; align-items: center; padding: .5rem .75rem; background: var(--panel); border-top: 1px solid var(--line); }
#tc { font: 600 1.15rem/1 ui-monospace, "SF Mono", Menlo, Consolas, monospace; min-width: 8.5rem; }
#state { margin-left: auto; color: var(--dim); font-size: .85rem; }
#noteForm { display: flex; gap: .5rem; padding: .5rem .75rem; background: var(--panel); }
#notes { margin: 0; padding: 0 .75rem .5rem 2rem; max-height: 22vh; overflow: auto; background: var(--panel); font-size: .9rem; }
#notes li { margin: .2rem 0; }
#notes .tc { font-family: ui-monospace, Menlo, Consolas, monospace; color: var(--dim); margin-right: .5rem; }
#notes .sent { color: var(--dim); margin-left: .5rem; }
</style>
</head>
<body>
<form id="join" autocomplete="on">
  <h1>Montage live review</h1>
  <label for="author">Your name</label>
  <input id="author" name="name" autocomplete="name" maxlength="100" required>
  <label for="pass">Passphrase</label>
  <input id="pass" type="password" autocomplete="current-password" maxlength="79" required>
  <button type="submit">Watch</button>
  <p id="joinError" role="alert"></p>
</form>
<main id="review" hidden>
  <div id="stage">
    <video id="video" playsinline autoplay></video>
    <canvas id="ink"></canvas>
    <div id="pointers"></div>
  </div>
  <div id="bar">
    <span id="tc" title="The timecode of the frame shown">--:--:--:--</span>
    <button type="button" id="sound">Sound off</button>
    <button type="button" id="draw" title="Draw on the picture; the drawing goes with your next note">Draw</button>
    <button type="button" id="clear">Clear drawing</button>
    <button type="button" id="point" title="Show the others where you point">Point</button>
    <button type="button" id="full">Full screen</button>
    <span id="state" role="status"></span>
  </div>
  <form id="noteForm">
    <input id="note" placeholder="A note on the frame shown (Enter to send)" maxlength="10000" autocomplete="off">
    <button type="submit">Add note</button>
  </form>
  <p id="noteError" role="alert"></p>
  <ol id="notes"></ol>
</main>
<script src="/review.js"></script>
</body>
</html>
)html";
}

const char* browserReviewScript() {
    return R"js('use strict';
(() => {
  const $ = (id) => document.getElementById(id);
  // What the page knows, kept where automatic checks can read it.
  const state = {
    stage: 'idle', error: '', author: '', fps: [25, 1], sequence: 0, color: '#ff3b30', codec: '',
    frame: -1, tc: '', shownSequence: 0, rtp: null, presented: 0,
    tags: new Map(), tagOrder: [], notes: new Map(), strokes: [], onFrame: [],
    pc: null, dc: null, session: null, pass: '',
  };
  window.__montage = state;

  const say = (text) => { $('state').textContent = text; };
  const newId = () => (crypto.randomUUID ? crypto.randomUUID()
    : Array.from(crypto.getRandomValues(new Uint8Array(16)), (b) => b.toString(16).padStart(2, '0')).join(''));

  function gathered(pc, ms) {
    if (pc.iceGatheringState === 'complete') return Promise.resolve();
    return new Promise((done) => {
      const t = setTimeout(done, ms);
      pc.addEventListener('icegatheringstatechange', () => {
        if (pc.iceGatheringState === 'complete') { clearTimeout(t); done(); }
      });
    });
  }

  async function join(author, pass) {
    state.author = author; state.pass = pass; state.stage = 'joining'; state.error = '';
    const auth = { Authorization: 'Bearer ' + pass };
    const r = await fetch('/config', { headers: auth, cache: 'no-store' });
    if (r.status === 401) { state.stage = 'refused'; throw new Error('Wrong passphrase'); }
    if (!r.ok) throw new Error((await r.text()) || r.statusText);
    const cfg = await r.json();
    state.fps = cfg.fps; state.sequence = cfg.sequence;
    if (state.pc) state.pc.close();  // (an earlier try's)
    const pc = new RTCPeerConnection({ iceServers: cfg.iceServers || [] });
    state.pc = pc;
    pc.addTransceiver('video', { direction: 'recvonly' });
    pc.addTransceiver('audio', { direction: 'recvonly' });
    const dc = pc.createDataChannel('montage');
    state.dc = dc;
    dc.onopen = () => dc.send(JSON.stringify({ t: 'hi', author }));
    dc.onmessage = (e) => { try { message(JSON.parse(e.data)); } catch (err) { /* not ours */ } };
    const stream = new MediaStream();
    pc.ontrack = (e) => {
      stream.addTrack(e.track);
      // As little buffering as the browser allows: this is live.
      if ('jitterBufferTarget' in e.receiver) e.receiver.jitterBufferTarget = 0;
      else if ('playoutDelayHint' in e.receiver) e.receiver.playoutDelayHint = 0;
      $('video').srcObject = stream;
    };
    pc.onconnectionstatechange = () => {
      state.stage = pc.connectionState;
      say({ connected: 'Live', connecting: 'Connecting…', failed: 'The connection failed', disconnected: 'Connection lost…', closed: 'Ended' }[pc.connectionState] || pc.connectionState);
    };
    await pc.setLocalDescription(await pc.createOffer());
    await gathered(pc, 2000);
    const w = await fetch('/whep', { method: 'POST', headers: { ...auth, 'Content-Type': 'application/sdp' }, body: pc.localDescription.sdp });
    if (w.status === 401) { state.stage = 'refused'; throw new Error('Wrong passphrase'); }
    if (w.status !== 201) throw new Error((await w.text()) || ('The review refused: ' + w.status));
    state.session = w.headers.get('Location');
    await pc.setRemoteDescription({ type: 'answer', sdp: await w.text() });
    const v = $('video');
    v.muted = false;
    try { await v.play(); } catch (e) { v.muted = true; v.play().catch(() => {}); }
    $('sound').textContent = v.muted ? 'Sound on' : 'Sound off';
    watchFrames();
  }
  state.join = join;

  function message(m) {
    if (m.t === 'f') {
      const r = m.r >>> 0;
      state.tags.set(r, { f: m.f, tc: m.tc, seq: m.seq, at: performance.now() });
      state.tagOrder.push(r);
      while (state.tagOrder.length > 600) state.tags.delete(state.tagOrder.shift());
    } else if (m.t === 'hello') {
      state.fps = m.fps; state.sequence = m.sequence; state.color = m.color || state.color; state.codec = m.codec || '';
    } else if (m.t === 'ack' || m.t === 'err') {
      const n = state.notes.get(m.id);
      if (n) { n.status = m.t === 'ack' ? 'sent' : ('not sent: ' + (m.why || '')); n.el.querySelector('.sent').textContent = m.t === 'ack' ? '✓' : n.status; }
    } else if (m.t === 'p') {
      pointer(m);
    }
  }

  // Which frame is on screen: the picture's RTP timestamp names it (else, where the browser does not say, the newest
  // frame told of a frame's time ago).
  function shown(meta) {
    if (meta && meta.rtpTimestamp !== undefined) return state.tags.get(meta.rtpTimestamp >>> 0);
    const now = performance.now(), wait = 1000 * state.fps[1] / state.fps[0];
    for (let i = state.tagOrder.length - 1; i >= 0; --i) {
      const t = state.tags.get(state.tagOrder[i]);
      if (t && now - t.at >= wait) return t;
    }
    return undefined;
  }

  function watchFrames() {
    const v = $('video');
    const step = (now, meta) => {
      state.presented++;
      const t = shown(meta);
      if (t) {
        state.frame = t.f; state.tc = t.tc; state.shownSequence = t.seq; state.rtp = meta ? meta.rtpTimestamp : null;
        $('tc').textContent = t.tc;
      }
      for (const f of state.onFrame) { try { f(now, meta, v, t); } catch (e) { /* a check's own */ } }
      if (v.requestVideoFrameCallback) v.requestVideoFrameCallback(step);
    };
    if (v.requestVideoFrameCallback) v.requestVideoFrameCallback(step);
    else setInterval(() => step(performance.now(), undefined), 40);
    v.addEventListener('resize', layout);
    window.addEventListener('resize', layout);
    layout();
  }

  // ---- Notes ----

  function note(text, strokes) {
    if (!state.dc || state.dc.readyState !== 'open') return null;
    if (state.frame < 0) return null;
    const id = newId();
    const drawing = strokes || state.strokes;
    const json = JSON.stringify({ t: 'note', id, f: state.frame, tc: state.tc, seq: state.shownSequence || state.sequence, text, drawing });
    // (what Montage takes in one message; the channel's own limit is about the same)
    if (json.length > 240000) { state.error = 'That drawing is too detailed to send: clear it and draw it again more simply'; $('noteError').textContent = state.error; return null; }
    try { state.dc.send(json); } catch (err) { state.error = err.message; $('noteError').textContent = err.message; return null; }
    $('noteError').textContent = '';
    const li = document.createElement('li');
    const tc = document.createElement('span'); tc.className = 'tc'; tc.textContent = state.tc;
    const body = document.createElement('span'); body.textContent = text || (drawing.length ? '(a drawing)' : '');
    const sent = document.createElement('span'); sent.className = 'sent'; sent.textContent = '…';
    li.append(tc, body, sent);
    $('notes').prepend(li);
    state.notes.set(id, { text, frame: state.frame, status: 'sending', el: li });
    state.strokes = [];
    paint();
    return id;
  }
  state.note = note;

  // ---- Drawing and pointing, in 0..1 of the picture ----

  let drawing = false, pointing = false, stroke = null, lastPointer = 0;
  const ink = $('ink');
  function box() {
    const v = $('video'), r = v.getBoundingClientRect();
    const vw = v.videoWidth || 16, vh = v.videoHeight || 9, s = Math.min(r.width / vw, r.height / vh);
    const w = vw * s, h = vh * s;
    return { x: (r.width - w) / 2, y: (r.height - h) / 2, w, h };
  }
  function layout() {
    const b = box();
    Object.assign(ink.style, { left: b.x + 'px', top: b.y + 'px', width: b.w + 'px', height: b.h + 'px' });
    ink.width = Math.round(b.w * devicePixelRatio); ink.height = Math.round(b.h * devicePixelRatio);
    const p = $('pointers').style;
    Object.assign(p, { left: b.x + 'px', top: b.y + 'px', width: b.w + 'px', height: b.h + 'px', inset: 'auto' });
    paint();
  }
  function paint() {
    const g = ink.getContext('2d');
    g.clearRect(0, 0, ink.width, ink.height);
    for (const s of state.strokes.concat(stroke ? [stroke] : [])) {
      g.strokeStyle = s.color; g.lineWidth = s.width * ink.width; g.lineCap = g.lineJoin = 'round';
      g.beginPath();
      s.points.forEach(([x, y], i) => (i ? g.lineTo : g.moveTo).call(g, x * ink.width, y * ink.height));
      g.stroke();
    }
  }
  const at = (e) => {
    const r = ink.getBoundingClientRect();
    return [Math.min(1, Math.max(0, (e.clientX - r.left) / r.width)), Math.min(1, Math.max(0, (e.clientY - r.top) / r.height))];
  };
  ink.addEventListener('pointerdown', (e) => {
    if (!drawing) return;
    ink.setPointerCapture(e.pointerId);
    stroke = { tool: 'pen', color: state.color, width: 0.005, points: [at(e)] };
  });
  ink.addEventListener('pointermove', (e) => {
    if (stroke && stroke.points.length < 4000) { stroke.points.push(at(e).map((v) => Math.round(v * 1e4) / 1e4)); paint(); }
    if (pointing && state.dc && state.dc.readyState === 'open' && performance.now() - lastPointer > 50) {
      lastPointer = performance.now();
      const [x, y] = at(e);
      state.dc.send(JSON.stringify({ t: 'p', x, y }));
    }
  });
  const endStroke = () => { if (stroke && stroke.points.length > 1 && state.strokes.length < 200) state.strokes.push(stroke); stroke = null; paint(); };
  ink.addEventListener('pointerup', endStroke);
  ink.addEventListener('pointercancel', endStroke);
  ink.addEventListener('pointerleave', () => { if (pointing && state.dc && state.dc.readyState === 'open') state.dc.send(JSON.stringify({ t: 'p', off: true })); });

  const others = new Map();
  function pointer(m) {
    let el = others.get(m.id);
    if (m.off) { if (el) { el.remove(); others.delete(m.id); } return; }
    if (!el) {
      el = document.createElement('div'); el.className = 'pointer';
      el.append(document.createElement('span'));
      $('pointers').append(el); others.set(m.id, el);
    }
    el.style.left = (m.x * 100) + '%'; el.style.top = (m.y * 100) + '%';
    el.style.background = /^#[0-9a-f]{6}$/i.test(m.c || '') ? m.c : '#ffffff';
    el.firstChild.textContent = m.who || '';
    clearTimeout(el.gone);
    el.gone = setTimeout(() => { el.remove(); others.delete(m.id); }, 4000);
  }

  // ---- The page ----

  try { $('author').value = localStorage.getItem('montage.review.author') || ''; } catch (e) { /* storage off */ }
  $('join').addEventListener('submit', async (e) => {
    e.preventDefault();
    const author = $('author').value.trim(), pass = $('pass').value;
    try { localStorage.setItem('montage.review.author', author); } catch (err) { /* storage off */ }
    $('joinError').textContent = '';
    const button = $('join').querySelector('button');
    if (button.disabled) return;
    button.disabled = true;
    try {
      await join(author, pass);
      $('join').hidden = true; $('review').hidden = false;
      layout();
    } catch (err) {
      state.error = err.message; $('joinError').textContent = err.message;
    } finally {
      button.disabled = false;
    }
  });
  $('noteForm').addEventListener('submit', (e) => {
    e.preventDefault();
    const text = $('note').value.trim();
    if (!text && !state.strokes.length) return;
    if (note(text)) $('note').value = '';
  });
  $('sound').addEventListener('click', () => { const v = $('video'); v.muted = !v.muted; $('sound').textContent = v.muted ? 'Sound on' : 'Sound off'; });
  $('draw').addEventListener('click', () => { drawing = !drawing; $('draw').classList.toggle('on', drawing); ink.style.cursor = drawing ? 'crosshair' : ''; });
  $('clear').addEventListener('click', () => { state.strokes = []; paint(); });
  $('point').addEventListener('click', () => { pointing = !pointing; $('point').classList.toggle('on', pointing); });
  $('full').addEventListener('click', () => { const s = $('stage'); (document.fullscreenElement ? document.exitFullscreen() : s.requestFullscreen()).catch(() => {}); });
  window.addEventListener('pagehide', () => {
    if (state.session) fetch(state.session, { method: 'DELETE', headers: { Authorization: 'Bearer ' + state.pass }, keepalive: true }).catch(() => {});
  });
})();
)js";
}

}  // namespace montage::output
