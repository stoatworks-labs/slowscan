# demo/ — the browser demo

Live at **<https://slowscan-demo.stoatworks-labs.com>**. Not served from this
README: `.assetsignore` keeps this file and `tools/` out of the upload.

    index.html    the shell
    plugin.js     this plugin's parameters, Controls.h ported, its shaders, the GL passes
    sstv.js       source/sstv/ ported to JavaScript: the station, the channel, the decoder
    vendor/       the shared kit, copied in by hand — DO NOT EDIT
    tools/        check_shaders.py, run by tools/verify.sh
    _headers      CSP and caching, honoured by the Cloudflare assets runtime

## What this page is, exactly

A **port**, not a recording and not the plugin.

The shaders are the plugin's, copied across unedited. `VERTEX`, `READBACK` and
`COMPOSE` in `plugin.js` are the three `R"( ... )"` bodies in
`source/shaders/`, run in the same two passes as `Slowscan::ProcessOpenGL`.
`tools/check_shaders.py` compares them character for character and
`../tools/verify.sh` runs it.

The signal chain is a port, and a complete one: `sstv.js` is `Rng.h`, `Modes`,
`Transmitter`, `Channel`, `Receiver` and `Engine`, function for function, with
the same constants, the same integer line timing and the same seeded
xorshift128+ (in 32-bit halves, so the draws are the C++'s draws bit for bit).
The conversions in `plugin.js` are `Controls.h` and `Slowscan::resolve()`.
**Nothing checks the port but a reader.** When it was written it was driven side
by side with the C++ chain (a scratch driver linking `source/sstv/*.cpp`) through
four channels — the defaults; Robot 36 at 3 dB with deep fast fading, multipath,
QRM, Line Sync and −200 ppm; Auto VIS with Live transmit at 120x; Scottie S1
switching to Robot 36 mid-run — for 900 frames each, and the composites agreed
byte for byte. That was a one-off, not a check anything keeps running. Change
`source/sstv/` or `Controls.h` and change this page by hand to match.

Everything else is not the plugin: no Resolume, no composition, no FFGL, and
GLSL ES 3.00 in WebGL2 rather than desktop GL 4.1 core.

## Where it differs from the plugin, on purpose

- **The readback is synchronous.** The plugin reads into two pixel-pack buffers
  and maps last frame's, so its transmitter sees a frame one frame old and never
  stalls. WebGL2 cannot map a buffer: the page calls `readPixels` and the
  transmitter sees this frame. Chrome logs "GPU stall due to ReadPixels" for it,
  four times and then stops; that warning is expected.
- **At the top Speeds it does what the plugin does, and no more.** The frame's
  duration is clamped to 1/240 … 1/24 s and every sample it is worth is run, up to
  55,125 a frame. No other cap. A machine that cannot run 1.32 M samples a second
  drops frames until the clamp holds and then runs the signal slower than Speed
  asks; the line under the canvas reports the achieved rate against the page's
  own clock. On an M4 Max the chain alone runs about 5 M samples a second in
  Node (4.2 ms a frame at 120x); headless Chrome through SwiftShader managed
  about 52–115x of 120x, because the software GL eats the frame too.
- **No audio.** `Audio` (the host's FFT buffer), `Audio QRM` and `Bin Spacing`
  are not on the panel: there is no host FFT in a browser tab, and a slider there
  would be a dead one. The interference path is ported and fed silence; the two
  controls take the constructor's defaults.
- **Restart** is an FF_TYPE_EVENT. The transport's Restart button (the clock back
  to zero) triggers the engine's `Restart()` as the event does.
- **The host-clock unit vote** is not ported: the page's clock is seconds.
- **The About block** is absent.

## Working on it

```bash
python3 -m http.server 8947          # from this directory
python3 tools/check_shaders.py       # the copies still match the C++
../tools/verify.sh                   # everything, including the above
```

There is no build step. It is hand-written ES modules and what is committed is
what is served.

**After changing a shader in `source/shaders/`, copy it across here too** —
`check_shaders.py` will tell you which one and where the first difference is.
Do not edit the GLSL in `plugin.js` to make something compile in WebGL2.

**`vendor/` is a copy.** Fix the kit in
`stoatworks-backend/resolume-demo/kit/` and re-run
`stoatworks-backend/resolume-demo/sync.sh slowscan`.

## Deploying

From the **repository root**, not from here:

```bash
cf-run npx wrangler deploy
curl -s 'https://slowscan-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'
```

Verify by **content**, never by status code: a stale page returns a cheerful 200.

## Embed mode

`?embed=1` renders the output and nothing else, so the page can be a video
source. `?size=1920x1080`, `?bg=black|checker|white`, `?clip=` and any parameter
id work as query parameters; the "Copy link" button already produces them.

    https://slowscan-demo.stoatworks-labs.com/?embed=1&size=1920x1080&clip=bars&speed=1
