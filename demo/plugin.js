/**
 * Slowscan — browser demo.
 *
 * Slow-scan television over HF. The clip is sent the way an SSTV station sends
 * a still: one audio tone per pixel, 1500 Hz for black to 2300 Hz for white, a
 * line at a time with a 1200 Hz sync between lines, down an HF channel that
 * fades, echoes and hisses, into an FM discriminator that paints each line as
 * it arrives. Nothing is drawn: the slant, the colour fringes, the noise
 * streaks below the FM threshold, the bands of a fade and the stripes a
 * carrier drags through the picture all fall out of the chain.
 *
 * So this plugin is **not a shader**. It is a GPU readback of the clip down to
 * the mode's raster, a CPU signal chain at 11,025 samples a second times Speed,
 * and a GPU compose of the picture the receiver made. The CPU middle is the
 * whole plugin and has to exist here, so it is ported — and the two halves are
 * not equally faithful:
 *
 *   The shaders are the plugin's. `VERTEX`, `READBACK` and `COMPOSE` below are
 *   `kVertex`, `kReadbackFragment` and `kComposeFragment` from
 *   `source/shaders/`, copied across unedited and run in the same two passes
 *   as `Slowscan::ProcessOpenGL`. `demo/tools/check_shaders.py` compares them
 *   character for character and `tools/verify.sh` runs it.
 *
 *   The chain is a port — `sstv.js`, of `source/sstv/` (Rng, Modes,
 *   Transmitter, Channel, Receiver, Engine) — and the parameter mapping below
 *   is a port of `Controls.h` and `Slowscan::resolve()`. **Nothing checks a
 *   port but a reader.** `sstest` checks the C++ and has never heard of this
 *   page. (When the page was written the port was driven side by side with the
 *   C++ chain through four channels, and the pictures agreed byte for byte;
 *   that was a one-off comparison, not a check anything keeps running.)
 *
 * ------------------------------------------------------ what this page runs
 *
 * Per frame, in `ProcessOpenGL`'s order: the controls resolve to the engine's
 * parameters; the clip is box-filtered down onto 320 × 256 (320 × 240 for
 * Robot 36) through the plugin's own Readback shader and read back with
 * `readPixels`; the engine runs as many samples as the frame is worth; the
 * received picture is composited to RGB8, uploaded, and composed onto the
 * output by the plugin's own Compose shader, at 4:3 with the cursor and the mix.
 *
 * **The readback is synchronous here.** The plugin reads into a pair of
 * pixel-pack buffers and maps the one issued the frame before, so its
 * transmitter sees a frame one frame old and its readback never stalls. WebGL2
 * has no mapped buffers, so the page calls `readPixels` into a typed array and
 * hands the transmitter this frame. At 320 × 256 × 4 bytes it is a small stall,
 * and against a line that takes a third of a second even at 120x the one-frame
 * difference does not show.
 *
 * **At the top Speeds the page does what the plugin does, and nothing else.**
 * The frame's duration is clamped to 1/240 … 1/24 s, and the engine runs every
 * sample that duration is worth — 22,050 a frame at 60 fps and 120x, 55,125 at
 * most. There is no further cap. On a machine that cannot run 1.32 M samples a
 * second the frame rate falls until the clamp holds, and past that the signal
 * runs slower than Speed asks; the line under the canvas says so, with the
 * rate actually achieved.
 *
 * ------------------------------------------------------- what is missing
 *
 * **No audio.** The plugin reads Resolume's 64 FFT bins through a buffer
 * parameter (`Audio`) and puts them on the air as noise they shape. A browser
 * tab has no host FFT to read, so `Audio`, `Audio QRM` and `Bin Spacing` are
 * not on the panel — a slider there would do nothing. The interference path is
 * ported all the same and is fed silence, so it never runs.
 *
 * **Restart is the transport's.** The plugin's `Restart` is an FF_TYPE_EVENT,
 * which the kit has no control for. The transport's Restart button winds the
 * page's clock back to zero, and the page takes that as the event: the engine
 * restarts exactly as `SetFloatParameter( SS_RESTART, 1 )` makes it.
 *
 * **The host clock is seconds.** The plugin votes on whether its host's clock
 * is seconds or milliseconds (Resolume's is milliseconds). The page's clock is
 * seconds, so there is nothing to vote on and that code is not ported.
 *
 * **The About block is absent**, as on every page in this suite.
 *
 * And what every page in this suite is not: this is the plugin's shaders and a
 * port of its C++, not the plugin. No Resolume, no composition, no FFGL, and
 * GLSL ES 3.00 in a browser rather than desktop GL 4.1 core.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';
import { Engine, Mode } from './sstv.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/shaders/. Do not edit here.
//
// check_shaders.py compares each of these with its C++ raw string, character
// for character, and fails tools/verify.sh on any difference.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core
uniform vec2 MaxUV;

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV * MaxUV;
}
`;

const READBACK = `#version 410 core
uniform sampler2D InputTexture;
uniform vec2 MaxUV;
uniform vec2 InputSize;
uniform vec2 TargetSize;

in vec2 uv;

out vec4 fragColor;

void main()
{
	vec2 flipped = vec2( uv.x, MaxUV.y - uv.y );
	vec2 ratio = InputSize / max( TargetSize, vec2( 1.0 ) );

	// One tap per source texel covered, capped so a 4K composition costs a
	// bounded amount. 8 taps each way is a 2560x2048 source onto 320x256.
	ivec2 taps = ivec2( clamp( ceil( ratio ), vec2( 1.0 ), vec2( 8.0 ) ) );
	vec2 texel = MaxUV / max( InputSize, vec2( 1.0 ) );

	vec4 sum = vec4( 0.0 );
	for( int y = 0; y < taps.y; ++y )
	{
		for( int x = 0; x < taps.x; ++x )
		{
			vec2 f = ( vec2( x, y ) + 0.5 ) / vec2( taps ) - 0.5;
			sum += texture( InputTexture, flipped + f * ratio * texel );
		}
	}

	vec4 color = sum / float( taps.x * taps.y );
	if( color.a > 0.0 )
		color.rgb /= color.a;

	fragColor = vec4( color.rgb, 1.0 );
}
`;

const COMPOSE = `#version 410 core
uniform sampler2D InputTexture;
uniform sampler2D PictureTexture;

uniform vec2 InputMaxUV;
uniform vec2 RectOrigin;//bottom-left of the picture, in output uv
uniform vec2 RectSize;
uniform ivec2 PictureSize;
uniform int CursorRow;//picture row from the top to mark, or -1
uniform float RowsPerPixel;//picture rows one output pixel spans
uniform vec3 CursorColour;
uniform float Mix;

in vec2 uv;

out vec4 fragColor;

void main()
{
	vec4 clip = texture( InputTexture, uv * InputMaxUV );

	vec2 inner = ( uv - RectOrigin ) / max( RectSize, vec2( 1e-6 ) );

	vec4 pic;
	if( inner.x < 0.0 || inner.x >= 1.0 || inner.y < 0.0 || inner.y >= 1.0 )
	{
		pic = vec4( 0.0 );
	}
	else
	{
		int px = clamp( int( inner.x * float( PictureSize.x ) ), 0, PictureSize.x - 1 );
		float rowY = ( 1.0 - inner.y ) * float( PictureSize.y );
		int py = clamp( int( rowY ), 0, PictureSize.y - 1 );
		vec3 rgb = texelFetch( PictureTexture, ivec2( px, py ), 0 ).rgb;
		float cursorCentre = float( CursorRow ) + 0.5;
		float halfSpan = 0.5 * RowsPerPixel;
		if( CursorRow >= 0 && ( py == CursorRow || ( cursorCentre > rowY - halfSpan && cursorCentre <= rowY + halfSpan ) ) )
			rgb = CursorColour;
		pic = vec4( rgb, 1.0 );
	}

	fragColor = mix( clip, pic, Mix );
}
`;

//---------------------------------------------------------------------------
// Controls.h, ported. Every slider is a float in the plugin (`float params[]`),
// so the host's value is rounded to float before it is converted, as the C++
// does by storing it.
//---------------------------------------------------------------------------

const f32 = Math.fround;
const clamp01 = (v) => { const f = f32(v); return f < 0 ? 0 : f > 1 ? 1 : f; };

const kSpeedMax = 120.0;
const Speed = (v) => Math.pow(kSpeedMax, clamp01(v));

const kSnrMinDb = -10.0;
const kSnrMaxDb = 40.0;
const SnrDb = (v) => kSnrMinDb + (kSnrMaxDb - kSnrMinDb) * clamp01(v);

const FadeDepth = (v) => clamp01(v);

const kFadeRateMinHz = 0.02;
const kFadeRateMaxHz = 5.0;
const FadeRateHz = (v) => kFadeRateMinHz * Math.pow(kFadeRateMaxHz / kFadeRateMinHz, clamp01(v));

const kMultipathMaxMs = 8.0;
const MultipathSeconds = (v) => kMultipathMaxMs * 1e-3 * clamp01(v);
const MultipathLevel = (v) => clamp01(v);

const kQrmMinHz = 300.0;
const kQrmMaxHz = 3000.0;
const QrmHz = (v) => kQrmMinHz * Math.pow(kQrmMaxHz / kQrmMinHz, clamp01(v));
const QrmLevel = (v) => clamp01(v);

const kRxBandwidthMinHz = 100.0;
const kRxBandwidthMaxHz = 3000.0;
const RxBandwidthHz = (v) => kRxBandwidthMinHz * Math.pow(kRxBandwidthMaxHz / kRxBandwidthMinHz, clamp01(v));

const kPpmRange = 300.0;
const Ppm = (v) => (clamp01(v) - 0.5) * 2.0 * kPpmRange;

// The option indices, as Slowscan.h names them.
const kModeAutoVis = 3;
const kTransmitLive = 1;
const kSyncLineSync = 1;
const kAspectFill = 1;

//---------------------------------------------------------------------------
// Slowscan.cpp, the parts that are not GL.
//---------------------------------------------------------------------------

/** std::lround: half away from zero. */
const lround = (x) => (x < 0 ? -Math.round(-x) : Math.round(x));

