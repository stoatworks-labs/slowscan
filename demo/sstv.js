/**
 * The SSTV chain, ported from `source/sstv/` to JavaScript for the browser demo.
 *
 * **This is a PORT, not the plugin's code, and nothing checks it but a reader.**
 * The repository's `sstest` checks the C++ originals and has never heard of
 * this file. It is a hand translation, function for function and in the same
 * order as the C++ files, with the same constants, the same integer timing and
 * the same seeded generators:
 *
 *   Rng.h            xorshift128+ and SplitMix64 seeding, in 32-bit halves so
 *                    the draws are the C++'s draws bit for bit (the 64-bit
 *                    multiply in the seeding is BigInt, once per seed)
 *   Modes.{h,cpp}    the three modes and the VIS header, integer microseconds
 *   Transmitter.*    the station: picture in, phase-continuous analytic tone out
 *   Channel.*        Clarke fading, the second path, the QRM carrier, AWGN, and
 *                    the audio-interference filter (which the page never feeds)
 *   Receiver.*       the 63-tap analytic FIR, the phase-difference
 *                    discriminator, the one-pole, VIS, Free-run / Line Sync,
 *                    the picture planes and Composite()
 *   Engine.*         the three stepped a block at a time; SamplesForFrame
 *
 * What differs, and why:
 *
 *   - Every `int64_t` is a JavaScript double. None of them gets near 2^53:
 *     the largest product is a sample index times 1e6, 1.3e12 for a whole
 *     Martin picture, so the integer timing is still exact integer arithmetic
 *     (floor division is Math.floor on non-negative operands, as in the C++).
 *   - `std::llround` / `std::lround` round half AWAY from zero and Math.round
 *     rounds half UP; `lround()` below is the C++ behaviour.
 *   - The picture planes are Float32Array, which is what the C++'s
 *     `std::vector< float >` stores, so every write rounds to float as it does.
 *   - `Math.cos`, `Math.sin`, `Math.atan2`, `Math.log` and `Math.exp` are not
 *     the platform's libm and may differ in the last bit, so a draw here is
 *     the same draw in kind, not a bit-identical picture.
 *   - The test hooks (`Debug*`) and the harness accessors are left out: they
 *     exist for `sstest --negative`, and nothing on a page could reach them.
 */

//---------------------------------------------------------------------------
// Rng.h
//---------------------------------------------------------------------------

const MASK64 = (1n << 64n) - 1n;

/** xorshift128+, seeded through SplitMix64. The state is four uint32 halves. */
export class Rng {
  constructor(seed = 0x9E3779B97F4A7C15n) {
    this.s0hi = 0; this.s0lo = 0; this.s1hi = 0; this.s1lo = 0;
    this.spare = 0.0;
    this.hasSpare = false;
    this.Seed(seed);
  }

  Seed(seed) {
    // SplitMix64 to fill the state: seeding xorshift with a small integer
    // directly gives a first few hundred outputs with visible structure.
    const x = { v: BigInt.asUintN(64, BigInt(seed)) };
    const a = Rng.splitMix(x);
    const b = Rng.splitMix(x);
    let s0 = a;
    const s1 = b;
    if (s0 === 0n && s1 === 0n) s0 = 0x9E3779B97F4A7C15n;
    this.s0hi = Number(s0 >> 32n) >>> 0; this.s0lo = Number(s0 & 0xFFFFFFFFn) >>> 0;
    this.s1hi = Number(s1 >> 32n) >>> 0; this.s1lo = Number(s1 & 0xFFFFFFFFn) >>> 0;
    this.spare = 0.0;
    this.hasSpare = false;
  }

  static splitMix(x) {
    x.v = (x.v + 0x9E3779B97F4A7C15n) & MASK64;
    let z = x.v;
    z = ((z ^ (z >> 30n)) * 0xBF58476D1CE4E5B9n) & MASK64;
    z = ((z ^ (z >> 27n)) * 0x94D049BB133111EBn) & MASK64;
    return z ^ (z >> 31n);
  }

  /**
   * One step. Leaves the 64-bit result in (this.outHi, this.outLo) rather than
   * returning it, so a sample's worth of noise allocates nothing.
   */
  Next() {
    // uint64_t s1 = state[ 0 ]; const uint64_t s0 = state[ 1 ]; state[ 0 ] = s0;
    let ahi = this.s0hi, alo = this.s0lo;
    const bhi = this.s1hi, blo = this.s1lo;
    this.s0hi = bhi; this.s0lo = blo;
    // s1 ^= s1 << 23;
    const shi = ((ahi << 23) | (alo >>> 9)) >>> 0;
    const slo = (alo << 23) >>> 0;
    ahi = (ahi ^ shi) >>> 0; alo = (alo ^ slo) >>> 0;
    // state[ 1 ] = s1 ^ s0 ^ ( s1 >> 18 ) ^ ( s0 >> 5 );
    const r18hi = ahi >>> 18, r18lo = ((alo >>> 18) | (ahi << 14)) >>> 0;
    const r5hi = bhi >>> 5, r5lo = ((blo >>> 5) | (bhi << 27)) >>> 0;
    this.s1hi = (ahi ^ bhi ^ r18hi ^ r5hi) >>> 0;
    this.s1lo = (alo ^ blo ^ r18lo ^ r5lo) >>> 0;
    // return state[ 1 ] + s0;
    const lo = this.s1lo + blo;
    this.outLo = lo >>> 0;
    this.outHi = (this.s1hi + bhi + (lo > 0xFFFFFFFF ? 1 : 0)) >>> 0;
  }

  /** Uniform in [0, 1). 53 bits, so a double's whole mantissa. */
  Unit() {
    this.Next();
    // ( Next() >> 11 ) * 2^-53, exact: the top 32 bits times 2^21 plus the
    // top 21 bits of the low half.
    return (this.outHi * 2097152 + (this.outLo >>> 11)) * (1.0 / 9007199254740992.0);
  }

  /** Standard normal, by Box-Muller. Two per pair; the second is kept. */
  Gaussian() {
    if (this.hasSpare) {
      this.hasSpare = false;
      return this.spare;
    }
    let u1 = this.Unit();
    // log( 0 ) is -inf; a zero draw is one in 2^53 but it only has to
    // happen once to put a NaN through the whole receiver for ever.
    if (u1 < 1e-300) u1 = 1e-300;
    const u2 = this.Unit();
    const r = Math.sqrt(-2.0 * Math.log(u1));
    const a = 6.283185307179586 * u2;
    this.spare = r * Math.sin(a);
    this.hasSpare = true;
    return r * Math.cos(a);
  }
}

//---------------------------------------------------------------------------
// Helpers for the C++ library calls whose JavaScript spelling differs.
//---------------------------------------------------------------------------

/** std::lround / std::llround: half away from zero. */
export function lround(x) {
  return x < 0 ? -Math.round(-x) : Math.round(x);
}

const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

//---------------------------------------------------------------------------
// Modes.h / Modes.cpp
//---------------------------------------------------------------------------

export const kSampleRate = 11025;
export const kSampleRateHz = 11025.0;
export const kTicksPerSample = 1000000;

export const kToneSync = 1200.0;
export const kToneBlack = 1500.0;
export const kToneWhite = 2300.0;
export const kToneLeader = 1900.0;
export const kToneBit1 = 1100.0;
export const kToneBit0 = 1300.0;
export const kToneSpan = kToneWhite - kToneBlack;

