#include "Slowscan.h"

//The SDK's umbrella FFGLSDK.h pulls in every other scoped binding but leaves
//this one out (SDK b1afaf9), so it has to be reached for by hand.
#include <ffglex/FFGLScopedFBOBinding.h>

#include "Controls.h"
#include "Diag.h"
#include "Shaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace ffglex;
using namespace slowscan;
using namespace slowscan::sstv;

//---------------------------------------------------------------------------
// The name is `SW Slowscan`, with the prefix every released plugin in this
// fleet carries. The FFGL name field is char[16] and is NOT null-terminated,
// so a longer name is truncated by the host without a word; this is eleven.
//---------------------------------------------------------------------------
static CFFGLPluginInfo PluginInfo(
	PluginFactory< Slowscan >,// Create method
	"SS01",                   // Plugin unique ID of maximum length 4.
	"SW Slowscan",            // Plugin name
	2,                        // API major version number
	1,                        // API minor version number
	0,                        // Plugin major version number
	1,                        // Plugin minor version number
	FF_EFFECT,                // Plugin type
	"Slow-scan television over HF. The clip is sent as audio tones - 1500 Hz black to 2300 Hz white, a line at a time with a 1200 Hz sync between lines - down a fading, echoing, hissing channel into an FM discriminator that paints each line as it arrives.\n\nEverything the hobby is known for falls out of the chain: slant from a sound card a few ppm off, the new picture overwriting the old from the top, noise streaks below the FM threshold, fades across bands of lines, a carrier on frequency pulling the picture into stripes. Audio routed in is interference on the channel.\n\nMartin M1, Scottie S1 and Robot 36, with the real timings. Speed runs the two-minute picture in as little as a second.",// Plugin description
	"slowscan FFGL effect"    // About
);

namespace
{
/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// A frame shorter than this is a duplicate call or a clock that has not
/// moved; one longer is a stall, a scrub or a dropped frame. Both are clamped
/// rather than believed: an unclamped delta after a window drag would ask the
/// engine for seconds of signal in one block.
constexpr double kMinFrameSeconds = 1.0 / 240.0;
constexpr double kMaxFrameSeconds = 1.0 / 24.0;
constexpr double kFirstFrameSeconds = 1.0 / 60.0;

/// The picture is displayed 4:3, as every decoder displays these modes:
/// Martin's and Scottie's 320x256 pixels are not square, and Robot's 320x240
/// are.
constexpr float kPictureAspect = 4.0f / 3.0f;

/// The cursor is the green a decoder's progress line usually is. Each
/// component is an exact multiple of 1/255, so the row reads back as exactly
/// (51, 255, 102) and the harness can compare it byte for byte.
constexpr float kCursorR = 51.0f / 255.0f, kCursorG = 1.0f, kCursorB = 102.0f / 255.0f;
} // namespace

