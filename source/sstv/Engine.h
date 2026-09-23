#pragma once

#include "Channel.h"
#include "Receiver.h"
#include "Transmitter.h"

#include <cstdint>
#include <vector>

namespace slowscan::sstv
{
/**
	The whole chain -- station, HF path, decoder -- stepped a block of samples
	at a time. No GL anywhere in here, which is what lets nine of the harness's
	eleven check groups run without a context.

	`SamplesForFrame` is the only place video time enters. A frame of `dt`
	seconds at Speed s is `dt * s * 11025` samples, carried in double so the
	fractional sample is not lost between frames; everything inside the block
	is defined per sample and knows nothing about s.
*/
struct EngineParams
{
	int mode    = 0;///< 0..2, or -1 for Auto VIS
	double speed = 40.0;
	bool live   = false;
	Channel::Params channel;
	double rxBandwidthHz  = 1200.0;
	bool lineSync         = false;
	double clockErrorPpm  = 0.0;///< the effective error: Clock Error minus Slant Correct
};

class Engine
{
public:
	Engine();

	void SetParams( const EngineParams& p );
	/// The host's spectrum for this frame, ramped in over the next block.
	void SetAudioBins( const float* bins, int count );
	/// The source frame at the transmitter's resolution, RGBA8, top row first.
	void SetSource( const uint8_t* rgba, int width, int height );
	/// Start a new picture now.
	void Restart();

	/// How many samples this frame is worth, with the fractional carry.
	int SamplesForFrame( double frameSeconds );
	/// The largest block one frame can ask for. 1/24 s at 120x is 55125.
	static constexpr int kMaxBlock = 65536;

	void Run( int samples );

	Transmitter& Tx() { return tx; }
	Channel& Ch() { return ch; }
	Receiver& Rx() { return rx; }
	const Transmitter& Tx() const { return tx; }
	const Receiver& Rx() const { return rx; }

	/// Samples run since construction, for the harness.
	int64_t SamplesRun() const { return samplesRun; }
	/// The last real audio sample the receiver was handed.
	double LastAudio() const { return lastAudio; }

	/// The first true sample of a line of the transmitter's current picture,
	/// counted from the picture's first VIS sample.
	static int64_t LineStartSample( int mode, int line );

private:
	void manualStartIfNeeded();

	EngineParams params;
	Transmitter tx;
	Channel ch;
	Receiver rx;

	double carry       = 0.0;
	int64_t samplesRun = 0;
	double lastAudio   = 0.0;
	int pendingRamp    = 0;

	int lastTxPictures = 0;
	int rxPicturesAtTxStart = 0;
};

} // namespace slowscan::sstv
