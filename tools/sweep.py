#!/usr/bin/env python3
"""Every control must actually change the picture.

A GLSL uniform whose name does not match the C++ is ignored without a word,
and so is an engine parameter nobody copies across: a slider can be wired to
nothing while the plugin compiles, links, loads and renders perfectly. Nothing
in a build catches it and nothing in the picture looks wrong -- the control
just does not do anything.

This renders each parameter at both ends of its range against the same test
card and reports any that made no difference at all.

    python3 tools/sweep.py [--binary build/sstest] [--size WxH] [--jobs N]

Exit code 1 means something is dead.

------------------------------------------------------------------ the traps

**It runs at high Speed, so that it finishes.** A Martin M1 picture is two
minutes of signal; at the default 40x forty frames is a quarter of it. Every
render here is at Speed 1.0 (120x) unless Speed is what is being swept, so
forty frames is 88 seconds of signal and most of a picture.

**Auto VIS starts with Martin M1.** The station cycles Martin, Scottie,
Robot, and its first picture is Martin -- so Mode's two ends, Martin and Auto
VIS, render the same picture for the first two minutes of signal. Mode is
swept Martin against Robot 36.

**Latch and Live differ only when the clip moves.** On a still card every
line of a live picture is the line the latched one would have sent. Transmit
is swept with `--motion`, which scrolls the card every frame.

**The channel controls are conditional.** Multipath does nothing until
Multipath Level is up, QRM Freq nothing until QRM Level is, Fade Rate nothing
without Fade Depth. Audio QRM and Bin Spacing need a spectrum, which the
harness supplies with `--audio`. Each carries the context it needs in
`CONTEXT`; a parameter missing from that table is swept on the defaults.

**Restart is an event.** A value of 1 is a press on the first frame, before
anything has been sent, and restarts nothing. It is swept as a press half way
through the run against no press at all.

**An option's range is its element count.** `sstest --list` prints it for
exactly this reason, and the ends are what get swept.

**Every name must be unique.** `--set` finds a parameter by name.

**Never sweep the About block.** Those are buttons that open a web browser.
"""
import argparse
import concurrent.futures
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent

WIDTH, HEIGHT = 320, 180
FRAMES = 40

# Every render, unless the key is swept itself. Keys beginning with "_" are
# HARNESS settings, not plugin parameters:
#   _frames     how many frames to render (default 40)
#   _low/_high  the two positions to compare, when not the range's ends
#   _args       extra harness arguments for BOTH renders
#   _args_high  extra harness arguments for the high render only
BASE = {"Speed": 1.0}
AUDIO = ["--audio", "1"]
CONTEXT = {
    # Auto VIS's first picture is Martin M1, the other end.
    "Mode": {"_high": 2},
    # On a still card a live line is the latched line.
    "Transmit": {"_args": ["--motion"]},
    "Fade Rate": {"Fade Depth": 1.0},
    "Multipath": {"Multipath Level": 0.8},
    "QRM Freq": {"QRM Level": 0.5},
    "Audio QRM": {"_args": AUDIO},
    "Bin Spacing": {"Audio QRM": 1.0, "_args": AUDIO},
    # A press half way through against no press.
    "Restart": {"_low": 0, "_high": 0, "_args_high": ["--press", "Restart@20"]},
}
def parameters(binary):
    """id, name, kind, low, high from the harness's own declaration."""
    out = subprocess.run([binary, "--list"], capture_output=True, text=True)
    if out.returncode != 0:
        print("could not list parameters:", out.stdout, out.stderr)
        sys.exit(1)

    found = []
    for line in out.stdout.splitlines():
        m = re.match(
            r"\s*(\d+)\s+(.+?)\s{2,}(\S+)\s+([\d.eE+-]+)\s+\[\s*([\d.eE+-]+)\s*\.\.\s*([\d.eE+-]+)\s*\]",
            line,
        )
        if m:
            found.append((int(m.group(1)), m.group(2).strip(), m.group(3),
                          float(m.group(5)), float(m.group(6))))
    return found


def render(binary, path, overrides, extra):
    frames = overrides.get("_frames", FRAMES)
    args = [binary, "--out", path, "--size", f"{WIDTH}x{HEIGHT}", "--frames", str(frames)]
    for name, value in overrides.items():
        if not name.startswith("_"):
            args += ["--set", f"{name}={value}"]
    args += extra
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", " ".join(args), r.stdout, r.stderr)
        sys.exit(1)
    return pathlib.Path(path).read_bytes()


