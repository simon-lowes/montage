#!/usr/bin/env python3
"""Flag suspect Opus-MT drafts in translations/*.ts with Jev (TypeSafe AI).

Dev-time only; Montage itself is unchanged. Jev judges, this script decides:

  * Placeholders (%1, %L1, %n), the & accelerator and tabbed shortcuts are
    checked in code first; a draft that breaks them is flagged without a call.
  * Same-meaning p < DROP_BELOW  -> the draft is removed (English shows instead).
  * DROP_BELOW <= p < KEEP_FROM  -> a translatorcomment "check first: ..." is
    added, which Qt Linguist shows.
  * p >= KEEP_FROM               -> left as it is.
  Nothing is ever marked done: only a speaker does that (translations/README.md).

Off unless TYPESAFE_API_KEY is set; without it only the code checks run. Any API
failure leaves that message untouched. Report only, unless --apply is given.

  python3 scripts/jev-check-drafts.py translations/montage_de.ts [--apply] [--limit N]
"""
import argparse, html, json, os, re, sys, urllib.request

API_URL = "https://api.typesafe.ai/v1/systemone"
MODEL = os.environ.get("JEV_MODEL") or "jev-latest"
DROP_BELOW = 0.2   # uncalibrated: check ~100 strings per language with a speaker first
KEEP_FROM = 0.8
EDIT_TERMS = ("clip", "cut", "track", "take", "render", "bin", "marker")
CONTEXT_NEIGHBOURS = 4
LANG_NAMES = {"ar": "Arabic", "de": "German", "es": "Spanish", "fr": "French",
              "it": "Italian", "ru": "Russian", "zh_CN": "Simplified Chinese"}


def questions(source, draft, lang):
    """All Jev questions in one place."""
    q = {}
    if source.strip() == draft.strip():
        q["keep_english"] = {"type": "noul",
            "instructions": f"In a video editor's interface in {lang}, it is normal to keep the English text '{source}' untranslated."}
    else:
        q["same_meaning"] = {"type": "noul",
            "instructions": f"In a video editor's interface, the {lang} draft means the same as the English source."}
        for term in EDIT_TERMS:
            if re.search(rf"\b{term}", source, re.I):
                q["editing_sense"] = {"type": "noul",
                    "instructions": f"The {lang} draft uses the video-editing sense of '{term}' (as in a video editor's timeline), not another sense."}
                break
    q["natural"] = {"type": "score",
        "instructions": f"How natural the draft reads to a native {lang} speaker.",
        "criteria": "0 = awkward, 0.5 = acceptable, 1 = natural"}
    return q


def code_checks(source, draft):
    """Problems code can find without a model."""
    problems = []
    ph = lambda s: sorted(re.findall(r"%L?\d+|%n", s))
    if ph(source) != ph(draft):
        problems.append(f"placeholders differ ({' '.join(ph(source))} vs {' '.join(ph(draft))})")
    acc = lambda s: len(re.findall(r"&(?![&a-zA-Z#]+;)(?!&)\S", s))
    if (acc(source) > 0) != (acc(draft) > 0):
        problems.append("& accelerator missing or added")
    if source.split("\t")[1:] != draft.split("\t")[1:]:
        problems.append("tabbed shortcut changed")
    return problems


