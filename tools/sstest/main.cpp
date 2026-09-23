/**
	sstest -- the offline harness.

	"It compiled" is not evidence that a slow-scan picture slants by the right
	number of pixels per line. So this drives the REAL `Slowscan` class and
	the REAL `sstv::Engine` it owns, and checks what comes out.

	The shape of it:

	  * **Nine of the eleven check groups open no GL context at all.** The
	    chain -- tones, channel, discriminator, timing, picture -- is CPU code
	    in `source/sstv/`, so the timing, slant, levels, threshold, progressive
	    arrival, header, sync and clock checks step it directly and cannot
	    depend on a rasteriser or on a raster.
	  * **The two that rasterise run at 1920x1080 and 320x180.** The second is
	    what CI uses and is too small to resolve every picture pixel, which the
	    probe validator has to notice rather than quietly measure a neighbour.
	  * **Every tolerance is derived**, and the derivation is beside the
	    number. AGENTS.md carries the table.
	  * **`--negative` breaks the model and asserts the checks fail.** A check
	    that cannot fail is not a check.

		sstest --list
		sstest --out /tmp/slowscan.png --size 1920x1080 --set "Speed=0.9"
		sstest --timing --slant --levels --threshold --progressive --vis --sync
		sstest --clock --names --negative
		sstest --render --bench --engine
*/

#include "Slowscan.h"

#include "Controls.h"
#include "sstv/Engine.h"
#include "sstv/Modes.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace slowscan;
using namespace slowscan::sstv;

namespace
{
//---------------------------------------------------------------------------
// Reporting.
//---------------------------------------------------------------------------
int g_failures = 0;
int g_checks   = 0;
bool g_quiet   = false;

void Check( bool ok, const std::string& what )
{
	++g_checks;
	if( !ok )
		++g_failures;
	if( !g_quiet )
		std::printf( "   %s  %s\n", ok ? "ok  " : "FAIL", what.c_str() );
}

#if defined( __GNUC__ )
__attribute__( ( format( printf, 1, 2 ) ) )
#endif
void Say( const char* format, ... )
{
	if( g_quiet )
		return;
	va_list args;
	va_start( args, format );
	std::vprintf( format, args );
	va_end( args );
}

std::string F( double v, int places = 4 )
{
	char buf[ 64 ];
	std::snprintf( buf, sizeof( buf ), "%.*f", places, v );
	return buf;
}

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Test pictures at OUTPUT size, y from the top, flipped once into GL's
// bottom-up order here.
//---------------------------------------------------------------------------
void setPixel( std::vector< unsigned char >& image, int w, int h, int x, int y, float r, float g, float b )
{
	const size_t i = ( static_cast< size_t >( h - 1 - y ) * w + x ) * 4;
	auto q         = []( float v ) { return static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) ); };
	image[ i + 0 ] = q( r );
	image[ i + 1 ] = q( g );
	image[ i + 2 ] = q( b );
	image[ i + 3 ] = 255;
}

void hsvToRgb( float hue, float s, float v, float& r, float& g, float& b )
{
	const float c  = v * s;
	const float hp = hue * 6.0f;
	const float x  = c * ( 1.0f - std::fabs( std::fmod( hp, 2.0f ) - 1.0f ) );
	const float m  = v - c;
	float rr = 0.0f, gg = 0.0f, bb = 0.0f;
	if( hp < 1.0f )      { rr = c; gg = x; }
	else if( hp < 2.0f ) { rr = x; gg = c; }
	else if( hp < 3.0f ) { gg = c; bb = x; }
	else if( hp < 4.0f ) { gg = x; bb = c; }
	else if( hp < 5.0f ) { rr = x; bb = c; }
	else                 { rr = c; bb = x; }
	r = rr + m;
	g = gg + m;
	b = bb + m;
}

/// The default card, scrolled by `shift` pixels: a hue field, three discs on
/// grey, a grey ramp, a fine checkerboard. Each band makes a different wrong
/// answer visible. The bands are nesolume's; see ATTRIBUTIONS.md.
std::vector< unsigned char > buildCard( int w, int h, int shift = 0 )
{
	std::vector< unsigned char > image( static_cast< size_t >( w ) * h * 4, 0 );
	const int hueEnd  = h * 45 / 100;
	const int discEnd = h * 70 / 100;
	const int rampEnd = h * 85 / 100;

	for( int y = 0; y < h; ++y )
		for( int x = 0; x < w; ++x )
		{
			const int xs   = ( ( x + shift ) % w + w ) % w;
			const float u  = static_cast< float >( xs ) / static_cast< float >( w );
			if( y < hueEnd )
			{
				const float v = 1.0f - 0.85f * static_cast< float >( y ) / static_cast< float >( hueEnd );
				float r, g, b;
				hsvToRgb( u, 0.9f, v, r, g, b );
				setPixel( image, w, h, x, y, r, g, b );
			}
			else if( y < discEnd )
			{
				const float bandY = ( static_cast< float >( y - hueEnd ) / static_cast< float >( discEnd - hueEnd ) - 0.5f ) * 2.0f;
				float r = 0.45f, g = 0.45f, b = 0.45f;
				const struct { float cx, cr, cg, cb; } discs[ 3 ] = {
					{ 0.35f, 0.9f, 0.1f, 0.1f }, { 0.50f, 0.1f, 0.8f, 0.15f }, { 0.65f, 0.15f, 0.2f, 0.9f } };
				for( const auto& d : discs )
				{
					const float dx = ( u - d.cx ) * ( static_cast< float >( w ) / static_cast< float >( discEnd - hueEnd ) );
					if( dx * dx + bandY * bandY < 0.8f )
					{
						r = d.cr;
						g = d.cg;
						b = d.cb;
					}
				}
				setPixel( image, w, h, x, y, r, g, b );
			}
			else if( y < rampEnd )
				setPixel( image, w, h, x, y, u, u, u );
			else
			{
				const bool on = ( ( xs / 3 ) + ( y / 3 ) ) % 2 == 0;
				setPixel( image, w, h, x, y, on ? 0.95f : 0.05f, on ? 0.2f : 0.7f, 0.3f );
			}
		}
	return image;
}

//---------------------------------------------------------------------------
// Source pictures at the TRANSMITTER's resolution, RGBA8, row 0 at the top,
// which is how the plugin hands them to the engine.
//---------------------------------------------------------------------------
std::vector< uint8_t > flatSource( int w, int h, int value )
{
	std::vector< uint8_t > img( static_cast< size_t >( w ) * h * 4, 255 );
	for( size_t i = 0; i < img.size(); i += 4 )
		img[ i ] = img[ i + 1 ] = img[ i + 2 ] = static_cast< uint8_t >( value );
	return img;
}

/// Black to the left of x0, white from x0, in every channel.
std::vector< uint8_t > edgeSource( int w, int h, int x0 )
{
	std::vector< uint8_t > img( static_cast< size_t >( w ) * h * 4, 255 );
	for( int y = 0; y < h; ++y )
		for( int x = 0; x < w; ++x )
		{
			const uint8_t v = x >= x0 ? 255 : 0;
			uint8_t* p      = img.data() + ( static_cast< size_t >( y ) * w + x ) * 4;
			p[ 0 ] = p[ 1 ] = p[ 2 ] = v;
		}
	return img;
}

/// A grey ramp, 0 at the left to 255 at the right, quantised to 8 bits like
/// any real source.
std::vector< uint8_t > rampSource( int w, int h )
{
	std::vector< uint8_t > img( static_cast< size_t >( w ) * h * 4, 255 );
	for( int y = 0; y < h; ++y )
		for( int x = 0; x < w; ++x )
		{
			const uint8_t v = static_cast< uint8_t >( std::lround( 255.0 * x / ( w - 1 ) ) );
			uint8_t* p      = img.data() + ( static_cast< size_t >( y ) * w + x ) * 4;
			p[ 0 ] = p[ 1 ] = p[ 2 ] = v;
		}
	return img;
}

/// Three horizontal bands: black, mid grey, white.
std::vector< uint8_t > bandsSource( int w, int h )
{
	std::vector< uint8_t > img( static_cast< size_t >( w ) * h * 4, 255 );
	for( int y = 0; y < h; ++y )
	{
		const uint8_t v = y < h / 3 ? 0 : ( y < 2 * h / 3 ? 128 : 255 );
		for( int x = 0; x < w; ++x )
		{
			uint8_t* p = img.data() + ( static_cast< size_t >( y ) * w + x ) * 4;
			p[ 0 ] = p[ 1 ] = p[ 2 ] = v;
		}
	}
	return img;
}

//---------------------------------------------------------------------------
// The engine, driven directly. No GL.
//---------------------------------------------------------------------------

/// A channel that does nothing: 200 dB SNR is a noise amplitude of 1e-10,
/// under the double rounding of the discriminator. (80 dB was tried first,
/// and its 1e-4 showed in the edge's tail once 1 - v was below 1e-3.)
EngineParams cleanParams( int mode )
{
	EngineParams p;
	p.mode                    = mode;
	p.speed                   = 40.0;
	p.live                    = false;
	p.channel.snrDb           = 200.0;
	p.channel.fadeDepth       = 0.0;
	p.channel.multipathLevel  = 0.0;
	p.channel.qrmLevel        = 0.0;
	p.channel.audioLevel      = 0.0;
	p.rxBandwidthHz           = 1200.0;
	p.lineSync                = false;
	p.clockErrorPpm           = 0.0;
	return p;
}

/// Samples per pixel of the first scan segment of a mode.
double pixelSamples( int mode )
{
	const ModeSpec& m = Mode( mode );
	for( int s = 0; s < m.segmentCount; ++s )
		if( m.segments[ s ].kind == Segment::Scan )
			return static_cast< double >( m.segments[ s ].micros ) * kSampleRateHz / 1e6 / m.width;
	return 1.0;
}

double lineSamples( int mode )
{
	return static_cast< double >( Mode( mode ).lineMicros ) * kSampleRateHz / 1e6;
}

/// The slant the model predicts, in pixels per line: e * T_line / T_pixel.
double predictedSlant( int mode, double ppm )
{
	return ppm * 1e-6 * lineSamples( mode ) / pixelSamples( mode );
}

/// Run one whole picture -- header, lead-in, every line -- plus a tail for
/// the receiver's latency, so the last line is in the picture.
void runOnePicture( Engine& e )
{
	e.Run( static_cast< int >( e.Tx().PictureSamples() ) + 200 );
}

