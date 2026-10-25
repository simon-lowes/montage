#!/usr/bin/env python3
"""Reads Montage's window over AT-SPI, the way a Linux screen reader (Orca) does, and checks what it hears: the
timeline a table whose rows are the tracks and whose cells name each clip with its track and times, the monitors'
transport buttons named, and the program monitor saying where it is. Run it under X and a D-Bus session, with pyatspi:

  dbus-run-session -- xvfb-run -a python3 scripts/atspi-check.py build/src/app/montage project.montage

The project should have a clip on its timeline. Prints the table, and exits non-zero on what is missing."""
import os
import re
import subprocess
import sys
import time

import pyatspi

CELL = re.compile(r"^.+, [VA]\d+, \d\d:\d\d:\d\d[:;]\d\d to \d\d:\d\d:\d\d[:;]\d\d")


def walk(node, depth=0, limit=40):
    """Every accessible under node, depth first (a dead one skipped)."""
    if node is None or depth > limit:
        return
    yield node
    try:
        count = node.childCount
    except Exception:  # (gone while reading)
        return
    for i in range(min(count, 5000)):
        try:
            child = node.getChildAtIndex(i)
        except Exception:
            continue
        yield from walk(child, depth + 1, limit)


def find_app(name, timeout):
    end = time.time() + timeout
    while time.time() < end:
        desktop = pyatspi.Registry.getDesktop(0)
        for i in range(desktop.childCount):
            app = desktop.getChildAtIndex(i)
            if app is not None and app.name.lower() == name:
                return app
        time.sleep(0.5)
    return None


def find(app, role, name=None, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        for node in walk(app):
            try:
                if node.getRole() == role and (name is None or node.name == name):
                    return node
            except Exception:
                continue
        time.sleep(0.5)
    return None


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    env = dict(os.environ, QT_LINUX_ACCESSIBILITY_ALWAYS_ON="1", QT_QPA_PLATFORM="xcb", MONTAGE_NO_RECOVERY_PROMPT="1")
    app_process = subprocess.Popen([sys.argv[1], os.path.abspath(sys.argv[2])], env=env)
    failures = []
    try:
        app = find_app("montage", 90)
        if app is None:
            sys.exit("Montage did not appear on the accessibility bus")
        table = find(app, pyatspi.ROLE_TABLE, "Timeline")
        if table is None:
            sys.exit("No table called Timeline")
        t = table.queryTable()
        rows = []
        for r in range(t.nRows):
            cells = []
            for c in range(t.nColumns):
                cell = t.getAccessibleAt(r, c)
                if cell is not None:
                    cells.append(cell.name)
            rows.append((t.getRowDescription(r), cells))
            print(f"{t.getRowDescription(r)}: " + " | ".join(cells))
        clips = [cell for _, cells in rows for cell in cells if not cell.startswith("Gap, ") and cell]
        if not rows:
            failures.append("the timeline has no rows")
        if not clips:
            failures.append("no clip cells")
        for cell in clips:
            if not CELL.match(cell):
                failures.append(f"a clip cell does not give its track and times: {cell!r}")
        for name in ("Mark In", "Mark Out", "Play / Pause"):
            if find(app, pyatspi.ROLE_PUSH_BUTTON, name, timeout=5) is None:
                failures.append(f"no button called {name}")
        monitor = find(app, pyatspi.ROLE_IMAGE, "Program monitor", timeout=5)
        if monitor is None:
            failures.append("no Program monitor")
        else:
            print(f"Program monitor: {monitor.description}")
            if not re.match(r"^\d\d:\d\d:\d\d[:;]\d\d, In ", monitor.description):
                failures.append(f"the program monitor does not say where it is: {monitor.description!r}")
    finally:
        app_process.terminate()
        try:
            app_process.wait(10)
        except subprocess.TimeoutExpired:
            app_process.kill()
    for f in failures:
        print("FAIL:", f)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
