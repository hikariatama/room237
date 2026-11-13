#!/usr/bin/env python3
"""Stands between a QA app and its core (ROOM237_QA_CORE_PROCESS) and forwards to the app's own core server
($LISTING_PROXY_APP --qa-core-server). Only for test_listing_order.py: replies to get_albums_detached are held, released
in a chosen order and optionally stripped of album rows, as an album walk that raced a move would return them.
Control: JSON in $LISTING_PROXY_CONTROL."""
import json, os, subprocess, sys, threading, time
here = os.path.dirname(os.path.abspath(__file__))
real = subprocess.Popen([os.environ["LISTING_PROXY_APP"], "--qa-core-server", *sys.argv[1:]], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
control = os.environ["LISTING_PROXY_CONTROL"]
lock = threading.Lock()
listing_ids, held = [], {}
def rule():
    try:
        with open(control) as f: return json.load(f)
    except (OSError, ValueError): return {}
def out(line):
    with lock:
        sys.stdout.buffer.write(line); sys.stdout.buffer.flush()
def upstream():
    for line in sys.stdin.buffer:
        msg = json.loads(line)
        if msg.get("method") == "get_albums_detached":
            with lock:
                listing_ids.append(msg["id"])
                with open(control + ".count", "w") as f: f.write(str(len(listing_ids)))
        real.stdin.write(line); real.stdin.flush()
    real.stdin.close()
release_lock = threading.Lock()
def release_ready(r):
  with release_lock:
    release_ready_locked(r)
def release_ready_locked(r):
    # Release held answers in the configured order once all of them are in.
    order = r.get("order", [])
    if r.get("waitForRelease") and not r.get("release"): return
    if order and all(i in held for i in order):
        for i in order:
            msg = held.pop(i)
            drop = set(r.get("drop", {}).get(str(i), []))
            if drop and isinstance(msg.get("result"), list):
                msg["result"] = [a for a in msg["result"] if a.get("relative_path") not in drop]
            out((json.dumps(msg) + "\n").encode())
        with open(control + ".released", "a") as f: f.write(json.dumps(order) + "\n")
def downstream():
    for line in real.stdout:
        try: msg = json.loads(line)
        except ValueError: out(line); continue
        r = rule()
        with lock: number = listing_ids.index(msg["id"]) + 1 if msg.get("id") in listing_ids else None
        if number is not None and number in r.get("hold", []):
            held[number] = msg; release_ready(r); continue
        if number is not None and str(number) in r.get("drop", {}):
            drop = set(r["drop"][str(number)])
            if isinstance(msg.get("result"), list): msg["result"] = [a for a in msg["result"] if a.get("relative_path") not in drop]
            line = (json.dumps(msg) + "\n").encode()
        out(line)
def poll():
    while True:
        time.sleep(.05)
        with lock: pending = bool(held)
        if pending: release_ready(rule())
threading.Thread(target=upstream, daemon=True).start()
threading.Thread(target=poll, daemon=True).start()
downstream()
sys.exit(real.wait())