Slowscan::Slowscan() :
	startTime( std::chrono::steady_clock::now() )
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The signal runs on the host's clock so a re-render of the same
	//composition produces the same picture, and so a paused transport pauses
	//the transmission.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults: a clean-ish HF path with a little fading and a slight slant,
	// at a speed where a picture takes a few seconds. An effect that is
	// unwatchable until four sliders are moved is an effect nobody keeps.
	//---------------------------------------------------------------------
	params[ SS_MODE ]            = float( kModeMartinM1 );
	params[ SS_SPEED ]           = 0.77f;//about 40x: three seconds a picture
	params[ SS_TRANSMIT ]        = float( kTransmitLatch );

	params[ SS_SNR ]             = 0.70f;//25 dB
	params[ SS_FADE_DEPTH ]      = 0.25f;
	params[ SS_FADE_RATE ]       = 0.50f;//about 0.3 Hz
	params[ SS_MULTIPATH ]       = 0.25f;//2 ms, so Multipath Level has something to do
	params[ SS_MULTIPATH_LEVEL ] = 0.0f;
	params[ SS_QRM_FREQ ]        = 0.80f;//about 1.9 kHz, in the picture band
	params[ SS_QRM_LEVEL ]       = 0.0f;
	params[ SS_AUDIO_QRM ]       = 0.0f;//nothing until audio is routed
	params[ SS_BIN_SPACING ]     = 1.0f;//logarithmic

	params[ SS_RX_BANDWIDTH ]    = 0.73f;//about 1.2 kHz
	params[ SS_SYNC ]            = float( kSyncFreeRun );
	params[ SS_CLOCK_ERROR ]     = 0.55f;//+30 ppm: a visible lean
	params[ SS_SLANT_CORRECT ]   = 0.50f;//none

	params[ SS_CURSOR ]          = 1.0f;
	params[ SS_ASPECT ]          = float( kAspectFit );
	params[ SS_MIX ]             = 1.0f;
	params[ SS_RESTART ]         = 0.0f;

	//---------------------------------------------------------------------
	// Declaration. Grouped the way Resolume shows them.
	//---------------------------------------------------------------------
	SetOptionParamInfo( SS_MODE, "Mode", 4, params[ SS_MODE ] );
	SetParamElementInfo( SS_MODE, kModeMartinM1, "Martin M1", float( kModeMartinM1 ) );
	SetParamElementInfo( SS_MODE, kModeScottieS1, "Scottie S1", float( kModeScottieS1 ) );
	SetParamElementInfo( SS_MODE, kModeRobot36, "Robot 36", float( kModeRobot36 ) );
	SetParamElementInfo( SS_MODE, kModeAutoVis, "Auto VIS", float( kModeAutoVis ) );

	SetParamInfof( SS_SPEED, "Speed", FF_TYPE_STANDARD );

	SetOptionParamInfo( SS_TRANSMIT, "Transmit", 2, params[ SS_TRANSMIT ] );
	SetParamElementInfo( SS_TRANSMIT, kTransmitLatch, "Latch", float( kTransmitLatch ) );
	SetParamElementInfo( SS_TRANSMIT, kTransmitLive, "Live", float( kTransmitLive ) );

	SetParamInfof( SS_SNR, "SNR", FF_TYPE_STANDARD );
	SetParamInfof( SS_FADE_DEPTH, "Fade Depth", FF_TYPE_STANDARD );
	SetParamInfof( SS_FADE_RATE, "Fade Rate", FF_TYPE_STANDARD );
	SetParamInfof( SS_MULTIPATH, "Multipath", FF_TYPE_STANDARD );
	SetParamInfof( SS_MULTIPATH_LEVEL, "Multipath Level", FF_TYPE_STANDARD );
	SetParamInfof( SS_QRM_FREQ, "QRM Freq", FF_TYPE_STANDARD );
	SetParamInfof( SS_QRM_LEVEL, "QRM Level", FF_TYPE_STANDARD );

	//The spectrum. Resolume fills a buffer parameter with usage FF_USAGE_FFT
	//with 64 bins per frame, and hands a plugin nothing else about the audio.
	SetBufferParamInfo( SS_AUDIO, "Audio", kAudioBins, FF_USAGE_FFT );
	for( int i = 0; i < kAudioBins; ++i )
		SetParamElementInfo( SS_AUDIO, i, "", 0.0f );

	//"Audio Interference" is eighteen characters and the house limit is
	//sixteen; QRM is the ham's word for man-made interference anyway.
	SetParamInfof( SS_AUDIO_QRM, "Audio QRM", FF_TYPE_STANDARD );

	SetOptionParamInfo( SS_BIN_SPACING, "Bin Spacing", 2, params[ SS_BIN_SPACING ] );
	SetParamElementInfo( SS_BIN_SPACING, 0, "Linear", 0.0f );
	SetParamElementInfo( SS_BIN_SPACING, 1, "Log", 1.0f );

	SetParamInfof( SS_RX_BANDWIDTH, "Rx Bandwidth", FF_TYPE_STANDARD );

	SetOptionParamInfo( SS_SYNC, "Sync", 2, params[ SS_SYNC ] );
	SetParamElementInfo( SS_SYNC, kSyncFreeRun, "Free-run", float( kSyncFreeRun ) );
	SetParamElementInfo( SS_SYNC, kSyncLineSync, "Line Sync", float( kSyncLineSync ) );

	SetParamInfof( SS_CLOCK_ERROR, "Clock Error", FF_TYPE_STANDARD );
	SetParamInfof( SS_SLANT_CORRECT, "Slant Correct", FF_TYPE_STANDARD );

	SetParamInfo( SS_CURSOR, "Cursor", FF_TYPE_BOOLEAN, params[ SS_CURSOR ] > 0.5f );

	SetOptionParamInfo( SS_ASPECT, "Aspect", 2, params[ SS_ASPECT ] );
	SetParamElementInfo( SS_ASPECT, kAspectFit, "Fit", float( kAspectFit ) );
	SetParamElementInfo( SS_ASPECT, kAspectFill, "Fill", float( kAspectFill ) );

	SetParamInfof( SS_MIX, "Mix", FF_TYPE_STANDARD );
	SetParamInfo( SS_RESTART, "Restart", FF_TYPE_EVENT, false );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( SS_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = SS_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}

	for( FFUInt32 i = SS_MODE; i <= SS_TRANSMIT; ++i )
		SetParamGroup( i, "Mode" );
	for( FFUInt32 i = SS_SNR; i <= SS_BIN_SPACING; ++i )
		SetParamGroup( i, "Channel" );
	for( FFUInt32 i = SS_RX_BANDWIDTH; i <= SS_SLANT_CORRECT; ++i )
		SetParamGroup( i, "Receiver" );
	for( FFUInt32 i = SS_CURSOR; i <= SS_RESTART; ++i )
		SetParamGroup( i, "Display" );

	FFGLLog::LogToHost( "Created slowscan effect" );

	diag::init();
}

