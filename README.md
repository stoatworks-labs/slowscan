# slowscan

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The radio is not asserted
> but measured: an offline harness drives the real plugin class and the real signal
> chain it owns, and checks each claim against its closed form. Every line, segment
> and pixel boundary of Martin M1, Scottie S1 and Robot 36 lands on its constant to
> the sample over a whole frame. A clock error of *e* leans a vertical edge by
> `e × T_line / T_pixel` pixels a line, fitted in the decoder's picture and again in
> the rendered frame at two rasters. Above the FM threshold the pixel noise is the
> linearised discriminator's closed form to 2%, and the knee is where Rice's click
> rate puts it. The VIS header decodes back to the mode it was sent in, Line Sync
> takes a 75-pixel slant down to a quarter of a pixel, and Speed changes nothing
> per sample, bit for bit. Twelve negative controls prove the checks can fail. It
> has **never been loaded into Resolume**. It is loaded by
> [oxbow](https://github.com/stoatworks-labs/oxbow), which is a real FFGL host and
> is not Resolume. See [Status](#status).

Slow-scan television over HF, as an FFGL effect for [Resolume](https://resolume.com)
Arena and Avenue.

![A test card received over a fading HF path: the new picture overwriting the old from the top with a green cursor on the arriving line, a slant from a receiver clock 60 ppm off, noise streaks, and a colour fringe down the left edge where the slant has pulled the end of one scan into the start of the next](docs/hero.png)

<sub>One frame, rendered by `sstest`, the offline harness — not captured from
Resolume. Martin M1 at 40x through a 15 dB channel with deep fading and a receiver
clock 60 ppm fast. The card scrolls, so the picture arriving (above the cursor) is
not the picture it is overwriting (below it).</sub>

## The one idea

Slow-scan television sends a still picture down a voice radio channel as audio. Each
pixel is a tone, 1500 Hz for black to 2300 Hz for white, sent a line at a time, with
a 1200 Hz pulse between lines. A Martin M1 picture takes almost two minutes. At the
other end an FM discriminator turns the tones back into brightness, and the decoder
paints each line as it arrives.

The plugin builds the actual chain, **source → tones → HF channel → discriminator →
line timing → picture**, and runs it on the CPU at 11,025 samples a second. The
clip is read back at the mode's resolution and sent. What reaches the screen is
whatever the receiver made of it.

## What falls out

None of these is drawn. Each is the chain doing what it does:

- **Slant.** The receiver times lines from its own sound-card clock. If that clock
  is a few ppm off, each line starts a fraction of a pixel early or late, and a
  vertical edge leans by `e × T_line / T_pixel` pixels a line: 0.29 at 300 ppm in
  Martin M1. `Slant Correct` subtracts a set ppm, as the decoders' slant adjustment
  does. `Line Sync` re-anchors every line on its 1200 Hz pulse and the lean goes.
- **Colour fringes on the slanted edge.** Martin sends green, blue and red one after
  another. When the slant pulls a line early, the start of each colour scan picks
  up the end of the one before it.
- **Progressive arrival.** The new picture overwrites the old one from the top, a
  line at a time, and the line arriving is marked by the cursor.
- **Noise streaks.** Below the FM threshold, around 3 dB SNR in 3 kHz here, the
  discriminator starts throwing clicks, whole-cycle phase slips, and the picture
  turns to streaks.
- **Horizontal smear.** `Rx Bandwidth` is the discriminator's lowpass. Narrow it and
  edges smear to the right over its time constant.
- **Fading.** A seeded Clarke model with a Doppler spread of `Fade Rate`. It dims
  whole bands of lines and takes the SNR with it.
- **Ghosts and combing.** A second path at `Multipath` ms interferes with the first.
- **Vertical stripes.** A carrier on frequency (`QRM Freq`, `QRM Level`) beats with
  the picture tones and drags the discriminator with it.
- **Audio is interference.** Resolume hands an effect 64 FFT bins, not a waveform.
  So the audio routed into the plugin goes on air as noise shaped by those bins,
  and the music lands in the picture where its spectrum overlaps the SSTV band.

`Speed` runs the two-minute picture in as little as a second. It only changes how
many samples a video frame is worth. Every process is defined per sample, and the
harness proves that 24 lines of a noisy, fading picture come out bit-identical at 1x
and at 120x.

### The honest limit

Audio interference is noise shaped by the host's spectrum, because a spectrum is all
the host gives. Nobody has measured how Resolume lays out its 64 bins, so
`Bin Spacing` exposes the assumption (Linear to 22.05 kHz, or Log from 20 Hz to 20
kHz), and the bins are read as magnitudes. The fading is flat. The HF channel is one
path, or two, and not the multi-tap ionospheric model a propagation lab would use.
The receiver is one decoder design (analytic bandpass, phase-difference
discriminator, one-pole lowpass), not MMSSTV's or QSSTV's. When a header fails to
decode, the receiver starts the picture from the station's own clock two lines in,
as an operator would press Start. It does not search for the picture's start by
correlation.

## Controls

| Group | |
| --- | --- |
| **Mode** | Mode (Martin M1, Scottie S1, Robot 36, or Auto VIS, which cycles the three and decodes each from its header), Speed (1x–120x, log), Transmit (Latch: the frame grabbed when a picture starts, as a station sends a stored image; Live: each line from the clip as it is sent). |
| **Channel** | SNR (−10 to 40 dB in 3 kHz), Fade Depth (0 is a wire, 1 is pure Rayleigh), Fade Rate (0.02–5 Hz Doppler spread), Multipath (0–8 ms), Multipath Level, QRM Freq (300 Hz–3 kHz), QRM Level, Audio QRM (the audio input as interference), Bin Spacing (Linear or Log: the unmeasured bin layout). |
| **Receiver** | Rx Bandwidth (100 Hz–3 kHz discriminator lowpass), Sync (Free-run or Line Sync), Clock Error (±300 ppm), Slant Correct (±300 ppm, subtracted). |
| **Display** | Cursor, Aspect (Fit, a 4:3 box with a transparent surround; Fill, stretched), Mix, Restart (a new picture now). |

The defaults are a picture every three seconds (Martin M1 at 40x) over a clean-ish
25 dB path with some slow fading, and a receiver clock 30 ppm fast, so the picture
leans a little.

## Status

**v0.1.0, and honestly early. 23 September 2026.**

### Measured offline, on macOS

`tools/verify.sh` passes on this machine (M4 Max, macOS 26.4.1) against a fresh
universal Release build, running every check at **two rasters**, 320×180 and
1280×720. What it establishes, in numbers:

| check | result |
| --- | --- |
| `--timing` | all 256 / 256 / 240 line starts, every segment boundary on three lines and all 320 pixel boundaries of a scan equal `ceil( t × fs )` for their constants, in all three modes; drift over a frame is a ceiling (worst 0.9997 samples), never a sum; the phase advances by exactly f/fs every sample |
| `--slant` | at ±100 and ±300 ppm the fitted lean is within **5e-5 px/line** of `e × T_line / T_pixel` in Martin M1 and Scottie S1, against a tolerance of 0.0023 (one sample through the fit); a whole-pixel drift of 40 moves the thresholded edge 40 columns, a fractional 40.5 moves it 40 or 41 |
| `--levels` | flat black, grey and white come back to the exact 8-bit value (180,090 samples); a ramp is within 0.0027 of its closed form (bound 0.0051); an edge's tail decays with τ = **5.797 px** against the one-pole's 5.797 |
| `--threshold` | above the knee the pixel variance is **within 2%** of the linearised discriminator's closed form, computed from the receiver's own filters (tolerance 5.7–7.1%, four standard errors plus 1/CNR); the knee, 1 dB over the linear law, measured at **3.60 dB** SNR against Rice's **3.11 dB** (tolerance 2.78 dB, derived from the model's two approximations) |
| `--progressive` | at 1x, 40x and 120x, every asserted frame had exactly `floor( t s / T_line )` lines replaced (789 frames); 24 lines of a noisy, fading, multipath picture at 1x and 120x are **bit-identical** |
| `--vis` | VIS 44, 60 and 8 decode back to Martin, Scottie and Robot with no manual start, line 0 placed within 1 sample; Auto VIS decodes three pictures in turn |
| `--sync` | at 300 ppm, where Free-run would drift 75 px, Line Sync holds the edge within **0.24 px** (bound 0.40, derived), one sync pulse per line, no jumps |
| `--clock` | 600 frames at t = 0 and at t = 499,217 s ask for the same running sample total to one sample; the same subtraction in a float gets 599 of 599 frame durations wrong |
| `--render` | every resolvable probe of the frame is the decoder's pixel, byte for byte, cursor included; the letterbox is transparent black; the readback hands the station the clip the right way up; Mix 0 is the clip byte for byte; a resize mid-run keeps every row not being written |
| `--raster` | the lean fitted in the **rendered** frame at ±150 ppm is 0.14637 px/line against 0.14634 at 1280×720 (tolerance 0.018) and 0.14684 at 320×180 (tolerance 0.030) |
| `--negative` | twelve broken models, each failing the bound that should catch it: a 50 ppm line, the clock term dropped (twice, CPU and rendered), the tone's phase reset per pixel, no lowpass, the SNR stated 3 dB wrong, the signal 1% fast, the fade rate multiplied by Speed, a wrong parity bit, Line Sync off, a float clock, the picture uploaded a row low |
| mutation | one character of the shipped compose shader (`1.0 - inner.y` to `1.0 + inner.y`) was caught by `--render` and `--raster` at both rasters, then reverted |
| `tools/sweep.py` | all **20** swept controls measurably change the picture |
| shaders | all 3 compile through `glslc`, not merely through Apple's driver |
| the bundle | universal (`x86_64 arm64`), exports `plugMain`, ad-hoc signs; `oxbow` reports `SW Slowscan` / `SS01` / `effect` and renders 120 frames through `plugMain` |

Render cost at the defaults (40x), best of three runs of 60 frames after a warm-up,
`glFinish` both sides, on a GPU and CPU shared with other work: **1.01 ms** at 720p,
**1.01 ms** at 1080p, **1.04 ms** at 4K. The cost is nearly all the CPU signal
chain, which does not care about the output size. It scales with Speed: **0.33 ms**
a frame at 1x and **2.54 ms** at 120x (1080p). The chain alone runs 11 million samples
a second at the defaults, 6 million with everything on (multipath, QRM, audio). At
120x that is 2.0 ms and 3.6 ms of CPU a frame, on the host's render thread.
macOS figures only.

### Not established

It has **never been loaded into Resolume**, on either platform. Everything above was
compiled, rendered and measured offline against the real plugin class in a headless
CGL context, plus an `oxbow` load. Still untested:

- how 21 parameters in four groups, one of them the audio input, present in
  Arena's inspector;
- whether Resolume's FFT bins are laid out as either `Bin Spacing` assumes;
- whether 3.6 ms of CPU a frame at 120x is comfortable on a busy show machine;
- what the host's real clock does over a long session.

The Windows build is CI-only and has never run. Nothing has been through a show.
There is no OpenFX port, no browser demo and no user guide; none was in scope for
0.1.0.

## Build

Needs CMake 3.15+, a C++17 compiler, and the FFGL SDK submodule.

```bash
git clone --recursive https://github.com/stoatworks-labs/slowscan
cd slowscan
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build     # into ~/Documents/Resolume Arena/Extra Effects
```

macOS builds are universal (Apple Silicon + Intel) by default. Add
`-DCMAKE_OSX_ARCHITECTURES=arm64` for a faster dev build. Windows needs GLEW via
vcpkg.

## Building and testing

The offline harness renders the real plugin class headlessly, on a synthetic 60 fps
clock:

```bash
./build/sstest --out /tmp/frame.png --size 1920x1080   # the test card, received
./build/sstest --list                                  # every control, kind and default
./build/sstest --timing --slant --levels --threshold   # the chain, no GL
./build/sstest --progressive --vis --sync --clock
./build/sstest --render --raster --size 320x180        # the rendered frame
./build/sstest --negative                              # and the checks can fail
./build/sstest --bench --engine                        # 720p to 4K, and the CPU chain
python3 tools/sweep.py                                 # no control is silently dead
tools/verify.sh                                        # all of it, on a fresh universal build
```

Every check takes `--size`. Run the rendered ones at 320×180 as well as the raster
you care about. `--offline` runs everything that needs no GL, which is what CI does.

See [`CLAUDE.md`](CLAUDE.md) for the full command reference and
[`AGENTS.md`](AGENTS.md) for the model and the traps.

## Licence

MIT — see [LICENSE](LICENSE). Third-party components are listed in
[ATTRIBUTIONS.md](ATTRIBUTIONS.md).
