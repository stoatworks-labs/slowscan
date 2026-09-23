#include "Engine.h"

#include <algorithm>
#include <cmath>

namespace slowscan::sstv
{
Engine::Engine()
{
	SetParams( params );
	lastTxPictures      = tx.PicturesStarted();
	rxPicturesAtTxStart = rx.PicturesStarted();
}

void Engine::SetParams( const EngineParams& p )
{
	params = p;
	tx.SetMode( params.mode );
	tx.SetLive( params.live );
	Channel::Params c = params.channel;
	if( debugFadeTimesSpeed )
		c.fadeRateHz *= params.speed;
	ch.SetParams( c );

	Receiver::Params r;
	r.bandwidthHz   = params.rxBandwidthHz;
	r.lineSync      = params.lineSync;
	r.clockErrorPpm = params.clockErrorPpm;
	r.mode          = params.mode;
	rx.SetParams( r );
}

void Engine::SetAudioBins( const float* bins, int count )
{
	ch.SetAudioBins( bins, count );
}

void Engine::SetSource( const uint8_t* rgba, int width, int height )
{
	tx.SetSource( rgba, width, height );
}

void Engine::Restart()
{
	tx.Restart();
	rx.Reset();
	lastTxPictures      = tx.PicturesStarted();
	rxPicturesAtTxStart = rx.PicturesStarted();
}

int Engine::SamplesForFrame( double frameSeconds )
{
	const double wanted = frameSeconds * params.speed * ( 1.0 + debugSpeedError ) * kSampleRateHz + carry;
	const int whole     = static_cast< int >( std::floor( wanted ) );
	carry               = wanted - whole;
	return std::clamp( whole, 0, kMaxBlock );
}

int64_t Engine::LineStartSample( int mode, int line )
{
	const ModeSpec& m = Mode( mode );
	return FirstSampleAtOrAfter( kVisMicros + m.leadInMicros + static_cast< int64_t >( line ) * m.lineMicros );
}

void Engine::manualStartIfNeeded()
{
	//A new picture has begun at the station: remember whether the decoder
	//had started one by then, so it can be told apart from this one.
	if( tx.PicturesStarted() != lastTxPictures )
	{
		lastTxPictures      = tx.PicturesStarted();
		rxPicturesAtTxStart = rx.PicturesStarted();
	}

	//The header did not decode -- too much noise, or a mode the decoder was
	//not expecting -- and two lines have gone by. A real operator presses the
	//start button in their software; this does the same, from the station's
	//own clock, so a picture below the FM threshold is streaks rather than a
	//frozen frame. The FIR's group delay is taken off so the manual start
	//lands where a decoded one would have.
	if( rx.InPicture() || rx.PicturesStarted() != rxPicturesAtTxStart )
		return;
	if( tx.Line() < 2 )
		return;

	const int mode          = tx.CurrentMode();
	const int64_t lineStart = LineStartSample( mode, tx.Line() );
	const double within     = static_cast< double >( tx.PictureSample() - lineStart ) - Receiver::kGroupDelay;
	const double rxStep     = 1.0 + params.clockErrorPpm * 1e-6;
	rx.ForceStart( mode, tx.Line(), std::max( 0.0, within ) * rxStep );
}

void Engine::Run( int samples )
{
	for( int i = 0; i < samples; ++i )
	{
		double re, im;
		tx.Step( re, im );
		lastAudio = ch.Step( re, im );
		rx.Step( lastAudio );
		++samplesRun;

		manualStartIfNeeded();
	}
}

} // namespace slowscan::sstv