export const kVisLeaderMicros = 300000;
export const kVisBreakMicros = 10000;
export const kVisBitMicros = 30000;
export const kVisDataBits = 7;
export const kVisBitSlots = 1 + kVisDataBits + 1 + 1;
export const kVisMicros = kVisLeaderMicros + kVisBreakMicros + kVisLeaderMicros + kVisBitSlots * kVisBitMicros;

export const Segment = { Sync: 0, Porch: 1, Separator: 2, Scan: 3 };
export const Component = { None: 0, Red: 1, Green: 2, Blue: 3, Luma: 4, ChromaAlternating: 5 };

const seg = (kind, micros, component, tone) => ({ kind, micros, component, tone });

// Martin M1. Sync 4.862 ms, porch 0.572 ms, then green, blue, red at 146.432 ms
// each, each followed by a 0.572 ms separator. 446.446 ms a line, 256 lines.
const kMartinSyncMicros = 4862;
const kMartinPorchMicros = 572;
const kMartinScanMicros = 146432;
const kMartinSepMicros = 572;
const kMartinM1Segments = [
  seg(Segment.Sync, kMartinSyncMicros, Component.None, kToneSync),
  seg(Segment.Porch, kMartinPorchMicros, Component.None, kToneBlack),
  seg(Segment.Scan, kMartinScanMicros, Component.Green, 0.0),
  seg(Segment.Separator, kMartinSepMicros, Component.None, kToneBlack),
  seg(Segment.Scan, kMartinScanMicros, Component.Blue, 0.0),
  seg(Segment.Separator, kMartinSepMicros, Component.None, kToneBlack),
  seg(Segment.Scan, kMartinScanMicros, Component.Red, 0.0),
  seg(Segment.Separator, kMartinSepMicros, Component.None, kToneBlack),
];
const kMartinLineMicros = kMartinSyncMicros + kMartinPorchMicros + 3 * kMartinScanMicros + 3 * kMartinSepMicros;

// Scottie S1. The sync is in the MIDDLE of the line, so a leading sync is sent
// before line 0.
const kScottieSepMicros = 1500;
const kScottieScanMicros = 138240;
const kScottieSyncMicros = 9000;
const kScottiePorchMicros = 1500;
const kScottieS1Segments = [
  seg(Segment.Separator, kScottieSepMicros, Component.None, kToneBlack),
  seg(Segment.Scan, kScottieScanMicros, Component.Green, 0.0),
  seg(Segment.Separator, kScottieSepMicros, Component.None, kToneBlack),
  seg(Segment.Scan, kScottieScanMicros, Component.Blue, 0.0),
  seg(Segment.Sync, kScottieSyncMicros, Component.None, kToneSync),
  seg(Segment.Porch, kScottiePorchMicros, Component.None, kToneBlack),
  seg(Segment.Scan, kScottieScanMicros, Component.Red, 0.0),
];
const kScottieLineMicros = 2 * kScottieSepMicros + 3 * kScottieScanMicros + kScottieSyncMicros + kScottiePorchMicros;

// Robot 36. Sync 9 ms, porch 3 ms, Y 88 ms, separator 4.5 ms (1500 Hz on even
// lines, meaning R-Y follows; 2300 Hz on odd lines, meaning B-Y), porch 1.5 ms
// at 1900 Hz, chroma 44 ms. 150 ms a line, 240 lines.
const kRobotSyncMicros = 9000;
const kRobotPorchMicros = 3000;
const kRobotLumaMicros = 88000;
const kRobotSepMicros = 4500;
const kRobotPorch2Micros = 1500;
const kRobotChromaMicros = 44000;
const kRobot36Segments = [
  seg(Segment.Sync, kRobotSyncMicros, Component.None, kToneSync),
  seg(Segment.Porch, kRobotPorchMicros, Component.None, kToneBlack),
  seg(Segment.Scan, kRobotLumaMicros, Component.Luma, 0.0),
  seg(Segment.Separator, kRobotSepMicros, Component.None, 0.0), // alternating: see Transmitter
  seg(Segment.Porch, kRobotPorch2Micros, Component.None, kToneLeader),
  seg(Segment.Scan, kRobotChromaMicros, Component.ChromaAlternating, 0.0),
];
const kRobotLineMicros = kRobotSyncMicros + kRobotPorchMicros + kRobotLumaMicros + kRobotSepMicros + kRobotPorch2Micros + kRobotChromaMicros;

// The C++ static_asserts, as load-time checks.
if (kMartinLineMicros !== 446446 || kScottieLineMicros !== 428220 || kRobotLineMicros !== 150000) {
  throw new Error('sstv.js: a mode table does not sum to its line');
}

export const kMartinM1 = 0;
export const kScottieS1 = 1;
export const kRobot36 = 2;
export const kModeCount = 3;

const mode = (name, vis, width, height, lineMicros, leadInMicros, segments, syncIndex, chromaAlternates) => ({
  name, vis, width, height, lineMicros, leadInMicros, segmentCount: segments.length, segments, syncIndex, chromaAlternates,
});

const kModes = [
  mode('Martin M1', 44, 320, 256, kMartinLineMicros, 0, kMartinM1Segments, 0, false),
  mode('Scottie S1', 60, 320, 256, kScottieLineMicros, kScottieSyncMicros, kScottieS1Segments, 4, false),
  mode('Robot 36', 8, 320, 240, kRobotLineMicros, 0, kRobot36Segments, 0, true),
];

export function Mode(index) {
  return kModes[clamp(index, 0, kModeCount - 1)];
}

export function ModeForVis(vis) {
  for (let i = 0; i < kModeCount; i += 1) if (kModes[i].vis === vis) return i;
  return -1;
}

export function VisParity(vis) {
  let ones = 0;
  for (let b = 0; b < kVisDataBits; b += 1) ones += (vis >> b) & 1;
  return ones & 1;
}

// Robot 36 colour: BT.601 studio range.
export const robot = {
  kYOffset: 16.0,
  kYR: 65.738 / 256.0,
  kYG: 129.057 / 256.0,
  kYB: 25.064 / 256.0,
  kCOffset: 128.0,
  kRYR: 112.439 / 256.0,
  kRYG: -94.154 / 256.0,
  kRYB: -18.285 / 256.0,
  kBYR: -37.945 / 256.0,
  kBYG: -74.494 / 256.0,
  kBYB: 112.439 / 256.0,
};

/** RGB (0..255) to Y, R-Y, B-Y (0..255), into `out`. */
function robotEncode(r, g, b, out) {
  out[0] = robot.kYOffset + robot.kYR * r + robot.kYG * g + robot.kYB * b;
  out[1] = robot.kCOffset + robot.kRYR * r + robot.kRYG * g + robot.kRYB * b;
  out[2] = robot.kCOffset + robot.kBYR * r + robot.kBYG * g + robot.kBYB * b;
}

/** ...and back, clamped to 0..255. */
function robotDecode(y, ry, by, out) {
  const yy = (y - robot.kYOffset) * (255.0 / 219.0);
  const cr = (ry - robot.kCOffset) * (255.0 / 224.0);
  const cb = (by - robot.kCOffset) * (255.0 / 224.0);
  out[0] = clamp(yy + 1.402 * cr, 0.0, 255.0);
  out[1] = clamp(yy - 0.344136 * cb - 0.714136 * cr, 0.0, 255.0);
  out[2] = clamp(yy + 1.772 * cb, 0.0, 255.0);
}

/** The first sample whose start time is at or after `micros`. */
export function FirstSampleAtOrAfter(micros) {
  const numerator = micros * kSampleRate;
  return Math.floor((numerator + kTicksPerSample - 1) / kTicksPerSample);
}

function SampleTicks(n) {
  return n * kTicksPerSample;
}