/// Where a row of plane `p` crosses 0.5, by linear interpolation, searching
/// from `from`. Returns -1 if it never does. `column` is the first pixel at
/// or above 0.5 -- the thresholded position.
double edgeCrossing( const Receiver& rx, int p, int row, int from, int& column )
{
	const std::vector< float >& plane = rx.Plane( p );
	const int w                       = rx.Width();
	for( int x = std::max( from, 1 ); x < w; ++x )
	{
		const float a = plane[ static_cast< size_t >( row ) * w + x - 1 ];
		const float b = plane[ static_cast< size_t >( row ) * w + x ];
		if( b >= 0.5f && a < 0.5f )
		{
			column = x;
			return ( x - 1 ) + ( 0.5 - a ) / ( b - a );
		}
	}
	column = -1;
	return -1.0;
}

/// Least-squares slope and intercept of y over x = 0..n-1.
void fitLine( const std::vector< double >& y, double& slope, double& intercept )
{
	const int n   = static_cast< int >( y.size() );
	double sx = 0, sy = 0, sxx = 0, sxy = 0;
	for( int i = 0; i < n; ++i )
	{
		sx += i;
		sy += y[ i ];
		sxx += static_cast< double >( i ) * i;
		sxy += i * y[ i ];
	}
	const double d = n * sxx - sx * sx;
	slope          = d != 0.0 ? ( n * sxy - sx * sy ) / d : 0.0;
	intercept      = ( sy - slope * sx ) / n;
}

//===========================================================================
// CPU checks.
//===========================================================================

//---------------------------------------------------------------------------
/// --timing: every boundary the transmitter produces is its constant.
//---------------------------------------------------------------------------
int runTiming( double detunePpm )
{
	Say( "timing: line, segment and pixel boundaries against the constants\n\n" );

	for( int mode = 0; mode < kModeCount; ++mode )
	{
		const ModeSpec& m = Mode( mode );
		Engine e;
		e.SetParams( cleanParams( mode ) );
		e.Tx().DebugDetuneLinePpm( detunePpm );
		const std::vector< uint8_t > img = flatSource( m.width, m.height, 128 );
		e.SetSource( img.data(), m.width, m.height );

		//The closed form in DOUBLE, independently of the integer tick
		//comparison the transmitter uses: line k starts at
		//ceil( micros * fs / 1e6 ), with `micros` an exact integer and the
		//product exact in a double (well under 2^53), so the one rounding is
		//the division -- correctly rounded, and exact when the quotient is a
		//whole number.
		const int64_t headerMicros = kVisMicros + m.leadInMicros;
		auto exactStart            = [ & ]( int64_t micros ) {
			return static_cast< double >( micros ) * kSampleRateHz / 1e6;
		};
		auto expectedLineStart = [ & ]( int k ) {
			return std::ceil( exactStart( headerMicros + k * m.lineMicros ) );
		};
		const double headerSeconds = headerMicros / 1e6;
		const double lineSeconds   = m.lineMicros / 1e6;

		//How close a line boundary comes to a whole sample, from the integer
		//remainder. A boundary that IS a whole sample (Robot 36's 150 ms is
		//1653.75 samples, so every fourth line) is exact in both arithmetics;
		//one that is not must be far enough from one that the division's
		//rounding (under 1e-10) cannot move the ceiling.
		double closest = 1.0;
		int whole      = 0;
		for( int k = 0; k <= m.height; ++k )
		{
			const int64_t rem = ( ( headerMicros + k * m.lineMicros ) * kSampleRate ) % kTicksPerSample;
			if( rem == 0 )
				++whole;
			else
				closest = std::min( closest, std::min( rem, kTicksPerSample - rem ) / static_cast< double >( kTicksPerSample ) );
		}

		//Walk the whole picture and record where each line and segment begins.
		std::vector< int64_t > lineStart( m.height, -1 );
		std::vector< std::vector< int64_t > > segStart( m.height, std::vector< int64_t >( m.segmentCount, -1 ) );
		std::vector< int64_t > pixelStart;//line 0, first scan segment
		int firstScan = -1;
		for( int s = 0; s < m.segmentCount; ++s )
			if( m.segments[ s ].kind == Segment::Scan && firstScan < 0 )
				firstScan = s;

		const int64_t total = e.Tx().PictureSamples();
		double maxPhaseError = 0.0;
		bool phaseInRange    = true;
		int lastLine = -1, lastSeg = -1, lastPixel = -1;
		for( int64_t n = 0; n < total; ++n )
		{
			const double before = e.Tx().PhaseCycles();
			e.Run( 1 );
			const double after = e.Tx().PhaseCycles();
			const double hz    = e.Tx().LastFrequencyHz();

			//Phase continuity: the advance is exactly f / fs, modulo 1.
			double advance = after - before;
			if( advance < 0.0 )
				advance += 1.0;
			maxPhaseError = std::max( maxPhaseError, std::fabs( advance - hz / kSampleRateHz ) );
			if( after < 0.0 || after >= 1.0 )
				phaseInRange = false;

			const int line = e.Tx().Line();
			const int seg  = e.Tx().SegmentIndex();
			const int px   = e.Tx().Pixel();
			if( line >= 0 && line != lastLine )
				lineStart[ line ] = n;
			if( line >= 0 && ( seg != lastSeg || line != lastLine ) && seg >= 0 )
				segStart[ line ][ seg ] = n;
			if( line == 0 && seg == firstScan && px != lastPixel && px >= 0 )
				pixelStart.push_back( n );
			lastLine  = line;
			lastSeg   = seg;
			lastPixel = px;
		}

		//Lines.
		int wrongLines = 0;
		double worstDrift = 0.0;
		for( int k = 0; k < m.height; ++k )
		{
			if( lineStart[ k ] != static_cast< int64_t >( expectedLineStart( k ) ) )
				++wrongLines;
			//Accumulated drift: the boundary against the un-rounded product.
			const double drift = lineStart[ k ] - exactStart( headerMicros + k * m.lineMicros );
			worstDrift         = std::max( worstDrift, std::fabs( drift ) );
		}
		Check( closest > 1e-6, std::string( m.name ) + ": " + std::to_string( whole ) + " line boundaries are whole samples and no other is within 1e-6 of one (closest " + F( closest, 8 ) + "), so integer and double arithmetic cannot disagree" );
		Check( wrongLines == 0, std::string( m.name ) + ": all " + std::to_string( m.height ) + " line starts equal ceil( (header + k*" + F( lineSeconds * 1000.0, 3 ) + " ms) * fs ) exactly (wrong: " + std::to_string( wrongLines ) + ")" );
		Check( worstDrift < 1.0, std::string( m.name ) + ": accumulated drift over the frame is under one sample (worst " + F( worstDrift, 4 ) + " samples: a ceiling, never a sum)" );

		//Segments, on three lines.
		int wrongSegs = 0;
		for( int k : { 0, m.height / 2, m.height - 1 } )
		{
			int64_t at = headerMicros + k * m.lineMicros;
			for( int s = 0; s < m.segmentCount; ++s )
			{
				const double expected = std::ceil( exactStart( at ) );
				if( segStart[ k ][ s ] != static_cast< int64_t >( expected ) )
					++wrongSegs;
				at += m.segments[ s ].micros;
			}
		}
		Check( wrongSegs == 0, std::string( m.name ) + ": every segment boundary on lines 0, " + std::to_string( m.height / 2 ) + " and " + std::to_string( m.height - 1 ) + " is its constant (wrong: " + std::to_string( wrongSegs ) + ")" );

		//Pixels, on line 0's first scan.
		int wrongPixels = 0;
		{
			int64_t at = headerMicros;
			for( int s = 0; s < firstScan; ++s )
				at += m.segments[ s ].micros;
			const double scanSamples = exactStart( m.segments[ firstScan ].micros );
			for( int px = 0; px < m.width && px < static_cast< int >( pixelStart.size() ); ++px )
			{
				//Pixel px begins at scanStart + px * scan / W; the transmitter
				//decides it from an integer comparison, this from a double.
				const double expected = std::ceil( exactStart( at ) + px * scanSamples / m.width );
				if( pixelStart[ px ] != static_cast< int64_t >( expected ) )
					++wrongPixels;
			}
		}
		Check( static_cast< int >( pixelStart.size() ) == m.width && wrongPixels == 0,
		       std::string( m.name ) + ": all " + std::to_string( m.width ) + " pixel boundaries of line 0's first scan are their constants (seen " + std::to_string( pixelStart.size() ) + ", wrong: " + std::to_string( wrongPixels ) + ")" );

		//Phase.
		Check( maxPhaseError < 1e-12 && phaseInRange,
		       std::string( m.name ) + ": the phase advances by exactly f/fs every sample (worst |error| " + F( maxPhaseError, 15 ) + ", bound 1e-12 = double rounding of a sum below 1) and stays in [0, 1)" );

		//The whole picture is its constant, to the sample.
		const double expectedTotal = std::ceil( ( headerSeconds + m.height * lineSeconds ) * kSampleRateHz );
		Check( static_cast< double >( total ) == expectedTotal,
		       std::string( m.name ) + ": the picture is " + std::to_string( total ) + " samples = ceil( " + F( headerSeconds + m.height * lineSeconds, 6 ) + " s * fs )" );
	}
	return g_failures;
}

