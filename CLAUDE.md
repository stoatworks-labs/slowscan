# slowscan

Slow-scan television over HF, as an FFGL **effect** for Resolume Arena/Avenue.
C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows `.dll`. MIT.

Read `AGENTS.md` before changing the signal chain in `source/sstv/`, the receiver's
timing, or any check's tolerance.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64` (the session build is `build-dev`)
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel`
- Install into Arena: `cmake --install build`. **Do not run this from a session.** It
  writes into `~/Documents/Resolume Arena/Extra Effects`.
- Render a frame offline: `./build/sstest --out /tmp/f.png --size 1920x1080 --frames 200`
- Set anything by name: `--set "Mode=2" --set "SNR=0.3" --set "Clock Error=0.7"`
  (0..1 for sliders, the element index for options)
- Press an event: `--press "Restart@30"`. Feed a synthetic spectrum: `--audio 1`.
  Scroll the card so Live differs from Latch: `--motion`
- List parameters, kinds, defaults and ranges: `./build/sstest --list`
- Footage through the real plugin — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH`, `--fps N` and an optional `--script` of timed cues:
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/sstest --pipe --size 1920x1080 --fps 30 [--script cues.txt] | ffmpeg …`
  A cue line is `frame  Parameter Name  value` (`#` starts a comment), in the same
  units as `--set`; `@audio` is the synthetic spectrum's level. Values interpolate
  linearly between a name's cues and hold before the first and after the last, so a
  step needs two cues a frame apart. Frame *n* is clocked at `n / --fps` (default 60).
  An unknown name exits 2 before any frame; a partial frame at EOF ends the stream
  with exit 0; a closed stdout exits 1.

## Verify
- Everything: `tools/verify.sh` (the shaders through glslc, a fresh universal build,
  every check at 320x180 AND 1280x720, the sweep, the bench, the bundle; ~35 s)
- The shaders alone: `tools/check-shaders.sh` (needs `brew install shaderc`)
- Line, segment and pixel boundaries to the sample: `./build/sstest --timing`
- The lean is `e T_line / T_pixel`: `./build/sstest --slant`
- Flat fields exact, the ramp, the edge's tau: `./build/sstest --levels`
- Noise against SNR, the closed form and the knee: `./build/sstest --threshold`
- `floor( t s / T_line )` lines; Speed inert per sample: `./build/sstest --progressive`
- The header decodes: `./build/sstest --vis`
- Line Sync holds the edge: `./build/sstest --sync`
- The six-day clock: `./build/sstest --clock`
- Names fit 16 characters: `./build/sstest --names`
- The frame is the decoder's picture: `./build/sstest --render --size 320x180`
- The lean, fitted in the rendered frame: `./build/sstest --raster --size 320x180`
- The checks can fail: `./build/sstest --negative`
- Everything with no GL (what CI runs): `./build/sstest --offline`
- `--pipe` keeps the fleet frame format: `tools/verify.sh` feeds it 2.5 frames and wants
  exactly 2 back, a cue naming no control and wants exit 2, and a closed stdout and wants exit 1
- No dead controls: `python3 tools/sweep.py --binary build/sstest` (`--size WxH`, `--jobs N`)
- Render cost: `./build/sstest --bench`. The CPU chain alone: `./build/sstest --engine`
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Slowscan.bundle`

## Notes
- **The chain is CPU code, the GPU is two passes.** `source/sstv/` is the whole radio:
  `Transmitter` → `Channel` → `Receiver`, stepped per sample by `Engine`. The GPU only
  reads the clip down to 320xN (`Readback`) and composes the received picture
  (`Compose`). A wrong picture is almost always a `source/sstv/` fix.
- **Time is integer.** Segment boundaries are compared in microseconds × fs, as
  64-bit integers; nothing about line timing is accumulated in floating point. The
  only float state is the oscillator phase, reduced to [0, 1) every sample.
- **Speed is how many samples a frame is worth, and nothing else.** No per-sample
  process may read it. `--progressive` proves 1x and 120x bit-identical.
- **Nothing absolute crosses from the host clock.** Frame durations are differences
  of doubles, clamped to 1/240..1/24 s, with the fractional sample carried.
  Resolume's clock has been seen at 499,217 s.
- **Test hooks are always off in the plugin**: `Debug*` on `Transmitter`, `Channel`,
  `Receiver`, `Engine` and `Slowscan`. They exist so `--negative` can break the
  model, not the check.
- **Audio is noise shaped by the bins**, through a 64-tap FIR redesigned when the bins
  change. `Bin Spacing` is the unmeasured bin layout; the bins are read as magnitudes.
- **Parameter names must be unique**: `--set` and the sweep find them by name.
- `SetParamInfo` clamps a STANDARD default into 0..1, so every slider is 0..1 and
  `Controls.h` holds the units. Options are mapped by index.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `slowscan_core` is an OBJECT library, not STATIC: the plugin registers itself from a
  file-scope constructor nothing references by name.
- `FFGLScopedFBOBinding.h` is not in the umbrella header; include `<ffglex/FFGLScopedFBOBinding.h>`.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `SS01`, display name `SW Slowscan`.

## Not done yet
- **Never loaded into Resolume.** Everything numeric is measured offline on macOS,
  plus an `oxbow` load. The Windows build is CI-only and has never run.
- No user guide, no OpenFX port, no browser demo, no factory presets.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies with `guide=""`.

## Diagnostics

`source/Diag.{h,cpp}`: a log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/slowscan/slowscan.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\slowscan\logs\slowscan.YYYY-MM-DD.log   (Windows)