//---------------------------------------------------------------------------
// Transmitter.cpp
//---------------------------------------------------------------------------

const kTwoPi = 6.283185307179586;
const kPi = 3.141592653589793;

function visTone(micros, vis) {
  if (micros < kVisLeaderMicros) return kToneLeader;
  micros -= kVisLeaderMicros;
  if (micros < kVisBreakMicros) return kToneSync;
  micros -= kVisBreakMicros;
  if (micros < kVisLeaderMicros) return kToneLeader;
  micros -= kVisLeaderMicros;

  const slot = Math.floor(micros / kVisBitMicros);
  if (slot === 0) return kToneSync; // start bit
  if (slot >= 1 && slot <= kVisDataBits) {
    const bit = (vis >> (slot - 1)) & 1; // LSB first
    return bit ? kToneBit1 : kToneBit0;
  }
  if (slot === kVisDataBits + 1) return VisParity(vis) ? kToneBit1 : kToneBit0;
  return kToneSync; // stop bit
}

export class Transmitter {
  constructor() {
    this.requestedMode = 0; // -1 for Auto
    this.modeIndex = 0;
    this.autoCursor = 0;
    this.live = false;
    this.hasSource = false;
    this.liveImage = new Uint8Array(0); // RGBA8, TxWidth x TxHeight
    this.latchedImage = new Uint8Array(0); // what this picture is sending
    this.liveWidth = 0;
    this.liveHeight = 0;
    this.sample = 0;
    this.phase = 0.0;
    this.lastHz = 0.0;
    this.line = -1;
    this.segment = -1;
    this.pixel = -1;
    this.pictures = 0;
    this.lastLineCopied = -1;
    this.yuv = new Float64Array(3);
    this.startPicture();
  }

  TxWidth() { return Mode(this.modeIndex).width; }
  TxHeight() { return Mode(this.modeIndex).height; }
  CurrentMode() { return this.modeIndex; }
  PictureSample() { return this.sample; }
  Line() { return this.line; }
  PicturesStarted() { return this.pictures; }
  HasSource() { return this.hasSource; }

  SetMode(modeRequested) {
    const wanted = modeRequested < 0 ? -1 : clamp(modeRequested, 0, kModeCount - 1);
    if (wanted === this.requestedMode) return;
    this.requestedMode = wanted;

    // A picture in progress finishes in the mode it began in. One that has
    // not started takes the new mode now.
    if (!this.hasSource || this.sample === 0) {
      this.sample = 0;
      this.startPicture();
      this.pictures -= 1; // not a new picture: the same unstarted one, re-described
    }
  }

  SetLive(isLive) {
    this.live = isLive;
  }

  SetSource(rgba, width, height) {
    if (rgba == null || width !== this.TxWidth() || height !== this.TxHeight()) return;
    const bytes = width * height * 4;
    if (this.liveImage.length !== bytes) this.liveImage = new Uint8Array(bytes);
    this.liveImage.set(rgba.subarray(0, bytes));
    this.liveWidth = width;
    this.liveHeight = height;

    // The first frame ever: the picture that has been waiting at sample 0
    // latches it and the transmission begins.
    if (!this.hasSource) {
      this.hasSource = true;
      this.latchedImage = this.liveImage.slice();
    }
  }

  Restart() {
    this.sample = 0;
    this.startPicture();
  }

  PictureSamples() {
    const m = Mode(this.modeIndex);
    return FirstSampleAtOrAfter(kVisMicros + m.leadInMicros + m.height * m.lineMicros);
  }

  startPicture() {
    if (this.requestedMode < 0) {
      this.modeIndex = this.autoCursor % kModeCount;
      this.autoCursor = (this.autoCursor + 1) % kModeCount;
    } else {
      this.modeIndex = this.requestedMode;
    }

    const w = this.TxWidth();
    const h = this.TxHeight();
    const bytes = w * h * 4;
    if (this.liveImage.length === bytes) {
      this.latchedImage = this.liveImage.slice();
    } else if (this.liveImage.length > 0 && this.liveWidth > 0 && this.liveHeight > 0) {
      // The mode just changed size and the readback has not caught up.
      // Resample the frame we have by nearest row rather than send black.
      this.latchedImage = new Uint8Array(bytes);
      for (let y = 0; y < h; y += 1) {
        const sy = Math.min(this.liveHeight - 1, Math.floor((y * this.liveHeight) / h));
        for (let x = 0; x < w; x += 1) {
          const sx = Math.min(this.liveWidth - 1, Math.floor((x * this.liveWidth) / w));
          const to = (y * w + x) * 4;
          const from = (sy * this.liveWidth + sx) * 4;
          this.latchedImage[to] = this.liveImage[from];
          this.latchedImage[to + 1] = this.liveImage[from + 1];
          this.latchedImage[to + 2] = this.liveImage[from + 2];
          this.latchedImage[to + 3] = this.liveImage[from + 3];
        }
      }
    } else {
      this.latchedImage = new Uint8Array(bytes);
    }

    this.sample = 0;
    this.line = this.segment = this.pixel = -1;
    this.lastLineCopied = -1;
    this.pictures += 1;
  }

  pixelValue(lineIndex, px, component) {
    const w = this.TxWidth();
    if (this.latchedImage.length === 0) return 0.0;
    const i = (lineIndex * w + px) * 4;
    const r = this.latchedImage[i];
    const g = this.latchedImage[i + 1];
    const b = this.latchedImage[i + 2];

    switch (component) {
      case Component.Red: return r / 255.0;
      case Component.Green: return g / 255.0;
      case Component.Blue: return b / 255.0;
      case Component.Luma:
      case Component.ChromaAlternating: {
        robotEncode(r, g, b, this.yuv);
        if (component === Component.Luma) return this.yuv[0] / 255.0;
        return ((lineIndex & 1) === 0 ? this.yuv[1] : this.yuv[2]) / 255.0;
      }
      default: return 0.0;
    }
  }

  frequencyForSample(n) {
    const m = Mode(this.modeIndex);
    const ticks = SampleTicks(n);

    // The header.
    const visTicks = kVisMicros * kSampleRate;
    if (ticks < visTicks) {
      this.line = this.segment = this.pixel = -1;
      return visTone(Math.floor(ticks / kSampleRate), m.vis);
    }

    const leadTicks = m.leadInMicros * kSampleRate;
    if (ticks < visTicks + leadTicks) {
      this.line = this.segment = this.pixel = -1;
      return kToneSync;
    }

    // The lines. All integer: line index, then position within the line.
    const lineTicks = m.lineMicros * kSampleRate;
    const rel = ticks - visTicks - leadTicks;
    const lineIndex = Math.floor(rel / lineTicks);
    if (lineIndex >= m.height) {
      // Picture over: the next one starts on this very sample.
      this.startPicture();
      return this.frequencyForSample(0);
    }
    this.line = lineIndex;

    // Live transmit: this line reads the frame that is current as it starts.
    if (this.live && this.line !== this.lastLineCopied) {
      this.lastLineCopied = this.line;
      const rowBytes = this.TxWidth() * 4;
      if (this.liveImage.length === this.latchedImage.length && this.liveImage.length > 0) {
        this.latchedImage.set(this.liveImage.subarray(this.line * rowBytes, (this.line + 1) * rowBytes), this.line * rowBytes);
      }
    }

    let within = rel - lineIndex * lineTicks;
    for (let s = 0; s < m.segmentCount; s += 1) {
      const sg = m.segments[s];
      const segTicks = sg.micros * kSampleRate;
      if (within < segTicks) {
        this.segment = s;
        switch (sg.kind) {
          case Segment.Sync:
          case Segment.Porch:
            this.pixel = -1;
            return sg.tone;
          case Segment.Separator:
            this.pixel = -1;
            if (sg.tone === 0.0) return (this.line & 1) === 0 ? kToneBlack : kToneWhite;
            return sg.tone;
          case Segment.Scan: {
            const px = Math.floor((within * m.width) / segTicks);
            this.pixel = clamp(px, 0, m.width - 1);
            return kToneBlack + kToneSpan * this.pixelValue(this.line, this.pixel, sg.component);
          }
          default:
            break;
        }
      }
      within -= segTicks;
    }

    this.segment = m.segmentCount - 1;
    this.pixel = -1;
    return kToneBlack;
  }