//---------------------------------------------------------------------------
// The clock.
//---------------------------------------------------------------------------
FFResult Slowscan::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

void Slowscan::ForceSecondsClock()
{
	clockScale = 1.0;
}

double Slowscan::elapsedSeconds()
{
	// FFGL never says what unit SetTime arrives in, and hosts disagree:
	// Resolume sends MILLISECONDS (its own SDK's Particles sample divides by
	// 1000), while this repo's harness sends seconds. So measure rather than
	// assume: steady_clock says how much real time passed, the host says how
	// much host time passed, and the ratio names the unit outright.
	const double wallNow =
	    std::chrono::duration< double >( std::chrono::steady_clock::now() - startTime ).count();

	if( !hostTimeSeen )
		return wallNow;

	const double raw = hostTime;

	if( clockScale == 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
			{
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
				diag::info( std::string( "host clock is " )
				            + ( clockScale == 0.001 ? "milliseconds" : "seconds" ) );
			}
		}
	}

	lastRawTime  = raw;
	lastWallTime = wallNow;

	//Until the unit is settled, run on the real clock rather than assume one:
	//wrong in origin but right in rate.
	return clockScale != 0.0 ? raw * clockScale : wallNow;
}

//---------------------------------------------------------------------------
EngineParams Slowscan::resolve() const
{
	EngineParams p;
	const int mode = static_cast< int >( std::lround( params[ SS_MODE ] ) );
	p.mode         = mode == kModeAutoVis ? -1 : std::clamp( mode, 0, kModeCount - 1 );
	p.speed        = controls::Speed( params[ SS_SPEED ] );
	p.live         = std::lround( params[ SS_TRANSMIT ] ) == kTransmitLive;

	p.channel.snrDb            = controls::SnrDb( params[ SS_SNR ] );
	p.channel.fadeDepth        = controls::FadeDepth( params[ SS_FADE_DEPTH ] );
	p.channel.fadeRateHz       = controls::FadeRateHz( params[ SS_FADE_RATE ] );
	p.channel.multipathSeconds = controls::MultipathSeconds( params[ SS_MULTIPATH ] );
	p.channel.multipathLevel   = controls::MultipathLevel( params[ SS_MULTIPATH_LEVEL ] );
	p.channel.qrmHz            = controls::QrmHz( params[ SS_QRM_FREQ ] );
	p.channel.qrmLevel         = controls::QrmLevel( params[ SS_QRM_LEVEL ] );
	p.channel.audioLevel       = controls::AudioLevel( params[ SS_AUDIO_QRM ] );
	p.channel.binSpacing       = std::lround( params[ SS_BIN_SPACING ] ) == 0 ? 0 : 1;

	p.rxBandwidthHz = controls::RxBandwidthHz( params[ SS_RX_BANDWIDTH ] );
	p.lineSync      = std::lround( params[ SS_SYNC ] ) == kSyncLineSync;
	//Slant Correct subtracts a set ppm from the receiver's clock, which is
	//exactly what the decoders' slant adjustment does.
	p.clockErrorPpm = controls::Ppm( params[ SS_CLOCK_ERROR ] ) - controls::Ppm( params[ SS_SLANT_CORRECT ] );
	return p;
}

