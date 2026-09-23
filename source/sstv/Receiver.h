#pragma once

#include "Modes.h"

#include <cstdint>
#include <vector>

namespace slowscan::sstv
{
/**
	The decoder. Real audio in, a picture out, one sample at a time.

	  analytic     a complex band-pass FIR -- the Hilbert pair -- turns the
	               real audio into the analytic signal and rejects what is
	               outside 1000..2500 Hz at the same time.
	  discriminator the instantaneous frequency is the phase advance between
	               consecutive analytic samples. Exact for a tone, and below
	               the FM threshold it throws the clicks the hobby knows.
	  lowpass      a one-pole at Rx Bandwidth on the frequency. Its step
	               response is 1 - exp( -t / tau ) in closed form, which is what
	               `sstest --levels` measures in pixels.
	  VIS          leader, break, leader, start bit, seven bits, parity, stop,
	               read off the discriminator. A good header starts a picture
	               and, in Auto, chooses its mode.
	  timing       Free-run counts the receiver's own samples at a clock that
	               is Clock Error ppm off, so the picture slants. Line Sync
	               re-anchors each line on a detected 1200 Hz pulse instead.
	  picture      float planes at the mode's resolution, written pixel by
	               pixel as each arrives, over the previous picture.

	Time inside a picture is kept as a position within the CURRENT line, in
	the receiver's own sample units, and never as an absolute count -- so the
	numbers stay small however long the plugin runs.
*/
class Receiver
{
public:
	struct Params
	{
		double bandwidthHz  = 1200.0;
		bool lineSync       = false;
		double clockErrorPpm = 0.0;
		/// The mode to decode as, or -1 to take it from the VIS header.
		int mode = 0;
	};

	/// The analytic band-pass. 63 taps at 11025 Hz, Hamming, 1000..2500 Hz:
	/// every SSTV tone from the 1100 Hz VIS bit to the 2300 Hz white sits
	/// inside it, with the transition bands clear of both.
	static constexpr int kFirTaps      = 63;
	static constexpr double kFirLowHz  = 1000.0;
	static constexpr double kFirHighHz = 2500.0;
	static constexpr int kGroupDelay   = ( kFirTaps - 1 ) / 2;

	/// Below this the discriminator is reading a sync, a VIS bit or the break;
	/// above it a porch, a separator, the leader or a pixel. Half way between
	/// the 1200 Hz sync and the 1500 Hz black.
	static constexpr double kLowThresholdHz = 1350.0;
	/// The leader is 1900 Hz; a pixel can be too, so the leader is recognised
	/// by how long it stays there rather than by frequency alone.
	static constexpr double kLeaderToleranceHz = 120.0;
	static constexpr int64_t kLeaderMinMicros  = 150000;

	Receiver();

	void SetParams( const Params& p );
	/// Back to waiting for a header. The picture is kept.
	void Reset();
	void ClearPicture();

	/// One real audio sample.
	void Step( double x );

	/// A manual start, for when the header did not decode: begin line `line`
	/// of `mode` with `lineTimeSamples` already elapsed, now. What an operator
	/// pressing the start button in their decoder does.
	void ForceStart( int mode, int line, double lineTimeSamples );

	//--- the picture --------------------------------------------------------
	int Width() const { return width; }
	int Height() const { return height; }
	int PictureMode() const { return pictureMode; }
	/// Plane p (0..2) of the picture, row 0 at the TOP, values 0..1. For the
	/// RGB modes the planes are R, G, B; for Robot 36 they are Y, R-Y, B-Y.
	const std::vector< float >& Plane( int p ) const { return planes[ p ]; }
	/// RGB8, row 0 at the top, ready to upload.
	void Composite( std::vector< uint8_t >& rgb ) const;

	//--- state --------------------------------------------------------------
	bool InPicture() const { return inPicture; }
	int Line() const { return lineIndex; }
	/// Position within the current line, in the receiver's samples.
	double LineTime() const { return lineTime; }
	double FrequencyHz() const { return lp; }
	double RawFrequencyHz() const { return rawHz; }
	int64_t SampleIndex() const { return sampleIndex; }

	int PicturesStarted() const { return picturesStarted; }
	int VisDecoded() const { return visDecoded; }
	int VisRejected() const { return visRejected; }
	int LastVisCode() const { return lastVisCode; }
	int SyncEvents() const { return syncEvents; }
	int LineJumps() const { return lineJumps; }
	int ForcedStarts() const { return forcedStarts; }