  /** One sample of the analytic tone, into out[0] (re) and out[1] (im). */
  Step(out) {
    if (!this.hasSource) {
      out[0] = 0.0;
      out[1] = 0.0;
      return;
    }

    const hz = this.frequencyForSample(this.sample);
    this.lastHz = hz;

    // Phase-continuous: the phase advances by exactly f / fs per sample and
    // is reduced to [0, 1).
    this.phase += hz / kSampleRateHz;
    this.phase -= Math.floor(this.phase);

    out[0] = Math.cos(kTwoPi * this.phase);
    out[1] = Math.sin(kTwoPi * this.phase);
    this.sample += 1;
  }
}

//---------------------------------------------------------------------------
// Channel.cpp
//---------------------------------------------------------------------------

const kRays = 8;
const kBins = 64;
const kAudioTaps = 64;
const kLinearNyquistHz = 22050.0;
const kLogLowHz = 20.0;
const kLogHighHz = 20000.0;
const kRenormaliseEvery = 4096;
const kDelayMask = 255;

/**
 * Channel::Phasor and Channel::Fader, flattened into typed arrays: the rays of
 * a fader are `re`, `im`, `stepRe`, `stepIm` at [0, kRays).
 */
class Fader {
  constructor() {
    this.re = new Float64Array(kRays).fill(1.0);
    this.im = new Float64Array(kRays);
    this.stepRe = new Float64Array(kRays).fill(1.0);
    this.stepIm = new Float64Array(kRays);
    this.angles = new Float64Array(kRays);
    this.gainRe = 0.0;
    this.gainIm = 0.0;
  }

  Seed(rng, dopplerHz) {
    const rotation = rng.Unit() * kTwoPi;
    for (let n = 0; n < kRays; n += 1) {
      this.angles[n] = rotation + (kTwoPi * (n + 0.5)) / kRays;
      const p = rng.Unit() * kTwoPi;
      this.re[n] = Math.cos(p);
      this.im[n] = Math.sin(p);
    }
    this.SetDoppler(dopplerHz);
  }

  SetDoppler(dopplerHz) {
    for (let n = 0; n < kRays; n += 1) {
      const w = (kTwoPi * (dopplerHz * Math.cos(this.angles[n]))) / kSampleRateHz;
      this.stepRe[n] = Math.cos(w);
      this.stepIm[n] = Math.sin(w);
    }
  }

  /** Into this.gainRe / this.gainIm. */
  Gain() {
    let sr = 0.0;
    let si = 0.0;
    for (let n = 0; n < kRays; n += 1) {
      sr += this.re[n];
      si += this.im[n];
    }
    const k = 1.0 / Math.sqrt(kRays);
    this.gainRe = sr * k;
    this.gainIm = si * k;
  }

  Advance() {
    for (let n = 0; n < kRays; n += 1) {
      const r = this.re[n] * this.stepRe[n] - this.im[n] * this.stepIm[n];
      this.im[n] = this.re[n] * this.stepIm[n] + this.im[n] * this.stepRe[n];
      this.re[n] = r;
    }
  }

  Renormalise() {
    for (let n = 0; n < kRays; n += 1) {
      const mag = Math.sqrt(this.re[n] * this.re[n] + this.im[n] * this.im[n]);
      if (mag > 0.0) {
        this.re[n] /= mag;
        this.im[n] /= mag;
      } else {
        this.re[n] = 1.0;
        this.im[n] = 0.0;
      }
    }
  }
}

export function defaultChannelParams() {
  return {
    snrDb: 25.0,
    fadeDepth: 0.0,
    fadeRateHz: 0.3,
    multipathSeconds: 0.0,
    multipathLevel: 0.0,
    qrmHz: 1900.0,
    qrmLevel: 0.0,
    audioLevel: 0.0,
    binSpacing: 1,
  };
}

export class Channel {
  constructor() {
    this.params = defaultChannelParams();
    this.rng = new Rng();
    this.direct = new Fader();
    this.reflected = new Fader();
    // The QRM phasor.
    this.qrmRe = 1.0; this.qrmIm = 0.0; this.qrmStepRe = 1.0; this.qrmStepIm = 0.0;
    this.delayRe = new Float64Array(kDelayMask + 1);
    this.delayIm = new Float64Array(kDelayMask + 1);
    this.delayWrite = 0;
    this.delaySamples = 0;
    this.bins = new Float32Array(kBins);
    this.audioTaps = new Float64Array(kAudioTaps);
    this.audioNoise = new Float64Array(kAudioTaps);
    this.audioWrite = 0;
    this.audioActive = false;
    this.audioRng = new Rng();
    this.noiseSigma = 0.0;
    this.sinceRenormalise = 0;
    this.binsInitialised = false;

    this.Reset();
    this.SetParams(this.params);
    // SetParams only retunes what CHANGED, and nothing has; without this the
    // carrier sat at 0 Hz (AGENTS.md, defect 2).
    this.qrmSetRateHz(this.params.qrmHz);
  }

  qrmSetRateHz(hz) {
    const w = (kTwoPi * hz) / kSampleRateHz;
    this.qrmStepRe = Math.cos(w);
    this.qrmStepIm = Math.sin(w);
  }

  Reset() {
    this.rng.Seed(0x5354565F484620n); // "STV_HF "
    this.direct.Seed(this.rng, this.params.fadeRateHz);
    this.reflected.Seed(this.rng, this.params.fadeRateHz);
    this.qrmRe = 1.0;
    this.qrmIm = 0.0;
    this.delayRe.fill(0.0);
    this.delayIm.fill(0.0);
    this.delayWrite = 0;
    this.audioRng.Seed(0x415544494F514D00n); // "AUDIOQM"
    this.audioNoise.fill(0.0);
    this.audioWrite = 0;
    this.sinceRenormalise = 0;
  }

  SetParams(p) {
    const rateChanged = p.fadeRateHz !== this.params.fadeRateHz;
    const qrmChanged = p.qrmHz !== this.params.qrmHz;
    const binsChanged = p.binSpacing !== this.params.binSpacing;
    this.params = { ...p };

    if (rateChanged) {
      this.direct.SetDoppler(this.params.fadeRateHz);
      this.reflected.SetDoppler(this.params.fadeRateHz);
    }
    if (qrmChanged) this.qrmSetRateHz(this.params.qrmHz);

    this.delaySamples = clamp(lround(this.params.multipathSeconds * kSampleRateHz), 0, kDelayMask);
    this.noiseSigma = Channel.NoiseSigma(this.params.snrDb);

    if (binsChanged || !this.binsInitialised) {
      this.binsInitialised = true;
      this.designAudioFilter();
    }
  }

  /** SNR in a 3 kHz bandwidth against a unit-amplitude tone. */
  static NoiseSigma(snrDb) {
    const kSnrBandwidthHz = 3000.0;
    const noiseShare = kSnrBandwidthHz / (kSampleRateHz / 2.0);
    const noisePower = 0.5 / noiseShare / Math.pow(10.0, snrDb / 10.0);
    return Math.sqrt(noisePower);
  }