/// A frame shorter than this is a duplicate call; one longer is a stall, a
/// scrub or a dropped frame. Both are clamped rather than believed.
const kMinFrameSeconds = 1.0 / 240.0;
const kMaxFrameSeconds = 1.0 / 24.0;
const kFirstFrameSeconds = 1.0 / 60.0;

/// Every mode is shown 4:3, as every decoder shows them.
const kPictureAspect = f32(4.0 / 3.0);

/// The cursor's green, exact multiples of 1/255.
const kCursorR = 51.0 / 255.0;
const kCursorG = 1.0;
const kCursorB = 102.0 / 255.0;

/** Slowscan::resolve(): the controls, as the engine's parameters. */
function resolve(p, audioLevel, binSpacing) {
  const modeIndex = lround(p('mode'));
  return {
    mode: modeIndex === kModeAutoVis ? -1 : Math.min(Math.max(modeIndex, 0), 2),
    speed: Speed(p('speed')),
    live: lround(p('transmit')) === kTransmitLive,
    channel: {
      snrDb: SnrDb(p('snr')),
      fadeDepth: FadeDepth(p('fadeDepth')),
      fadeRateHz: FadeRateHz(p('fadeRate')),
      multipathSeconds: MultipathSeconds(p('multipath')),
      multipathLevel: MultipathLevel(p('multipathLevel')),
      qrmHz: QrmHz(p('qrmFreq')),
      qrmLevel: QrmLevel(p('qrmLevel')),
      // Audio QRM and Bin Spacing are not on this page (no host FFT); these
      // are the plugin's constructor defaults, and with no bins the path they
      // drive is silent whatever they say.
      audioLevel,
      binSpacing,
    },
    rxBandwidthHz: RxBandwidthHz(p('rxBandwidth')),
    lineSync: lround(p('sync')) === kSyncLineSync,
    // Slant Correct subtracts a set ppm from the receiver's clock.
    clockErrorPpm: Ppm(p('clockError')) - Ppm(p('slantCorrect')),
  };
}