//---------------------------------------------------------------------------
/// --slant: the picture leans by e * T_line / T_pixel pixels per line.
//---------------------------------------------------------------------------
int runSlant( bool ignoreClock )
{
	Say( "slant: a vertical edge in Free-run against e * T_line / T_pixel\n\n" );

	constexpr int kEdgeX = 120;

	for( int mode : { kMartinM1, kScottieS1 } )
	{
		const ModeSpec& m   = Mode( mode );
		const double pxS    = pixelSamples( mode );
		const double perLine = 1.0 / pxS;//one transmitter sample, in pixels

		//The worst-case least-squares slope error from per-line errors of at
		//most one sample: |e| * sum|x - mean| / sum (x - mean)^2, which for
		//x = 0..N-1 is 3/N (exactly 16384 / 1398080 for N = 256).
		double sumAbs = 0.0, sumSq = 0.0;
		const double mean = ( m.height - 1 ) / 2.0;
		for( int i = 0; i < m.height; ++i )
		{
			sumAbs += std::fabs( i - mean );
			sumSq += ( i - mean ) * ( i - mean );
		}
		const double slopeTolerance = perLine * sumAbs / sumSq;

		for( double ppm : { 100.0, -100.0, 300.0, -300.0 } )
		{
			Engine e;
			EngineParams p  = cleanParams( mode );
			p.clockErrorPpm = ppm;
			e.Rx().DebugIgnoreClockError( ignoreClock );
			e.SetParams( p );
			const std::vector< uint8_t > img = edgeSource( m.width, m.height, kEdgeX );
			e.SetSource( img.data(), m.width, m.height );
			runOnePicture( e );

			std::vector< double > crossing;
			int missing = 0;
			for( int row = 0; row < m.height; ++row )
			{
				int column;
				const double c = edgeCrossing( e.Rx(), 0, row, 16, column );
				if( c < 0.0 )
					++missing;
				crossing.push_back( c );
			}
			double slope, intercept;
			fitLine( crossing, slope, intercept );
			const double expected = predictedSlant( mode, ppm );

			Check( missing == 0, std::string( m.name ) + " " + F( ppm, 0 ) + " ppm: the edge is found on every line" );
			Check( std::fabs( slope - expected ) <= slopeTolerance,
			       std::string( m.name ) + " " + F( ppm, 0 ) + " ppm: slope " + F( slope, 5 ) + " px/line against " + F( expected, 5 )
			           + " (tolerance " + F( slopeTolerance, 5 ) + " = one sample * 3/N)" );
			Check( ( slope > 0 ) == ( ppm > 0 ), std::string( m.name ) + " " + F( ppm, 0 ) + " ppm: the lean is the sign of the error" );
		}

		//------------------------------------------------------------------
		// A whole-pixel offset and a fractional one are different checks.
		//
		// The crossing on the last line against the first: the error is one
		// transmitter sample on each line (the pixel grid is quantised to the
		// sample), so 2/pxS pixels in all. A whole-pixel drift of exactly 40
		// moves the crossing by 40 within that, and the thresholded column by
		// 40 +- 1; a drift of 40.5 moves the crossing by 40.5 within that and
		// the column by 40 or 41 for any starting phase.
		//------------------------------------------------------------------
		const double lineRatio = lineSamples( mode ) / pxS;
		for( double drift : { 40.0, 40.5 } )
		{
			const double ppm = drift / ( ( m.height - 1 ) * lineRatio ) * 1e6;
			Engine e;
			EngineParams p  = cleanParams( mode );
			p.clockErrorPpm = ppm;
			e.Rx().DebugIgnoreClockError( ignoreClock );
			e.SetParams( p );
			const std::vector< uint8_t > img = edgeSource( m.width, m.height, kEdgeX );
			e.SetSource( img.data(), m.width, m.height );
			runOnePicture( e );

			int c0, c1;
			const double x0 = edgeCrossing( e.Rx(), 0, 0, 16, c0 );
			const double x1 = edgeCrossing( e.Rx(), 0, m.height - 1, 16, c1 );
			const double moved = x1 - x0;
			const double bound = 2.0 * perLine;
			Check( std::fabs( moved - drift ) <= bound,
			       std::string( m.name ) + ": " + F( ppm, 2 ) + " ppm moves the crossing " + F( moved, 3 ) + " px over the frame, expected " + F( drift, 1 ) + " (bound " + F( bound, 3 ) + " = two samples)" );
			const int colMoved = c1 - c0;
			if( drift == std::floor( drift ) )
				Check( std::abs( colMoved - static_cast< int >( drift ) ) <= 1,
				       std::string( m.name ) + ": whole-pixel: the thresholded column moves " + std::to_string( colMoved ) + ", expected " + F( drift, 0 ) + " +- 1" );
			else
				Check( colMoved == static_cast< int >( std::floor( drift ) ) || colMoved == static_cast< int >( std::ceil( drift ) ),
				       std::string( m.name ) + ": fractional: the thresholded column moves " + std::to_string( colMoved ) + ", expected " + F( std::floor( drift ), 0 ) + " or " + F( std::ceil( drift ), 0 ) );
		}
	}
	return g_failures;
}

//---------------------------------------------------------------------------
/// --levels: flat fields return exactly, a ramp within a step, and an edge
/// rises over the lowpass's closed-form step response.
//---------------------------------------------------------------------------
int runLevels( bool resetPhase, bool bypassLowpass )
{
	Say( "levels: a noiseless channel returns the picture\n\n" );

	const int mode    = kMartinM1;
	const ModeSpec& m = Mode( mode );
	const double pxS  = pixelSamples( mode );

	//The receiver's FIR is 63 taps: a scan's first 63 samples carry its
	//transient, which is 12.5 pixels, and the one-pole adds 5 tau = 7 samples
	//at 1200 Hz. Everything from pixel 16 on is settled.
	const int settled = static_cast< int >( std::ceil( ( Receiver::kFirTaps + 5.0 * Receiver::TauSamples( 1200.0 ) ) / pxS ) ) + 1;

	//------------------------------------------------------------------
	// Flat: three bands, exact to 8 bits.
	//------------------------------------------------------------------
	{
		Engine e;
		e.SetParams( cleanParams( mode ) );
		e.Tx().DebugResetPhaseAtPixels( resetPhase );
		e.Rx().DebugBypassLowpass( bypassLowpass );
		const std::vector< uint8_t > img = bandsSource( m.width, m.height );
		e.SetSource( img.data(), m.width, m.height );
		runOnePicture( e );

		int wrong = 0, probed = 0;
		const int third = m.height / 3;
		for( int band = 0; band < 3; ++band )
		{
			const int expected = band == 0 ? 0 : ( band == 1 ? 128 : 255 );
			//Rows clear of the band edges by eight, so no line is ambiguous;
			//pixels clear of both ends of the scan, where the FIR's window
			//still holds the separator before or reaches the one after.
			for( int row = band * third + 8; row < ( band + 1 ) * third - 8; ++row )
				for( int px = settled; px < m.width - settled; ++px )
					for( int p = 0; p < 3; ++p )
					{
						++probed;
						const int got = static_cast< int >( std::lround( e.Rx().Plane( p )[ static_cast< size_t >( row ) * m.width + px ] * 255.0 ) );
						if( got != expected )
							++wrong;
					}
		}
		Check( wrong == 0, "flat black, grey and white return their 8-bit values exactly over " + std::to_string( probed ) + " samples, pixels " + std::to_string( settled ) + ".." + std::to_string( m.width - settled - 1 ) + " (wrong: " + std::to_string( wrong ) + "); a filtered pure tone is a pure tone" );
	}

	//------------------------------------------------------------------
	// Ramp: within one transmitter step, after the lowpass's delay.
	//------------------------------------------------------------------
	{
		Engine e;
		e.SetParams( cleanParams( mode ) );
		e.Tx().DebugResetPhaseAtPixels( resetPhase );
		e.Rx().DebugBypassLowpass( bypassLowpass );
		const std::vector< uint8_t > img = rampSource( m.width, m.height );
		e.SetSource( img.data(), m.width, m.height );
		runOnePicture( e );

		//A one-pole delays a ramp by exactly tau, and the transmitter's
		//staircase averages half a pixel behind the line through its steps,
		//so the received pixel k reads (k - tau_px) / (W - 1). The bound is
		//one transmitter step (the staircase's ripple through the lowpass is
		//under half of one) plus the source's own 8-bit quantisation.
		const double tauPx = Receiver::TauSamples( 1200.0 ) / pxS;
		const double bound = 1.0 / ( m.width - 1 ) + 0.5 / 255.0;
		double worst       = 0.0;
		int probed         = 0;
		//Both ends of the scan are excluded: the FIR's window reaches 31
		//samples -- six pixels -- past the sample it is centred on, so the
		//last pixels of a scan see the separator that follows it.
		for( int row = 8; row < m.height - 8; row += 4 )
			for( int px = settled + 8; px < m.width - settled; ++px )
			{
				const double got      = e.Rx().Plane( 0 )[ static_cast< size_t >( row ) * m.width + px ];
				const double expected = ( px - tauPx ) / ( m.width - 1 );
				worst                 = std::max( worst, std::fabs( got - expected ) );
				++probed;
			}
		Check( worst <= bound, "the ramp returns within " + F( bound, 5 ) + " of (k - tau)/(W-1) over " + std::to_string( probed ) + " pixels (worst " + F( worst, 5 ) + "; bound = one step + half an 8-bit code; the spectral-splatter bound)" );
	}

	//------------------------------------------------------------------
	// Edge: the tail decays as exp( -x / tau ), tau in pixels from the
	// closed form, at a bandwidth low enough for the tail to span pixels.
	//------------------------------------------------------------------
	{
		//60 Hz: tau is 5.8 pixels, so the tail spans twenty pixels after the
		//FIR has fully settled and before it reaches the floor below.
		constexpr double kBandwidthHz = 60.0;
		constexpr int kEdgeX          = 100;
		Engine e;
		EngineParams p  = cleanParams( mode );
		p.rxBandwidthHz = kBandwidthHz;
		e.SetParams( p );
		e.Tx().DebugResetPhaseAtPixels( resetPhase );
		e.Rx().DebugBypassLowpass( bypassLowpass );
		const std::vector< uint8_t > img = edgeSource( m.width, m.height, kEdgeX );
		e.SetSource( img.data(), m.width, m.height );
		runOnePicture( e );

		const double tauSamples = Receiver::TauSamples( kBandwidthHz );
		const double tauPx      = tauSamples / pxS;
		//After the FIR has fully passed the step (63 taps = 12.5 px) the
		//lowpass output is final + C exp( -(x - x0)/tau ) for ANY input that
		//has become constant, so ln |1 - v| is a line of slope -1/tau_px
		//whatever the FIR did before -- and C's sign is fixed, so the
		//deviation never changes sign. (It is negative here: the FIR's
		//ringing overshoots white, and the planes keep that.) Fit from 14 px
		//past the edge, down to a floor set by the analytic FIR's image: a
		//Hamming window's first sidelobe is -43 dB, so up to 0.7% of the
		//-2300 Hz image leaks through, twice that against the tone's own
		//transition-band gain -- 1.5% of amplitude is 0.015 rad of phase
		//wobble at the 4600 Hz beat, 69 Hz of discriminator ripple, which the
		//one-pole divides by 4600/fc. Three times that bound is the floor;
		//the ripple is periodic and averages out of the fit far below it.
		const int from      = kEdgeX + static_cast< int >( std::ceil( Receiver::kFirTaps / pxS ) ) + 2;
		const double ripple = 0.015 * 2.0 * kToneWhite / ( 2.0 * kToneWhite / kBandwidthHz ) / kToneSpan;
		const double floor_ = 3.0 * ripple;
		std::vector< double > logs;
		bool oneSign  = true;
		int sign      = 0;
		const int row = m.height / 2;
		for( int px = from; px < m.width; ++px )
		{
			const double d = 1.0 - e.Rx().Plane( 0 )[ static_cast< size_t >( row ) * m.width + px ];
			if( std::fabs( d ) < floor_ )
				break;
			const int s = d > 0 ? 1 : -1;
			if( sign == 0 )
				sign = s;
			else if( s != sign )
				oneSign = false;
			logs.push_back( std::log( std::fabs( d ) ) );
		}
		double slope = 0.0, intercept = 0.0;
		if( logs.size() >= 3 )
			fitLine( logs, slope, intercept );
		const double tauMeasured = slope < 0.0 ? -1.0 / slope : 0.0;
		//1%: linear interpolation of an exponential at one-sample spacing
		//errs by at most (1/8)(1/tau_samples)^2 = 0.015% relative at tau =
		//29 samples, and the image ripple is under a third of the smallest
		//term and zero-mean over the fit. Ten times the larger.
		Check( oneSign && logs.size() >= 10 && std::fabs( tauMeasured - tauPx ) <= 0.01 * tauPx,
		       "the edge's tail decays with tau = " + F( tauMeasured, 3 ) + " px against the closed form " + F( tauPx, 3 ) + " = fs/(2 pi " + F( kBandwidthHz, 0 ) + ") / samples-per-pixel, over " + std::to_string( logs.size() ) + " pixels from " + std::to_string( from ) + " down to " + F( floor_, 4 ) + ", deviation " + ( sign < 0 ? "an overshoot" : "an undershoot" ) + " throughout (tolerance 1%)" );

		//For the record: the 10-90% rise in pixels, against 2.197 tau.
		double x10 = -1, x90 = -1;
		for( int px = kEdgeX - 4; px < m.width - 1; ++px )
		{
			const double a = e.Rx().Plane( 0 )[ static_cast< size_t >( row ) * m.width + px ];
			const double b = e.Rx().Plane( 0 )[ static_cast< size_t >( row ) * m.width + px + 1 ];
			if( x10 < 0 && b >= 0.1 && a < 0.1 )
				x10 = px + ( 0.1 - a ) / ( b - a );
			if( x90 < 0 && b >= 0.9 && a < 0.9 )
				x90 = px + ( 0.9 - a ) / ( b - a );
		}
		Say( "   10-90%% rise %.2f px at %.0f Hz (a pure one-pole would be %.2f = 2.197 tau; the FIR's own rise adds to it)\n", x90 - x10, kBandwidthHz, 2.197 * tauPx );
	}
	return g_failures;
}

