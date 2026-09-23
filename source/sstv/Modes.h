#pragma once

#include <cstdint>

/**
	The three SSTV modes, as tables of named constants.

	Every number here is from J. L. Barber (N7CXI), *Proposal for SSTV Mode
	Specifications*, Dayton SSTV forum, May 2000 -- the document the decoders
	people actually run (MMSSTV, QSSTV) cite for their timings. Where the
	build brief's own figures were cross-checked against it they agree; the one
	thing the brief does not state is the frequency of Robot 36's second
	porch, which Barber gives as 1900 Hz, and that is what is used. See
	AGENTS.md for the cross-check.

	Time is kept in MICROSECONDS, as integers. Every duration in the standard
	is a whole number of microseconds, and the sample rate is a whole number of
	samples per second, so "which sample does this segment start on" is an
	integer question with an integer answer: sample n starts at time n / fs,
	so n belongs to a segment starting at t_us when n * 1000000 >= t_us * fs.
	Nothing about line timing is ever accumulated in floating point, which is
	what lets `sstest --timing` assert zero drift over a whole frame rather
	than a small one.
*/
namespace slowscan::sstv
{
/// The sample rate the whole chain runs at. 11025 Hz is the rate the hobby's
/// software historically ran at, and it is comfortably above the 2300 Hz top
/// of the signal.
constexpr int kSampleRate = 11025;
constexpr double kSampleRateHz = 11025.0;

/// One microsecond in "ticks", the unit segment boundaries are compared in:
/// microseconds multiplied by the sample rate, so that a sample index times
/// 1e6 can be compared against them exactly in 64-bit integers.
constexpr int64_t kTicksPerSample = 1000000;

//---------------------------------------------------------------------------
// Tones, in hertz.
//---------------------------------------------------------------------------
constexpr double kToneSync   = 1200.0;///< sync pulses, VIS start/stop bits, the VIS break
constexpr double kToneBlack  = 1500.0;///< pixel value 0, and the porches and separators
constexpr double kToneWhite  = 2300.0;///< pixel value 1
constexpr double kToneLeader = 1900.0;///< the VIS leader, and Robot 36's second porch
constexpr double kToneBit1   = 1100.0;///< a VIS data bit of 1
constexpr double kToneBit0   = 1300.0;///< a VIS data bit of 0

/// Pixel value v (0..1) is sent as kToneBlack + v * kToneSpan.
constexpr double kToneSpan = kToneWhite - kToneBlack;

//---------------------------------------------------------------------------
// The VIS header. Leader, break, leader, start bit, seven data bits LSB first,
// even parity, stop bit.
//---------------------------------------------------------------------------
constexpr int64_t kVisLeaderMicros = 300000;
constexpr int64_t kVisBreakMicros  = 10000;
constexpr int64_t kVisBitMicros    = 30000;
constexpr int kVisDataBits         = 7;
/// Start bit + 7 data + parity + stop = 10 bit slots after the second leader.
constexpr int kVisBitSlots = 1 + kVisDataBits + 1 + 1;
constexpr int64_t kVisMicros =
	kVisLeaderMicros + kVisBreakMicros + kVisLeaderMicros + kVisBitSlots * kVisBitMicros;

//---------------------------------------------------------------------------
// What a segment of a line is.
//---------------------------------------------------------------------------
enum class Segment
{
	Sync,     ///< 1200 Hz
	Porch,    ///< a fixed tone after a sync
	Separator,///< a fixed tone between scans
	Scan,     ///< one channel of one line, pixel by pixel
};

/// Which picture component a scan carries.
enum class Component
{
	None,
	Red,
	Green,
	Blue,
	Luma,
	/// Robot 36: R-Y on even lines, B-Y on odd lines. The separator before it
	/// says which, at 1500 Hz for even and 2300 Hz for odd.
	ChromaAlternating,
};

struct SegmentSpec
{
	Segment kind;
	int64_t micros;
	Component component;///< Scan only
	double tone;        ///< Sync, Porch, Separator only; 0 for "alternating"
};

struct ModeSpec
{
	const char* name;
	int vis;   ///< the VIS code, 0..127
	int width; ///< pixels per scan
	int height;///< lines per picture
	int64_t lineMicros;
	/// A sync sent once before line 0 so a decoder can find the first line
	/// when the mode's own sync sits in the middle of a line (Scottie).
	int64_t leadInMicros;
	int segmentCount;
	const SegmentSpec* segments;
	/// Index into `segments` of the sync pulse the decoder locks to.
	int syncIndex;
	/// Robot 36: the chroma scan alternates between R-Y and B-Y by line.
	bool chromaAlternates;
};

enum ModeIndex
{
	kMartinM1  = 0,
	kScottieS1 = 1,
	kRobot36   = 2,
	kModeCount = 3,
};

const ModeSpec& Mode( int index );

/// The mode a VIS code names, or -1.
int ModeForVis( int vis );

/// Even parity over the seven data bits.
int VisParity( int vis );

//---------------------------------------------------------------------------
// Robot 36 colour. Y, R-Y and B-Y in the studio-range scaling of ITU-R BT.601,
// which is what the Robot Research modes and the decoders that followed them
// use: Y = 16..235, chroma about 128.
//---------------------------------------------------------------------------
namespace robot
{
constexpr double kYOffset = 16.0;
constexpr double kYR      = 65.738 / 256.0;
constexpr double kYG      = 129.057 / 256.0;
constexpr double kYB      = 25.064 / 256.0;
constexpr double kCOffset = 128.0;
constexpr double kRYR     = 112.439 / 256.0;
constexpr double kRYG     = -94.154 / 256.0;
constexpr double kRYB     = -18.285 / 256.0;
constexpr double kBYR     = -37.945 / 256.0;
constexpr double kBYG     = -74.494 / 256.0;
constexpr double kBYB     = 112.439 / 256.0;

/// RGB (0..255) to the three components (0..255).
void Encode( double r, double g, double b, double& y, double& ry, double& by );
/// ...and back. Clamped to 0..255.
void Decode( double y, double ry, double by, double& r, double& g, double& b );
} // namespace robot

//---------------------------------------------------------------------------
// Integer timing helpers.
//---------------------------------------------------------------------------

/// The first sample whose start time is at or after `micros`.
inline int64_t FirstSampleAtOrAfter( int64_t micros )
{
	//ceil( micros * fs / 1e6 ) in integers: (a + b - 1) / b for non-negative a.
	const int64_t numerator = micros * kSampleRate;
	return ( numerator + kTicksPerSample - 1 ) / kTicksPerSample;
}

/// The tick value of sample n, comparable against `micros * kSampleRate`.
inline int64_t SampleTicks( int64_t n )
{
	return n * kTicksPerSample;
}

/// The number of samples in [0, micros): the sample count of a segment that
/// starts at 0. For a segment starting elsewhere the count is the difference
/// of two FirstSampleAtOrAfter() values, which is what --timing asserts.
inline int64_t SamplesIn( int64_t micros )
{
	return FirstSampleAtOrAfter( micros );
}

} // namespace slowscan::sstv
