# Attributions

Slowscan is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### CPU signal engine feeding a GL renderer, and the seeded generator — Stoatworks vectrix

<https://github.com/stoatworks-labs/vectrix>  
Licence: MIT  
Copyright: Stoatworks Labs

The shape of a CPU signal engine at an audio-ish rate feeding a GL renderer, and the seeded xorshift128+ generator with its SplitMix64 seeding (a published construction, Vigna 2014 and Steele et al. 2014), are vectrix's, adapted.

### Host clock-unit voting — Stoatworks readout

<https://github.com/stoatworks-labs/readout>  
Licence: MIT  
Copyright: Stoatworks Labs

The host clock-unit voting is readout's.

### Harness test card — Stoatworks nesolume

<https://github.com/stoatworks-labs/nesolume>  
Licence: MIT  
Copyright: Stoatworks Labs

The harness's test card (a hue field, three discs on grey, a grey ramp, a fine checkerboard) is nesolume's.

### PassBuffer, sweep, verify and CI shape — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

PassBuffer, tools/sweep.py, tools/verify.sh and the CI shape are tinsel's by way of pitch. The probe validator that refuses to measure a neighbour is pilot's idea.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Standards and published specifications

What the implementation is measured against.

- **J. L. Barber (N7CXI), "Proposal for SSTV Mode Specifications", Dayton SSTV forum, 2000** — The source of every timing constant in source/sstv/Modes.cpp: the Martin M1, Scottie S1 and Robot 36 line structures, their tones and VIS codes, and the VIS header's framing, each written out as a named constant.
- **ITU-R BT.601** — The studio-range Y, R-Y, B-Y matrix Robot 36 carries its colour in.
- **S. O. Rice, "Noise in FM receivers", in Time Series Analysis, M. Rosenblatt (ed.), Wiley, 1963; and Taub & Schilling, Principles of Communication Systems** — sstest --threshold states the knee from Rice's click rate r erfc(sqrt(CNR)), and uses the textbook's definition of the threshold (the output noise 1 dB over the above-threshold formula).
- **R. H. Clarke, "A statistical theory of mobile-radio reception", Bell System Technical Journal 47, 1968; W. C. Jakes, Microwave Mobile Communications, 1974** — The channel's flat fading is Clarke's model as a sum of sinusoids after Jakes: eight rays.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