//---------------------------------------------------------------------------
/// --threshold: pixel noise variance against SNR shows the FM knee.
//---------------------------------------------------------------------------
int runThreshold()
{
	Say( "threshold: pixel noise variance against SNR\n\n" );

	const int mode    = kMartinM1;
	const ModeSpec& m = Mode( mode );
	constexpr int kLines = 40;

	const std::vector< double > snrs = { 40, 30, 25, 20, 15, 10, 5, 0, -5 };
	std::vector< double > variance;

	Say( "   %8s %14s %12s\n", "SNR dB", "pixel var", "x per 5 dB" );
	for( size_t i = 0; i < snrs.size(); ++i )
	{
		Engine e;
		EngineParams p  = cleanParams( mode );
		p.channel.snrDb = snrs[ i ];
		e.SetParams( p );
		const std::vector< uint8_t > img = flatSource( m.width, m.height, 128 );
		e.SetSource( img.data(), m.width, m.height );
		e.Run( static_cast< int >( Engine::LineStartSample( mode, kLines ) ) + 200 );

		double sum = 0, sumSq = 0;
		int n = 0;
		for( int row = 4; row < kLines; ++row )
			for( int px = 20; px < m.width - 20; ++px )
			{
				const double v = e.Rx().Plane( 0 )[ static_cast< size_t >( row ) * m.width + px ];
				sum += v;
				sumSq += v * v;
				++n;
			}
		const double var = sumSq / n - ( sum / n ) * ( sum / n );
		variance.push_back( var );
		if( i == 0 )
			Say( "   %8.0f %14.3e\n", snrs[ i ], var );
		else
			Say( "   %8.0f %14.3e %12.2f\n", snrs[ i ], var, var / variance[ i - 1 ] );
	}

	//Above the knee the discriminator's output noise is proportional to the
	//noise power: 5 dB is x3.162. The ratio is estimated from 36 lines x 280
	//pixels = 10080 samples, whose variance estimate has a relative standard
	//error of sqrt( 2/N ) = 1.4%; the next term in the expansion is 1/CNR,
	//under 1% at 20 dB. 10% either way is seven of those.
	auto ratio = [ & ]( double hi, double lo ) {
		size_t a = std::find( snrs.begin(), snrs.end(), hi ) - snrs.begin();
		size_t b = std::find( snrs.begin(), snrs.end(), lo ) - snrs.begin();
		return variance[ b ] / variance[ a ];
	};
	const double linear = std::pow( 10.0, 0.5 );
	for( auto pair : { std::pair< double, double >{ 30, 25 }, std::pair< double, double >{ 25, 20 } } )
	{
		const double r = ratio( pair.first, pair.second );
		Check( r >= linear * 0.9 && r <= linear * 1.1,
		       "above the knee, " + F( pair.first, 0 ) + " -> " + F( pair.second, 0 ) + " dB multiplies the variance by " + F( r, 2 ) + " (the linear law's 3.16, tolerance 10%)" );
	}

	//Below it the clicks take over and the variance climbs far faster than
	//the linear law: at least twice as fast, which is the stated knee.
	//The knee sits where the CNR in the FIR's 1.5 kHz passband reaches about
	//10 dB, which is 7 dB in the 3 kHz the SNR control is stated in.
	const double steep = ratio( 10, 5 );
	Check( steep >= 2.0 * linear,
	       "below the knee, 10 -> 5 dB multiplies the variance by " + F( steep, 2 ) + ", at least twice the linear law's 3.16: the FM threshold, at about 7 dB in 3 kHz" );
	return g_failures;
}

//---------------------------------------------------------------------------
/// --progressive: after t seconds at Speed s, floor( t s / T_line ) lines.
//---------------------------------------------------------------------------
int runProgressive()
{
	Say( "progressive: lines replaced against floor( t * s / T_line )\n\n" );

	const int mode    = kMartinM1;
	const ModeSpec& m = Mode( mode );
	const double lineS = lineSamples( mode );
	const double headerS = ( kVisMicros + m.leadInMicros ) * kSampleRateHz / 1e6;

	//A row is replaced when its last pixel lands: the last scan's last pixel
	//centre, half a pixel plus the trailing separator before the line ends,
	//seen through the FIR's group delay and 5 tau of lowpass. Sample points
	//closer than that to a line boundary are not asserted, and the count of
	//those that are is asserted instead, so the check cannot go vacuous.
	const double guard = pixelSamples( mode ) + ( m.segments[ m.segmentCount - 1 ].micros * kSampleRateHz / 1e6 )
	                     + Receiver::kGroupDelay + 5.0 * Receiver::TauSamples( 1200.0 ) + 2.0;

	for( double speed : { 40.0, 120.0, 1.0 } )
	{
		Engine e;
		EngineParams p = cleanParams( mode );
		p.speed        = speed;
		e.SetParams( p );
		const std::vector< uint8_t > img = flatSource( m.width, m.height, 128 );
		e.SetSource( img.data(), m.width, m.height );

		int asserted = 0, wrong = 0, frames = 0;
		int64_t counted = 0;
		for( int f = 1; f <= 200; ++f )
		{
			const int n = e.SamplesForFrame( 1.0 / 60.0 );
			counted += n;
			e.Run( n );
			++frames;

			const double t     = static_cast< double >( e.SamplesRun() ) / kSampleRateHz;
			const double tPic  = t * kSampleRateHz - headerS;//samples into the picture
			if( tPic >= m.height * lineS )
				break;//the first picture is over
			const double expectedLines = tPic < 0 ? 0 : std::floor( tPic / lineS );
			const double within        = tPic - expectedLines * lineS;
			if( tPic >= 0 && ( within < guard || within > lineS - guard ) )
				continue;

			//Rows replaced: the source is flat 128 and the picture was black,
			//so a row is replaced when its red plane reads 128 at the end.
			int rows = 0;
			for( int row = 0; row < m.height; ++row )
				if( std::lround( e.Rx().Plane( 0 )[ static_cast< size_t >( row ) * m.width + m.width - 1 ] * 255.0 ) == 128 )
					++rows;
			++asserted;
			if( rows != static_cast< int >( expectedLines ) )
			{
				++wrong;
				Say( "   frame %d: %d rows, expected %.0f (t=%.3f s signal)\n", f, rows, expectedLines, t );
			}
		}
		//The sample count is t * s * fs to the carry.
		const double expectedSamples = frames / 60.0 * speed * kSampleRateHz;
		Check( std::fabs( counted - expectedSamples ) < 1.0, "at " + F( speed, 0 ) + "x, " + std::to_string( frames ) + " frames ran " + std::to_string( counted ) + " samples = t*s*fs within the carried fraction (" + F( expectedSamples, 2 ) + ")" );
		Check( asserted >= ( speed >= 40 ? 60 : 1 ) && wrong == 0,
		       "at " + F( speed, 0 ) + "x, all " + std::to_string( asserted ) + " asserted frames had exactly floor( t s / T_line ) rows replaced (wrong: " + std::to_string( wrong ) + "; guard " + F( guard, 1 ) + " samples either side of a line boundary)" );
	}
	return g_failures;
}