  static BinRangeHz(spacing, i) {
    if (spacing === 0) {
      const width = kLinearNyquistHz / kBins;
      return [i * width, (i + 1) * width];
    }
    const ratio = kLogHighHz / kLogLowHz;
    return [kLogLowHz * Math.pow(ratio, i / kBins), kLogLowHz * Math.pow(ratio, (i + 1) / kBins)];
  }

  /**
   * The host's bins. **The page never calls this with anything but silence**:
   * there is no FFT in a browser tab to hand it. It is ported so the chain is
   * the whole chain; with every bin at zero, `audioActive` stays false and the
   * interference filter never runs.
   */
  SetAudioBins(values, count) {
    let changed = false;
    for (let i = 0; i < kBins; i += 1) {
      const v = Math.fround(values != null && i < count ? Math.max(0.0, values[i]) : 0.0);
      changed = changed || v !== this.bins[i];
      this.bins[i] = v;
    }
    if (changed) this.designAudioFilter();
  }

  designAudioFilter() {
    const L = kAudioTaps;
    const cell = kSampleRateHz / L;
    const nyquistHz = kSampleRateHz / 2.0;
    const amplitude = new Float64Array(L / 2 + 1);
    this.audioActive = false;
    for (let k = 0; k <= L / 2; k += 1) {
      const lo = Math.max(0.0, k * cell - cell / 2.0);
      const hi = Math.min(nyquistHz, k * cell + cell / 2.0);
      let power = 0.0;
      for (let i = 0; i < kBins; i += 1) {
        if (this.bins[i] <= 0.0) continue;
        const [bl, bh] = Channel.BinRangeHz(this.params.binSpacing, i);
        const overlap = Math.min(hi, bh) - Math.max(lo, bl);
        if (overlap > 0.0) power += this.bins[i] * this.bins[i] * overlap;
      }
      amplitude[k] = Math.sqrt(power / (hi - lo));
      this.audioActive = this.audioActive || amplitude[k] > 0.0;
    }

    for (let n = 0; n < L; n += 1) {
      const m = n - L / 2;
      let h = amplitude[0] + amplitude[L / 2] * ((m & 1) ? -1.0 : 1.0);
      for (let k = 1; k < L / 2; k += 1) h += 2.0 * amplitude[k] * Math.cos((kTwoPi * k * m) / L);
      this.audioTaps[n] = (Math.sqrt(0.5) * h) / L;
    }
  }

  /** One sample: the transmitter's analytic tone in, the real audio out. */
  Step(re, im) {
    const d = this.params.fadeDepth;

    // The direct path, faded. (DirectGain)
    this.direct.Gain();
    const gr = (1.0 - d) + d * this.direct.gainRe;
    const gi = d * this.direct.gainIm;
    let outRe = gr * re - gi * im;

    // The second path: the tone as it was `delaySamples` ago, through its own
    // fade, at its own level. The delay line always runs.
    this.delayRe[this.delayWrite] = re;
    this.delayIm[this.delayWrite] = im;
    if (this.params.multipathLevel > 0.0 && this.delaySamples > 0) {
      const readAt = (this.delayWrite - this.delaySamples) & kDelayMask;
      const dr = this.delayRe[readAt];
      const di = this.delayIm[readAt];
      this.reflected.Gain();
      const hr = (1.0 - d) + d * this.reflected.gainRe;
      const hi = d * this.reflected.gainIm;
      outRe += this.params.multipathLevel * (hr * dr - hi * di);
    }
    this.delayWrite = (this.delayWrite + 1) & kDelayMask;

    // An interfering carrier.
    if (this.params.qrmLevel > 0.0) outRe += this.params.qrmLevel * this.qrmRe;

    this.direct.Advance();
    if (this.params.multipathLevel > 0.0) this.reflected.Advance();
    {
      const r = this.qrmRe * this.qrmStepRe - this.qrmIm * this.qrmStepIm;
      this.qrmIm = this.qrmRe * this.qrmStepIm + this.qrmIm * this.qrmStepRe;
      this.qrmRe = r;
    }

    this.sinceRenormalise += 1;
    if (this.sinceRenormalise >= kRenormaliseEvery) {
      this.sinceRenormalise = 0;
      this.direct.Renormalise();
      this.reflected.Renormalise();
      const mag = Math.sqrt(this.qrmRe * this.qrmRe + this.qrmIm * this.qrmIm);
      if (mag > 0.0) {
        this.qrmRe /= mag;
        this.qrmIm /= mag;
      } else {
        this.qrmRe = 1.0;
        this.qrmIm = 0.0;
      }
    }

    // The audio a receiver hears is the real part, and the noise is added to
    // that. (The C++ computes the imaginary part of the output and discards
    // it; it is not computed here.)
    let out = outRe + this.noiseSigma * this.rng.Gaussian();

    // The host's spectrum, as noise it shapes. Never active on this page.
    if (this.audioActive && this.params.audioLevel > 0.0) {
      this.audioNoise[this.audioWrite] = this.audioRng.Gaussian();
      let y = 0.0;
      for (let k = 0; k < kAudioTaps; k += 1) y += this.audioTaps[k] * this.audioNoise[(this.audioWrite - k) & (kAudioTaps - 1)];
      this.audioWrite = (this.audioWrite + 1) & (kAudioTaps - 1);
      out += this.params.audioLevel * y;
    }
    return out;
  }
}

//---------------------------------------------------------------------------
// Receiver.cpp
//---------------------------------------------------------------------------

export const kFirTaps = 63;
const kFirLowHz = 1000.0;
const kFirHighHz = 2500.0;
export const kGroupDelay = (kFirTaps - 1) / 2;
const kLowThresholdHz = 1350.0;
const kLeaderToleranceHz = 120.0;
const kLeaderMinMicros = 150000;

function samplesFor(micros) {
  return (micros * kSampleRateHz) / 1e6;
}

const kBreakMin = samplesFor(5000);
const kBreakMax = samplesFor(20000);
const kOtherMax = samplesFor(20000);

const Vis = { WaitLeader: 0, InLeader: 1, InBreak: 2, SecondLeader: 3, StartBit: 4, Bits: 5 };

function TauSamples(bandwidthHz) {
  return kSampleRateHz / (kTwoPi * Math.max(bandwidthHz, 1.0));
}

function CrossingLagSamples(bandwidthHz, fromHz, toHz, thresholdHz) {
  const span = Math.abs(fromHz - toHz);
  const left = Math.abs(thresholdHz - toHz);
  if (span <= 0.0 || left <= 0.0 || left >= span) return 0.0;
  return TauSamples(bandwidthHz) * Math.log(span / left);
}

