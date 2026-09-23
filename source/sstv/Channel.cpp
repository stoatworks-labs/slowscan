#include "Channel.h"

#include "Modes.h"

#include <algorithm>
#include <cmath>

namespace slowscan::sstv
{
namespace
{
constexpr double kTwoPi = 6.283185307179586;

/// How many samples between renormalising the rotating phasors. A complex
/// multiply drifts in magnitude by about one ulp per step; after 4096 steps
/// that is 1e-12, and the renormalisation is a sqrt every 4096 samples.
constexpr int kRenormaliseEvery = 4096;
} // namespace

//---------------------------------------------------------------------------
void Channel::Phasor::SetRateHz( double hz )
{
	const double w = kTwoPi * hz / kSampleRateHz;
	stepRe         = std::cos( w );
	stepIm         = std::sin( w );
}

void Channel::Phasor::Renormalise()
{
	const double mag = std::sqrt( re * re + im * im );
	if( mag > 0.0 )
	{
		re /= mag;
		im /= mag;
	}
	else
	{
		re = 1.0;
		im = 0.0;
	}
}

//---------------------------------------------------------------------------
void Channel::Fader::Seed( Rng& rng, double dopplerHz )
{
	//Clarke's model: N rays arriving from angles spread round the circle,
	//each Doppler-shifted by f_d cos( angle ), with a random phase. The
	//angles are offset by a random rotation so the two paths, seeded from the
	//same generator in turn, do not share a ray.
	const double rotation = rng.Unit() * kTwoPi;
	for( int n = 0; n < kRays; ++n )
	{
		angles[ n ]   = rotation + ( kTwoPi * ( n + 0.5 ) ) / kRays;
		const double p = rng.Unit() * kTwoPi;
		rays[ n ].re  = std::cos( p );
		rays[ n ].im  = std::sin( p );
	}
	SetDoppler( dopplerHz );
}

void Channel::Fader::SetDoppler( double dopplerHz )
{
	for( int n = 0; n < kRays; ++n )
		rays[ n ].SetRateHz( dopplerHz * std::cos( angles[ n ] ) );
}

void Channel::Fader::Gain( double& re, double& im ) const
{
	double sr = 0.0, si = 0.0;
	for( int n = 0; n < kRays; ++n )
	{
		sr += rays[ n ].re;
		si += rays[ n ].im;
	}
	//1/sqrt(N): the mean power of the sum is 1, so a full fade neither adds
	//nor removes signal on average -- it only redistributes it in time.
	const double k = 1.0 / std::sqrt( static_cast< double >( kRays ) );
	re             = sr * k;
	im             = si * k;
}

void Channel::Fader::Advance()
{
	for( int n = 0; n < kRays; ++n )
		rays[ n ].Advance();
}

//---------------------------------------------------------------------------
Channel::Channel() :
	delayRe( kDelayMask + 1, 0.0 ),
	delayIm( kDelayMask + 1, 0.0 )
{
	Reset();
	SetParams( params );
}

void Channel::Reset()
{
	rng.Seed( 0x5354565F484620ull );//"STV_HF "
	direct.Seed( rng, params.fadeRateHz );
	reflected.Seed( rng, params.fadeRateHz );
	qrm.re = 1.0;
	qrm.im = 0.0;
	std::fill( delayRe.begin(), delayRe.end(), 0.0 );
	std::fill( delayIm.begin(), delayIm.end(), 0.0 );
	delayWrite = 0;
	for( int i = 0; i < kBins; ++i )
	{
		audio[ i ].re     = 1.0;
		audio[ i ].im     = 0.0;
		audioAmp[ i ]     = 0.0;
		audioTarget[ i ]  = 0.0;
		audioSlope[ i ]   = 0.0;
		audioActive[ i ]  = false;
	}
	audioRamp        = 0;
	sinceRenormalise = 0;
}

void Channel::SetParams( const Params& p )
{
	const bool rateChanged = p.fadeRateHz != params.fadeRateHz;
	const bool qrmChanged  = p.qrmHz != params.qrmHz;
	const bool binsChanged = p.binSpacing != params.binSpacing;
	params                 = p;

	if( rateChanged )
	{
		direct.SetDoppler( params.fadeRateHz );
		reflected.SetDoppler( params.fadeRateHz );
	}
	if( qrmChanged )
		qrm.SetRateHz( params.qrmHz );

	delaySamples = std::clamp( static_cast< int >( std::lround( params.multipathSeconds * kSampleRateHz ) ), 0, kDelayMask );

	//------------------------------------------------------------------
	// SNR in a 3 kHz bandwidth. The tone has unit amplitude in its analytic
	// form, so the real audio is a cosine of amplitude 1 and power 1/2. White
	// noise of variance sigma^2 on a real signal sampled at fs spreads that
	// power over 0..fs/2, so the share inside 3 kHz is sigma^2 * 3000 / (fs/2).
	//
	//     SNR = (1/2) / ( sigma^2 * 3000 / 5512.5 )
	//     sigma^2 = (1/2) * (5512.5 / 3000) / 10^(SNR/10)
	//------------------------------------------------------------------
	constexpr double kSnrBandwidthHz = 3000.0;
	const double noiseShare          = kSnrBandwidthHz / ( kSampleRateHz / 2.0 );
	const double noisePower          = 0.5 / noiseShare / std::pow( 10.0, params.snrDb / 10.0 );
	noiseSigma                       = std::sqrt( noisePower );

	if( binsChanged || !binsInitialised )
	{
		binsInitialised = true;
		for( int i = 0; i < kBins; ++i )
		{
			const double t = ( i + 0.5 ) / kBins;
			const double hz = params.binSpacing == 0
			                      ? t * kLinearNyquistHz
			                      : kLogLowHz * std::pow( kLogHighHz / kLogLowHz, t );
			audio[ i ].SetRateHz( hz );
		}
	}
}

void Channel::SetAudioBins( const float* bins, int count, int rampSamples )
{
	audioRamp = std::max( 1, rampSamples );
	for( int i = 0; i < kBins; ++i )
	{
		const double target = ( bins != nullptr && i < count ) ? std::max( 0.0, static_cast< double >( bins[ i ] ) ) : 0.0;
		audioTarget[ i ]    = target;
		audioSlope[ i ]     = ( target - audioAmp[ i ] ) / audioRamp;
		audioActive[ i ]    = target > 1e-5 || audioAmp[ i ] > 1e-5;
	}
}

void Channel::DirectGain( double& re, double& im ) const
{
	double fr, fi;
	direct.Gain( fr, fi );
	const double d = params.fadeDepth;
	re             = ( 1.0 - d ) + d * fr;
	im             = d * fi;
}

double Channel::Step( double re, double im )
{
	//The direct path, faded.
	double gr, gi;
	DirectGain( gr, gi );
	double outRe = gr * re - gi * im;
	double outIm = gr * im + gi * re;

	//The second path: the tone as it was `delaySamples` ago, through its own
	//fade, at its own level. The delay line always runs so a change of delay
	//has history to read.
	delayRe[ delayWrite ] = re;
	delayIm[ delayWrite ] = im;
	if( params.multipathLevel > 0.0 && delaySamples > 0 )
	{
		const int readAt = ( delayWrite - delaySamples ) & kDelayMask;
		const double dr  = delayRe[ readAt ];
		const double di  = delayIm[ readAt ];
		double fr, fi;
		reflected.Gain( fr, fi );
		const double d  = params.fadeDepth;
		const double hr = ( 1.0 - d ) + d * fr;
		const double hi = d * fi;
		outRe += params.multipathLevel * ( hr * dr - hi * di );
		outIm += params.multipathLevel * ( hr * di + hi * dr );
	}
	delayWrite = ( delayWrite + 1 ) & kDelayMask;

	//An interfering carrier.
	if( params.qrmLevel > 0.0 )
	{
		outRe += params.qrmLevel * qrm.re;
		outIm += params.qrmLevel * qrm.im;
	}

	//The host's spectrum, as tones.
	if( params.audioLevel > 0.0 )
	{
		for( int i = 0; i < kBins; ++i )
		{
			if( !audioActive[ i ] )
				continue;
			if( audioRamp > 0 )
				audioAmp[ i ] += audioSlope[ i ];
			const double a = params.audioLevel * audioAmp[ i ];
			outRe += a * audio[ i ].re;
			outIm += a * audio[ i ].im;
			audio[ i ].Advance();
		}
		if( audioRamp > 0 && --audioRamp == 0 )
			for( int i = 0; i < kBins; ++i )
			{
				audioAmp[ i ]    = audioTarget[ i ];
				audioActive[ i ] = audioAmp[ i ] > 1e-5;
			}
	}

	direct.Advance();
	if( params.multipathLevel > 0.0 )
		reflected.Advance();
	qrm.Advance();

	if( ++sinceRenormalise >= kRenormaliseEvery )
	{
		sinceRenormalise = 0;
		for( int n = 0; n < kRays; ++n )
		{
			direct.rays[ n ].Renormalise();
			reflected.rays[ n ].Renormalise();
		}
		qrm.Renormalise();
		for( int i = 0; i < kBins; ++i )
			audio[ i ].Renormalise();
	}

	//The audio a receiver hears is the real part, and the noise is added to
	//that: noise on the imaginary part would be noise nobody can hear.
	(void)outIm;
	return outRe + noiseSigma * rng.Gaussian();
}

} // namespace slowscan::sstv
