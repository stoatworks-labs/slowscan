#include "Receiver.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace slowscan::sstv
{
namespace
{
constexpr double kPi    = 3.141592653589793;
constexpr double kTwoPi = 6.283185307179586;

double samplesFor( int64_t micros )
{
	return static_cast< double >( micros ) * kSampleRateHz / 1e6;
}

/// Runs of the VIS framing, in samples. The break is 10 ms and the start bit
/// 30 ms; a run is accepted inside a window round its nominal length, wide
/// enough for the filters' lag at both ends and narrow enough that a sync
/// pulse (4.862 or 9 ms) is not mistaken for a start bit.
const double kBreakMin = samplesFor( 5000 );
const double kBreakMax = samplesFor( 20000 );
const double kOtherMax = samplesFor( 20000 );
} // namespace

//---------------------------------------------------------------------------
double Receiver::TauSamples( double bandwidthHz )
{
	return kSampleRateHz / ( kTwoPi * std::max( bandwidthHz, 1.0 ) );
}

double Receiver::CrossingLagSamples( double bandwidthHz, double fromHz, double toHz, double thresholdHz )
{
	//A one-pole moving from `from` to `to` sits at to + (from - to) e^(-t/tau),
	//so it crosses `threshold` at t = tau ln( (from - to) / (threshold - to) ).
	const double span = std::fabs( fromHz - toHz );
	const double left = std::fabs( thresholdHz - toHz );
	if( span <= 0.0 || left <= 0.0 || left >= span )
		return 0.0;
	return TauSamples( bandwidthHz ) * std::log( span / left );
}

//---------------------------------------------------------------------------
Receiver::Receiver()
{
	buildFir();
	SetParams( params );
	configureMode( params.mode < 0 ? kMartinM1 : params.mode );
	ClearPicture();
	Reset();
}

void Receiver::buildFir()
{
	//A windowed-sinc lowpass of half the band's width, modulated up to the
	//band's centre. The modulation by a complex exponential is what makes it
	//analytic: its response at negative frequencies is the lowpass shifted the
	//other way, which is outside the audio band entirely. Normalised so the
	//passband gain is 1 at the centre; the discriminator does not care, but
	//the harness reads amplitudes.
	const double halfBand = ( kFirHighHz - kFirLowHz ) / 2.0;
	const double centre   = ( kFirHighHz + kFirLowHz ) / 2.0;
	double prototype[ kFirTaps ];
	double sum = 0.0;
	for( int n = 0; n < kFirTaps; ++n )
	{
		const double m = n - kGroupDelay;
		const double x = 2.0 * halfBand * m / kSampleRateHz;
		const double sinc = m == 0 ? 1.0 : std::sin( kPi * x ) / ( kPi * x );
		const double w    = 0.54 - 0.46 * std::cos( kTwoPi * n / ( kFirTaps - 1 ) );
		prototype[ n ]    = sinc * w;
		sum += prototype[ n ];
	}
	for( int n = 0; n < kFirTaps; ++n )
	{
		const double m   = n - kGroupDelay;
		const double arg = kTwoPi * centre * m / kSampleRateHz;
		const double h   = prototype[ n ] / sum;
		firRe[ n ]       = h * std::cos( arg );
		firIm[ n ]       = h * std::sin( arg );
	}
}

void Receiver::SetParams( const Params& p )
{
	params = p;
	lpCoef = 1.0 - std::exp( -1.0 / TauSamples( params.bandwidthHz ) );
	rxStep = debugIgnoreClock ? 1.0 : 1.0 + params.clockErrorPpm * 1e-6;

	//A picture in progress keeps its mode. Between pictures the chosen mode
	//applies at once, so the sync detector is looking for the right pulse
	//length and the picture buffer is the right size before line 0 arrives.
	if( !inPicture && params.mode >= 0 && params.mode != pictureMode )
	{
		configureMode( params.mode );
		ClearPicture();
	}
}

void Receiver::Reset()
{
	inPicture    = false;
	vis          = Vis::WaitLeader;
	leaderRun = lowRun = otherRun = 0;
	startPending = false;
	syncLowRun   = 0;
	wasLow       = false;
	std::memset( ring, 0, sizeof( ring ) );
	ringWrite = 0;
	zPrevRe = zPrevIm = 0.0;
	rawHz = lp = lpPrev = kToneBlack;
	rxStep = debugIgnoreClock ? 1.0 : 1.0 + params.clockErrorPpm * 1e-6;
}

void Receiver::ClearPicture()
{
	const ModeSpec& m = Mode( pictureMode );
	for( int p = 0; p < 3; ++p )
		planes[ p ].assign( static_cast< size_t >( width ) * height, 0.0f );
	if( m.chromaAlternates )
	{
		std::fill( planes[ 1 ].begin(), planes[ 1 ].end(), static_cast< float >( robot::kCOffset / 255.0 ) );
		std::fill( planes[ 2 ].begin(), planes[ 2 ].end(), static_cast< float >( robot::kCOffset / 255.0 ) );
	}
}

void Receiver::configureMode( int mode )
{
	const ModeSpec& m = Mode( mode );
	if( m.width != width || m.height != height )
	{
		//Resize, keeping what overlaps: the old picture stays on screen and
		//is overwritten from the top, whatever size it was.
		std::vector< float > fresh[ 3 ];
		for( int p = 0; p < 3; ++p )
		{
			fresh[ p ].assign( static_cast< size_t >( m.width ) * m.height, 0.0f );
			const int rows = std::min( height, m.height );
			const int cols = std::min( width, m.width );
			for( int y = 0; y < rows; ++y )
				for( int x = 0; x < cols; ++x )
					fresh[ p ][ static_cast< size_t >( y ) * m.width + x ] = planes[ p ][ static_cast< size_t >( y ) * width + x ];
			planes[ p ].swap( fresh[ p ] );
		}
		width  = m.width;
		height = m.height;
	}
	pictureMode = mode;

	lineRx   = samplesFor( m.lineMicros );
	segCount = m.segmentCount;
	double at = 0.0;
	for( int s = 0; s < segCount; ++s )
	{
		segStartRx[ s ] = at;
		at += samplesFor( m.segments[ s ].micros );
		segEndRx[ s ] = at;
	}
	syncEndRx   = segEndRx[ m.syncIndex ];
	syncSamples = samplesFor( m.segments[ m.syncIndex ].micros );
}

void Receiver::beginPicture( int mode, int line, double lineTimeSamples )
{
	configureMode( mode );
	inPicture    = true;
	lineIndex    = std::clamp( line, 0, height - 1 );
	lineTime     = lineTimeSamples;
	lineTimePrev = lineTime;
	seekCursors();
	syncLowRun = 0;
	wasLow     = false;
	++picturesStarted;
}

void Receiver::endPicture()
{
	inPicture = false;
}

void Receiver::ForceStart( int mode, int line, double lineTimeSamples )
{
	startPending = false;
	++forcedStarts;
	beginPicture( mode, line, lineTimeSamples );
}

//---------------------------------------------------------------------------
void Receiver::Step( double x )
{
	//The analytic signal.
	ring[ ringWrite ] = x;
	double zRe = 0.0, zIm = 0.0;
	for( int k = 0; k < kFirTaps; ++k )
	{
		const double v = ring[ ( ringWrite - k ) & 63 ];
		zRe += firRe[ k ] * v;
		zIm += firIm[ k ] * v;
	}
	ringWrite = ( ringWrite + 1 ) & 63;

	//The discriminator: the angle between this sample and the last is the
	//phase advance per sample, and that is the frequency. z * conj( zPrev ).
	const double cr = zRe * zPrevRe + zIm * zPrevIm;
	const double ci = zIm * zPrevRe - zRe * zPrevIm;
	if( cr * cr + ci * ci > 1e-24 )
		rawHz = kSampleRateHz / kTwoPi * std::atan2( ci, cr );
	zPrevRe = zRe;
	zPrevIm = zIm;

	//The lowpass.
	lpPrev = lp;
	if( debugBypassLowpass )
		lp = rawHz;
	else
		lp += ( rawHz - lp ) * lpCoef;

	++sampleIndex;

	const bool isLow    = lp < kLowThresholdHz;
	const bool isLeader = std::fabs( lp - kToneLeader ) < kLeaderToleranceHz;

	trackVis( isLow, isLeader );

	if( startPending && sampleIndex >= startAtSample )
	{
		startPending = false;
		//If the decision came after the nominal start (it never does by more
		//than a few samples), the line has already run for the difference.
		beginPicture( pendingMode, 0, static_cast< double >( sampleIndex - startAtSample ) * rxStep );
	}

	if( !inPicture )
		return;

	lineTimePrev = lineTime;
	lineTime += rxStep;

	trackSync( isLow );
	if( !inPicture )
		return;

	//Pixels whose centres this sample has reached.
	const ModeSpec& m = Mode( pictureMode );
	while( segIndex < segCount )
	{
		const SegmentSpec& seg = m.segments[ segIndex ];
		if( seg.kind == Segment::Scan )
		{
			const double scanRx = segEndRx[ segIndex ] - segStartRx[ segIndex ];
			while( pixelCursor < width )
			{
				const double centre = segStartRx[ segIndex ] + ( pixelCursor + 0.5 ) * scanRx / width;
				if( centre > lineTime )
					break;
				const double hz = interpolatedAt( centre );
				writePixel( lineIndex, pixelCursor, seg.component, ( hz - kToneBlack ) / kToneSpan );
				++pixelCursor;
			}
		}
		if( lineTime >= segEndRx[ segIndex ] )
		{
			++segIndex;
			pixelCursor = 0;
		}
		else
			break;
	}

	if( lineTime >= lineRx )
		advanceLine();
}

double Receiver::interpolatedAt( double rxTime ) const
{
	const double span = lineTime - lineTimePrev;
	if( span <= 0.0 )
		return lp;
	const double f = std::clamp( ( rxTime - lineTimePrev ) / span, 0.0, 1.0 );
	return lpPrev + ( lp - lpPrev ) * f;
}

void Receiver::advanceLine()
{
	lineTime -= lineRx;
	lineTimePrev -= lineRx;
	segIndex    = 0;
	pixelCursor = 0;
	++lineIndex;
	if( lineIndex >= height )
		endPicture();
}

void Receiver::seekCursors()
{
	const ModeSpec& m = Mode( pictureMode );
	segIndex          = 0;
	while( segIndex < segCount && lineTime >= segEndRx[ segIndex ] )
		++segIndex;
	pixelCursor = 0;
	if( segIndex < segCount && m.segments[ segIndex ].kind == Segment::Scan )
	{
		const double scanRx = segEndRx[ segIndex ] - segStartRx[ segIndex ];
		while( pixelCursor < width
		       && segStartRx[ segIndex ] + ( pixelCursor + 0.5 ) * scanRx / width <= lineTime )
			++pixelCursor;
	}
}

void Receiver::writePixel( int line, int px, Component c, double value )
{
	//Unclamped: the FIR's ringing overshoots a hard edge past white, and the
	//planes keep that so a measurement can see the whole tail. Composite()
	//clamps for display, where beyond white is white.
	const float v = static_cast< float >( value );
	auto put      = [ & ]( int plane, int row ) {
		if( row >= 0 && row < height )
			planes[ plane ][ static_cast< size_t >( row ) * width + px ] = v;
	};
	switch( c )
	{
	case Component::Red: put( 0, line ); break;
	case Component::Green: put( 1, line ); break;
	case Component::Blue: put( 2, line ); break;
	case Component::Luma: put( 0, line ); break;
	case Component::ChromaAlternating:
		//Robot 36 sends one chroma component per line and the pair shares
		//both: R-Y on the even line covers it and the odd line after it, B-Y
		//on the odd line covers it and the even line before it.
		if( ( line & 1 ) == 0 )
		{
			put( 1, line );
			put( 1, line + 1 );
		}
		else
		{
			put( 2, line - 1 );
			put( 2, line );
		}
		break;
	default: break;
	}
}

//---------------------------------------------------------------------------
void Receiver::trackSync( bool isLow )
{
	if( isLow )
	{
		++syncLowRun;
		wasLow = true;
		return;
	}
	if( !wasLow )
		return;
	const int64_t run = syncLowRun;
	syncLowRun        = 0;
	wasLow            = false;

	if( run < 0.6 * syncSamples || run > 1.6 * syncSamples )
		return;
	++syncEvents;
	if( !params.lineSync )
		return;

	//The pulse ended `lag` samples ago: the one-pole climbing from 1200 to
	//the 1350 Hz threshold on its way to 1500. The FIR's group delay is NOT
	//taken off, here or in the header: the receiver's whole timeline runs on
	//the FIR's output, which is the signal G samples late, and the pixels are
	//read off that same output -- so the line origin and the pixel clock
	//agree, and the group delay cancels. Taking G off here alone shifted
	//every picture six pixels to the right, which the ramp check caught.
	const double lag = CrossingLagSamples( params.bandwidthHz, kToneSync, kToneBlack, kLowThresholdHz ) * rxStep;
	const double eventRx = lineTime - lag;
	const double delta   = eventRx - syncEndRx;
	const long long k    = std::llround( delta / lineRx );
	const int newLine    = lineIndex + static_cast< int >( k );
	if( newLine < 0 )
		return;//the lead-in sync before line 0, which anchors nothing

	//A correction of more than a pixel's worth is a visible jump; a smaller
	//one is the ordinary re-anchoring that takes the slant out.
	const ModeSpec& m = Mode( pictureMode );
	double pixelRx    = lineRx;
	for( int s = 0; s < segCount; ++s )
		if( m.segments[ s ].kind == Segment::Scan )
		{
			pixelRx = ( segEndRx[ s ] - segStartRx[ s ] ) / width;
			break;
		}
	if( std::fabs( delta - static_cast< double >( k ) * lineRx ) > pixelRx )
		++lineJumps;

	lineIndex = newLine;
	if( lineIndex >= height )
	{
		endPicture();
		return;
	}
	lineTime     = syncEndRx + lag;
	lineTimePrev = lineTime;
	seekCursors();
}

//---------------------------------------------------------------------------
void Receiver::trackVis( bool isLow, bool isLeader )
{
	const double leaderMin = samplesFor( kLeaderMinMicros );

	switch( vis )
	{
	case Vis::WaitLeader:
		leaderRun = isLeader ? leaderRun + 1 : 0;
		if( leaderRun >= leaderMin )
		{
			vis      = Vis::InLeader;
			otherRun = 0;
		}
		break;

	case Vis::InLeader:
		if( isLeader )
			otherRun = 0;
		else if( isLow )
		{
			vis    = Vis::InBreak;
			lowRun = 1;
		}
		else if( ++otherRun > kOtherMax )
		{
			vis       = Vis::WaitLeader;
			leaderRun = 0;
		}
		break;

	case Vis::InBreak:
		if( isLow )
		{
			++lowRun;
			break;
		}
		if( lowRun >= kBreakMin && lowRun <= kBreakMax )
		{
			vis       = Vis::SecondLeader;
			leaderRun = isLeader ? 1 : 0;
			otherRun  = 0;
		}
		else
		{
			vis       = Vis::WaitLeader;
			leaderRun = isLeader ? 1 : 0;
		}
		break;

	case Vis::SecondLeader:
		if( isLeader )
		{
			++leaderRun;
			otherRun = 0;
		}
		else if( isLow )
		{
			if( leaderRun >= leaderMin )
			{
				//The falling edge from the leader into the start bit is the
				//header's one clean timing reference; everything after it is
				//read at a fixed offset from here.
				vis            = Vis::Bits;
				visStartSample = sampleIndex;
				for( int s = 0; s < kVisBitSlots; ++s )
				{
					bitSum[ s ]   = 0.0;
					bitCount[ s ] = 0;
				}
			}
			else
			{
				vis       = Vis::WaitLeader;
				leaderRun = 0;
			}
		}
		else if( ++otherRun > kOtherMax )
		{
			vis       = Vis::WaitLeader;
			leaderRun = 0;
		}
		break;

	case Vis::StartBit:
		//Unused: the start bit is read as slot 0 of the bit windows below.
		vis = Vis::Bits;
		break;

	case Vis::Bits:
	{
		const double t      = static_cast< double >( sampleIndex - visStartSample );
		const double slotRx = samplesFor( kVisBitMicros );
		//Each slot is read over its middle third: 10..20 ms into a 30 ms bit,
		//clear of the filters' settling at both edges.
		const int slot = static_cast< int >( t / slotRx );
		if( slot < kVisBitSlots )
		{
			const double within = t - slot * slotRx;
			if( within >= slotRx / 3.0 && within < 2.0 * slotRx / 3.0 )
			{
				bitSum[ slot ] += lp;
				++bitCount[ slot ];
			}
			break;
		}

		//All ten slots read. Decide.
		vis       = Vis::WaitLeader;
		leaderRun = 0;

		auto mean = [ & ]( int s ) { return bitCount[ s ] > 0 ? bitSum[ s ] / bitCount[ s ] : 1e9; };
		const bool startOk = mean( 0 ) < kLowThresholdHz;
		const bool stopOk  = mean( kVisBitSlots - 1 ) < kLowThresholdHz;
		int code = 0;
		for( int b = 0; b < kVisDataBits; ++b )
			if( mean( 1 + b ) < ( kToneBit1 + kToneBit0 ) / 2.0 )
				code |= 1 << b;
		const int parity   = mean( 1 + kVisDataBits ) < ( kToneBit1 + kToneBit0 ) / 2.0 ? 1 : 0;
		const bool parityOk = parity == VisParity( code ) || debugIgnoreParity;
		const int decoded  = ModeForVis( code );
		const bool modeOk  = params.mode >= 0 || decoded >= 0;

		if( startOk && stopOk && parityOk && modeOk )
		{
			++visDecoded;
			lastVisCode = code;
			pendingMode = params.mode >= 0 ? params.mode : decoded;

			//The start bit began `lag` before it was seen: the one-pole falling
			//from 1900 through 1350 towards 1200. (Not the FIR's group delay:
			//see trackSync.) The picture begins ten bit slots after that, plus
			//the mode's lead-in.
			const double lag = CrossingLagSamples( params.bandwidthHz, kToneLeader, kToneSync, kLowThresholdHz );
			const double trueStart = static_cast< double >( visStartSample ) - lag;
			const ModeSpec& m      = Mode( pendingMode );
			startAtSample          = static_cast< int64_t >( std::llround( trueStart + samplesFor( kVisBitSlots * kVisBitMicros + m.leadInMicros ) ) );
			startPending           = true;
		}
		else
		{
			++visRejected;
		}
		break;
	}
	}
}

//---------------------------------------------------------------------------
void Receiver::Composite( std::vector< uint8_t >& rgb ) const
{
	const ModeSpec& m = Mode( pictureMode );
	rgb.resize( static_cast< size_t >( width ) * height * 3 );
	const size_t n = static_cast< size_t >( width ) * height;
	for( size_t i = 0; i < n; ++i )
	{
		double r, g, b;
		if( m.chromaAlternates )
		{
			robot::Decode( planes[ 0 ][ i ] * 255.0, planes[ 1 ][ i ] * 255.0, planes[ 2 ][ i ] * 255.0, r, g, b );
		}
		else
		{
			r = planes[ 0 ][ i ] * 255.0;
			g = planes[ 1 ][ i ] * 255.0;
			b = planes[ 2 ][ i ] * 255.0;
		}
		rgb[ i * 3 + 0 ] = static_cast< uint8_t >( std::lround( std::clamp( r, 0.0, 255.0 ) ) );
		rgb[ i * 3 + 1 ] = static_cast< uint8_t >( std::lround( std::clamp( g, 0.0, 255.0 ) ) );
		rgb[ i * 3 + 2 ] = static_cast< uint8_t >( std::lround( std::clamp( b, 0.0, 255.0 ) ) );
	}
}

} // namespace slowscan::sstv