/** Slowscan::pictureRect(), in float as the C++ is: x, y from the TOP. */
function pictureRect(fill, outputW, outputH) {
  const ow = f32(Math.max(1, outputW));
  const oh = f32(Math.max(1, outputH));
  if (fill) return { x: 0, y: 0, w: ow, h: oh };
  let w;
  let h;
  if (f32(ow / oh) > kPictureAspect) {
    h = oh;
    w = f32(oh * kPictureAspect);
  } else {
    w = ow;
    h = f32(ow / kPictureAspect);
  }
  return { x: f32((ow - w) * 0.5), y: f32((oh - h) * 0.5), w, h };
}

//---------------------------------------------------------------------------
// What the line under the canvas reports. Filled by the renderer, read by a
// timer.
//---------------------------------------------------------------------------
const telemetry = {
  txMode: '',
  rxMode: '',
  inPicture: false,
  line: 0,
  height: 0,
  vis: -1,
  forced: 0,
  decoded: 0,
  samplesPerSecond: 0,
  speedAsked: 0,
  samplesFrame: 0,
  cpuMillis: 0,
};

function createRenderer(gl, quad) {
  const readbackShader = new Program(gl, VERTEX, READBACK, 'readback');
  const composeShader = new Program(gl, VERTEX, COMPOSE, 'compose');

  const readbackBuffer = new PassBuffer(gl, { filter: 'nearest' });
  let readback = new Uint8Array(0);

  let pictureTexture = null;
  let pictureWidth = 0;
  let pictureHeight = 0;
  let pictureRgb = null;

  const engine = new Engine();
  // The plugin's `float audioBins[ 64 ] = {}`, which on this page nothing ever
  // fills. Handed over every frame as the plugin does, and always silence.
  const audioBins = new Float32Array(64);

  let lastSeconds = -1;
  // For the stats line: samples run against seconds of the page's own clock,
  // over about a second. The page's clock and not the clamped frame duration,
  // or a browser falling behind would never show: the clamp is exactly what
  // hides it.
  const tally = { samples: 0, seconds: 0 };

  function ensurePictureTexture(w, h) {
    if (pictureTexture && pictureWidth === w && pictureHeight === h) return;
    if (pictureTexture) gl.deleteTexture(pictureTexture);
    pictureTexture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, pictureTexture);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGB8, w, h, 0, gl.RGB, gl.UNSIGNED_BYTE, null);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.bindTexture(gl.TEXTURE_2D, null);
    pictureWidth = w;
    pictureHeight = h;
  }

  /** Slowscan::frameSecondsFor(), on the page's clock. */
  function frameSecondsFor(seconds) {
    let frameSeconds = kFirstFrameSeconds;
    if (lastSeconds >= 0.0) {
      const delta = seconds - lastSeconds;
      frameSeconds = Math.min(Math.max(delta, kMinFrameSeconds), kMaxFrameSeconds);
    }
    lastSeconds = seconds;
    return frameSeconds;
  }

  return {
    render({ input, params, width, height, time }) {
      const p = (id) => params.get(id);

      //------------------------------------------------------------------
      // The controls, the audio, the restart. The transport's Restart winds
      // the clock back to zero; that is this page's Restart event.
      //------------------------------------------------------------------
      const restartPending = lastSeconds >= 0 && time < lastSeconds;
      const pageSeconds = lastSeconds >= 0 ? time - lastSeconds : 0;
      const resolved = resolve(p, AUDIO_QRM_DEFAULT, BIN_SPACING_DEFAULT);
      engine.SetParams(resolved);
      engine.SetAudioBins(audioBins, audioBins.length);
      if (restartPending) engine.Restart();

      const txW = engine.Tx().TxWidth();
      const txH = engine.Tx().TxHeight();
      readbackBuffer.ensure(txW, txH, gl.RGBA8);
      if (readback.length !== txW * txH * 4) readback = new Uint8Array(txW * txH * 4);
      ensurePictureTexture(engine.Rx().Width(), engine.Rx().Height());

      //------------------------------------------------------------------
      // 1. The clip, down onto the transmitter's grid, and back to the CPU.
      //    Synchronously: see the note at the top of this file.
      //------------------------------------------------------------------
      readbackBuffer.bind();
      gl.disable(gl.BLEND);
      readbackShader.use();
      bindTexture(gl, 0, input.texture);
      readbackShader.setSampler('InputTexture', 0);
      readbackShader.set('MaxUV', 1.0, 1.0);
      readbackShader.set('InputSize', input.width, input.height);
      readbackShader.set('TargetSize', txW, txH);
      quad.draw();
      gl.pixelStorei(gl.PACK_ALIGNMENT, 1);
      gl.readPixels(0, 0, txW, txH, gl.RGBA, gl.UNSIGNED_BYTE, readback);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);

      const cpuStart = performance.now();
      engine.SetSource(readback, txW, txH);

      //------------------------------------------------------------------
      // 2. The signal. As many samples as this frame is worth.
      //------------------------------------------------------------------
      const frameSeconds = frameSecondsFor(time);
      const samples = engine.SamplesForFrame(frameSeconds);
      engine.Run(samples);

      //------------------------------------------------------------------
      // 3. The received picture, up to the GPU.
      //------------------------------------------------------------------
      ensurePictureTexture(engine.Rx().Width(), engine.Rx().Height());
      pictureRgb = engine.Rx().Composite(pictureRgb);
      const cpuMillis = performance.now() - cpuStart;

      gl.bindTexture(gl.TEXTURE_2D, pictureTexture);
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, pictureWidth, pictureHeight, gl.RGB, gl.UNSIGNED_BYTE, pictureRgb);
      gl.bindTexture(gl.TEXTURE_2D, null);

      //------------------------------------------------------------------
      // 4. The output.
      //------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);
      composeShader.use();
      bindTexture(gl, 0, input.texture);
      bindTexture(gl, 1, pictureTexture);
      gl.activeTexture(gl.TEXTURE0);

      composeShader.setSampler('InputTexture', 0);
      composeShader.setSampler('PictureTexture', 1);
      composeShader.set('MaxUV', 1.0, 1.0);
      composeShader.set('InputMaxUV', 1.0, 1.0);

      const rect = pictureRect(lround(p('aspect')) === kAspectFill, width, height);
      // The rect is computed with y from the top; the shader's uv has y from
      // the bottom, so the origin is the rect's BOTTOM edge measured up.
      composeShader.set('RectOrigin', rect.x / width, (height - rect.y - rect.h) / height);
      composeShader.set('RectSize', rect.w / width, rect.h / height);
      const sizeLocation = composeShader.location('PictureSize');
      if (sizeLocation !== null) gl.uniform2i(sizeLocation, pictureWidth, pictureHeight);

      // Slowscan::CursorRowForTest(): the row below the one arriving.
      let cursorRow = -1;
      if (f32(p('cursor')) >= 0.5 && engine.Rx().InPicture()) {
        const row = engine.Rx().Line() + 1;
        cursorRow = row < engine.Rx().Height() ? row : -1;
      }
      composeShader.setInt('CursorRow', cursorRow);
      composeShader.set('RowsPerPixel', pictureHeight / Math.max(rect.h, 1.0));
      composeShader.set('CursorColour', kCursorR, kCursorG, kCursorB);
      composeShader.set('Mix', clamp01(p('mix')));

      quad.draw();

      bindTexture(gl, 1, null);
      bindTexture(gl, 0, null);

      //------------------------------------------------------------------
      // The stats line.
      //------------------------------------------------------------------
      if (pageSeconds > 0) {
        tally.samples += samples;
        tally.seconds += pageSeconds;
        if (tally.seconds >= 1.0) {
          telemetry.samplesPerSecond = tally.samples / tally.seconds;
          tally.samples = 0;
          tally.seconds = 0;
        }
      }
      const rx = engine.Rx();
      telemetry.txMode = Mode(engine.Tx().CurrentMode()).name;
      telemetry.rxMode = Mode(rx.PictureMode()).name;
      telemetry.inPicture = rx.InPicture();
      telemetry.line = rx.Line();
      telemetry.height = rx.Height();
      telemetry.vis = rx.lastVisCode;
      telemetry.decoded = rx.visDecoded;
      telemetry.forced = rx.forcedStarts;
      telemetry.speedAsked = resolved.speed;
      telemetry.samplesFrame = samples;
      telemetry.cpuMillis = telemetry.cpuMillis * 0.9 + cpuMillis * 0.1;
    },
  };
}