//---------------------------------------------------------------------------
/// --vis: the header decodes back to the mode it was sent in.
//---------------------------------------------------------------------------
int runVis( bool flipParity )
{
	Say( "vis: the header, decoded\n\n" );

	for( int mode = 0; mode < kModeCount; ++mode )
	{
		const ModeSpec& m = Mode( mode );
		Engine e;
		EngineParams p = cleanParams( -1 );//the decoder is in Auto
		e.SetParams( p );
		e.Tx().SetMode( mode );//...but the station sends one mode
		e.Tx().DebugFlipParity( flipParity );
		const std::vector< uint8_t > img = flatSource( m.width, m.height, 128 );
		e.SetSource( img.data(), m.width, m.height );

		//Run until the decoder starts a picture or two lines have gone by.
		const int64_t limit = Engine::LineStartSample( mode, 3 );
		int64_t startedAt      = -1;
		double lineTimeAtStart = 0.0;
		while( e.Tx().PictureSample() < limit )
		{
			e.Run( 1 );
			if( startedAt < 0 && e.Rx().PicturesStarted() > 0 )
			{
				startedAt       = e.Tx().PictureSample();
				lineTimeAtStart = e.Rx().LineTime();
			}
		}

		Check( e.Rx().VisDecoded() == 1 && e.Rx().LastVisCode() == m.vis && e.Rx().PictureMode() == mode && e.Rx().ForcedStarts() == 0,
		       std::string( m.name ) + ": VIS " + std::to_string( e.Rx().LastVisCode() ) + " decoded (expected " + std::to_string( m.vis ) + "), mode " + Mode( e.Rx().PictureMode() ).name + ", no manual start" );

		//The picture's line 0 origin sits where it should: the decoder's
		//timeline runs on the FIR's output, G samples behind the station, and
		//the closed-form lag of the falling edge is taken off -- so what is
		//left is the FIR's transient shaping the edge the one-pole crosses:
		//under the FIR's half-width, fs/B = 7 samples. The origin is the
		//sample the picture began on less how far into the line it began.
		const double origin = static_cast< double >( startedAt ) - lineTimeAtStart;
		const double offset = origin - Engine::LineStartSample( mode, 0 ) - Receiver::kGroupDelay;
		Check( startedAt >= 0 && std::fabs( offset ) <= kSampleRateHz / ( Receiver::kFirHighHz - Receiver::kFirLowHz ),
		       std::string( m.name ) + ": line 0's origin is " + F( offset, 1 ) + " samples from the station's plus the group delay (bound 7.35 = fs / FIR bandwidth)" );
	}

	//Auto VIS end to end: the station cycles, the decoder follows.
	{
		Engine e;
		e.SetParams( cleanParams( -1 ) );
		const std::vector< uint8_t > img = flatSource( 320, 256, 128 );
		e.SetSource( img.data(), 320, 256 );
		std::vector< int > seen;
		int lastDecoded = 0;
		for( int picture = 0; picture < 3; ++picture )
		{
			runOnePicture( e );
			if( e.Rx().VisDecoded() > lastDecoded )
			{
				seen.push_back( e.Rx().LastVisCode() );
				lastDecoded = e.Rx().VisDecoded();
			}
			//Robot's frame is a different size, which the readback would
			//deliver a frame later; the check gives it the right one.
			const std::vector< uint8_t > next = flatSource( e.Tx().TxWidth(), e.Tx().TxHeight(), 128 );
			e.SetSource( next.data(), e.Tx().TxWidth(), e.Tx().TxHeight() );
		}
		const bool order = seen.size() == 3 && seen[ 0 ] == 44 && seen[ 1 ] == 60 && seen[ 2 ] == 8;
		Check( order, "Auto VIS: three pictures decoded as 44, 60, 8 (Martin, Scottie, Robot) in turn (saw " + std::to_string( seen.size() ) + ")" );
	}
	return g_failures;
}

//---------------------------------------------------------------------------
/// --sync: Line Sync takes the slant out to within a pixel.
//---------------------------------------------------------------------------
int runSync( bool lineSync )
{
	Say( "sync: Line Sync against Free-run at the same clock error\n\n" );

	constexpr int kEdgeX   = 120;
	constexpr double kPpm  = 300.0;

	for( int mode : { kMartinM1, kScottieS1 } )
	{
		const ModeSpec& m = Mode( mode );
		Engine e;
		EngineParams p  = cleanParams( mode );
		p.clockErrorPpm = kPpm;
		p.lineSync      = lineSync;
		e.SetParams( p );
		const std::vector< uint8_t > img = edgeSource( m.width, m.height, kEdgeX );
		e.SetSource( img.data(), m.width, m.height );
		runOnePicture( e );

		std::vector< double > crossing;
		double lo = 1e9, hi = -1e9;
		int missing = 0;
		for( int row = 1; row < m.height; ++row )
		{
			int column;
			const double c = edgeCrossing( e.Rx(), 0, row, 16, column );
			if( c < 0.0 )
			{
				++missing;
				continue;
			}
			crossing.push_back( c );
			lo = std::min( lo, c );
			hi = std::max( hi, c );
		}
		double slope, intercept;
		fitLine( crossing, slope, intercept );
		const double freeRun = predictedSlant( mode, kPpm ) * ( m.height - 1 );

		//One sample of sync-detection quantisation and one of the station's
		//pixel grid, each 1/pxS pixels: 0.4 px. The drift inside a line is
		//the same on every line and so contributes nothing to the spread.
		const double bound = 2.0 / pixelSamples( mode );
		Check( missing == 0 && hi - lo <= 1.0,
		       std::string( m.name ) + " at " + F( kPpm, 0 ) + " ppm: the edge stays within " + F( hi - lo, 3 ) + " px over the frame (Free-run would drift " + F( freeRun, 1 ) + "; one pixel asked, " + F( bound, 2 ) + " derived)" );
		Check( std::fabs( slope * ( m.height - 1 ) ) <= 1.0,
		       std::string( m.name ) + ": the fitted lean over the frame is " + F( slope * ( m.height - 1 ), 3 ) + " px, under one" );
		//One per line. Scottie's lead-in pulse ends where the picture begins,
		//so the detector, which only runs inside a picture, never sees it.
		Check( e.Rx().SyncEvents() == m.height && e.Rx().LineJumps() == 0,
		       std::string( m.name ) + ": " + std::to_string( e.Rx().SyncEvents() ) + " sync pulses seen (one per line), " + std::to_string( e.Rx().LineJumps() ) + " line jumps" );
	}
	return g_failures;
}

//---------------------------------------------------------------------------
/// --clock: a six-day host clock runs the same signal as a fresh one.
//---------------------------------------------------------------------------
int runClock()
{
	Say( "clock: the frame duration survives a six-day host clock\n\n" );

	//Resolume was measured at 499 million milliseconds: 5.8 days. A float
	//holding that resolves 0.03 s, which is twice a frame.
	constexpr double kSixDays = 499217.238;//seconds
	constexpr int kFrames     = 600;

	std::vector< int > fresh, aged;
	{
		Slowscan plugin;
		plugin.ForceSecondsClock();
		Engine e;
		e.SetParams( cleanParams( kMartinM1 ) );
		for( int f = 0; f < kFrames; ++f )
		{
			plugin.SetTime( f / 60.0 );
			fresh.push_back( e.SamplesForFrame( plugin.FrameSecondsForTest( plugin.ElapsedSecondsForTest() ) ) );
		}
	}
	{
		Slowscan plugin;
		plugin.ForceSecondsClock();
		Engine e;
		e.SetParams( cleanParams( kMartinM1 ) );
		for( int f = 0; f < kFrames; ++f )
		{
			plugin.SetTime( kSixDays + f / 60.0 );
			aged.push_back( e.SamplesForFrame( plugin.FrameSecondsForTest( plugin.ElapsedSecondsForTest() ) ) );
		}
	}
	int differ = 0;
	long long total = 0;
	for( int f = 0; f < kFrames; ++f )
	{
		if( fresh[ f ] != aged[ f ] )
			++differ;
		total += aged[ f ];
	}
	Check( differ == 0, "600 frames at t = 0 and at t = 499,217 s ask for identical sample counts (differ: " + std::to_string( differ ) + "; " + std::to_string( total ) + " samples = 10 s at 40x)" );

	//The negative control: the same difference in FLOAT is wrong.
	int wrongFloat = 0;
	for( int f = 1; f < kFrames; ++f )
	{
		const float a  = static_cast< float >( kSixDays + f / 60.0 );
		const float b  = static_cast< float >( kSixDays + ( f - 1 ) / 60.0 );
		const double d = static_cast< double >( a - b );
		if( std::fabs( d - 1.0 / 60.0 ) > 1e-4 )
			++wrongFloat;
	}
	Check( wrongFloat > 0, "...and the same subtraction in a float gets " + std::to_string( wrongFloat ) + " of 599 frame durations wrong, which is the trap" );

	//The signal's own clock is a sample count within the picture and a
	//phase in [0, 1): after a long run both are as small as on frame one.
	{
		Engine e;
		e.SetParams( cleanParams( kMartinM1 ) );
		const std::vector< uint8_t > img = flatSource( 320, 256, 128 );
		e.SetSource( img.data(), 320, 256 );
		e.Run( 3000000 );//four and a half minutes of signal, two pictures and a bit
		Check( e.Tx().PictureSample() < e.Tx().PictureSamples() && e.Tx().PhaseCycles() >= 0.0 && e.Tx().PhaseCycles() < 1.0,
		       "after 3,000,000 samples the station's clock is " + std::to_string( e.Tx().PictureSample() ) + " samples into picture " + std::to_string( e.Tx().PicturesStarted() ) + " and the phase is " + F( e.Tx().PhaseCycles(), 6 ) + " cycles" );
	}
	return g_failures;
}

//---------------------------------------------------------------------------
/// --names: what the host will actually show.
//---------------------------------------------------------------------------
int runNames()
{
	Say( "names: what the host will actually show\n\n" );

	Slowscan plugin;

	const std::string display = "SW Slowscan";
	Check( display.size() <= 16, "the plugin name '" + display + "' is " + std::to_string( display.size() ) + " of 16 characters" );

	bool unique = true, sized = true;
	std::vector< std::string > names;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const char* n          = plugin.GetParamName( i );
		const std::string name = n ? n : "";
		if( name.size() > 16 )
		{
			sized = false;
			Say( "   parameter %u is '%s', %zu characters\n", i, name.c_str(), name.size() );
		}
		for( const std::string& seen : names )
			if( seen == name )
			{
				unique = false;
				Say( "   parameter %u repeats the name '%s'\n", i, name.c_str() );
			}
		names.push_back( name );
	}
	Check( sized, "every parameter name is 16 characters or fewer" );
	Check( unique, "every parameter name is unique" );
	Check( plugin.GetNumParams() == Slowscan::SS_COUNT, "the host is told about all " + std::to_string( Slowscan::SS_COUNT ) + " parameters (got " + std::to_string( plugin.GetNumParams() ) + ")" );
	return g_failures;
}

//---------------------------------------------------------------------------
/// --negative: break the model, and every check above must notice.
//---------------------------------------------------------------------------
int runNegative()
{
	std::printf( "negative: each broken model must fail its check\n\n" );

	struct Case
	{
		const char* what;
		std::function< void() > run;
	};
	const Case cases[] = {
		{ "timing: a line 50 ppm long", [] { runTiming( 50.0 ); } },
		{ "slant: the Clock Error term dropped", [] { runSlant( true ); } },
		{ "levels: the phase reset at every pixel (spectral splatter)", [] { runLevels( true, false ); } },
		{ "levels: no lowpass", [] { runLevels( false, true ); } },
		{ "vis: the parity bit sent wrong", [] { runVis( true ); } },
		{ "sync: Line Sync switched off", [] { runSync( false ); } },
	};

	const int outerChecks = g_checks;
	int outerFailures     = g_failures;
	for( const Case& c : cases )
	{
		g_quiet             = true;
		const int before    = g_failures;
		const int checksBefore = g_checks;
		c.run();
		const int raised    = g_failures - before;
		const int ran       = g_checks - checksBefore;
		g_quiet             = false;
		//The broken run's own failures are the point; they are not this
		//run's failures.
		g_failures = before;
		g_checks   = checksBefore;
		++g_checks;
		if( raised == 0 )
			++g_failures;
		std::printf( "   %s  %s: %d of %d checks failed\n", raised > 0 ? "ok  " : "FAIL", c.what, raised, ran );
	}
	(void)outerChecks;
	(void)outerFailures;
	return g_failures;
}

