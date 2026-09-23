#pragma once

/**
	The GLSL, one stage per file under `source/shaders/`.

	Two passes, and the picture never touches either of them:

	  Readback  the clip, box-filtered down onto the mode's 320x256 (or
	            320x240) and written upside down, so that glReadPixels --
	            bottom row first, into a pixel-pack buffer -- hands the CPU
	            the top of the picture first. The transmitter reads that.
	  Compose   the output: the received picture, uploaded from the CPU as an
	            RGB8 texture, placed at the mode's aspect, with the cursor and
	            the mix. texelFetch, not a filtered sample: a slow-scan picture
	            IS 320 pixels wide, and blowing it up crisp is what the
	            decoders do.

	Everything between those two -- tones, channel, discriminator, timing --
	is CPU code in `source/sstv/`, and that is not an optimisation: it is what
	lets every physics check in `sstest` run without a GL context.

	GLSL 4.10 core. Reserved words the fleet has been bitten by -- `layout`,
	`flat`, `active`, `filter`, `input`, `output`, `sample`, `common`, `half`,
	`patch` -- are not used as identifiers here.
*/
namespace slowscan::shaders
{
extern const char* const kVertex;
extern const char* const kReadbackFragment;
extern const char* const kComposeFragment;
} // namespace slowscan::shaders