EngineParams Slowscan::ResolvedForTest() const
{
	return resolve();
}

double Slowscan::frameSecondsFor( double seconds )
{
	//The duration of the frame just entered, from the difference of two clock
	//readings, clamped. Double throughout: Resolume's clock is milliseconds
	//since the composition opened, past 499 million after six days, where a
	//float resolves only ~0.03 s and a 1/60 s frame would read as 0 or 0.03.
	double frameSeconds = kFirstFrameSeconds;
	if( lastSeconds >= 0.0 )
		frameSeconds = std::clamp( seconds - lastSeconds, kMinFrameSeconds, kMaxFrameSeconds );
	lastSeconds = seconds;
	return frameSeconds;
}

double Slowscan::FrameSecondsForTest( double seconds )
{
	return frameSecondsFor( seconds );
}

void Slowscan::pictureRect( int outputW, int outputH, float& x, float& y, float& w, float& h ) const
{
	const float ow = std::max( 1.0f, static_cast< float >( outputW ) );
	const float oh = std::max( 1.0f, static_cast< float >( outputH ) );
	if( std::lround( params[ SS_ASPECT ] ) == kAspectFill )
	{
		x = 0.0f;
		y = 0.0f;
		w = ow;
		h = oh;
		return;
	}
	//The largest 4:3 box that fits, centred.
	if( ow / oh > kPictureAspect )
	{
		h = oh;
		w = oh * kPictureAspect;
	}
	else
	{
		w = ow;
		h = ow / kPictureAspect;
	}
	x = ( ow - w ) * 0.5f;
	y = ( oh - h ) * 0.5f;
}

void Slowscan::PictureRectForTest( int outputW, int outputH, float& x, float& y, float& w, float& h ) const
{
	pictureRect( outputW, outputH, x, y, w, h );
}

int Slowscan::CursorRowForTest() const
{
	if( params[ SS_CURSOR ] < 0.5f || !engine.Rx().InPicture() )
		return -1;
	//The row below the one arriving, so the arriving pixels stay visible.
	const int row = engine.Rx().Line() + 1;
	return row < engine.Rx().Height() ? row : -1;
}

void Slowscan::readAudio()
{
	const ParamInfo* info = FindParamInfo( SS_AUDIO );
	if( info == nullptr )
		return;
	const size_t bins = std::min< size_t >( info->elements.size(), kAudioBins );
	for( size_t i = 0; i < bins; ++i )
		audioBins[ i ] = std::max( 0.0f, info->elements[ i ].value );
	engine.SetAudioBins( audioBins, static_cast< int >( bins ) );
}

//---------------------------------------------------------------------------
bool Slowscan::compileShaders()
{
	struct Stage
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	};

	const Stage stages[] = {
		{ &readbackShader, shaders::kReadbackFragment, "readback" },
		{ &composeShader, shaders::kComposeFragment, "compose" },
	};

	for( const Stage& stage : stages )
	{
		if( !stage.shader->Compile( shaders::kVertex, stage.fragment ) )
		{
			//Returning FF_FAIL from InitGL is invisible to the operator: the
			//effect simply does nothing in Resolume, with no message anywhere.
			//This line is the only record of which stage it was.
			diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
			FFGLLog::LogToHost( "slowscan: shader failed to compile" );
			return false;
		}
	}
	return true;
}