//---------------------------------------------------------------------------
// The parameters, in the constructor's order and groups, with its defaults.
//
// Omitted, and said on the page: `Audio` (FF_TYPE_BUFFER, the host's FFT),
// `Audio QRM` and `Bin Spacing` (which only shape that FFT), `Restart`
// (FF_TYPE_EVENT; the transport's Restart does it), and the About block.
//---------------------------------------------------------------------------

/// The constructor's values for the two omitted Channel controls, handed to
/// the engine as the plugin would have them. They drive a path fed silence.
const AUDIO_QRM_DEFAULT = 0.0;
const BIN_SPACING_DEFAULT = 1;

const std = (id, name, def, group, extra = {}) => ({
  id,
  name,
  type: 'standard',
  default: def,
  group,
  ...(typeof extra === 'string' ? { hint: extra } : extra),
});

const opt = (id, name, elements, def, group, hint) => ({
  id, name, type: 'option', elements, default: def, group, hint,
});

const signed = (v) => (v > 0 ? `+${v.toFixed(0)}` : v.toFixed(0));

const demo = mountDemo({
  name: 'Slowscan',
  pluginId: 'SS01',
  tagline:
    'Slow-scan television over HF. The clip is sent as audio tones — 1500 Hz black to 2300 Hz white, a line at a time with a 1200 Hz sync between lines — down a fading, echoing, hissing channel into an FM discriminator that paints each line as it arrives. Slant from a sound card a few ppm off, the new picture over the old from the top, noise streaks below the FM threshold, fades across bands of lines, a carrier pulling the picture into stripes: none of it is drawn, all of it falls out of the chain. The two shaders are the plugin’s own; the signal chain is a port of its C++.',
  repo: 'https://github.com/stoatworks-labs/slowscan',

  blurb:
    'It is Slowscan’s own GLSL, ported from the repository to WebGL2, around a JavaScript port of its C++ signal chain — the station, the HF channel and the decoder, run in this page at 11,025 samples a second times Speed on generated clips. Nothing checks that port but a reader.',

  // Aspect Fit letterboxes a 4:3 picture with a transparent surround, so the
  // layer below shows — what sits behind it is a real question.
  showBackdrop: true,

  params: [
    opt('mode', 'Mode', ['Martin M1', 'Scottie S1', 'Robot 36', 'Auto VIS'], 0, 'Mode',
      'Martin M1 and Scottie S1 send 320 × 256 as green, blue, red scans; Robot 36 sends 320 × 240 as luma and alternating chroma. Auto VIS cycles the three and the receiver picks each from its header. A picture in progress finishes in the mode it began in.'),
    std('speed', 'Speed', 0.77, 'Mode', {
      display: (v) => `${Speed(v).toFixed(1)}×`,
      hint: 'How much signal a second of video is worth: 1x is real time (nearly two minutes a Martin picture), 120x a picture a second. Everything in the channel is per sample of the 11,025 Hz signal, so a fade at 120x flickers 120 times as fast on screen — the whole signal is sped up.',
    }),
    opt('transmit', 'Transmit', ['Latch', 'Live'], 0, 'Mode',
      'Latch sends the frame that was current when the picture started, as a station sends a stored still. Live reads each line from the frame current as that line starts, so motion tears down the picture.'),

    std('snr', 'SNR', 0.70, 'Channel', {
      display: (v) => `${SnrDb(v).toFixed(1)} dB`,
      hint: 'Signal to noise in a 3 kHz bandwidth. Above the FM threshold the noise is fine grain that shrinks as the SNR rises; below it — about 3–4 dB here — the discriminator starts throwing clicks, and the picture goes to streaks. The header stops decoding at about 12 dB, and the receiver then starts the picture by hand, as an operator would.',
    }),
    std('fadeDepth', 'Fade Depth', 0.25, 'Channel', {
      display: (v) => FadeDepth(v).toFixed(2),
      hint: '0 is a wire, 1 pure Rayleigh scatter, Rician between. FM does not dim: a fade takes the SNR down with it, so a deep fade is a band of noisy lines.',
    }),
    std('fadeRate', 'Fade Rate', 0.50, 'Channel', {
      display: (v) => `${FadeRateHz(v).toFixed(2)} Hz`,
      hint: 'The Doppler spread of the fading. HF is usually 0.1–1 Hz.',
    }),
    std('multipath', 'Multipath', 0.25, 'Channel', {
      display: (v) => `${(MultipathSeconds(v) * 1000).toFixed(2)} ms`,
      hint: 'The second path’s delay. A pixel is about half a millisecond, so 8 ms is a ghost sixteen pixels to the right. Does nothing until Multipath Level is up.',
    }),
    std('multipathLevel', 'Multipath Level', 0.0, 'Channel', {
      display: (v) => MultipathLevel(v).toFixed(2),
      hint: 'The second path’s level, with its own fading. The two paths interfere, which is what makes HF frequency-selective rather than merely dim.',
    }),
    std('qrmFreq', 'QRM Freq', 0.80, 'Channel', {
      display: (v) => `${QrmHz(v).toFixed(0)} Hz`,
      hint: 'An interfering carrier. Inside 1500–2300 Hz it beats with the picture’s tones and drags the discriminator into stripes.',
    }),
    std('qrmLevel', 'QRM Level', 0.0, 'Channel', {
      display: (v) => QrmLevel(v).toFixed(2),
      hint: 'The carrier’s amplitude relative to the signal: 1 is as loud as the picture.',
    }),

    std('rxBandwidth', 'Rx Bandwidth', 0.73, 'Receiver', {
      display: (v) => `${RxBandwidthHz(v).toFixed(0)} Hz`,
      hint: 'The one-pole lowpass after the discriminator. Narrow is less noise and more horizontal smear: an edge’s tail with the one-pole’s time constant.',
    }),
    opt('sync', 'Sync', ['Free-run', 'Line Sync'], 0, 'Receiver',
      'Free-run times lines from the receiver’s own clock, so a clock error slants the picture. Line Sync re-anchors each line on the 1200 Hz pulse it detects, which takes the slant out — until noise fakes or hides a pulse.'),
    std('clockError', 'Clock Error', 0.55, 'Receiver', {
      display: (v) => `${signed(Ppm(v))} ppm`,
      hint: 'How far the receiving sound card’s clock is off. Tens of ppm is typical; 300 slants a Martin M1 about 75 pixels over the frame.',
    }),
    std('slantCorrect', 'Slant Correct', 0.50, 'Receiver', {
      display: (v) => `${signed(Ppm(v))} ppm`,
      hint: 'Subtracted from the clock error, which is exactly what a decoder’s slant adjustment does. Set it equal to Clock Error and the picture stands up straight.',
    }),

    { id: 'cursor', name: 'Cursor', type: 'boolean', default: 1, group: 'Display',
      hint: 'A green line under the row arriving, as a decoder shows its progress.' },
    opt('aspect', 'Aspect', ['Fit', 'Fill'], 0, 'Display',
      'Fit is the largest 4:3 box that fits, with a transparent surround for the layer below. Fill stretches the picture to the frame.'),
    std('mix', 'Mix', 1.0, 'Display'),
  ],

  // Colour bars first: the one picture whose correct reading everyone knows,
  // and the one that shows a slant, a fringe and a smear at a glance.
  sources: ['bars', 'grid', 'scene', 'ramp', 'spot', 'detail'],

  // The plugin ships no factory presets. These are the page's own, made
  // entirely of the plugin's parameters and reachable with the sliders; a
  // preset sets what it names and puts everything else back to default.
  presets: {
    'Clean path (40 dB, no fading, no slant)': { snr: 1.0, fadeDepth: 0.0, clockError: 0.5 },
    'Below the FM threshold (3 dB)': { snr: 0.26 },
    'Deep, fast fading': { fadeDepth: 1.0, fadeRate: 0.75 },
    'Ghost (6 ms second path)': { multipath: 0.75, multipathLevel: 0.6 },
    'Carrier on frequency': { qrmLevel: 0.4 },
    'Bad sound card (+300 ppm)': { clockError: 1.0 },
    'Same, with Line Sync': { clockError: 1.0, sync: 1 },
    'Same, slant corrected': { clockError: 1.0, slantCorrect: 1.0 },
    'Robot 36': { mode: 2 },
    'Auto VIS (cycles the three modes)': { mode: 3 },
    'Real time (1x)': { speed: 0.0 },
    'Live transmit at 120x': { transmit: 1, speed: 1.0 },
  },

  differences: [
    'The signal chain is a PORT, not the plugin’s own code. Slowscan is not a shader: between a readback of the clip and a compose of the picture it is a transmitter, an HF channel and an FM decoder in plain C++ — source/sstv/ and Controls.h — and all of it is ported here, function for function, with the same constants, the same integer line timing and the same seeded noise, because without it the page has nothing to show. Nothing checks a port but a reader: the repository’s sstest checks the C++ and has never heard of this page.',
    'The two shaders are not a port. The readback and the compose are the plugin’s own GLSL, and demo/tools/check_shaders.py fails the repository’s verify script if a character of any of the three stages drifts.',
    'The readback is synchronous here. The plugin reads the clip into a pair of pixel-pack buffers and maps the one from the frame before, so its transmitter sees a frame one frame old and never stalls the GPU. WebGL2 cannot map a buffer, so this page calls readPixels every frame and the transmitter sees this frame. Against a line that takes a third of a second even at 120x, the difference does not show.',
    'At the top Speeds the page does exactly what the plugin does: the frame’s duration is clamped to between 1/240 and 1/24 of a second and every sample it is worth is run, up to 55,125 a frame. Nothing else caps it. A machine that cannot run 1.32 million samples a second through a 63-tap complex filter drops frames until the clamp holds and then runs the signal slower than Speed asks — the line under the canvas reports the rate actually achieved.',
    'No audio. The plugin puts Resolume’s 64 FFT bins on the air as noise they shape — music as interference. A browser tab has no host FFT, so the Audio buffer, Audio QRM and Bin Spacing are not on the panel; a slider there could only be a dead one. The interference path is ported and is fed silence.',
    'Restart is an event in the plugin, and the demo kit has no event control. The transport’s Restart button does it instead: winding the page’s clock back to zero restarts the transmission and the decoder exactly as the plugin’s Restart does.',
    'The noise is the plugin’s seeded xorshift128+, bit for bit, but the cosines, logarithms and arctangents are the browser’s rather than the platform’s maths library, so a picture here is the same draw in kind rather than byte for byte the plugin’s.',
    'The plugin’s proof — every line, segment and pixel boundary to the sample, the slant against e × T_line / T_pixel, the noise against a closed form above the FM threshold and Rice’s formula at it, the header decoded, the rendered frame byte for byte against the decoder — is an offline harness in the repository. Nothing on this page measures anything.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// The stats line.
//
// Not decoration and not a measurement: a slow-scan picture takes seconds to
// arrive, and without a line saying which mode is on the air, which line is
// being received and whether the header decoded, a picture half-painted over
// the last one reads as a broken page. It also says when the browser cannot
// keep up with Speed. Skipped in embed mode, where there is no reader.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    line.dataset.stats = '';
    stage.append(line);

    const number = (n) => Math.round(n).toLocaleString('en-GB');

    setInterval(() => {
      const t = telemetry;
      if (t.txMode === '') return;
      const receiving = t.inPicture
        ? `receiving ${t.rxMode}, line ${t.line + 1} of ${t.height}`
        : `waiting for a header (${t.txMode} on the air)`;
      const header = t.decoded > 0
        ? `last VIS header ${t.vis}`
        : t.forced > 0 ? 'no header decoded, started by hand' : 'no header decoded yet';
      let rate = `${number(t.samplesFrame)} samples this frame`;
      if (t.samplesPerSecond > 0) {
        const achieved = t.samplesPerSecond / 11025;
        rate += achieved < t.speedAsked * 0.97
          ? `; the page is running the signal at ${achieved.toFixed(1)}× of the ${t.speedAsked.toFixed(1)}× asked (the frame clamp is holding)`
          : ` (${achieved.toFixed(1)}×)`;
      }
      line.textContent = `${receiving}; ${header}. ${rate}. ${t.cpuMillis.toFixed(1)} ms on the CPU chain.`;
    }, 250);
  }
}
