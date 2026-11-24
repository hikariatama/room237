"""Fast scrolling through the real album: blank tiles, superseded page requests and frame gaps.

scroll.py BINARY IMAGE PROFILE OUT ALBUM   (same image layout as scenarios.py)"""
import json, shutil, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from diag_lib import launch, wait, read_trace, now_ms, save_json  # noqa: E402
from scenarios import mount, unmount, MNT, IO_LIB  # noqa: E402


def main():
    binary, image, profile_src, out, album = sys.argv[1:6]
    out = Path(out); shutil.rmtree(out, ignore_errors=True); out.mkdir(parents=True)
    clone = mount(Path(image), out)
    profile = out / "profile"; shutil.copytree(profile_src, profile)
    app = None
    results = {}
    try:
        app = launch(binary, MNT / "lib", profile, out, "app", io_lib=IO_LIB, extra_env={"R237_IO_ROOT": str(MNT / "lib")})
        app.wait_albums(120)
        wait(lambda: not app.op("processing_state")["visible"], 300, 0.2)
        app.op("activate", album=album, timeout=120000)
        for name, step, pause in (("fling", 900, 0.016), ("drag", 120, 0.016), ("jumps", 25000, 0.25)):
            app.op("scroll_to", y=0); time.sleep(1.5)
            app.op("diag", reset=True)
            t0 = now_ms()
            app.op("record_start")
            y, top = 0, app.op("scroll_to", y=0)["max"]
            positions = 0
            while y < top and positions < 400:
                y = min(top, y + step)
                app.op("scroll_to", y=y)
                positions += 1
                time.sleep(pause)
            time.sleep(1.0)
            rec = app.op("record_stop")
            trace = read_trace(app.trace_path, t0)
            samples = rec["samples"]
            blank_samples = sum(1 for s in samples if s["blank"] > 0)
            unready = sum(1 for s in samples if s["ready"] < s["expected"])
            lat = sorted(rec["tileLatencies"])
            pct = lambda p: lat[min(len(lat) - 1, int(len(lat) * p))] if lat else None
            results[name] = {
                "positions": positions, "samples": len(samples), "samplesWithBlank": blank_samples, "samplesNotReady": unready,
                "maxBlank": max((s["blank"] for s in samples), default=0),
                "superseded": sum(1 for _, k, d in trace if k == "log" and "superseded" in d),
                "pageCalls": app.op("diag")["core"].get("get_media_page", {}).get("count", 0),
                "tileLatencyP50": pct(0.5), "tileLatencyP95": pct(0.95), "censored": rec["censored"], "tilesSeen": rec["tilesSeen"],
            }
            print(name, json.dumps(results[name]), flush=True)
    finally:
        if app:
            app.close()
        unmount(clone)
    save_json(out / "scroll.json", results)


if __name__ == "__main__":
    main()