bool Slowscan::ensureReadback( int w, int h )
{
	if( !readbackBuffer.Ensure( w, h, GL_RGBA8 ) )
		return false;

	if( pbo[ 0 ] != 0 && pboWidth == w && pboHeight == h )
		return true;

	if( pbo[ 0 ] != 0 )
		glDeleteBuffers( 2, pbo );

	//Two pixel-pack buffers: this frame's readback is issued into one while
	//the one issued last frame is mapped, so glReadPixels never waits for
	//the GPU. The picture the transmitter sees is one frame old, which is
	//nothing against a line that takes a third of a second even at 120x.
	glGenBuffers( 2, pbo );
	for( int i = 0; i < 2; ++i )
	{
		glBindBuffer( GL_PIXEL_PACK_BUFFER, pbo[ i ] );
		glBufferData( GL_PIXEL_PACK_BUFFER, static_cast< GLsizeiptr >( w ) * h * 4, nullptr, GL_STREAM_READ );
	}
	glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
	pboWidth  = w;
	pboHeight = h;
	pboIndex  = 0;
	pboPrimed = false;
	return true;
}

bool Slowscan::ensurePictureTexture( int w, int h )
{
	if( pictureTexture != 0 && pictureWidth == w && pictureHeight == h )
		return true;
	if( pictureTexture != 0 )
		glDeleteTextures( 1, &pictureTexture );

	glGenTextures( 1, &pictureTexture );
	if( pictureTexture == 0 )
		return false;
	glBindTexture( GL_TEXTURE_2D, pictureTexture );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	pictureWidth  = w;
	pictureHeight = h;
	return true;
}

FFResult Slowscan::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	if( !compileShaders() )
	{
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

FFResult Slowscan::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];

	//The host's viewport, not the one InitGL was handed: Resolume changes
	//composition resolution without reinitialising the plugin. Captured up
	//front and restored before the composite, because ScopedFBOBinding
	//restores the framebuffer and ONLY the framebuffer.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );
	const int outputW = std::max( 1, static_cast< int >( hostViewport[ 2 ] ) );
	const int outputH = std::max( 1, static_cast< int >( hostViewport[ 3 ] ) );

	//------------------------------------------------------------------
	// The controls, the audio, the restart.
	//------------------------------------------------------------------
	engine.SetParams( resolve() );
	readAudio();
	if( restartPending )
	{
		restartPending = false;
		engine.Restart();
	}

	//------------------------------------------------------------------
	// Allocate BEFORE anything is bound. FFGLFBO::Initialise sizes its
	// colour texture under a scoped binding, and every ffglex::Scoped*
	// clears its binding to 0 on exit rather than restoring it.
	//------------------------------------------------------------------
	const int txW = engine.Tx().TxWidth();
	const int txH = engine.Tx().TxHeight();
	if( !ensureReadback( txW, txH ) )
	{
		diag::error( "could not allocate the readback buffer" );
		return FF_FAIL;
	}
	if( !ensurePictureTexture( engine.Rx().Width(), engine.Rx().Height() ) )
	{
		diag::error( "could not allocate the picture texture" );
		return FF_FAIL;
	}

	//------------------------------------------------------------------
	// 1. The clip, down onto the transmitter's grid, and back to the CPU.
	//------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( readbackBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		readbackBuffer.ResizeViewPort();
		ScopedShaderBinding shader( readbackShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( input.Handle );

		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );
		readbackShader.Set( "InputTexture", 0 );
		readbackShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		readbackShader.Set( "InputSize", static_cast< float >( input.Width ), static_cast< float >( input.Height ) );
		readbackShader.Set( "TargetSize", static_cast< float >( txW ), static_cast< float >( txH ) );
		quad.Draw();

		//Issue this frame's readback into one buffer. Into a bound
		//PIXEL_PACK_BUFFER glReadPixels returns at once: it is a DMA
		//request, not a stall.
		glBindBuffer( GL_PIXEL_PACK_BUFFER, pbo[ pboIndex ] );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, txW, txH, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
		glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
	}

	//...and map the one issued last frame, which the GPU has long finished.
	const int other = 1 - pboIndex;
	if( pboPrimed )
	{
		glBindBuffer( GL_PIXEL_PACK_BUFFER, pbo[ other ] );
		const void* mapped = glMapBuffer( GL_PIXEL_PACK_BUFFER, GL_READ_ONLY );
		if( mapped != nullptr )
		{
			engine.SetSource( static_cast< const uint8_t* >( mapped ), txW, txH );
			glUnmapBuffer( GL_PIXEL_PACK_BUFFER );
		}
		glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
	}
	pboIndex  = other;
	pboPrimed = true;

	//------------------------------------------------------------------
	// 2. The signal. As many samples as this frame is worth.
	//------------------------------------------------------------------
	engine.Run( engine.SamplesForFrame( frameSecondsFor( elapsedSeconds() ) ) );

	//------------------------------------------------------------------
	// 3. The received picture, up to the GPU.
	//------------------------------------------------------------------
	if( !ensurePictureTexture( engine.Rx().Width(), engine.Rx().Height() ) )
		return FF_FAIL;
	engine.Rx().Composite( pictureRgb );
	glBindTexture( GL_TEXTURE_2D, pictureTexture );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, pictureWidth, pictureHeight, GL_RGB, GL_UNSIGNED_BYTE, pictureRgb.data() );
	glBindTexture( GL_TEXTURE_2D, 0 );

	//------------------------------------------------------------------
	// 4. The output.
	//------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( composeShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, input.Handle );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, pictureTexture );
		glActiveTexture( GL_TEXTURE0 );

		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );
		composeShader.Set( "InputTexture", 0 );
		composeShader.Set( "PictureTexture", 1 );
		composeShader.Set( "MaxUV", 1.0f, 1.0f );
		composeShader.Set( "InputMaxUV", maxCoords.s, maxCoords.t );

		float rx, ry, rw, rh;
		pictureRect( outputW, outputH, rx, ry, rw, rh );
		//The rect is computed with y from the top; the shader's uv has y from
		//the bottom, so the origin is the rect's BOTTOM edge measured up.
		composeShader.Set( "RectOrigin", rx / outputW, ( outputH - ry - rh ) / outputH );
		composeShader.Set( "RectSize", rw / outputW, rh / outputH );
		glUniform2i( glGetUniformLocation( composeShader.GetGLID(), "PictureSize" ), pictureWidth, pictureHeight );
		composeShader.Set( "CursorRow", CursorRowForTest() );
		composeShader.Set( "CursorColour", kCursorR, kCursorG, kCursorB );
		composeShader.Set( "Mix", std::clamp( params[ SS_MIX ], 0.0f, 1.0f ) );

		quad.Draw();

		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	return FF_SUCCESS;
}

