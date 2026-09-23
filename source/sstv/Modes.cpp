#include "Modes.h"

#include <algorithm>

namespace slowscan::sstv
{
namespace
{
// Martin M1. Sync 4.862 ms, porch 0.572 ms, then green, blue, red at 146.432 ms
// each, each followed by a 0.572 ms separator. 446.446 ms a line, 256 lines.
constexpr int64_t kMartinSyncMicros  = 4862;
constexpr int64_t kMartinPorchMicros = 572;
constexpr int64_t kMartinScanMicros  = 146432;
constexpr int64_t kMartinSepMicros   = 572;

const SegmentSpec kMartinM1Segments[] = {
	{ Segment::Sync, kMartinSyncMicros, Component::None, kToneSync },
	{ Segment::Porch, kMartinPorchMicros, Component::None, kToneBlack },
	{ Segment::Scan, kMartinScanMicros, Component::Green, 0.0 },
	{ Segment::Separator, kMartinSepMicros, Component::None, kToneBlack },
	{ Segment::Scan, kMartinScanMicros, Component::Blue, 0.0 },
	{ Segment::Separator, kMartinSepMicros, Component::None, kToneBlack },
	{ Segment::Scan, kMartinScanMicros, Component::Red, 0.0 },
	{ Segment::Separator, kMartinSepMicros, Component::None, kToneBlack },
};
constexpr int64_t kMartinLineMicros =
	kMartinSyncMicros + kMartinPorchMicros + 3 * kMartinScanMicros + 3 * kMartinSepMicros;
static_assert( kMartinLineMicros == 446446, "Martin M1 line is 446.446 ms" );

// Scottie S1. Separator 1.5 ms, green 138.240, separator 1.5, blue 138.240,
// sync 9 ms, porch 1.5 ms, red 138.240. 428.22 ms a line, 256 lines. The sync
// is in the MIDDLE of the line, so a leading sync is sent before line 0.
constexpr int64_t kScottieSepMicros   = 1500;
constexpr int64_t kScottieScanMicros  = 138240;
constexpr int64_t kScottieSyncMicros  = 9000;
constexpr int64_t kScottiePorchMicros = 1500;

const SegmentSpec kScottieS1Segments[] = {
	{ Segment::Separator, kScottieSepMicros, Component::None, kToneBlack },
	{ Segment::Scan, kScottieScanMicros, Component::Green, 0.0 },
	{ Segment::Separator, kScottieSepMicros, Component::None, kToneBlack },
	{ Segment::Scan, kScottieScanMicros, Component::Blue, 0.0 },
	{ Segment::Sync, kScottieSyncMicros, Component::None, kToneSync },
	{ Segment::Porch, kScottiePorchMicros, Component::None, kToneBlack },
	{ Segment::Scan, kScottieScanMicros, Component::Red, 0.0 },
};
constexpr int64_t kScottieLineMicros =
	2 * kScottieSepMicros + 3 * kScottieScanMicros + kScottieSyncMicros + kScottiePorchMicros;
static_assert( kScottieLineMicros == 428220, "Scottie S1 line is 428.22 ms" );

// Robot 36. Sync 9 ms, porch 3 ms, Y 88 ms, separator 4.5 ms (1500 Hz on even
// lines, meaning R-Y follows; 2300 Hz on odd lines, meaning B-Y), porch 1.5 ms
// at 1900 Hz, chroma 44 ms. 150 ms a line, 240 lines.
constexpr int64_t kRobotSyncMicros   = 9000;
constexpr int64_t kRobotPorchMicros  = 3000;
constexpr int64_t kRobotLumaMicros   = 88000;
constexpr int64_t kRobotSepMicros    = 4500;
constexpr int64_t kRobotPorch2Micros = 1500;
constexpr int64_t kRobotChromaMicros = 44000;

const SegmentSpec kRobot36Segments[] = {
	{ Segment::Sync, kRobotSyncMicros, Component::None, kToneSync },
	{ Segment::Porch, kRobotPorchMicros, Component::None, kToneBlack },
	{ Segment::Scan, kRobotLumaMicros, Component::Luma, 0.0 },
	{ Segment::Separator, kRobotSepMicros, Component::None, 0.0 },//alternating: see Transmitter
	{ Segment::Porch, kRobotPorch2Micros, Component::None, kToneLeader },
	{ Segment::Scan, kRobotChromaMicros, Component::ChromaAlternating, 0.0 },
};
constexpr int64_t kRobotLineMicros =
	kRobotSyncMicros + kRobotPorchMicros + kRobotLumaMicros + kRobotSepMicros + kRobotPorch2Micros + kRobotChromaMicros;
static_assert( kRobotLineMicros == 150000, "Robot 36 line is 150 ms" );

const ModeSpec kModes[ kModeCount ] = {
	{ "Martin M1", 44, 320, 256, kMartinLineMicros, 0, 8, kMartinM1Segments, 0, false },
	{ "Scottie S1", 60, 320, 256, kScottieLineMicros, kScottieSyncMicros, 7, kScottieS1Segments, 4, false },
	{ "Robot 36", 8, 320, 240, kRobotLineMicros, 0, 6, kRobot36Segments, 0, true },
};
} // namespace

const ModeSpec& Mode( int index )
{
	return kModes[ std::clamp( index, 0, kModeCount - 1 ) ];
}

int ModeForVis( int vis )
{
	for( int i = 0; i < kModeCount; ++i )
		if( kModes[ i ].vis == vis )
			return i;
	return -1;
}

int VisParity( int vis )
{
	int ones = 0;
	for( int b = 0; b < kVisDataBits; ++b )
		ones += ( vis >> b ) & 1;
	return ones & 1;
}

namespace robot
{
void Encode( double r, double g, double b, double& y, double& ry, double& by )
{
	y  = kYOffset + kYR * r + kYG * g + kYB * b;
	ry = kCOffset + kRYR * r + kRYG * g + kRYB * b;
	by = kCOffset + kBYR * r + kBYG * g + kBYB * b;
}

void Decode( double y, double ry, double by, double& r, double& g, double& b )
{
	//The inverse of the 601 studio-range matrix above. Derived by inverting
	//the 3x3, not copied: R = 1.164 (Y-16) + 1.596 (Cr-128), and so on.
	const double yy = ( y - kYOffset ) * ( 255.0 / 219.0 );
	const double cr = ( ry - kCOffset ) * ( 255.0 / 224.0 );
	const double cb = ( by - kCOffset ) * ( 255.0 / 224.0 );
	r = yy + 1.402 * cr;
	g = yy - 0.344136 * cb - 0.714136 * cr;
	b = yy + 1.772 * cb;
	r = std::clamp( r, 0.0, 255.0 );
	g = std::clamp( g, 0.0, 255.0 );
	b = std::clamp( b, 0.0, 255.0 );
}
} // namespace robot

} // namespace slowscan::sstv