export class Receiver {
  constructor() {
    this.params = { bandwidthHz: 1200.0, lineSync: false, clockErrorPpm: 0.0, mode: 0 };

    // the front end
    this.firRe = new Float64Array(kFirTaps);
    this.firIm = new Float64Array(kFirTaps);
    this.ring = new Float64Array(64);
    this.ringWrite = 0;
    this.zPrevRe = 0.0;
    this.zPrevIm = 0.0;
    this.rawHz = kToneBlack;
    this.lp = kToneBlack;
    this.lpPrev = kToneBlack;
    this.lpCoef = 0.0;
    this.sampleIndex = 0;

    this.rxStep = 1.0;

    // VIS
    this.vis = Vis.WaitLeader;
    this.leaderRun = 0;
    this.lowRun = 0;
    this.otherRun = 0;
    this.visStartSample = 0;
    this.bitSum = new Float64Array(kVisBitSlots);
    this.bitCount = new Int32Array(kVisBitSlots);
    this.startPending = false;
    this.startAtSample = 0;
    this.pendingMode = 0;

    // sync
    this.syncLowRun = 0;
    this.wasLow = false;

    // the picture and the line
    this.width = 0;
    this.height = 0;
    this.pictureMode = 0;
    this.planes = [new Float32Array(0), new Float32Array(0), new Float32Array(0)];
    this.inPicture = false;
    this.lineIndex = 0;
    this.lineTime = 0.0;
    this.lineTimePrev = 0.0;
    this.lineRx = 0.0;
    this.segStartRx = new Float64Array(16);
    this.segEndRx = new Float64Array(16);
    this.segCount = 0;
    this.segIndex = 0;
    this.pixelCursor = 0;
    this.syncEndRx = 0.0;
    this.syncSamples = 0.0;

    this.picturesStarted = 0;
    this.visDecoded = 0;
    this.visRejected = 0;
    this.lastVisCode = -1;
    this.syncEvents = 0;
    this.lineJumps = 0;
    this.forcedStarts = 0;

    this.rgbScratch = new Float64Array(3);

    this.buildFir();
    this.SetParams(this.params);
    this.configureMode(this.params.mode < 0 ? kMartinM1 : this.params.mode);
    this.ClearPicture();
    this.Reset();
  }

  Width() { return this.width; }
  Height() { return this.height; }
  PictureMode() { return this.pictureMode; }
  InPicture() { return this.inPicture; }
  Line() { return this.lineIndex; }
  PicturesStarted() { return this.picturesStarted; }

  buildFir() {
    const halfBand = (kFirHighHz - kFirLowHz) / 2.0;
    const centre = (kFirHighHz + kFirLowHz) / 2.0;
    const prototype = new Float64Array(kFirTaps);
    let sum = 0.0;
    for (let n = 0; n < kFirTaps; n += 1) {
      const m = n - kGroupDelay;
      const x = (2.0 * halfBand * m) / kSampleRateHz;
      const sinc = m === 0 ? 1.0 : Math.sin(kPi * x) / (kPi * x);
      const w = 0.54 - 0.46 * Math.cos((kTwoPi * n) / (kFirTaps - 1));
      prototype[n] = sinc * w;
      sum += prototype[n];
    }
    for (let n = 0; n < kFirTaps; n += 1) {
      const m = n - kGroupDelay;
      const arg = (kTwoPi * centre * m) / kSampleRateHz;
      const h = prototype[n] / sum;
      this.firRe[n] = h * Math.cos(arg);
      this.firIm[n] = h * Math.sin(arg);
    }
  }

  SetParams(p) {
    this.params = { ...p };
    this.lpCoef = 1.0 - Math.exp(-1.0 / TauSamples(this.params.bandwidthHz));
    this.rxStep = 1.0 + this.params.clockErrorPpm * 1e-6;

    // A picture in progress keeps its mode. Between pictures the chosen mode
    // applies at once; the old picture is kept (converted if the colour model
    // changed).
    if (!this.inPicture && this.params.mode >= 0 && this.params.mode !== this.pictureMode) {
      this.configureMode(this.params.mode);
    }
  }

  Reset() {
    this.inPicture = false;
    this.vis = Vis.WaitLeader;
    this.leaderRun = this.lowRun = this.otherRun = 0;
    this.startPending = false;
    this.syncLowRun = 0;
    this.wasLow = false;
    this.ring.fill(0);
    this.ringWrite = 0;
    this.zPrevRe = this.zPrevIm = 0.0;
    this.rawHz = this.lp = this.lpPrev = kToneBlack;
    this.rxStep = 1.0 + this.params.clockErrorPpm * 1e-6;
  }

  ClearPicture() {
    const m = Mode(this.pictureMode);
    for (let p = 0; p < 3; p += 1) this.planes[p] = new Float32Array(this.width * this.height);
    if (m.chromaAlternates) {
      this.planes[1].fill(robot.kCOffset / 255.0);
      this.planes[2].fill(robot.kCOffset / 255.0);
    }
  }

  configureMode(modeIndex) {
    const m = Mode(modeIndex);
    const wasRobot = this.width > 0 && Mode(this.pictureMode).chromaAlternates;
    if (m.width !== this.width || m.height !== this.height) {
      // Resize, keeping what overlaps. Rows the old picture did not have are
      // black in ITS colour model, and converted with the rest below.
      const chromaBlack = Math.fround(robot.kCOffset / 255.0);
      const fill = [0.0, wasRobot ? chromaBlack : 0.0, wasRobot ? chromaBlack : 0.0];
      for (let p = 0; p < 3; p += 1) {
        const fresh = new Float32Array(m.width * m.height).fill(fill[p]);
        const rows = Math.min(this.height, m.height);
        const cols = Math.min(this.width, m.width);
        for (let y = 0; y < rows; y += 1) {
          for (let x = 0; x < cols; x += 1) fresh[y * m.width + x] = this.planes[p][y * this.width + x];
        }
        this.planes[p] = fresh;
      }
      this.width = m.width;
      this.height = m.height;
    }

    // The planes are R, G, B in Martin and Scottie and Y, R-Y, B-Y in Robot 36;
    // when the colour model changes the old picture is converted.
    const nowRobot = m.chromaAlternates;
    if (this.width > 0 && wasRobot !== nowRobot) {
      const n = this.width * this.height;
      const out = this.rgbScratch;
      const [P0, P1, P2] = this.planes;
      for (let i = 0; i < n; i += 1) {
        const a = P0[i] * 255.0;
        const b = P1[i] * 255.0;
        const c = P2[i] * 255.0;
        if (nowRobot) robotEncode(clamp(a, 0.0, 255.0), clamp(b, 0.0, 255.0), clamp(c, 0.0, 255.0), out);
        else robotDecode(a, b, c, out);
        P0[i] = out[0] / 255.0;
        P1[i] = out[1] / 255.0;
        P2[i] = out[2] / 255.0;
      }
    }
    this.pictureMode = modeIndex;

    this.lineRx = samplesFor(m.lineMicros);
    this.segCount = m.segmentCount;
    let at = 0.0;
    for (let s = 0; s < this.segCount; s += 1) {
      this.segStartRx[s] = at;
      at += samplesFor(m.segments[s].micros);
      this.segEndRx[s] = at;
    }
    this.syncEndRx = this.segEndRx[m.syncIndex];
    this.syncSamples = samplesFor(m.segments[m.syncIndex].micros);
  }

  beginPicture(modeIndex, line, lineTimeSamples) {
    this.configureMode(modeIndex);
    this.inPicture = true;
    this.lineIndex = clamp(line, 0, this.height - 1);
    this.lineTime = lineTimeSamples;
    this.lineTimePrev = this.lineTime;
    this.seekCursors();
    this.syncLowRun = 0;
    this.wasLow = false;
    this.picturesStarted += 1;
  }

  endPicture() {
    this.inPicture = false;
  }

  ForceStart(modeIndex, line, lineTimeSamples) {
    this.startPending = false;
    this.forcedStarts += 1;
    this.beginPicture(modeIndex, line, lineTimeSamples);
  }

