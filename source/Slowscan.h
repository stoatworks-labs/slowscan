#pragma once

#include <FFGLSDK.h>

#include <chrono>
#include <string>
#include <vector>

#include "PassBuffer.h"
#include "StoatworksAboutParams.h"
#include "sstv/Engine.h"

/**
	slowscan -- SSTV over HF, as an FFGL effect.

	The clip is sent the way a slow-scan television station sends a still:
	one audio tone per pixel, 1500 Hz for black to 2300 Hz for white, a line
	at a time with a 1200 Hz sync pulse between lines, down an HF channel
	that fades, echoes and hisses, into an FM discriminator that paints each
	line as it arrives.

	The whole chain -- source, tones, channel, discriminator, line timing,
	picture -- is CPU code in `source/sstv/`, run at 11025 Hz. The GPU does
	two things only: reads the clip back down to the mode's resolution for the
	transmitter, and puts the received picture on the output. See AGENTS.md
	for why, and for the traps.
*/
class Slowscan : public CFFGLPlugin
{
public:
	Slowscan();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char* GetTextParameter( unsigned int index ) override;
	float GetFloatParameter( unsigned int index ) override;

	FFResult SetTime( double time ) override;

	/// Everything the operator can reach, in the order Resolume shows them.
	/// Public because the harness names them.
	enum ParamID : FFUInt32
	{
		//Mode
		SS_MODE,
		SS_SPEED,
		SS_TRANSMIT,

		//Channel
		SS_SNR,
		SS_FADE_DEPTH,
		SS_FADE_RATE,
		SS_MULTIPATH,
		SS_MULTIPATH_LEVEL,
		SS_QRM_FREQ,
		SS_QRM_LEVEL,
		SS_AUDIO,//the host's FFT
		SS_AUDIO_QRM,
		SS_BIN_SPACING,

		//Receiver
		SS_RX_BANDWIDTH,
		SS_SYNC,
		SS_CLOCK_ERROR,
		SS_SLANT_CORRECT,

		//Display
		SS_CURSOR,
		SS_ASPECT,
		SS_MIX,
		SS_RESTART,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. See StoatworksAboutParams.h.
		SS_ABOUT_FIRST,
		SS_COUNT = SS_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	enum ModeOption
	{
		kModeMartinM1  = 0,
		kModeScottieS1 = 1,
		kModeRobot36   = 2,
		kModeAutoVis   = 3,
	};

	enum TransmitOption
	{
		kTransmitLatch = 0,
		kTransmitLive  = 1,
	};

	enum SyncOption
	{
		kSyncFreeRun  = 0,
		kSyncLineSync = 1,
	};

	enum AspectOption
	{
		kAspectFit  = 0,
		kAspectFill = 1,
	};

	/// The number of FFT bins asked of the host. The fleet's figure.
	static constexpr int kAudioBins = 64;

	//-----------------------------------------------------------------------
	// Test hooks.
	//-----------------------------------------------------------------------

	/// The harness declares its clock unit rather than leaving elapsedSeconds()
	/// to infer one from wall time, which a harness rendering as fast as the
	/// GPU allows cannot measure.
	void ForceSecondsClock();

	/// The chain itself, so the harness can step it with no GL at all.
	slowscan::sstv::Engine& EngineForTest() { return engine; }
	const slowscan::sstv::Engine& EngineForTestConst() const { return engine; }

	/// The engine parameters the current controls resolve to.
	slowscan::sstv::EngineParams ResolvedForTest() const;

	/// Where the picture sits on an output of this size, in output pixels
	/// with y from the TOP. The same arithmetic the compose pass is handed.
	void PictureRectForTest( int outputW, int outputH, float& x, float& y, float& w, float& h ) const;

	/// The row the cursor would mark this frame, or -1.
	int CursorRowForTest() const;

	/// The frame duration the plugin would derive from this host clock
	/// reading, in seconds -- the one place video time enters the signal.
	double FrameSecondsForTest( double seconds );

	/// The normalised clock, as ProcessOpenGL reads it after SetTime.
	double ElapsedSecondsForTest() { return elapsedSeconds(); }

	/// Negative control: take the frame's duration as a difference of two
	/// FLOAT clock readings, the trap. `--clock` must see it.
	void DebugFloatClock( bool on ) { debugFloatClock = on; }
	/// Negative control: upload the picture shifted down by this many rows.
	/// `--render` must see the frame disagree with the decoder.
	void DebugUploadRowOffset( int rows ) { debugRowOffset = rows; }

private:
	bool compileShaders();
	double frameSecondsFor( double seconds );
	bool ensureReadback( int w, int h );
	bool ensurePictureTexture( int w, int h );
	void releaseBuffers();
	double elapsedSeconds();
	void readAudio();
	slowscan::sstv::EngineParams resolve() const;
	void pictureRect( int outputW, int outputH, float& x, float& y, float& w, float& h ) const;

	ffglex::FFGLShader readbackShader;
	ffglex::FFGLShader composeShader;
	ffglex::FFGLScreenQuad quad;

	slowscan::PassBuffer readbackBuffer;
	GLuint pbo[ 2 ]  = { 0, 0 };
	int pboWidth     = 0;
	int pboHeight    = 0;
	int pboIndex     = 0;
	bool pboPrimed   = false;

	GLuint pictureTexture = 0;
	int pictureWidth      = 0;
	int pictureHeight     = 0;
	std::vector< uint8_t > pictureRgb;

	slowscan::sstv::Engine engine;
	float audioBins[ kAudioBins ] = {};
	bool restartPending = false;

	//--- the clock ----------------------------------------------------------
	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double lastRawTime  = -1.0;
	double lastWallTime = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	bool hostTimeSeen   = false;
	double lastSeconds  = -1.0;
	std::chrono::steady_clock::time_point startTime;

	float params[ SS_COUNT ] = {};

	bool debugFloatClock = false;
	int debugRowOffset   = 0;

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