	/// The analytic FIR's taps, for the harness's closed forms.
	const double* FirRe() const { return firRe; }
	const double* FirIm() const { return firIm; }

	/// The one-pole's time constant, in samples, for a cutoff.
	static double TauSamples( double bandwidthHz );
	/// How many samples after a frequency step from `fromHz` to `toHz` the
	/// lowpass output crosses `thresholdHz`, from the closed form.
	static double CrossingLagSamples( double bandwidthHz, double fromHz, double toHz, double thresholdHz );

	/// Negative control: time lines as if the clock were perfect, whatever
	/// Clock Error says. `--slant` must fail.
	void DebugIgnoreClockError( bool on )
	{
		debugIgnoreClock = on;
		rxStep           = on ? 1.0 : 1.0 + params.clockErrorPpm * 1e-6;
	}
	/// Negative control: accept a header whose parity is wrong.
	void DebugIgnoreParity( bool on ) { debugIgnoreParity = on; }
	/// Negative control: no lowpass at all.
	void DebugBypassLowpass( bool on ) { debugBypassLowpass = on; }
	/// Negative control: keep the planes as they are when the colour model
	/// changes (RGB to Robot's YCbCr or back), as the draft did. `--progressive`
	/// must see the old picture change colour.
	void DebugKeepColourModel( bool on ) { debugKeepColourModel = on; }

private:
	void buildFir();
	void configureMode( int mode );
	void beginPicture( int mode, int line, double lineTimeSamples );
	void endPicture();
	void trackVis( bool isLow, bool isLeader );
	void trackSync( bool isLow );
	void advanceLine();
	void seekCursors();
	void writePixel( int line, int px, Component c, double value );
	double interpolatedAt( double rxTime ) const;

	Params params;

	//--- the front end -----------------------------------------------------
	double firRe[ kFirTaps ] = {};
	double firIm[ kFirTaps ] = {};
	double ring[ 64 ]        = {};
	int ringWrite            = 0;
	double zPrevRe = 0.0, zPrevIm = 0.0;
	double rawHz   = kToneBlack;
	double lp      = kToneBlack;
	double lpPrev  = kToneBlack;
	double lpCoef  = 0.0;///< 1 - exp( -2 pi fc / fs )
	int64_t sampleIndex = 0;

	//--- the receiver's clock ----------------------------------------------
	double rxStep = 1.0;///< receiver samples per true sample: 1 + e

	//--- VIS ---------------------------------------------------------------
	enum class Vis
	{
		WaitLeader,
		InLeader,
		InBreak,
		SecondLeader,
		StartBit,
		Bits,
	};
	Vis vis            = Vis::WaitLeader;
	int64_t leaderRun  = 0;
	int64_t lowRun     = 0;
	int64_t otherRun   = 0;
	int64_t visStartSample = 0;///< observed start of the start bit
	//One accumulator per bit slot: the start bit, seven data bits, parity
	//and the stop bit. Ten, and sized by the constant that says so.
	double bitSum[ kVisBitSlots ] = {};
	int bitCount[ kVisBitSlots ]  = {};
	bool startPending    = false;
	int64_t startAtSample = 0;
	int pendingMode      = 0;

	//--- sync ---------------------------------------------------------------
	int64_t syncLowRun = 0;
	bool wasLow        = false;

	//--- the picture and the line ------------------------------------------
	int width = 0, height = 0;
	int pictureMode = 0;
	std::vector< float > planes[ 3 ];

	bool inPicture  = false;
	int lineIndex   = 0;
	double lineTime = 0.0;
	double lineTimePrev = 0.0;
	double lineRx   = 0.0;
	double segStartRx[ 16 ] = {};
	double segEndRx[ 16 ]   = {};
	int segCount    = 0;
	int segIndex    = 0;
	int pixelCursor = 0;
	double syncEndRx = 0.0;
	double syncSamples = 0.0;

	int picturesStarted = 0;
	int visDecoded      = 0;
	int visRejected     = 0;
	int lastVisCode     = -1;
	int syncEvents      = 0;
	int lineJumps       = 0;
	int forcedStarts    = 0;

	bool debugIgnoreClock   = false;
	bool debugIgnoreParity  = false;
	bool debugBypassLowpass = false;
	bool debugKeepColourModel = false;
};

} // namespace slowscan::sstv