  /** One real audio sample. */
  Step(x) {
    // The analytic signal.
    const ring = this.ring;
    const firRe = this.firRe;
    const firIm = this.firIm;
    const w = this.ringWrite;
    ring[w] = x;
    let zRe = 0.0;
    let zIm = 0.0;
    for (let k = 0; k < kFirTaps; k += 1) {
      const v = ring[(w - k) & 63];
      zRe += firRe[k] * v;
      zIm += firIm[k] * v;
    }
    this.ringWrite = (w + 1) & 63;

    // The discriminator: z * conj( zPrev ).
    const cr = zRe * this.zPrevRe + zIm * this.zPrevIm;
    const ci = zIm * this.zPrevRe - zRe * this.zPrevIm;
    if (cr * cr + ci * ci > 1e-24) this.rawHz = (kSampleRateHz / kTwoPi) * Math.atan2(ci, cr);
    this.zPrevRe = zRe;
    this.zPrevIm = zIm;

    // The lowpass.
    this.lpPrev = this.lp;
    this.lp += (this.rawHz - this.lp) * this.lpCoef;

    this.sampleIndex += 1;

    const isLow = this.lp < kLowThresholdHz;
    const isLeader = Math.abs(this.lp - kToneLeader) < kLeaderToleranceHz;

    this.trackVis(isLow, isLeader);

    if (this.startPending && this.sampleIndex >= this.startAtSample) {
      this.startPending = false;
      this.beginPicture(this.pendingMode, 0, (this.sampleIndex - this.startAtSample) * this.rxStep);
    }

    if (!this.inPicture) return;

    this.lineTimePrev = this.lineTime;
    this.lineTime += this.rxStep;

    this.trackSync(isLow);
    if (!this.inPicture) return;

    // Pixels whose centres this sample has reached.
    const m = Mode(this.pictureMode);
    while (this.segIndex < this.segCount) {
      const sg = m.segments[this.segIndex];
      if (sg.kind === Segment.Scan) {
        const scanRx = this.segEndRx[this.segIndex] - this.segStartRx[this.segIndex];
        while (this.pixelCursor < this.width) {
          const centre = this.segStartRx[this.segIndex] + ((this.pixelCursor + 0.5) * scanRx) / this.width;
          if (centre > this.lineTime) break;
          const hz = this.interpolatedAt(centre);
          this.writePixel(this.lineIndex, this.pixelCursor, sg.component, (hz - kToneBlack) / kToneSpan);
          this.pixelCursor += 1;
        }
      }
      if (this.lineTime >= this.segEndRx[this.segIndex]) {
        this.segIndex += 1;
        this.pixelCursor = 0;
      } else break;
    }

    if (this.lineTime >= this.lineRx) this.advanceLine();
  }

  interpolatedAt(rxTime) {
    const span = this.lineTime - this.lineTimePrev;
    if (span <= 0.0) return this.lp;
    const f = clamp((rxTime - this.lineTimePrev) / span, 0.0, 1.0);
    return this.lpPrev + (this.lp - this.lpPrev) * f;
  }

  advanceLine() {
    this.lineTime -= this.lineRx;
    this.lineTimePrev -= this.lineRx;
    this.segIndex = 0;
    this.pixelCursor = 0;
    this.lineIndex += 1;
    if (this.lineIndex >= this.height) this.endPicture();
  }

  seekCursors() {
    const m = Mode(this.pictureMode);
    this.segIndex = 0;
    while (this.segIndex < this.segCount && this.lineTime >= this.segEndRx[this.segIndex]) this.segIndex += 1;
    this.pixelCursor = 0;
    if (this.segIndex < this.segCount && m.segments[this.segIndex].kind === Segment.Scan) {
      const scanRx = this.segEndRx[this.segIndex] - this.segStartRx[this.segIndex];
      while (this.pixelCursor < this.width
        && this.segStartRx[this.segIndex] + ((this.pixelCursor + 0.5) * scanRx) / this.width <= this.lineTime) {
        this.pixelCursor += 1;
      }
    }
  }

  writePixel(line, px, c, value) {
    // Unclamped: Composite() clamps for display. The Float32Array store is the
    // C++'s static_cast< float >. The C++'s `put` lambda is put() below rather
    // than a closure, so a pixel allocates nothing.
    switch (c) {
      case Component.Red: this.put(0, line, px, value); break;
      case Component.Green: this.put(1, line, px, value); break;
      case Component.Blue: this.put(2, line, px, value); break;
      case Component.Luma: this.put(0, line, px, value); break;
      case Component.ChromaAlternating:
        // R-Y on the even line covers it and the odd line after it; B-Y on
        // the odd line covers it and the even line before it.
        if ((line & 1) === 0) {
          this.put(1, line, px, value);
          this.put(1, line + 1, px, value);
        } else {
          this.put(2, line - 1, px, value);
          this.put(2, line, px, value);
        }
        break;
      default: break;
    }
  }

  put(plane, row, px, value) {
    if (row >= 0 && row < this.height) this.planes[plane][row * this.width + px] = value;
  }

  trackSync(isLow) {
    if (isLow) {
      this.syncLowRun += 1;
      this.wasLow = true;
      return;
    }
    if (!this.wasLow) return;
    const run = this.syncLowRun;
    this.syncLowRun = 0;
    this.wasLow = false;

    if (run < 0.6 * this.syncSamples || run > 1.6 * this.syncSamples) return;
    this.syncEvents += 1;
    if (!this.params.lineSync) return;

    // The pulse ended `lag` samples ago. The FIR's group delay is NOT taken
    // off: it cancels (AGENTS.md, "The FIR's group delay cancels").
    const lag = CrossingLagSamples(this.params.bandwidthHz, kToneSync, kToneBlack, kLowThresholdHz) * this.rxStep;
    const eventRx = this.lineTime - lag;
    const delta = eventRx - this.syncEndRx;
    const k = lround(delta / this.lineRx);
    const newLine = this.lineIndex + k;
    if (newLine < 0) return; // the lead-in sync before line 0, which anchors nothing

    const m = Mode(this.pictureMode);
    let pixelRx = this.lineRx;
    for (let s = 0; s < this.segCount; s += 1) {
      if (m.segments[s].kind === Segment.Scan) {
        pixelRx = (this.segEndRx[s] - this.segStartRx[s]) / this.width;
        break;
      }
    }
    if (Math.abs(delta - k * this.lineRx) > pixelRx) this.lineJumps += 1;

    this.lineIndex = newLine;
    if (this.lineIndex >= this.height) {
      this.endPicture();
      return;
    }
    this.lineTime = this.syncEndRx + lag;
    this.lineTimePrev = this.lineTime;
    this.seekCursors();
  }