//---------------------------------------------------------------------------
/// --engine: the CPU cost, samples per second and per video frame.
//---------------------------------------------------------------------------
int runEngineBench()
{
	std::printf( "engine: CPU cost of the chain\n\n" );

	struct Setup
	{
		const char* name;
		std::function< void( EngineParams& ) > apply;
	};
	const Setup setups[] = {
		{ "clean (no fading, noise negligible)", []( EngineParams& p ) { p = cleanParams( kMartinM1 ); } },
		{ "defaults (25 dB, fade 0.25)", []( EngineParams& p ) {
			 p = cleanParams( kMartinM1 );
			 p.channel.snrDb     = 25.0;
			 p.channel.fadeDepth = 0.25;
		 } },
		{ "everything (multipath, QRM, 32 audio bins)", []( EngineParams& p ) {
			 p = cleanParams( kMartinM1 );
			 p.channel.snrDb          = 15.0;
			 p.channel.fadeDepth      = 0.5;
			 p.channel.multipathLevel = 0.5;
			 p.channel.multipathSeconds = 0.002;
			 p.channel.qrmLevel       = 0.3;
			 p.channel.audioLevel     = 0.5;
			 p.lineSync               = true;
		 } },
	};

	constexpr int kSamples = 1000000;
	std::printf( "  %-44s %12s %10s %10s\n", "setup", "Msamples/s", "ms @ 1x", "ms @ 120x" );
	for( const Setup& s : setups )
	{
		Engine e;
		EngineParams p;
		s.apply( p );
		e.SetParams( p );
		const std::vector< uint8_t > img = flatSource( 320, 256, 128 );
		e.SetSource( img.data(), 320, 256 );
		float bins[ 64 ] = {};
		for( int i = 0; i < 32; ++i )
			bins[ i ] = 0.1f;
		e.SetAudioBins( bins, 64 );

		e.Run( 50000 );//warm
		const auto start = std::chrono::steady_clock::now();
		e.Run( kSamples );
		const double seconds = std::chrono::duration< double >( std::chrono::steady_clock::now() - start ).count();
		const double rate    = kSamples / seconds;
		std::printf( "  %-44s %12.2f %10.3f %10.3f\n", s.name, rate / 1e6,
		             kSampleRateHz / 60.0 / rate * 1000.0, 120.0 * kSampleRateHz / 60.0 / rate * 1000.0 );
	}
	std::printf( "\n  (120x Martin M1 is %.2f Msamples/s of signal; a 60 fps frame at 120x is %d samples)\n",
	             120.0 * kSampleRateHz / 1e6, static_cast< int >( 120.0 * kSampleRateHz / 60.0 ) );
	return 0;
}

//===========================================================================
// GL.
//===========================================================================
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

struct Target
{
	int w = 0, h = 0;
	GLuint texture = 0, fbo = 0;

	bool Create( int width, int height )
	{
		w = width;
		h = height;
		glGenTextures( 1, &texture );
		glBindTexture( GL_TEXTURE_2D, texture );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glGenFramebuffers( 1, &fbo );
		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
		const bool ok = glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		return ok;
	}
	void Destroy()
	{
		if( fbo )
			glDeleteFramebuffers( 1, &fbo );
		if( texture )
			glDeleteTextures( 1, &texture );
		fbo = texture = 0;
	}
};

GLuint makeInput( const std::vector< unsigned char >& picture, int w, int h )
{
	GLuint id = 0;
	glGenTextures( 1, &id );
	glBindTexture( GL_TEXTURE_2D, id );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, picture.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return id;
}

void updateInput( GLuint id, const std::vector< unsigned char >& picture, int w, int h )
{
	glBindTexture( GL_TEXTURE_2D, id );
	glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, picture.data() );
	glBindTexture( GL_TEXTURE_2D, 0 );
}

/// A frame, top-down, RGBA8.
struct Image
{
	int w = 0, h = 0;
	std::vector< unsigned char > px;
	const unsigned char* at( int x, int y ) const { return px.data() + ( static_cast< size_t >( y ) * w + x ) * 4; }
	std::string str( int x, int y ) const
	{
		const unsigned char* p = at( x, y );
		char buf[ 48 ];
		std::snprintf( buf, sizeof( buf ), "(%d, %d, %d, %d)", p[ 0 ], p[ 1 ], p[ 2 ], p[ 3 ] );
		return buf;
	}
};

Image readBack( const Target& t )
{
	std::vector< unsigned char > raw( static_cast< size_t >( t.w ) * t.h * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, t.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, t.w, t.h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data() );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	Image out;
	out.w = t.w;
	out.h = t.h;
	out.px.resize( raw.size() );
	const size_t stride = static_cast< size_t >( t.w ) * 4;
	for( int y = 0; y < t.h; ++y )
		std::memcpy( out.px.data() + static_cast< size_t >( y ) * stride, raw.data() + static_cast< size_t >( t.h - 1 - y ) * stride, stride );
	return out;
}

struct Instance
{
	Slowscan plugin;
	bool ok = false;

	Instance( int w, int h )
	{
		plugin.ForceSecondsClock();
		FFGLViewportStruct vp = { 0, 0, static_cast< FFUInt32 >( w ), static_cast< FFUInt32 >( h ) };
		ok                    = plugin.InitGL( &vp ) == FF_SUCCESS;
		if( !ok )
			std::printf( "   InitGL FAILED -- see the diagnostics log for which shader\n" );
	}
	~Instance() { plugin.DeInitGL(); }

	void set( Slowscan::ParamID id, float v ) { plugin.SetFloatParameter( id, v ); }
	void press( Slowscan::ParamID id )
	{
		plugin.SetFloatParameter( id, 1.0f );
		plugin.SetFloatParameter( id, 0.0f );
	}
};

/// A synthetic spectrum: a pink-ish tilt, a kick in the low bins, a little
/// hiss up top -- so that the Audio QRM control and Bin Spacing have
/// something to be a property of. Delivered through SetParamElementValue,
/// the same entry point the host uses.
void injectSpectrum( Slowscan& plugin, float level, double seconds )
{
	const float kick = static_cast< float >( std::exp( -6.0 * std::fmod( seconds, 0.5 ) ) );
	for( int i = 0; i < Slowscan::kAudioBins; ++i )
	{
		const float t = static_cast< float >( i ) / 63.0f;
		float value   = 0.20f * std::exp( -3.2f * t );
		if( i < 8 )
			value += kick;
		if( i >= 28 )
			value += 0.12f;
		plugin.SetParamElementValue( Slowscan::SS_AUDIO, static_cast< unsigned int >( i ), std::clamp( value * level, 0.0f, 1.0f ) );
	}
}

void renderOnly( Instance& i, const Target& t, GLuint input, int inputW, int inputH, double seconds )
{
	FFGLTextureStruct in = {};
	in.Width = in.HardwareWidth = static_cast< FFUInt32 >( inputW );
	in.Height = in.HardwareHeight = static_cast< FFUInt32 >( inputH );
	in.Handle                     = input;
	FFGLTextureStruct* inputs[ 1 ] = { &in };

	ProcessOpenGLStruct gl = {};
	gl.numInputTextures    = 1;
	gl.inputTextures       = inputs;
	gl.HostFBO             = t.fbo;

	glBindFramebuffer( GL_FRAMEBUFFER, t.fbo );
	glViewport( 0, 0, t.w, t.h );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );

	i.plugin.SetTime( seconds );
	i.plugin.ProcessOpenGL( &gl );
}

Image render( Instance& i, const Target& t, GLuint input, int inputW, int inputH, double seconds )
{
	renderOnly( i, t, input, inputW, inputH, seconds );
	return readBack( t );
}

//---------------------------------------------------------------------------
// The mapping between an output pixel and a picture pixel. Mirrors the
// compose shader in float and in the same order, to PLACE probes and then
// to VALIDATE that they landed.
//---------------------------------------------------------------------------
struct Mapping
{
	int w, h;
	float rx, ry, rw, rh;//the rect in output pixels, y from the top
	int pw, ph;

	bool toPicture( int ox, int oyTop, int& px, int& py ) const
	{
		const float uvx    = ( static_cast< float >( ox ) + 0.5f ) / static_cast< float >( w );
		const float uvy    = ( static_cast< float >( h - 1 - oyTop ) + 0.5f ) / static_cast< float >( h );
		const float ox0    = rx / w;
		const float oy0    = ( h - ry - rh ) / h;
		const float innerX = ( uvx - ox0 ) / std::max( rw / w, 1e-6f );
		const float innerY = ( uvy - oy0 ) / std::max( rh / h, 1e-6f );
		if( innerX < 0.0f || innerX >= 1.0f || innerY < 0.0f || innerY >= 1.0f )
			return false;
		px = std::clamp( static_cast< int >( innerX * static_cast< float >( pw ) ), 0, pw - 1 );
		py = std::clamp( static_cast< int >( ( 1.0f - innerY ) * static_cast< float >( ph ) ), 0, ph - 1 );
		return true;
	}

	void probeFor( int px, int py, int& ox, int& oyTop ) const
	{
		ox    = std::clamp( static_cast< int >( rx + ( px + 0.5f ) * rw / pw ), 0, w - 1 );
		oyTop = std::clamp( static_cast< int >( ry + ( py + 0.5f ) * rh / ph ), 0, h - 1 );
	}
};

Mapping mappingFor( const Slowscan& plugin, int w, int h )
{
	Mapping m;
	m.w = w;
	m.h = h;
	plugin.PictureRectForTest( w, h, m.rx, m.ry, m.rw, m.rh );
	m.pw = plugin.EngineForTestConst().Rx().Width();
	m.ph = plugin.EngineForTestConst().Rx().Height();
	return m;
}

/// Compare the rendered frame against the CPU picture through the mapping.
/// Returns the unresolved count; adds the wrong count to `wrong`.
int compareFrame( const Image& img, Slowscan& plugin, int& wrong, int& compared, int cursorRow )
{
	Mapping map = mappingFor( plugin, img.w, img.h );
	std::vector< uint8_t > rgb;
	plugin.EngineForTest().Rx().Composite( rgb );

	int unresolved = 0;
	for( int py = 0; py < map.ph; ++py )
		for( int px = 0; px < map.pw; ++px )
		{
			int ox, oy;
			map.probeFor( px, py, ox, oy );
			int gx, gy;
			if( !map.toPicture( ox, oy, gx, gy ) || gx != px || gy != py )
			{
				++unresolved;
				continue;
			}
			const unsigned char* got = img.at( ox, oy );
			unsigned char want[ 3 ];
			if( py == cursorRow )
			{
				want[ 0 ] = 51;
				want[ 1 ] = 255;
				want[ 2 ] = 102;
			}
			else
				std::memcpy( want, rgb.data() + ( static_cast< size_t >( py ) * map.pw + px ) * 3, 3 );
			++compared;
			if( got[ 0 ] != want[ 0 ] || got[ 1 ] != want[ 1 ] || got[ 2 ] != want[ 2 ] || got[ 3 ] != 255 )
				++wrong;
		}
	return unresolved;
}

