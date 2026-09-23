#pragma once

#include "Modes.h"

#include <cstdint>
#include <vector>

namespace slowscan::sstv
{
/**
	The station. A picture in, a phase-continuous tone out, one sample at a
	time.

	Time is a sample count within the current picture, and every segment
	boundary is an integer comparison (see Modes.h). The only floating-point
	state is the oscillator's phase, kept in cycles and reduced to [0, 1) every
	sample, so it is never large whatever the host's clock says and however
	long the plugin has been running.

	The output is the analytic tone -- cos and sin of the phase -- rather than
	the real audio. The channel wants the analytic form (a fade is a complex
	gain, a second path is a complex delayed copy) and it is free here, where
	the phase is known; the channel takes the real part at its output, which is
	the audio a radio would actually produce.
*/
class Transmitter
{
public:
	Transmitter();

	/// Mode of the NEXT picture. Auto (-1) cycles through the three.
	void SetMode( int mode );
	/// Live: each line reads the source frame current when that line starts.
	/// Latch: the frame current when the picture started.
	void SetLive( bool live );

	/// The source picture, RGBA8, at exactly TxWidth() x TxHeight(), row 0 at
	/// the TOP. Copied. A frame of the wrong size is ignored, which happens for
	/// one frame when the mode changes size.
	void SetSource( const uint8_t* rgba, int width, int height );

	/// Start the next picture from its VIS header, now.
	void Restart();

	/// One sample of the analytic tone.
	void Step( double& re, double& im );

	//--- state, mostly for the harness --------------------------------------
	int CurrentMode() const { return modeIndex; }
	/// False until the first source frame: the timeline holds at sample 0.
	bool HasSource() const { return hasSource; }
	/// The latest source frame, RGBA8, row 0 at the top, for the harness.
	const std::vector< uint8_t >& SourceImage() const { return liveImage; }
	int TxWidth() const { return Mode( modeIndex ).width; }
	int TxHeight() const { return Mode( modeIndex ).height; }
	/// Samples into the current picture, counting from the first VIS sample.
	int64_t PictureSample() const { return sample; }
	/// Total samples one picture takes: VIS, lead-in and every line.
	int64_t PictureSamples() const;
	/// The line the last sample belonged to, or -1 in the header.
	int Line() const { return line; }
	/// The segment index within the line, or -1 in the header.
	int SegmentIndex() const { return segment; }
	/// The pixel index within the current scan, or -1.
	int Pixel() const { return pixel; }
	double LastFrequencyHz() const { return lastHz; }
	/// The oscillator phase in cycles, [0, 1), after the last sample.
	double PhaseCycles() const { return phase; }
	int PicturesStarted() const { return pictures; }

	/// Negative control: reset the phase at every pixel boundary. This is what
	/// a tone generator that restarts its oscillator per pixel does, and it
	/// splatters the spectrum -- `sstest --negative` proves --levels sees it.
	void DebugResetPhaseAtPixels( bool on ) { debugResetPhase = on; }
	/// Negative control: stretch the line by this many parts per million.
	/// `--timing` must notice a line that is not its constant.
	void DebugDetuneLinePpm( double ppm ) { debugLinePpm = ppm; }
	/// Negative control: send the wrong parity bit. `--vis` must reject it.
	void DebugFlipParity( bool on ) { debugFlipParity = on; }

private:
	void startPicture();
	double frequencyForSample( int64_t n );
	double pixelValue( int lineIndex, int px, Component component ) const;

	int requestedMode = 0;///< -1 for Auto
	int modeIndex     = 0;
	int autoCursor    = 0;
	bool live         = false;
	bool hasSource    = false;

	std::vector< uint8_t > liveImage;   ///< RGBA8, TxWidth x TxHeight
	std::vector< uint8_t > latchedImage;///< what this picture is sending
	int liveWidth = 0, liveHeight = 0;

	int64_t sample = 0;
	double phase   = 0.0;
	double lastHz  = 0.0;
	int line       = -1;
	int segment    = -1;
	int pixel      = -1;
	int pictures   = 0;
	int lastLineCopied = -1;

	bool debugResetPhase = false;
	double debugLinePpm  = 0.0;
	bool debugFlipParity = false;
};

} // namespace slowscan::sstv
