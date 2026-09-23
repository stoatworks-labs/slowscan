# AGENTS.md — Slowscan

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you
tell anybody this works.

---

## What the plugin is

Slow-scan television over HF, as an FFGL 2.1 effect (`SS01`, shown as `SW Slowscan`)
for Resolume Arena and Avenue. C++17 + GLSL 4.10, CMake, universal macOS `.bundle`
and a Windows `.dll`. MIT, intended home `github.com/stoatworks-labs/slowscan`.

Built 2026-09-23. A first session wrote the scaffolding, the chain and the harness,
and was cut off before `verify.sh` had ever run (commit `a488654`). A second session
read all of it, kept what was sound, fixed what was not (see "Defects found in the
draft"), and finished it. Templates: vectrix for a CPU signal engine at an audio-ish
rate feeding a GL renderer; ferric and compander for one-dimensional processing in
scan order; pilot for a picture that arrives progressively over an audio channel,
and its probe validator; tinsel and pitch for the harness, sweep, verify and CI.

---

## The one idea

**Build the actual chain: source → tones → HF channel → discriminator → line timing
→ picture.** Run it on the CPU at 11,025 samples a second and show what the receiver
made of it.

| the chain, doing what it does | what comes out |
| --- | --- |
| a receiver that times lines from its own clock, `e` ppm off | **slant**, `e × T_line / T_pixel` pixels a line |
| the same slant pulling a line early, in a mode that sends G, B, R in turn | **colour fringes** on the leading edge, where each scan picks up the end of the one before |
| a picture buffer written line by line as the lines complete | **progressive arrival**, the new picture over the old from the top |
| Gaussian noise into a phase-difference discriminator | pixel noise ∝ 1/SNR above the **FM threshold**, and **clicks** (whole-cycle slips) below it |
| the discriminator's one-pole lowpass | **horizontal smear**, an edge's tail with the one-pole's τ |
| a Clarke sum of sinusoids on the path | **fading** that takes the SNR down with it: FM does not dim, so a fade is a band of noisy lines |
| a delayed second path | **ghosts** and comb notches |
| a carrier in the passband | a beat that drags the discriminator into **vertical stripes** |
| the host's FFT as a noise spectrum | **audio as interference**, landing where its spectrum overlaps 1500–2300 Hz |

The GPU does two things only. It reads the clip down to the mode's 320×256 (or
320×240) through a box filter into a double-buffered pixel-pack buffer, so the
readback never stalls. And it composes the received picture, uploaded as RGB8, at
the mode's aspect with the cursor and the mix. Everything between is CPU code in
`source/sstv/`. That is not an optimisation: it is what lets every physics check
run without a GL context, and so be raster-free by construction.

### Where the timings come from

Every constant in `source/sstv/Modes.cpp` is J. L. Barber (N7CXI), *Proposal for
SSTV Mode Specifications*, Dayton SSTV forum, 2000, as transcribed by the first
session. **This session did not re-read Barber's document.** What it did check:

