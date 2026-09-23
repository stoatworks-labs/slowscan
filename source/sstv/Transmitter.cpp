#include "Transmitter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace slowscan::sstv
{
namespace
{
constexpr double kTwoPi = 6.283185307179586;

/// Which of the ten VIS bit slots a header offset falls in, and its tone.
double visTone( int64_t micros, int vis, bool flipParity )
{
	if( micros < kVisLeaderMicros )
		return kToneLeader;
	micros -= kVisLeaderMicros;
	if( micros < kVisBreakMicros )
		return kToneSync;
	micros -= kVisBreakMicros;
	if( micros < kVisLeaderMicros )
		return kToneLeader;
	micros -= kVisLeaderMicros;

	const int slot = static_cast< int >( micros / kVisBitMicros );
	if( slot == 0 )
		return kToneSync;//start bit
	if( slot >= 1 && slot <= kVisDataBits )
	{
		const int bit = ( vis >> ( slot - 1 ) ) & 1;//LSB first
		return bit ? kToneBit1 : kToneBit0;
	}
	if( slot == kVisDataBits + 1 )
		return ( VisParity( vis ) ^ ( flipParity ? 1 : 0 ) ) ? kToneBit1 : kToneBit0;
	return kToneSync;//stop bit
}
} // namespace

Transmitter::Transmitter()
{
	startPicture();
}

void Transmitter::SetMode( int mode )
{
	const int wanted = mode < 0 ? -1 : std::clamp( mode, 0, kModeCount - 1 );
	if( wanted == requestedMode )
		return;
	requestedMode = wanted;

	//A picture in progress finishes in the mode it began in, as a real
	//station's would. One that has not started -- no source yet, or sample
	//zero -- takes the new mode now, so the operator's choice is the first
	//picture and not the second.
	if( !hasSource || sample == 0 )
	{
		sample = 0;
		startPicture();
		--pictures;//not a new picture: the same unstarted one, re-described
	}
}

void Transmitter::SetLive( bool isLive )
{
	live = isLive;
}

void Transmitter::SetSource( const uint8_t* rgba, int width, int height )
{
	if( rgba == nullptr || width != TxWidth() || height != TxHeight() )
		return;
	liveImage.assign( rgba, rgba + static_cast< size_t >( width ) * height * 4 );
	liveWidth  = width;
	liveHeight = height;

	//The first frame ever: the picture that has been waiting at sample 0
	//latches it and the transmission begins.
	if( !hasSource )
	{
		hasSource    = true;
		latchedImage = liveImage;
	}
}

void Transmitter::Restart()
{
	sample = 0;
	startPicture();
}

int64_t Transmitter::PictureSamples() const
{
	const ModeSpec& m = Mode( modeIndex );
	return FirstSampleAtOrAfter( kVisMicros + m.leadInMicros + m.height * m.lineMicros );
}

void Transmitter::startPicture()
{
	if( requestedMode < 0 )
	{
		modeIndex  = autoCursor % kModeCount;
		autoCursor = ( autoCursor + 1 ) % kModeCount;
	}
	else
	{
		modeIndex = requestedMode;
	}

	//A real station sends a stored image. Latch it now; Live overwrites each
	//line from the current frame as that line starts.
	const size_t bytes = static_cast< size_t >( TxWidth() ) * TxHeight() * 4;
	if( liveImage.size() == bytes )
	{
		latchedImage = liveImage;
	}
	else if( !liveImage.empty() && liveWidth > 0 && liveHeight > 0 )
	{
		//The mode just changed size (Auto VIS stepping from 256 lines to
		//Robot's 240) and the readback has not caught up. Resample the frame
		//we have by nearest row rather than send a black picture.
		latchedImage.assign( bytes, 0 );
		for( int y = 0; y < TxHeight(); ++y )
		{
			const int sy = std::min( liveHeight - 1, ( y * liveHeight ) / TxHeight() );
			for( int x = 0; x < TxWidth(); ++x )
			{
				const int sx = std::min( liveWidth - 1, ( x * liveWidth ) / TxWidth() );
				std::memcpy( latchedImage.data() + ( static_cast< size_t >( y ) * TxWidth() + x ) * 4,
				             liveImage.data() + ( static_cast< size_t >( sy ) * liveWidth + sx ) * 4, 4 );
			}
		}
	}
	else
	{
		latchedImage.assign( bytes, 0 );
	}

	sample = 0;
	line = segment = pixel = -1;
	lastLineCopied = -1;
	++pictures;
}

double Transmitter::pixelValue( int lineIndex, int px, Component component ) const
{
	const int w = TxWidth();
	if( latchedImage.empty() )
		return 0.0;
	const uint8_t* p = latchedImage.data() + ( static_cast< size_t >( lineIndex ) * w + px ) * 4;
	const double r = p[ 0 ], g = p[ 1 ], b = p[ 2 ];

	switch( component )
	{
	case Component::Red: return r / 255.0;
	case Component::Green: return g / 255.0;
	case Component::Blue: return b / 255.0;
	case Component::Luma:
	case Component::ChromaAlternating:
	{
		double y, ry, by;
		robot::Encode( r, g, b, y, ry, by );
		if( component == Component::Luma )
			return y / 255.0;
		return ( ( lineIndex & 1 ) == 0 ? ry : by ) / 255.0;
	}
	default: return 0.0;
	}
}

double Transmitter::frequencyForSample( int64_t n )
{
	const ModeSpec& m = Mode( modeIndex );
	const int64_t ticks = SampleTicks( n );

	//The header.
	const int64_t visTicks = kVisMicros * kSampleRate;
	if( ticks < visTicks )
	{
		line = segment = pixel = -1;
		//Microsecond of the header this sample starts in: floor( ticks / fs ).
		return visTone( ticks / kSampleRate, m.vis, debugFlipParity );
	}

	const int64_t leadTicks = m.leadInMicros * kSampleRate;
	if( ticks < visTicks + leadTicks )
	{
		line = segment = pixel = -1;
		return kToneSync;
	}

	//The lines. All integer: line index, then position within the line.
	const int64_t lineMicros = m.lineMicros + static_cast< int64_t >( std::llround( debugLinePpm * 1e-6 * m.lineMicros ) );
	const int64_t lineTicks = lineMicros * kSampleRate;
	const int64_t rel       = ticks - visTicks - leadTicks;
	const int64_t lineIndex = rel / lineTicks;
	if( lineIndex >= m.height )
	{
		//Picture over: the next one starts on this very sample.
		startPicture();
		return frequencyForSample( 0 );
	}
	line = static_cast< int >( lineIndex );

	//Live transmit: this line reads the frame that is current as it starts.
	if( live && line != lastLineCopied )
	{
		lastLineCopied = line;
		const size_t rowBytes = static_cast< size_t >( TxWidth() ) * 4;
		if( liveImage.size() == latchedImage.size() && !liveImage.empty() )
			std::memcpy( latchedImage.data() + line * rowBytes, liveImage.data() + line * rowBytes, rowBytes );
	}

	int64_t within = rel - lineIndex * lineTicks;
	for( int s = 0; s < m.segmentCount; ++s )
	{
		const SegmentSpec& seg = m.segments[ s ];
		const int64_t segTicks = seg.micros * kSampleRate;
		if( within < segTicks )
		{
			segment = s;
			switch( seg.kind )
			{
			case Segment::Sync:
			case Segment::Porch:
				pixel = -1;
				return seg.tone;
			case Segment::Separator:
				pixel = -1;
				//Robot 36's separator says which chroma follows: 1500 Hz on an
				//even line (R-Y), 2300 Hz on an odd one (B-Y).
				if( seg.tone == 0.0 )
					return ( line & 1 ) == 0 ? kToneBlack : kToneWhite;
				return seg.tone;
			case Segment::Scan:
			{
				const int px = static_cast< int >( ( within * m.width ) / segTicks );
				pixel        = std::clamp( px, 0, m.width - 1 );
				return kToneBlack + kToneSpan * pixelValue( line, pixel, seg.component );
			}
			}
		}
		within -= segTicks;
	}

	//Unreachable: the segments sum to the line. If a table were ever wrong
	//this is the last separator's tone rather than a crash in a host.
	segment = m.segmentCount - 1;
	pixel   = -1;
	return kToneBlack;
}

void Transmitter::Step( double& re, double& im )
{
	//A station with nothing to send sends nothing. Until the first source
	//frame arrives the timeline holds at sample 0, so the first picture is a
	//real frame rather than the black the readback had not yet delivered.
	if( !hasSource )
	{
		re = im = 0.0;
		return;
	}

	const int previousPixel = pixel;
	const double hz         = frequencyForSample( sample );
	lastHz                  = hz;

	if( debugResetPhase && pixel != previousPixel )
		phase = 0.0;

	//Phase-continuous: the phase advances by exactly f / fs per sample and is
	//reduced to [0, 1). It is never the product of a time and a frequency, so
	//it cannot grow, and a tone change is a change of slope with no step.
	phase += hz / kSampleRateHz;
	phase -= std::floor( phase );

	re = std::cos( kTwoPi * phase );
	im = std::sin( kTwoPi * phase );
	++sample;
}

} // namespace slowscan::sstv