//---------------------------------------------------------------------------
/// --render: the frame on screen is the picture the decoder holds.
//---------------------------------------------------------------------------
void renderAt( int w, int h )
{
	std::printf( "\n  at %dx%d\n", w, h );

	Target target;
	if( !target.Create( w, h ) )
	{
		Check( false, "output framebuffer is complete" );
		return;
	}
	const std::vector< unsigned char > card = buildCard( w, h );
	const GLuint input                       = makeInput( card, w, h );

	constexpr int kFrames = 40;
	constexpr float kSpeed = 0.9f;//about 74x: forty frames is fifty seconds of signal

	//------------------------------------------------------------------
	// The picture, pixel for pixel, with the cursor on.
	//------------------------------------------------------------------
	{
		Instance i( w, h );
		i.set( Slowscan::SS_SPEED, kSpeed );
		Image img;
		for( int f = 0; f < kFrames; ++f )
			img = render( i, target, input, w, h, f / 60.0 );

		int wrong = 0, compared = 0;
		const int cursor     = i.plugin.CursorRowForTest();
		const int unresolved = compareFrame( img, i.plugin, wrong, compared, cursor );
		const Receiver& rx   = i.plugin.EngineForTest().Rx();
		Check( rx.InPicture() && rx.Line() > 40, "  the decoder is " + std::to_string( rx.Line() ) + " lines into a picture after " + std::to_string( kFrames ) + " frames" );
		Check( wrong == 0 && compared > 0, "  every resolved probe is the decoder's pixel, byte for byte, cursor row " + std::to_string( cursor ) + " included (" + std::to_string( compared ) + " compared, " + std::to_string( wrong ) + " wrong)" );
		//At 1080p the 4:3 rect is 1440x1080 for 320x256: 4.5 x 4.2 output
		//pixels per picture pixel, so every probe resolves. At 320x180 the
		//rect is 240x180: 0.75 x 0.7, and most cannot -- the validator has to
		//say so rather than measure a neighbour.
		const bool fine = ( w * 3 >= h * 4 ? h : w * 3 / 4 ) >= rx.Height();
		if( fine )
			Check( unresolved == 0, "  every picture pixel resolves at this raster (unresolved: " + std::to_string( unresolved ) + ")" );
		else
			Check( unresolved > 0, "  the validator fires at a raster too small to resolve the picture (" + std::to_string( unresolved ) + " of " + std::to_string( rx.Width() * rx.Height() ) + " unresolved)" );

		//Outside the rect, on Fit: transparent black.
		Mapping map = mappingFor( i.plugin, w, h );
		int outsideWrong = 0, outsideProbed = 0;
		for( int oy = 0; oy < h; oy += 7 )
			for( int ox = 0; ox < w; ox += 7 )
			{
				int px, py;
				if( map.toPicture( ox, oy, px, py ) )
					continue;
				++outsideProbed;
				const unsigned char* p = img.at( ox, oy );
				if( p[ 0 ] != 0 || p[ 1 ] != 0 || p[ 2 ] != 0 || p[ 3 ] != 0 )
					++outsideWrong;
			}
		Check( outsideProbed > 0 && outsideWrong == 0, "  the letterbox is transparent black (" + std::to_string( outsideProbed ) + " probes, " + std::to_string( outsideWrong ) + " wrong)" );
	}

	//------------------------------------------------------------------
	// Fill: the rect is the whole output, and the cursor is off.
	//------------------------------------------------------------------
	{
		Instance i( w, h );
		i.set( Slowscan::SS_SPEED, kSpeed );
		i.set( Slowscan::SS_ASPECT, float( Slowscan::kAspectFill ) );
		i.set( Slowscan::SS_CURSOR, 0.0f );
		Image img;
		for( int f = 0; f < kFrames; ++f )
			img = render( i, target, input, w, h, f / 60.0 );
		int wrong = 0, compared = 0;
		compareFrame( img, i.plugin, wrong, compared, i.plugin.CursorRowForTest() );
		Check( i.plugin.CursorRowForTest() == -1 && wrong == 0 && compared > 0, "  Fill, Cursor off: every resolved probe is the decoder's pixel (" + std::to_string( compared ) + " compared, " + std::to_string( wrong ) + " wrong)" );
	}

	//------------------------------------------------------------------
	// Mix 0 is the clip, byte for byte, whatever else is set.
	//------------------------------------------------------------------
	{
		Image a, b;
		{
			Instance i( w, h );
			i.set( Slowscan::SS_MIX, 0.0f );
			i.set( Slowscan::SS_SPEED, kSpeed );
			for( int f = 0; f < 10; ++f )
				a = render( i, target, input, w, h, f / 60.0 );
		}
		{
			Instance i( w, h );
			i.set( Slowscan::SS_MIX, 0.0f );
			i.set( Slowscan::SS_SPEED, 1.0f );
			i.set( Slowscan::SS_MODE, float( Slowscan::kModeRobot36 ) );
			i.set( Slowscan::SS_ASPECT, float( Slowscan::kAspectFill ) );
			i.set( Slowscan::SS_SNR, 0.0f );
			for( int f = 0; f < 10; ++f )
				b = render( i, target, input, w, h, f / 60.0 );
		}
		Check( a.px == b.px, "  Mix 0 is byte-identical across two very different settings" );
		//...and equal to the card: the clip is sampled at texel centres,
		//which is exact for a texture the size of the output.
		Image cardImg;
		cardImg.w  = w;
		cardImg.h  = h;
		cardImg.px = card;
		const size_t stride = static_cast< size_t >( w ) * 4;
		std::vector< unsigned char > flipped( card.size() );
		for( int y = 0; y < h; ++y )
			std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride, card.data() + static_cast< size_t >( h - 1 - y ) * stride, stride );
		Check( a.px == flipped, "  ...and byte-identical to the input" );
	}

	//------------------------------------------------------------------
	// A resize mid-run keeps the picture; a mode change reallocates it.
	//------------------------------------------------------------------
	{
		Instance i( w, h );
		i.set( Slowscan::SS_SPEED, kSpeed );
		for( int f = 0; f < 30; ++f )
			render( i, target, input, w, h, f / 60.0 );
		std::vector< uint8_t > before;
		i.plugin.EngineForTest().Rx().Composite( before );
		const int lineBefore = i.plugin.EngineForTest().Rx().Line();

		//A different output size, one frame.
		Target small;
		small.Create( 960, 540 );
		const std::vector< unsigned char > smallCard = buildCard( 960, 540 );
		const GLuint smallInput                       = makeInput( smallCard, 960, 540 );
		Image img = render( i, small, smallInput, 960, 540, 30 / 60.0 );
		std::vector< uint8_t > after;
		i.plugin.EngineForTest().Rx().Composite( after );
		const int lineAfter = i.plugin.EngineForTest().Rx().Line();

		//Rows the frame did not touch are byte-identical: everything before
		//the line that was arriving and everything after the one arriving now.
		const int pw = i.plugin.EngineForTest().Rx().Width();
		int changedOutside = 0;
		for( int row = 0; row < i.plugin.EngineForTest().Rx().Height(); ++row )
		{
			if( row >= lineBefore - 1 && row <= lineAfter + 1 )
				continue;
			if( std::memcmp( before.data() + static_cast< size_t >( row ) * pw * 3, after.data() + static_cast< size_t >( row ) * pw * 3, static_cast< size_t >( pw ) * 3 ) != 0 )
				++changedOutside;
		}
		Check( lineAfter >= lineBefore && changedOutside == 0, "  a resize to 960x540 mid-run kept every row not being written (line " + std::to_string( lineBefore ) + " -> " + std::to_string( lineAfter ) + ", rows changed elsewhere: " + std::to_string( changedOutside ) + ")" );

		for( int f = 31; f < 45; ++f )
			img = render( i, small, smallInput, 960, 540, f / 60.0 );
		int wrong = 0, compared = 0;
		compareFrame( img, i.plugin, wrong, compared, i.plugin.CursorRowForTest() );
		Check( wrong == 0 && compared > 0, "  ...and renders the picture exactly at the new size (" + std::to_string( compared ) + " compared, " + std::to_string( wrong ) + " wrong)" );

		//Now Robot 36, restarted: a 320x240 picture, a new texture.
		i.set( Slowscan::SS_MODE, float( Slowscan::kModeRobot36 ) );
		i.press( Slowscan::SS_RESTART );
		for( int f = 45; f < 90; ++f )
			img = render( i, small, smallInput, 960, 540, f / 60.0 );
		wrong = compared = 0;
		compareFrame( img, i.plugin, wrong, compared, i.plugin.CursorRowForTest() );
		const Receiver& rx = i.plugin.EngineForTest().Rx();
		Check( rx.Height() == 240 && rx.InPicture() && wrong == 0 && compared > 0, "  Robot 36 after a restart: a " + std::to_string( rx.Width() ) + "x" + std::to_string( rx.Height() ) + " picture, line " + std::to_string( rx.Line() ) + ", rendered exactly (" + std::to_string( compared ) + " compared, " + std::to_string( wrong ) + " wrong)" );

		glDeleteTextures( 1, &smallInput );
		small.Destroy();
	}

	glDeleteTextures( 1, &input );
	target.Destroy();
}

int runRender()
{
	std::printf( "render: the frame against the decoder's picture\n" );
	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::printf( "   FAIL  could not create an OpenGL 4.1 core context\n" );
		return ++g_failures;
	}
	std::printf( "\n  GL %s / %s\n", glGetString( GL_VERSION ), glGetString( GL_RENDERER ) );
	renderAt( 1920, 1080 );
	renderAt( 320, 180 );
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return g_failures;
}