| figure | spec | code | agrees |
| --- | --- | --- | --- |
| Martin M1 sync / porch / scan / separator | 4.862 / 0.572 / 146.432 / 0.572 ms | same | yes; `static_assert` line = 446.446 ms |
| Martin M1 lines, width, VIS | 256, 320, 44 | same | yes |
| Scottie S1 sep / scan / sync / porch | 1.5 / 138.240 / 9 / 1.5 ms | same | yes; `static_assert` line = 428.22 ms |
| Scottie S1 leading sync | "the leading sync the spec describes" | 9 ms at 1200 Hz, first line only | yes |
| Scottie S1 lines, VIS | 256, 60 | same | yes |
| Robot 36 sync / porch / Y / sep / porch / chroma | 9 / 3 / 88 / 4.5 / 1.5 / 44 ms | same | yes; `static_assert` line = 150 ms |
| Robot 36 separator tone | 1500 or 2300 Hz, "which says whether R−Y or B−Y follows" | 1500 Hz on even lines (R−Y), 2300 Hz on odd (B−Y) | yes |
| Robot 36 **second porch tone** | **not stated** | 1900 Hz, Barber's figure | **the spec is silent; flagged** |
| Robot 36 lines, VIS | 240, 8 | same | yes |
| VIS | leader 1900 Hz 300 ms, break 1200 Hz 10 ms, leader, start bit 1200 Hz 30 ms, 7 bits LSB first + even parity (1100 = 1, 1300 = 0), stop 1200 Hz | same | yes |
| 120x Martin M1 | "about 0.9 M samples per second" | 120 × 11,025 = **1.32 M** | **no: the spec's figure is wrong; flagged**. Measured cost is below. |

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/sstv/Modes.{h,cpp}` | The three modes and the VIS header as tables of named integer-microsecond constants, the tones, BT.601 for Robot 36, and the integer sample-boundary helpers. |
| `source/sstv/Transmitter.*` | The station: picture in, phase-continuous analytic tone out, one sample at a time. Latch or Live. |
| `source/sstv/Channel.*` | The HF path on the analytic signal: Clarke fading, a second path, a QRM carrier, AWGN on the real audio, and the audio input as noise shaped by the bins. |
| `source/sstv/Receiver.*` | The decoder: a 63-tap analytic bandpass (Hilbert pair and band limit in one FIR), the phase-difference discriminator, the one-pole, the VIS decoder, Free-run or Line Sync timing, the picture planes. |
| `source/sstv/Engine.*` | The three, stepped a block at a time. The ONE place video time enters: `SamplesForFrame`. |
| `source/sstv/Rng.h` | vectrix's seeded xorshift128+. |
| `source/Controls.h` | What a 0..1 slider means in dB, Hz, ms, ppm. |
| `source/shaders/*.cpp` | Three complete shaders: the vertex quad, the readback, the compose. |
| `source/Slowscan.{h,cpp}` | The plugin: parameters, the clock, the readback and its PBOs, the upload, the compose. |
| `source/PassBuffer.*`, `Diag.*` | tinsel's FFGLFBO with the leak fixed; a log file. |
| `tools/sstest/` | The harness: renders, measures, benchmarks. |
| `tools/sweep.py`, `tools/verify.sh`, `tools/check-shaders.sh` | No dead controls; all of it; the GLSL through glslc. |

---

## Defects found in the draft

The first session's code built but had never been verified. Running it found:

1. **Four of its own checks failed**: `--threshold`, `--progressive`, `--clock`, and
   `--render` at 320×180. In every case the check was wrong, not the plugin. See the
   traps below.
2. **The QRM carrier sat at 0 Hz.** `Channel::SetParams` retuned the carrier only when
   `qrmHz` *changed*, and the constructor's first call compared the defaults with
   themselves. Until an operator moved QRM Freq the "carrier" was a DC offset. The
   plugin happened to escape it (its slider default maps to 1893 Hz, not 1900), but
   the harness's engine-level channel did not.
3. **Audio interference aliased.** It was 64 tones at the bins' frequencies. Under the
   Log spacing, bins 52–63 sit between 5.5 and 20 kHz, above the channel's 5512.5 Hz
   Nyquist, and a phasor at 15 kHz sampled at 11,025 Hz is a tone at 3.9 kHz. The spec
   asks for noise shaped by the bins. It is that now, and out-of-band bins drop out.
4. **The cursor was invisible at 320×180.** Nearest sampling puts 256 lines on 180
   rows and skips a third of them, so a one-row cursor blinked in and out as it walked
   down the frame. The sweep reported Cursor dead. The compose shader now marks every
   output pixel whose footprint holds the cursor row's centre.
5. **Nothing tested the readback.** Every chain check sets its source directly. A
   one-character mutation that flipped the readback upside down (`MaxUV.y - uv.y` →
   `MaxUV.y + uv.y`) passed every other check in the harness, the new `--raster`
   included (a vertical edge is the same flipped). `--render` now checks a quadrant card arrives
   at the station the right way up; that mutation fails it.
6. **No `--size`, no second raster, `--list` the sweep could not read, no sweep, no
   verify, no CI, no docs.**
7. **`--negative` covered six checks and did not name the bound.** It covers every
   physics check now, perturbing the model through a test hook each time, and each
   case names the check that must be among the failures.
8. **Every manual start came out six pixels to the right** (found filming the
   release video, where it showed as a purple bar down the left edge of any picture
   whose header had failed). `Engine::manualStartIfNeeded` fired on the first sample
   of line 2 and clamped `within`, which is 31 samples negative there, to 0: the line
   began a whole group delay early, and the first six pixels of green read the sync
   pulse. It now waits until the line's start has come out of the FIR. `--vis` checks
   a manual start's origin against the same 7.35-sample bound as a decoded one, in
   all three modes (0.0 samples; the draft's clamp is its negative control).
9. **A switch to Robot 36 showed the old picture green and magenta** (also found
   filming). The picture planes are R, G, B in Martin and Scottie and Y, R−Y, B−Y in
   Robot, and `configureMode` kept them as they were, so the old picture was
   composited in the wrong colour model until the new one had painted over all of it:
   0.9 s at 40x, 37 s at 1x. And a switch between pictures (through `SetParams`)
   blanked the screen instead, so what an operator saw depended on which side of a VIS
   header the change landed. The planes are now converted when the colour model
   changes, rows a larger mode adds are black, and the old picture stays either way.
   `--progressive` compares the composite before and after the change on every row
   not yet repainted, both directions, within one code (worst 1, 0 and 0).

The chain itself, the integer timing, the phase-continuous oscillator, the VIS
decoder, the Line Sync re-anchoring and the clock handling were sound, and are
unchanged, except for the manual start (defect 8).

---

## Traps

Roughly in the order they will bite.

### ☠️ A row's last pixel is not settled

`--progressive` counted a row as replaced when its LAST pixel read the new value, and
counted zero rows on every frame. The receiver's FIR reaches 31 samples (six pixels)
past the sample it is centred on, so the last pixels of a scan see the separator
after it and never read the flat 128. The probe is now the last pixel `--levels`
proves exact, and the guard either side of a line boundary is derived from where
*that* pixel lands: (settled + 1) pixels, the trailing separator, the group delay and
5τ of one-pole, 127 samples.

### ☠️ At 40x a frame is exactly a whole number of samples

`--clock` demanded the per-frame sample counts at t = 0 and at t = 499,217 s be
identical, and 245 of 600 were not. At 40x a 1/60 s frame is exactly 7350 samples,
so the carry lands *on* an integer every frame. One ULP of a six-day clock (5.8e-11 s,
5e-5 of a sample at this rate) flips the floor: 7349 then 7351. Nothing is wrong. The
frame durations telescope, so the *running total* cannot drift, and that is what is
asserted now, to one sample. Taking the difference in float (the actual trap) fails
it; that is the negative control.

### ☠️ The knee is not where "CNR = 10 dB" puts it, and twice-Gaussian is unreachable

The draft asserted the variance ratio from 10 to 5 dB was at least twice the linear
law, "the FM threshold at about 7 dB". It measured 3.49 against 6.32. The 7 dB came
from the folk rule that FM breaks at 10 dB CNR. Here the knee is at 3–4 dB SNR,
because the receiver's filter passes half the 3 kHz the SNR is stated in (+3 dB),
and because the tone sits inside a 1.5 kHz band.

The check now computes both regimes from the receiver's own filters:

- **Above the knee**: the linearised discriminator. Phase noise is the noise's
  quadrature component over the carrier amplitude; the discriminator differences it;
  the one-pole filters it; a pixel interpolates two outputs. Its variance is an
  integral over the FIR's response, `4 sin²(πν)` and the one-pole's `|H|²`. It matches
  the measurement to 1.4–1.9% from 15 to 30 dB.
- **The knee**: Rice's click rate `r erfc( sqrt( CNR ) )`, each click an impulse of
  one cycle into the one-pole (Campbell's theorem gives its variance). The knee is
  stated by the textbook's definition, the SNR where the output noise is 1 dB over the
  linear formula: **3.11 dB** predicted, **3.70** measured.

A first version defined the knee as the excess reaching 2× the Gaussian. The
measurement peaks at 2.09 and falls again, because a discriminator's output is
bounded and the variance saturates while the linear formula does not. The 1 dB
definition is both the textbook's and reachable.

The same saturation sank the draft's slope check, "steeper below the knee than
above it". The chord from the knee to 5 dB under it straddles the click regime and
the saturation, and came out 0.126 dB/dB steeper than the chord above on one draw
and 0.086 on the next (the tolerance is 0.103). The draw changed because defect 8's
fix moved every manual start by 31 samples, and a header does not decode below about
12 dB here. So the check is now the one the saturation cannot reach: the chord from 5
dB above the knee down to it is steeper than the far chord (15 to 10 dB, itself the
linear law) by more than the tolerance. The chord below is printed, not asserted.

### ☠️ One seed makes one draw look like a bias

Above the knee the measurement is −1.90%, −1.95%, −1.88%, −1.39% from the closed form
at 30, 25, 20 and 15 dB. That looks like a systematic 2% error. It is not: the noise
is seeded, so every SNR scales *the same* noise sequence, and the relative deviation
is the same single draw of a 1.42% standard error at every SNR. Do not "correct" the
closed form by 2%.

### ☠️ A probe on a pixel boundary is a coin toss

`--render` at 320×180 found 1575 wrong pixels. The rect is 240×180 for 320×256: 0.75
output pixels to a picture pixel, and whole columns of probe centres land within a
float rounding of a picture-pixel boundary. The shader (an interpolated varying) and
the harness (a CPU float) rounded them to opposite sides. A probe is now trusted only
when its picture coordinate is at least 1/16 of an *output* pixel from a boundary.
That is the minimum sub-pixel precision GL 4.1 guarantees (§14.6.1), and a full-screen
quad's interpolation rounds orders of magnitude inside it. The letterbox and cursor
checks use the same margin. The validator also has to *fire* at a raster too coarse
to show every pixel, and that is asserted.

### ☠️ Speed must not reach a per-sample process

Everything in the channel is defined per sample of the 11,025 Hz signal. So a Fade
Rate of 1 Hz fades 120 times as fast *on screen* at 120x. That is correct (the whole
signal is sped up) and it is tempting to "fix". The harness runs 24 lines of a noisy,
fading, multipath, QRM'd picture at 1x and at 120x and asserts the planes are
bit-identical. Multiplying the fade rate by Speed, the "fix", is the negative control.

### ☠️ The FIR's group delay cancels, so do not take it off

The receiver's whole timeline runs on the FIR's output, which is the signal 31
samples late, and the pixels are read off that same output. So the line origin and
the pixel clock agree and the group delay cancels. The first session took it off in
`trackSync` alone and shifted every picture six pixels right; the ramp check caught
it, and the comment is still there. The one place it IS taken off is the manual start
in `Engine`, which reads the *station's* clock, not the receiver's.

### ☠️ Auto VIS's first picture is Martin M1

So Mode's two ends, Martin M1 and Auto VIS, render the same picture for the first two
minutes of signal. The sweep compares Martin with Robot 36.

### ☠️ Mutation-test only a committed tree

pitch lost an uncommitted fix to `git checkout` after a mutation. Both mutations here
were made on a committed, clean tree and reverted with `git checkout`.

### Inherited from the fleet, and all still true here

`ScopedFBOBinding` does not restore the viewport (the host's is captured first and
put back before the compose); every `ffglex::Scoped*` clears to 0 on exit, so every
`Ensure()` happens before anything binds a texture; `FFGLFBO::Release()` leaks the
colour texture, which is why `PassBuffer::Destroy()` deletes it first; `SetParamInfo`
clamps a STANDARD default into 0..1 before `SetParamRange` can widen it; the core is
an **OBJECT** library; `SetTextParameter` must return `FF_SUCCESS` for the About
block; `FFGLScopedFBOBinding.h` is not in the umbrella header; the harness drives a
synthetic clock; `nm | grep -q` fails under pipefail when grep succeeds; an option's
range reads back 0..1 whatever its element count; Resolume's clock overflows a float;
a GitHub macOS runner cannot create an accelerated GL context.

---

## Would this hold on another rasteriser, at another raster?

One line per check. Every tolerance is derived, not fitted. `verify.sh` runs every
check at 320×180 and 1280×720. The chain's checks are raster-free by construction:
they measure the decoder's own 320×N planes, before any GL. So the question for them
is "on another CPU", and the answer is IEEE doubles plus libm, whose last-bit
differences are far inside every tolerance below.

| check | what it measures | tolerance and where it comes from | raster / platform dependence |
| --- | --- | --- | --- |
| `--timing` boundaries | line, segment, pixel starts vs `ceil( t fs )` | **exact**; guarded by "no boundary within 1e-6 of a whole sample" (closest 3e-4), so integer and double arithmetic cannot round apart | none: integer arithmetic |
| `--timing` phase | advance per sample vs f/fs | 1e-12, the double rounding of a sum below 1 (measured 0) | none |
| `--slant` fit | fitted lean vs `e T_line / T_pixel`, ±100 and ±300 ppm, two modes | one transmitter sample a line (1/pxS px) through the least-squares bound `Σ|x−x̄| / Σ(x−x̄)²`: 0.0023–0.0025 px/line; measured within 5e-5 | none |
| `--slant` whole pixel | column moved by a 40 px drift | ±1 column, the threshold's quantisation; crossing within two samples (0.40 px) | none |
| `--slant` fractional | column moved by a 40.5 px drift | 40 or 41, and nothing else: a different check from the whole-pixel one, as the addendum requires | none |
| `--levels` flat | 8-bit value of flat black, grey, white | **exact** after rounding; worst 0.275 of the 0.5 code allowed. The residue is the FIR's image and transition band, which are design constants, not platform ones | none |
| `--levels` ramp | pixel k vs (k − τ)/(W−1) | one transmitter step + half an 8-bit code (0.0051); measured 0.0027 | none |
| `--levels` edge | τ fitted from ln\|1 − v\| | 1%: ten times the larger of the interpolation error (0.015%) and the FIR image ripple, which is zero-mean over the fit | none |
| `--threshold` linear | variance vs the linearised closed form, 15–30 dB | 4 standard errors of a sample variance of N correlated values, `sqrt( 2(1+2ρ²)/N )` with ρ from the closed form's own autocorrelation, plus 1/CNR for the linearisation: 5.7–7.1% | seeded noise; another libm moves the draw, and the tolerance is a standard error |
| `--threshold` knee | SNR where variance is 1 dB over linear | Rice's formula's two known approximations, each solved from the closed forms: a factor of 3 in click energy (click width vs impulse) moves the knee 1.47 dB; the linear form's 1/CNR error moves it 1.06; plus a quarter of the 1 dB grid. 2.78 dB; measured 0.59 | as above |
| `--threshold` slopes | dB per dB, far above and into the knee | 4√2 standard errors + 1/CNR at 10 dB, in dB over 5 dB (0.103) | as above |
| `--progressive` count | rows replaced vs `floor( t s / T_line )` | **exact**, with frames within a derived guard (127 samples) of a line boundary excluded, and the count of asserted frames itself asserted | none |
| `--progressive` Speed | 1x vs 120x planes | **bit-identical**: one binary, the same operations in the same order, only block sizes differ | none |
| `--progressive` mode change | the composite of rows not yet repainted, before and after Martin → Robot and back | one 8-bit code: the flat field's decode error (0.275), the 601 inverse's three decimals (under 0.1) and the composite's rounding (0.5) | none |
| `--vis` | decoded code; line 0's origin; a manual start's origin | exact code; origin within fs / FIR bandwidth = 7.35 samples (measured 1.0 decoded, 0.0 manual) | none |
| `--sync` | edge spread under Line Sync at 300 ppm | two samples, one of sync detection and one of the station's pixel grid: 0.40–0.42 px (the spec asks one pixel; the derived bound is asserted); measured 0.24 | none |
| `--clock` | running sample totals, fresh vs six-day | one sample: two ULPs of 5e5 s × s × fs is 5e-5 of a sample, so floors can differ by one only where the sum sits on an integer | none |
| `--render` exactness | frame bytes vs the decoder's picture | **exact**: `texelFetch` is nearest, `mix( clip, pic, 1 )` is `pic` exactly; probes need a 1/16-output-pixel margin, GL 4.1's minimum sub-pixel precision | **yes**: at 320×180 58,400 of 81,920 pixels cannot be resolved and the validator must say so; 23,520 are still compared |
| `--render` Mix 0 | frame vs the input | **exact**: `mix( clip, pic, 0 )` is `clip`; the clip is sampled at texel centres of a same-size texture, and any weight error below half an 8-bit code survives rounding | relies on at least 8 bits of sub-texel precision, which every conformant GL has |
| `--render` readback | quadrant primaries at the station | **exact** primaries, two picture pixels clear of each boundary (the box filter's footprint is at most one; the second is for the bilinear taps) | any width that is a multiple of 8 puts the taps on texel centres; both rasters are |
| `--render` resize | rows not being written, before and after | **exact** bytes | the second size is 960×540 or 480×270 |
| `--raster` | lean fitted in the rendered frame | per row: pw/rw picture px (the two output pixels either side of the crossing) + 1 (nearest sampling) + 1/pxS (the decoder's own edge), through 3/N: 0.018 at 1280×720, 0.030 at 320×180; measured within 5e-4. Also asserts the lean is resolved (larger than the tolerance) | **yes, and the tolerance is derived per raster**; the edge lands on picture pixel 120 at any width that is a multiple of 8 |
| `--negative` | fourteen broken models fail | n/a | the two GL cases run at the raster asked |
| `sweep.py` | each control changes ≥ 1 subpixel | any change | run at 320×180 locally, 160×90 in CI |

Two things are deliberately not relied on. Exact cancellation: no check asserts
`== 0` on a difference of two computed floats; the closest is the phase error, which
has a stated bound. And `pow` returning exact values: no mapping in `Controls.h` is
assumed exact; the harness reads the *effective* ppm back from the plugin
(`ResolvedForTest`) rather than trusting the slider it set.

What might still differ on another rasteriser: a software GL that cannot create a
4.1 core context at all. That is the GitHub runner case: CI runs `--offline` and
compiles the shaders through glslc, and the rendered checks SKIP loudly.

---

## Negative controls and the mutation

`sstest --negative` perturbs the **model** (a test hook in the plugin or the engine,
always off in the plugin) and runs each check unchanged. Every case must fail, and
must fail the named bound:

| perturbation | check | failed |
| --- | --- | --- |
| a line 50 ppm long | `--timing` "line starts equal" | 9 of 21 |
| the Clock Error term dropped | `--slant` fit | 20 of 32 |
| the tone's phase reset at every pixel | `--levels` "the discriminator's error bound" | 3 of 3 |
| no lowpass | `--levels` the edge's closed-form τ | 2 of 3 |
| the SNR stated in 1.5 kHz, not 3 (3 dB) | `--threshold` the linearised closed form | 5 of 8 |
| the signal run 1% fast | `--progressive` rows replaced | 6 of 10 |
| the fade rate multiplied by Speed | `--progressive` "Speed changes nothing per sample" | 1 of 10 |
| the planes kept as they were when the colour model changes (the draft's) | `--progressive` "keeps the old picture" | 3 of 10 |
| the parity bit sent wrong | `--vis` decoded | 6 of 10 |
| the manual start clamped to the line's start (the draft's) | `--vis` "a manual start" | 3 of 10 |
| Line Sync switched off | `--sync` "stays within" | 4 of 6 |
| frame durations taken in float | `--clock` running totals | 1 of 3 |
| the picture uploaded one row low | `--render` byte for byte | 4 of 11 |
| the Clock Error term dropped | `--raster` the rendered lean | 4 of 4 |

The spec asks that a phase reset fail `--levels` "on its spectral-splatter bound".
The ramp bound is that bound. Splatter outside 1000–2500 Hz is rejected by the
receiver's FIR, and what reaches the picture is the in-band part, as discriminator
error on the ramp. The check's text says so, and the negative control names it.

**The GLSL mutation.** On a committed, clean tree, `float rowY = ( 1.0 - inner.y )`
in `source/shaders/Compose.cpp` was changed to `( 1.0 + inner.y )`, one character. It
was caught by `--render` (4 of 11 checks, at both rasters: 9,760 of 23,520 probes
wrong at 320×180, 34,418 of 81,920 at 1280×720) and by `--raster` (4 of 4: the fitted
lean went to 0.00000). Then it was reverted with `git checkout` and the checks
re-run green. That proves the harness drives the shaders the plugin ships, not a
copy.

A second mutation, `MaxUV.y - uv.y` → `MaxUV.y + uv.y` in `Readback.cpp`, passed
every other check in the harness. It is how the missing readback check was found (defect
5), and the new check fails it: 39,816 of 79,632 pixels wrong.

---

## Decisions taken without asking

- **The receiver is one FIR, not a Hilbert transformer then a bandpass.** A 63-tap
  Hamming lowpass of half the 1000–2500 Hz band, modulated up to its centre by a
  complex exponential, is analytic and band-limited at once. Every SSTV tone from the
  1100 Hz VIS bit to 2300 Hz white is inside it.
- **The discriminator is `atan2( z × conj( z_prev ) )`**, the phase advance per
  sample, which is exact for a tone. `Rx Bandwidth` is a one-pole after it, because
  its step response is closed form and `--levels` measures it.
- **The picture planes are unclamped.** The FIR's ringing overshoots white and the
  planes keep it so a measurement sees the whole tail; `Composite` clamps for display.
- **The cursor marks the row below the one arriving**, so the arriving pixels stay
  visible. It is at least one output pixel tall at any raster (defect 4).
- **Every mode is shown 4:3.** Martin's and Scottie's 320×256 pixels are not square;
  a decoder shows them 4:3. `Aspect` Fit is a 4:3 box with a transparent surround
  (the letterbox shows the layer below); Fill stretches.
- **Robot 36's chroma scan is 320 samples in 44 ms.** The standard's 160 chroma
  pixels are a receiver convention; the transmitted tone is continuous, and the
  channel's bandwidth, not the sample count, sets the horizontal chroma resolution.
  Each chroma line is shared with its pair, R−Y from the even line and B−Y from the
  odd.
- **A header that fails to decode is a manual start two lines in**, from the
  station's own clock, as an operator would press Start. Below the threshold the
  picture is streaks, not a frozen frame. There is no correlation search.
- **Latch reads a frame one frame old**, through the double-buffered PBO. Against a
  line of a third of a second, even at 120x, it does not matter, and the readback
  never stalls the host.
- **SNR is the tone's power over the noise in 3 kHz**, noise added to the real audio
  only (noise in the imaginary part is noise nobody can hear).
- **Fade Depth blends a steady path with the Rayleigh one**: 0 is a wire, 1 pure
  scatter, Rician between. Eight Clarke rays, seeded, as Jakes used.
- **The audio input is noise, not tones** (defect 3). A 64-tap frequency-sampling FIR
  whose response at each cell is the power average of the bins over it, Parseval-exact
  in power. A flat full-scale spectrum at Audio QRM 1 is interference as loud as the
  picture.
- **`Bin Spacing`, not `Bin Law`.** needle's `Bin Law` is magnitude against power.
  Here what matters most is *where* the bins sit, because that decides whether the
  music lands in the picture band. So the exposed assumption is the frequency layout
  (Linear to 22.05 kHz, or Log from 20 Hz to 20 kHz), and the bins are read as
  magnitudes. One unmeasured assumption per control is enough.
- **`Audio QRM`, not "Audio Interference"**: 18 characters, over the 16 the host
  shows. QRM is the ham's word for man-made interference.
- **Speed is 1x–120x, log, default 40x**: a picture every three seconds.
- **Frame durations are clamped to 1/240..1/24 s.** A stall or a scrub would
  otherwise ask the engine for seconds of signal in one block.
- **The knee is stated by the textbook's 1 dB definition** (see the trap).
- **`--raster` runs at ±150 ppm.** That is large enough that the lean is resolved at
  320×180 (0.146 px/line against a 0.030 tolerance), and small enough that the edge
  stays clear of the scan's first settled pixels over the frame.
- **No factory presets**, like pitch.
- **CI splits on GL**: `--offline` and glslc always; the rendered checks and the
  sweep with `--allow-no-gl`, which SKIPs loudly when there is no context at all.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4.1 (2026-09-23)

Every number below is `tools/verify.sh` on this machine against a fresh universal
Release build, at 320×180 and 1280×720.

- **Timing.** All 256/256/240 line starts, every segment on lines 0, middle and last,
  all 320 pixel boundaries of a scan: their constants, to the sample. Pictures of
  1,270,082 / 1,218,741 / 406,933 samples.
- **Slant.** ±100 and ±300 ppm in Martin and Scottie, within 5e-5 px/line of
  `e T_line / T_pixel` (0.29269 at 300 ppm in Martin), tolerance 0.0023. Whole-pixel
  40.0 and fractional 40.5 drifts land where they should.
- **Levels.** Flat fields exact (worst 0.275 of 0.5 codes); ramp 0.0027 of 0.0051;
  τ = 5.797 px against 5.797.
- **Threshold.** −1.4% to −1.9% from the closed form at 15–30 dB (tolerance
  5.7–7.1%); knee 3.70 dB against 3.11 (tolerance 2.78); 1.019 dB/dB far above,
  steepening to 1.265 on the 5 dB into the knee (1.351 under it, not asserted).
- **Progressive.** 789 asserted frames at 1x, 40x and 120x, all exact; 1x and 120x
  bit-identical through a noisy, fading, multipath channel; a mode change Martin →
  Robot 36 and back keeps the old picture's colours within one code.
- **VIS.** 44, 60, 8 decoded, origins 1.0 sample off; a manual start (parity sent
  wrong) 0.0 samples off in all three modes; Auto VIS decodes three in turn.
- **Sync.** 0.24 px spread at 300 ppm against Free-run's 75 px.
- **Clock.** Running totals within one sample after six days; float fails 599/599.
- **Render.** Byte for byte at both rasters; validator fires at 320×180; letterbox
  transparent; the readback the right way up; Mix 0 is the clip; resize mid-run
  keeps unwritten rows; Robot 36 after a restart renders exactly.
- **Raster.** 0.14637 against 0.14634 px/line at 1280×720; 0.14684 at 320×180.
- **Negative controls.** All fourteen fail as they should.
- **Mutation.** Caught by `--render` and `--raster`, reverted.
- **No dead controls.** All 20 sweepable parameters, at 320×180 and 160×90.
- **Every shader compiles** through glslc.
- **The bundle** is universal, exports `_plugMain`, carries
  `com.stoatworks.ffgl.slowscan`, ad-hoc signs, and `oxbow` reports `SW Slowscan` /
  `SS01` / `effect` and renders 120 frames through `plugMain`.
- **Cost**, best of three runs of 60 frames after a warm-up, `glFinish` both sides,
  engine included, on a GPU and CPU shared with other builds:

  | | speed | ms/frame | % of a 60fps frame |
  | --- | --- | --- | --- |
  | 1280×720 | 40x | 1.04 | 6.2% |
  | 1920×1080 | 40x | 1.02 | 6.1% |
  | 3840×2160 | 40x | 1.04 | 6.2% |
  | 1920×1080 | 1x | 0.29 | 1.7% |
  | 1920×1080 | 120x | 2.58 | 15.5% |

  The output size hardly matters; the CPU chain is the cost, and it scales with
  Speed. `--engine` alone: 11 Msamples/s at the defaults (0.017 ms a frame at 1x,
  2.0 ms at 120x), 6 Msamples/s with multipath, QRM and audio on (3.7 ms at 120x).
  That is on the host's render thread. The table is the final `verify.sh` run; an
  earlier run minutes before read 1.01 / 1.01 / 1.04 / 0.33 / 2.54 on the same
  shared machine.

### Assumed, or not done

- ☠️ **Never loaded into Resolume**, on either platform. The inspector presentation,
  the audio input's routing, the host's clock and its FFT bins are all untested.
- **Resolume's 64 bins are unmeasured**, as fleet-wide. `Bin Spacing` is the hedge.
- **The Windows build is CI-only**, and CI cannot run yet.
- **3.7 ms of CPU a frame at 120x with everything on** may be too much on a loaded
  show machine. The engine is single-threaded on the render thread.
- **Barber's document was not re-read** by the session that finished this; the
  constants were checked against the spec's own figures (see the table).
- **The fading is flat** and the channel at most two paths.
- **The decoder is one design.** MMSSTV and QSSTV differ in filter and sync detail.
- **No OpenFX port.** Not required for 0.1.0. The browser demo's signal chain is a hand
  port (see "The browser demo").
- **`StoatworksAbout.h` and `ATTRIBUTIONS.md` are generated** by stoatworks-backend's
  `sync-about.py` and `sync-attributions.py` from the website's projects.json and the
  attribution master lists. Edit those, not these files; the next sync overwrites them.
- **Nothing has been through a show.**

## The browser demo

`demo/` is the page at **slowscan-demo.stoatworks-labs.com** (Cloudflare Worker
`slowscan-demo`, `wrangler.toml`, no build step). Written 2026-09-24. `demo/README.md`
has the working detail; this is the why.

**What runs for real.** The three shaders, `kVertex`, `kReadbackFragment` and
`kComposeFragment`, copied unedited into `demo/plugin.js` and run in the same two passes
as `ProcessOpenGL`: the kit's generated clip goes through the plugin's Readback shader
onto 320×256 (320×240), is read back with `readPixels`, and the received picture comes
back through the plugin's Compose shader with the cursor, the 4:3 rect and the mix.
`demo/tools/check_shaders.py` compares them character for character and `verify.sh`
runs it. None of the three contains a backtick, a backslash or `${`, so there is no
escape to decode and the checker rejects any backslash on the JS side. Negative
control (2026-09-24): `1.0 - inner.y` → `1.0 + inner.y` in plugin.js's COMPOSE fails
it at line 32; restored.

**What is ported.** All of `source/sstv/` (`demo/sstv.js`: Rng, Modes, Transmitter,
Channel, Receiver, Engine), and `Controls.h`, `resolve()`, `frameSecondsFor()`,
`pictureRect()` and the cursor row (`demo/plugin.js`). Function for function, same
constants, same integer timing. xorshift128+ is done in 32-bit halves (SplitMix64
seeding in BigInt), so the noise is the C++'s noise bit for bit; `int64_t` is a JS
double, exact because nothing passes 1.3e12; `lround` is ported as half-away-from-zero;
the planes are `Float32Array`; each slider is `Math.fround`ed before conversion, as the
plugin stores it as a float. **Nothing checks the port but a reader.** When it was
written it was driven side by side with a scratch driver linking `source/sstv/*.cpp`,
900 frames each through four channels (defaults; Robot 36 at 3 dB with full fast
fading, multipath, QRM, Line Sync and −200 ppm; Auto VIS + Live at 120x; Scottie S1
switching to Robot 36), and the composites were **byte-identical** — so the browser's
Math.* and this libm agreed at least that far. That comparison is not kept running.

**What is omitted, and why.** `Audio` (FF_TYPE_BUFFER), `Audio QRM` and `Bin Spacing`:
no host FFT in a browser, so a control there could only be dead. The interference path
is still ported and fed 64 zero bins, and those two controls take the constructor's
defaults. `Restart` (FF_TYPE_EVENT): the kit has no event control, so the transport's
Restart (the page clock going backwards) calls `Engine.Restart()` as the event does.
The host-clock unit vote (`elapsedSeconds`): the page's clock is seconds. The test
hooks (`Debug*`) and the About block.

**Decisions taken without asking.**
- **Synchronous readback.** WebGL2 cannot map a pixel-pack buffer, so the page
  `readPixels` straight into a typed array and hands the transmitter this frame, not
  last frame's. Chrome logs "GPU stall due to ReadPixels" four times; that is this.
- **No CPU cap beyond the plugin's.** The page clamps the frame to 1/240…1/24 s and runs
  every sample it is worth (≤ 55,125), as the plugin does. A slow machine runs the
  signal slower than Speed asks, and the stats line says so against the page's own clock
  (not the clamped duration, which is exactly what would hide it). Measured: ~5 M
  samples/s in Node on the M4 Max, 4.2 ms a frame at 120x; in headless Chrome on
  SwiftShader the page managed 52–115x of 120x, the software GL taking the rest.
- **The port is its own module** (`sstv.js`, no DOM), so it can be run in Node against
  the C++ — which is how the byte-for-byte comparison above was done.
- **A stats line under the canvas**: mode on the air, line being received, whether the
  header decoded or the picture was started by hand, samples this frame, achieved speed,
  CPU ms. A picture half-painted over the last reads as a broken page without it.
- **Clips**: bars first (a slant, a fringe and a smear read at a glance), then grid,
  scene, ramp, spot, detail. `showBackdrop` is on because Fit's letterbox is transparent.
- **Presets are the page's own** (the plugin ships none), made only of its parameters:
  clean path, below the FM threshold, deep fading, ghost, carrier on frequency, +300 ppm
  free-run / with Line Sync / slant-corrected, Robot 36, Auto VIS, real time, Live at 120x.

---

## Open questions

- **Should the engine run on its own thread?** At 120x with everything on it is 3.7
  ms of the host's render thread. A worker a frame behind would cost nothing visible
  (a line is a third of a second even at 120x) but adds a lock and a lifetime.
- **Should Speed cap the fade rate's on-screen speed?** At 120x a 1 Hz Doppler spread
  flickers. That is correct, and an operator may hate it. The honest answer is a note
  in the guide, not a change to the physics.
- **Is the manual start right?** A real decoder without a header either waits or
  guesses by correlation. Starting from the station's clock is the operator pressing
  Start at exactly the right moment, which is kinder than reality.
- **More modes?** Martin M2 and Scottie S2/DX are new tables of the same shape.
  Robot 72 and the PD family are not: they carry colour differently and PD sends two
  lines per scan. The spec named three.
- **Bin magnitude or power?** needle's question, unanswered here too. An hour in Arena
  with a sine at known levels would settle it for the fleet.

---

## Siblings

- **vectrix**: the CPU signal engine feeding a GL renderer, the seeded generator.
- **ferric**, **compander**: one-dimensional processing in scan order.
- **pilot**: a picture that arrives progressively over an audio channel; the probe
  validator that must fire at a coarse raster.
- **readout**: the host clock-unit voting.
- **tinsel**, **pitch**: `PassBuffer`, the sweep, `verify.sh`, CI.
- **needle**: exposing an unmeasured FFT assumption as a control.
- **oxbow**: `oxbow probe` and `oxbow selftest` are what load this bundle as a host.