  trackVis(isLow, isLeader) {
    const leaderMin = samplesFor(kLeaderMinMicros);

    switch (this.vis) {
      case Vis.WaitLeader:
        this.leaderRun = isLeader ? this.leaderRun + 1 : 0;
        if (this.leaderRun >= leaderMin) {
          this.vis = Vis.InLeader;
          this.otherRun = 0;
        }
        break;

      case Vis.InLeader:
        if (isLeader) this.otherRun = 0;
        else if (isLow) {
          this.vis = Vis.InBreak;
          this.lowRun = 1;
        } else if (++this.otherRun > kOtherMax) {
          this.vis = Vis.WaitLeader;
          this.leaderRun = 0;
        }
        break;

      case Vis.InBreak:
        if (isLow) {
          this.lowRun += 1;
          break;
        }
        if (this.lowRun >= kBreakMin && this.lowRun <= kBreakMax) {
          this.vis = Vis.SecondLeader;
          this.leaderRun = isLeader ? 1 : 0;
          this.otherRun = 0;
        } else {
          this.vis = Vis.WaitLeader;
          this.leaderRun = isLeader ? 1 : 0;
        }
        break;

      case Vis.SecondLeader:
        if (isLeader) {
          this.leaderRun += 1;
          this.otherRun = 0;
        } else if (isLow) {
          if (this.leaderRun >= leaderMin) {
            this.vis = Vis.Bits;
            this.visStartSample = this.sampleIndex;
            this.bitSum.fill(0.0);
            this.bitCount.fill(0);
          } else {
            this.vis = Vis.WaitLeader;
            this.leaderRun = 0;
          }
        } else if (++this.otherRun > kOtherMax) {
          this.vis = Vis.WaitLeader;
          this.leaderRun = 0;
        }
        break;

      case Vis.StartBit:
        this.vis = Vis.Bits;
        break;

      case Vis.Bits: {
        const t = this.sampleIndex - this.visStartSample;
        const slotRx = samplesFor(kVisBitMicros);
        const slot = Math.floor(t / slotRx);
        if (slot < kVisBitSlots) {
          const within = t - slot * slotRx;
          if (within >= slotRx / 3.0 && within < (2.0 * slotRx) / 3.0) {
            this.bitSum[slot] += this.lp;
            this.bitCount[slot] += 1;
          }
          break;
        }

        // All ten slots read. Decide.
        this.vis = Vis.WaitLeader;
        this.leaderRun = 0;

        const mean = (s) => (this.bitCount[s] > 0 ? this.bitSum[s] / this.bitCount[s] : 1e9);
        const startOk = mean(0) < kLowThresholdHz;
        const stopOk = mean(kVisBitSlots - 1) < kLowThresholdHz;
        let code = 0;
        for (let b = 0; b < kVisDataBits; b += 1) if (mean(1 + b) < (kToneBit1 + kToneBit0) / 2.0) code |= 1 << b;
        const parity = mean(1 + kVisDataBits) < (kToneBit1 + kToneBit0) / 2.0 ? 1 : 0;
        const parityOk = parity === VisParity(code);
        const decoded = ModeForVis(code);
        const modeOk = this.params.mode >= 0 || decoded >= 0;

        if (startOk && stopOk && parityOk && modeOk) {
          this.visDecoded += 1;
          this.lastVisCode = code;
          this.pendingMode = this.params.mode >= 0 ? this.params.mode : decoded;

          const lag = CrossingLagSamples(this.params.bandwidthHz, kToneLeader, kToneSync, kLowThresholdHz);
          const trueStart = this.visStartSample - lag;
          const m = Mode(this.pendingMode);
          this.startAtSample = lround(trueStart + samplesFor(kVisBitSlots * kVisBitMicros + m.leadInMicros));
          this.startPending = true;
        } else {
          this.visRejected += 1;
        }
        break;
      }
      default:
        break;
    }
  }

  /** RGB8, row 0 at the top, ready to upload, into `rgb` (resized if needed). */
  Composite(rgb) {
    const m = Mode(this.pictureMode);
    const n = this.width * this.height;
    const out = rgb && rgb.length === n * 3 ? rgb : new Uint8Array(n * 3);
    const [P0, P1, P2] = this.planes;
    const c = this.rgbScratch;
    for (let i = 0; i < n; i += 1) {
      let r;
      let g;
      let b;
      if (m.chromaAlternates) {
        robotDecode(P0[i] * 255.0, P1[i] * 255.0, P2[i] * 255.0, c);
        r = c[0]; g = c[1]; b = c[2];
      } else {
        r = P0[i] * 255.0;
        g = P1[i] * 255.0;
        b = P2[i] * 255.0;
      }
      // lround( clamp ) of a value in 0..255: on non-negative values
      // Math.round is the same half-away-from-zero rounding.
      out[i * 3 + 0] = Math.round(clamp(r, 0.0, 255.0));
      out[i * 3 + 1] = Math.round(clamp(g, 0.0, 255.0));
      out[i * 3 + 2] = Math.round(clamp(b, 0.0, 255.0));
    }
    return out;
  }
}

//---------------------------------------------------------------------------
// Engine.cpp
//---------------------------------------------------------------------------

export function defaultEngineParams() {
  return {
    mode: 0, // 0..2, or -1 for Auto VIS
    speed: 40.0,
    live: false,
    channel: defaultChannelParams(),
    rxBandwidthHz: 1200.0,
    lineSync: false,
    clockErrorPpm: 0.0, // the effective error: Clock Error minus Slant Correct
  };
}

export class Engine {
  constructor() {
    this.params = defaultEngineParams();
    this.tx = new Transmitter();
    this.ch = new Channel();
    this.rx = new Receiver();
    this.carry = 0.0;
    this.samplesRun = 0;
    this.lastAudio = 0.0;
    this.lastTxPictures = 0;
    this.rxPicturesAtTxStart = 0;
    this.tone = new Float64Array(2);

    this.SetParams(this.params);
    this.lastTxPictures = this.tx.PicturesStarted();
    this.rxPicturesAtTxStart = this.rx.PicturesStarted();
  }

  static kMaxBlock = 65536;

  Tx() { return this.tx; }
  Ch() { return this.ch; }
  Rx() { return this.rx; }

  SetParams(p) {
    this.params = p;
    this.tx.SetMode(p.mode);
    this.tx.SetLive(p.live);
    this.ch.SetParams(p.channel);
    this.rx.SetParams({
      bandwidthHz: p.rxBandwidthHz,
      lineSync: p.lineSync,
      clockErrorPpm: p.clockErrorPpm,
      mode: p.mode,
    });
  }

  SetAudioBins(bins, count) {
    this.ch.SetAudioBins(bins, count);
  }

  SetSource(rgba, width, height) {
    this.tx.SetSource(rgba, width, height);
  }

  Restart() {
    this.tx.Restart();
    this.rx.Reset();
    this.lastTxPictures = this.tx.PicturesStarted();
    this.rxPicturesAtTxStart = this.rx.PicturesStarted();
  }

  /** How many samples this frame is worth, with the fractional carry. */
  SamplesForFrame(frameSeconds) {
    const wanted = frameSeconds * this.params.speed * kSampleRateHz + this.carry;
    const whole = Math.floor(wanted);
    this.carry = wanted - whole;
    return clamp(whole, 0, Engine.kMaxBlock);
  }

  static LineStartSample(modeIndex, line) {
    const m = Mode(modeIndex);
    return FirstSampleAtOrAfter(kVisMicros + m.leadInMicros + line * m.lineMicros);
  }

  manualStartIfNeeded() {
    const tx = this.tx;
    const rx = this.rx;
    if (tx.pictures !== this.lastTxPictures) {
      this.lastTxPictures = tx.pictures;
      this.rxPicturesAtTxStart = rx.picturesStarted;
    }

    // The header did not decode and two lines have gone by: press Start, from
    // the station's own clock, once the line's start has come out of the FIR.
    if (rx.inPicture || rx.picturesStarted !== this.rxPicturesAtTxStart) return;
    if (tx.line < 2) return;

    const modeIndex = tx.modeIndex;
    const lineStart = Engine.LineStartSample(modeIndex, tx.line);
    const within = tx.sample - lineStart - kGroupDelay;
    if (within < 0.0) return;
    const rxStep = 1.0 + this.params.clockErrorPpm * 1e-6;
    rx.ForceStart(modeIndex, tx.line, Math.max(0.0, within) * rxStep);
  }

  Run(samples) {
    const tone = this.tone;
    for (let i = 0; i < samples; i += 1) {
      this.tx.Step(tone);
      this.lastAudio = this.ch.Step(tone[0], tone[1]);
      this.rx.Step(this.lastAudio);
      this.samplesRun += 1;

      this.manualStartIfNeeded();
    }
  }
}