//---------------------------------------------------------------------------
/// --bench: the render cost, for the record.
//---------------------------------------------------------------------------
int runBench( int frames )
{
	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::printf( "bench: could not create an OpenGL 4.1 core context\n" );
		return 1;
	}
	std::printf( "bench: %s / %s\n\n", glGetString( GL_VERSION ), glGetString( GL_RENDERER ) );
	std::printf( "  %-12s %-8s %10s %10s\n", "size", "speed", "ms/frame", "% of 60fps" );

	struct Run
	{
		int w, h;
		float speed;
		const char* label;
	};
	const Run runs[] = {
		{ 1280, 720, 0.77f, "40x" },
		{ 1920, 1080, 0.77f, "40x" },
		{ 3840, 2160, 0.77f, "40x" },
		{ 1920, 1080, 0.0f, "1x" },
		{ 1920, 1080, 1.0f, "120x" },
	};
	for( const Run& r : runs )
	{
		Target target;
		target.Create( r.w, r.h );
		const std::vector< unsigned char > picture = buildCard( r.w, r.h );
		const GLuint input                         = makeInput( picture, r.w, r.h );

		Instance i( r.w, r.h );
		i.set( Slowscan::SS_SPEED, r.speed );

		//glFinish on both sides, or this times how fast the driver accepts
		//commands rather than how fast the GPU runs them. No readback.
		for( int f = 0; f < 60; ++f )
			renderOnly( i, target, input, r.w, r.h, f / 60.0 );
		glFinish();
		const auto start = std::chrono::steady_clock::now();
		for( int f = 0; f < frames; ++f )
			renderOnly( i, target, input, r.w, r.h, ( 60 + f ) / 60.0 );
		glFinish();
		const double ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() / frames;
		std::printf( "  %4dx%-7d %-8s %10s %9s%%\n", r.w, r.h, r.label, F( ms, 3 ).c_str(), F( ms / 16.667 * 100.0, 1 ).c_str() );

		glDeleteTextures( 1, &input );
		target.Destroy();
	}
	std::printf( "\n  (each frame includes the CPU engine: see --engine for it alone)\n" );
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return 0;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"sstest -- render the slowscan chain, and check what it claims\n"
		"\n"
		"  --out PATH        write a PNG (default /tmp/slowscan.png)\n"
		"  --size WxH        output size (default 1920x1080)\n"
		"  --frames N        frames to run before capturing (default 60)\n"
		"  --set \"Name=V\"    set a parameter by its display name\n"
		"  --press \"Name@F\"  press an event parameter before frame F\n"
		"  --audio L         feed a synthetic spectrum at level L (0..1)\n"
		"  --motion          scroll the card every frame, so Live differs from Latch\n"
		"  --list            print every parameter and its default, then exit\n"
		"\n"
		"  checks that need no GL context at all:\n"
		"  --timing          every line, segment and pixel boundary is its constant\n"
		"  --slant           the lean is e * T_line / T_pixel, both signs, two modes\n"
		"  --levels          flat fields exact, a ramp within a step, the edge's tau\n"
		"  --threshold       pixel variance against SNR shows the FM knee\n"
		"  --progressive     floor( t s / T_line ) lines replaced\n"
		"  --vis             the header decodes to the mode it was sent in\n"
		"  --sync            Line Sync holds the edge within a pixel\n"
		"  --clock           a six-day host clock runs the same signal\n"
		"  --names           nothing the host will silently truncate\n"
		"  --negative        break the model and prove the checks fail\n"
		"  --engine          the CPU cost of the chain\n"
		"\n"
		"  checks that render:\n"
		"  --render          the frame against the decoder's picture, two rasters\n"
		"  --bench           720p, 1080p and 4K\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outputPath = "/tmp/slowscan.png";
	int width = 1920, height = 1080;
	int frames      = 60;
	int benchFrames = 240;
	float audioLevel = -1.0f;
	bool motion      = false;
	std::vector< std::pair< std::string, float > > overrides;
	std::vector< std::pair< std::string, int > > presses;
	std::vector< std::string > modes;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];
		auto next             = [ & ]() -> std::string { return i + 1 < argc ? argv[ ++i ] : std::string(); };

		if( arg == "--out" )
			outputPath = next();
		else if( arg == "--size" )
		{
			const std::string s = next();
			const size_t x      = s.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "sstest: --size wants WxH, got '%s'\n", s.c_str() );
				return 2;
			}
			width  = std::atoi( s.substr( 0, x ).c_str() );
			height = std::atoi( s.substr( x + 1 ).c_str() );
		}
		else if( arg == "--frames" )
			frames = std::atoi( next().c_str() );
		else if( arg == "--bench-frames" )
			benchFrames = std::atoi( next().c_str() );
		else if( arg == "--audio" )
			audioLevel = std::strtof( next().c_str(), nullptr );
		else if( arg == "--motion" )
			motion = true;
		else if( arg == "--set" )
		{
			const std::string assignment = next();
			const size_t equals          = assignment.rfind( '=' );
			if( equals == std::string::npos )
			{
				std::fprintf( stderr, "sstest: --set wants Name=Value, got '%s'\n", assignment.c_str() );
				return 2;
			}
			overrides.emplace_back( assignment.substr( 0, equals ), std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
		}
		else if( arg == "--press" )
		{
			const std::string spec = next();
			const size_t at        = spec.rfind( '@' );
			if( at == std::string::npos )
			{
				std::fprintf( stderr, "sstest: --press wants Name@Frame, got '%s'\n", spec.c_str() );
				return 2;
			}
			presses.emplace_back( spec.substr( 0, at ), std::atoi( spec.substr( at + 1 ).c_str() ) );
		}
		else if( arg == "--help" || arg == "-h" )
		{
			usage();
			return 0;
		}
		else if( arg.rfind( "--", 0 ) == 0 )
			modes.push_back( arg );
		else
		{
			std::fprintf( stderr, "sstest: unknown argument '%s'\n", arg.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 )
	{
		std::fprintf( stderr, "sstest: width, height and frames must all be positive\n" );
		return 2;
	}

	//-----------------------------------------------------------------------
	// The checks that need no context run FIRST and return before one is made.
	//-----------------------------------------------------------------------
	if( !modes.empty() )
	{
		bool ranSomething = false;
		bool needGL       = false;
		for( const std::string& mode : modes )
		{
			if( mode == "--timing" )           { runTiming( 0.0 ); ranSomething = true; }
			else if( mode == "--slant" )       { runSlant( false ); ranSomething = true; }
			else if( mode == "--levels" )      { runLevels( false, false ); ranSomething = true; }
			else if( mode == "--threshold" )   { runThreshold(); ranSomething = true; }
			else if( mode == "--progressive" ) { runProgressive(); ranSomething = true; }
			else if( mode == "--vis" )         { runVis( false ); ranSomething = true; }
			else if( mode == "--sync" )        { runSync( true ); ranSomething = true; }
			else if( mode == "--clock" )       { runClock(); ranSomething = true; }
			else if( mode == "--names" )       { runNames(); ranSomething = true; }
			else if( mode == "--negative" )    { runNegative(); ranSomething = true; }
			else if( mode == "--engine" )      { runEngineBench(); ranSomething = true; }
			else if( mode == "--render" || mode == "--bench" || mode == "--list" )
				needGL = true;
			else
			{
				std::fprintf( stderr, "sstest: unknown mode '%s'\n", mode.c_str() );
				return 2;
			}
			if( ranSomething )
				std::printf( "\n" );
		}

		if( ranSomething && !needGL )
		{
			std::printf( "%d checks, %d failed\n", g_checks, g_failures );
			return g_failures == 0 ? 0 : 1;
		}

		for( const std::string& mode : modes )
			if( mode == "--list" )
			{
				Slowscan plugin;
				for( unsigned int p = 0; p < plugin.GetNumParams(); ++p )
					std::printf( "%2u  %-20s %.3f\n", p, plugin.GetParamName( p ), plugin.GetFloatParameter( p ) );
				return 0;
			}

		for( const std::string& mode : modes )
		{
			if( mode == "--render" )
				runRender();
			else if( mode == "--bench" )
				return runBench( benchFrames );
		}

		std::printf( "\n%d checks, %d failed\n", g_checks, g_failures );
		return g_failures == 0 ? 0 : 1;
	}

	//-----------------------------------------------------------------------
	// Render a frame.
	//-----------------------------------------------------------------------
	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "sstest: could not create an OpenGL 4.1 core context\n" );
		return 1;
	}
	std::printf( "GL %s / %s\n", glGetString( GL_VERSION ), glGetString( GL_RENDERER ) );

	Target target;
	if( !target.Create( width, height ) )
	{
		std::fprintf( stderr, "sstest: output framebuffer is incomplete\n" );
		return 1;
	}

	std::vector< unsigned char > picture = buildCard( width, height );
	const GLuint input                   = makeInput( picture, width, height );

	Instance instance( width, height );
	if( !instance.ok )
		return 1;

	auto indexOf = [ & ]( const std::string& name ) -> int {
		for( unsigned int p = 0; p < instance.plugin.GetNumParams(); ++p )
		{
			const char* declared = instance.plugin.GetParamName( p );
			if( declared != nullptr && name == declared )
				return static_cast< int >( p );
		}
		return -1;
	};

	for( const auto& o : overrides )
	{
		const int index = indexOf( o.first );
		if( index < 0 )
		{
			std::fprintf( stderr, "sstest: no parameter named '%s' (try --list)\n", o.first.c_str() );
			return 2;
		}
		instance.plugin.SetFloatParameter( static_cast< unsigned int >( index ), o.second );
	}
	for( const auto& p : presses )
		if( indexOf( p.first ) < 0 )
		{
			std::fprintf( stderr, "sstest: no parameter named '%s' (try --list)\n", p.first.c_str() );
			return 2;
		}

	Image img;
	for( int f = 0; f < frames; ++f )
	{
		for( const auto& p : presses )
			if( p.second == f )
			{
				const unsigned int index = static_cast< unsigned int >( indexOf( p.first ) );
				instance.plugin.SetFloatParameter( index, 1.0f );
				instance.plugin.SetFloatParameter( index, 0.0f );
			}
		if( audioLevel >= 0.0f )
			injectSpectrum( instance.plugin, audioLevel, f / 60.0 );
		if( motion && f > 0 )
		{
			picture = buildCard( width, height, f * 8 );
			updateInput( input, picture, width, height );
		}
		img = render( instance, target, input, width, height, f / 60.0 );
	}

	const GLenum error = glGetError();
	if( error != GL_NO_ERROR )
		std::fprintf( stderr, "sstest: GL error 0x%04x during render\n", error );

	//Premultiplied out; the colour is already the over-black composite.
	for( size_t i = 3; i < img.px.size(); i += 4 )
		img.px[ i ] = 255;

	if( !writePng( outputPath, width, height, img.px ) )
	{
		std::fprintf( stderr, "sstest: could not write %s\n", outputPath.c_str() );
		return 1;
	}
	std::printf( "wrote %s (%dx%d, %d frames)\n", outputPath.c_str(), width, height, frames );

	glDeleteTextures( 1, &input );
	target.Destroy();
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return 0;
}