def pixels(png):
    """Raw RGBA out of the harness's own PNG (filter 0 rows), so nothing else
    is a dependency."""
    i = 8
    idat = b""
    width = height = 0
    while i < len(png):
        length = int.from_bytes(png[i:i + 4], "big")
        kind = png[i + 4:i + 8]
        data = png[i + 8:i + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(data[0:4], "big")
            height = int.from_bytes(data[4:8], "big")
        elif kind == b"IDAT":
            idat += data
        i += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for row in range(height):
        out += raw[row * (stride + 1) + 1:(row + 1) * (stride + 1)]
    return out


def difference(a, b):
    pa, pb = pixels(a), pixels(b)
    if len(pa) != len(pb):
        return 1.0, len(pa)
    changed = sum(1 for x, y in zip(pa, pb) if x != y)
    return changed / max(len(pa), 1), changed


def sweep_one(job):
    binary, scratch, pid, name, low, high, context = job

    base = {k: v for k, v in BASE.items() if k != name}
    lo = dict(base, **context)
    hi = dict(base, **context)
    lo[name] = context.get("_low", low)
    hi[name] = context.get("_high", high)
    extra = list(context.get("_args", []))

    a = render(binary, f"{scratch}/{pid}_lo.png", lo, extra)
    b = render(binary, f"{scratch}/{pid}_hi.png", hi, extra + list(context.get("_args_high", [])))
    fraction, count = difference(a, b)
    # Progress as it happens, on stderr, so a run cut off by a CI timeout
    # still says how far it got.
    print(f"  swept {pid:3d} {name}", file=sys.stderr, flush=True)
    return pid, name, fraction, count


def main():
    global WIDTH, HEIGHT

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", default=str(ROOT / "build" / "sstest"))
    ap.add_argument("--size", default="%dx%d" % (WIDTH, HEIGHT))
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--allow-no-gl", action="store_true",
                    help="SKIP loudly, not FAIL, when the harness cannot create a GL context (CI)")
    args = ap.parse_args()
    if "x" in args.size:
        WIDTH, HEIGHT = (int(v) for v in args.size.split("x", 1))
    jobs = args.jobs or min(8, os.cpu_count() or 1)

    binary = str(pathlib.Path(args.binary).resolve())
    if not pathlib.Path(binary).exists():
        print(f"{binary} is not built")
        return 1

    scratch = tempfile.mkdtemp(prefix="sssweep")

    # One render first: a runner with no GL at all cannot sweep anything,
    # and that has to read as a skip, loudly, not as twenty dead controls.
    probe = subprocess.run([binary, "--out", f"{scratch}/probe.png", "--size", "16x16", "--frames", "1"],
                           capture_output=True, text=True)
    if probe.returncode != 0 and "could not create an OpenGL" in (probe.stdout + probe.stderr):
        print("NO GL CONTEXT: the sweep could not render at all.")
        if args.allow_no_gl:
            print("SKIPPED (--allow-no-gl): no control was checked for liveness on this machine.")
            return 0
        return 1

    skipped = []
    work = []
    for pid, name, kind, low, high in parameters(binary):
        if kind == "about":
            skipped.append((name, "a button that opens a web browser"))
            continue
        if kind in ("buffer", "text"):
            skipped.append((name, "no scalar to sweep"))
            continue
        work.append((binary, scratch, pid, name, low, high, CONTEXT.get(name, {})))

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for r in pool.map(sweep_one, work):
            results.append(r)

    dead = []
    for pid, name, fraction, count in sorted(results):
        if count == 0:
            dead.append(name)
            print(f"DEAD  {pid:4d}  {name}")
        else:
            print(f"ok    {pid:4d}  {name}  ({count} subpixels, {fraction * 100:.2f}%)")

    print()
    for name, why in skipped:
        print(f"skip  {name}: {why}")

    print(f"\n{len(results)} swept, {len(dead)} dead, {len(skipped)} skipped, {jobs} at a time")
    if dead:
        print("\nDEAD CONTROLS: " + ", ".join(dead))
        print("either the uniform name does not match the shader, or the sweep")
        print("needs a CONTEXT entry saying what else has to be true.")
        return 1
    print(f"all {len(results)} swept parameters measurably change the picture")
    return 0


if __name__ == "__main__":
    sys.exit(main())