void Slowscan::releaseBuffers()
{
	readbackBuffer.Destroy();
	if( pbo[ 0 ] != 0 )
	{
		glDeleteBuffers( 2, pbo );
		pbo[ 0 ] = pbo[ 1 ] = 0;
	}
	pboWidth = pboHeight = 0;
	pboPrimed = false;
	if( pictureTexture != 0 )
	{
		glDeleteTextures( 1, &pictureTexture );
		pictureTexture = 0;
	}
	pictureWidth = pictureHeight = 0;
}

FFResult Slowscan::DeInitGL()
{
	readbackShader.FreeGLResources();
	composeShader.FreeGLResources();
	quad.Release();
	releaseBuffers();
	return FF_SUCCESS;
}

FFResult Slowscan::SetFloatParameter( unsigned int index, float value )
{
	if( index >= SS_COUNT )
		return FF_FAIL;

	if( index >= SS_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - SS_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	//An event arrives as 1.0 on press and 0.0 on release. The press is
	//remembered until the next frame; the value itself is not kept.
	if( index == SS_RESTART )
	{
		if( value >= 0.5f )
			restartPending = true;
		return FF_SUCCESS;
	}

	params[ index ] = value;
	return FF_SUCCESS;
}

float Slowscan::GetFloatParameter( unsigned int index )
{
	if( index >= SS_COUNT )
		return 0.0f;
	return params[ index ];
}

FFResult Slowscan::SetTextParameter( unsigned int index, const char* )
{
	// The About text line is display-only, but the SDK's FF_INSTANTIATE_GL
	// pushes every declared default into a fresh instance -- including this
	// one, through here -- and destroys the instance on the first FF_FAIL.
	if( index == SS_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, nullptr );
}

char* Slowscan::GetTextParameter( unsigned int index )
{
	if( index == SS_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}
