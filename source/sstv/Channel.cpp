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
	//SetParams only retunes what CHANGED, and nothing has: the defaults are
	//the defaults. Without this the carrier sat at 0 Hz until the first
	//time QRM Freq moved -- a DC offset rather than a carrier.
	qrm.SetRateHz( params.qrmHz );
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
	audioRng.Seed( 0x415544494F514D00ull );//"AUDIOQM"
	std::fill( std::begin( audioNoise ), std::end( audioNoise ), 0.0 );
	audioWrite = 0;
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

	noiseSigma = NoiseSigma( params.snrDb - debugSnrOffsetDb );

	if( binsChanged || !binsInitialised )
	{
		binsInitialised = true;
		designAudioFilter();
	}
}

double Channel::NoiseSigma( double snrDb )
{
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
	const double noisePower          = 0.5 / noiseShare / std::pow( 10.0, snrDb / 10.0 );
	return std::sqrt( noisePower );
}

void Channel::BinRangeHz( int spacing, int i, double& lowHz, double& highHz )
{
	if( spacing == 0 )
	{
		const double width = kLinearNyquistHz / kBins;
		lowHz              = i * width;
		highHz             = ( i + 1 ) * width;
	}
	else
	{
		const double ratio = kLogHighHz / kLogLowHz;
		lowHz              = kLogLowHz * std::pow( ratio, static_cast< double >( i ) / kBins );
		highHz             = kLogLowHz * std::pow( ratio, static_cast< double >( i + 1 ) / kBins );
	}
}

void Channel::SetAudioBins( const float* values, int count )
{
	bool changed = false;
	for( int i = 0; i < kBins; ++i )
	{
		const float v = ( values != nullptr && i < count ) ? std::max( 0.0f, values[ i ] ) : 0.0f;
		changed       = changed || v != bins[ i ];
		bins[ i ]     = v;
	}
	if( changed )
		designAudioFilter();
}

void Channel::designAudioFilter()
{
	//------------------------------------------------------------------
	// Frequency sampling. The response is specified at L/2 + 1 frequencies
	// k fs / L, each the POWER average of the bins over the cell of width
	// fs / L round it (so a log bin narrower than a cell still counts, in
	// proportion), and the taps are its inverse DFT: zero-phase, shifted by
	// L/2 to be causal. Parseval makes the noise power exact whatever the
	// ripple between the sampled frequencies does:
	//
	//     sum h^2 = (1/L) sum_k A_k^2
	//
	// Scaled by sqrt(1/2): a bin of 1 everywhere is white noise of power 1/2,
	// the tone's own power, so Audio QRM at 1 with a full-scale spectrum is
	// interference as loud as the picture.
	//------------------------------------------------------------------
	constexpr int L        = kAudioTaps;
	const double cell      = kSampleRateHz / L;
	const double nyquistHz = kSampleRateHz / 2.0;
	double amplitude[ L / 2 + 1 ];
	audioActive = false;
	for( int k = 0; k <= L / 2; ++k )
	{
		const double lo   = std::max( 0.0, k * cell - cell / 2.0 );
		const double hi   = std::min( nyquistHz, k * cell + cell / 2.0 );
		double power      = 0.0;
		for( int i = 0; i < kBins; ++i )
		{
			if( bins[ i ] <= 0.0f )
				continue;
			double bl, bh;
			BinRangeHz( params.binSpacing, i, bl, bh );
			const double overlap = std::min( hi, bh ) - std::max( lo, bl );
			if( overlap > 0.0 )
				power += static_cast< double >( bins[ i ] ) * bins[ i ] * overlap;
		}
		amplitude[ k ] = std::sqrt( power / ( hi - lo ) );
		audioActive    = audioActive || amplitude[ k ] > 0.0;
	}

	for( int n = 0; n < L; ++n )
	{
		const int m = n - L / 2;
		double h    = amplitude[ 0 ] + amplitude[ L / 2 ] * ( ( m & 1 ) ? -1.0 : 1.0 );
		for( int k = 1; k < L / 2; ++k )
			h += 2.0 * amplitude[ k ] * std::cos( kTwoPi * k * m / L );
		audioTaps[ n ] = std::sqrt( 0.5 ) * h / L;
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
	}

	//The audio a receiver hears is the real part, and the noise is added to
	//that: noise on the imaginary part would be noise nobody can hear.
	(void)outIm;
	double out = outRe + noiseSigma * rng.Gaussian();

	//The host's spectrum, as noise it shapes. Real audio added to real audio.
	if( audioActive && params.audioLevel > 0.0 )
	{
		audioNoise[ audioWrite ] = audioRng.Gaussian();
		double y                 = 0.0;
		for( int k = 0; k < kAudioTaps; ++k )
			y += audioTaps[ k ] * audioNoise[ ( audioWrite - k ) & ( kAudioTaps - 1 ) ];
		audioWrite = ( audioWrite + 1 ) & ( kAudioTaps - 1 );
		out += params.audioLevel * y;
	}
	return out;
}

} // namespace slowscan::sstv