def ask_jev(key, state, qs):
    body = json.dumps({"model": MODEL, "state": state, "questions": qs}).encode()
    req = urllib.request.Request(API_URL, data=body, method="POST", headers={
        "Authorization": f"Bearer {key}", "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def decide(answers):
    """Return ('drop'|'comment'|'keep', reason) from Jev's answers."""
    get = lambda n: (answers.get(n) or {}).get("noul")
    p = get("same_meaning")
    if p is None:
        p = get("keep_english")
    if p is None:
        return "keep", ""
    sense = get("editing_sense")
    if sense is not None:
        p = min(p, sense)
    reason = f"Jev same-meaning {p:.2f}"
    if sense is not None and sense < KEEP_FROM:
        reason += f", editing sense {sense:.2f}"
    nat = (answers.get("natural") or {}).get("score")
    if nat is not None and nat < 0.34:
        reason += ", reads awkwardly"
    if p < DROP_BELOW:
        return "drop", reason
    if p < KEEP_FROM:
        return "comment", reason
    return "keep", reason


MSG = re.compile(r"(?P<indent>[ \t]*)<message(?P<attrs>[^>]*)>(?P<body>.*?)</message>", re.S)
CTX = re.compile(r"<context>\s*<name>(.*?)</name>(.*?)</context>", re.S)
TRANS = re.compile(r'(<translation type="unfinished">)(.*?)(</translation>)', re.S)


def text(tag, body):
    m = re.search(rf"<{tag}>(.*?)</{tag}>", body, re.S)
    return html.unescape(m.group(1)) if m else None


def process(path, key, apply, limit):
    lang_code = re.search(r"montage_(.+)\.ts$", path).group(1)
    lang = LANG_NAMES.get(lang_code, lang_code)
    src = open(path, encoding="utf-8").read()
    counts = {"checked": 0, "drop": 0, "comment": 0, "keep": 0, "error": 0}

    def ctx_sub(cm):
        name = html.unescape(cm.group(1))
        sources = [text("source", m.group("body")) or "" for m in MSG.finditer(cm.group(2))]

        def msg_sub(m):
            body = m.group("body")
            if "numerus" in m.group("attrs") or "<translatorcomment>" in body:
                return m.group(0)
            t = TRANS.search(body)
            source = text("source", body)
            if not t or source is None or not t.group(2).strip():
                return m.group(0)
            if limit and counts["checked"] >= limit:
                return m.group(0)
            draft = html.unescape(t.group(2))
            counts["checked"] += 1
            problems = code_checks(source, draft)
            action, reason = ("comment", "; ".join(problems)) if problems else ("keep", "")
            if not problems and key:
                i = sources.index(source) if source in sources else 0
                near = [s for s in sources[max(0, i - CONTEXT_NEIGHBOURS):i + CONTEXT_NEIGHBOURS + 1] if s != source]
                state = (f"Video editor interface text, {lang}. Context: {name}. "
                         f"Nearby texts: {json.dumps(near)}. English: {json.dumps(source)}. Draft: {json.dumps(draft)}")
                try:
                    action, reason = decide(ask_jev(key, state, questions(source, draft, lang)))
                except Exception as e:  # any failure: leave the message as it is
                    counts["error"] += 1
                    print(f"  jev error, left as is: {e}", file=sys.stderr)
                    return m.group(0)
            counts[action] += 1
            if action == "keep":
                return m.group(0)
            print(f"{action:7} [{name}] {source!r} -> {draft!r}: {reason}")
            if action == "drop":
                body = body[:t.start(2)] + body[t.end(2):]
            else:
                ind = m.group("indent") + "    "
                body = body[:t.start()] + f"<translatorcomment>check first: {html.escape(reason, quote=False)}</translatorcomment>\n{ind}" + body[t.start():]
            return f'{m.group("indent")}<message{m.group("attrs")}>{body}</message>'

        return cm.group(0)[:cm.start(2) - cm.start()] + MSG.sub(msg_sub, cm.group(2)) + "</context>"

    out = CTX.sub(ctx_sub, src)
    if apply and out != src:
        open(path, "w", encoding="utf-8").write(out)
    print(f"{path}: {counts}" + ("" if apply else " (report only; --apply to write)"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--apply", action="store_true", help="write comments and removals into the files")
    ap.add_argument("--limit", type=int, default=0, help="check at most N drafts per file")
    a = ap.parse_args()
    key = os.environ.get("TYPESAFE_API_KEY")
    if not key:
        print("TYPESAFE_API_KEY not set: running the code checks only", file=sys.stderr)
    for f in a.files:
        process(f, key, a.apply, a.limit)


if __name__ == "__main__":
    main()
