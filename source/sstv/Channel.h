#pragma once

#include "Rng.h"

#include <cstdint>
#include <vector>

namespace slowscan::sstv
{
/**
	The HF path, one sample at a time, on the analytic signal.

	  fading      flat Rayleigh, a seeded sum-of-sinusoids Clarke model with
	              the Doppler spread the Fade Rate control sets. Fade Depth
	              blends a steady path into it, so 0 is a wire and 1 is pure
	              scatter (Rician in between).
	  multipath   a second path, delayed, at its own level and with its own
	              fading, so the two interfere -- which is what makes HF
	              frequency-selective rather than merely dim.
	  QRM         an interfering carrier at a set frequency and level.
	  audio       the host's 64 FFT bins, each one a tone at the frequency the
	              Bin Spacing assumption assigns it. Resolume gives a spectrum
	              and not a waveform, and nobody has measured how those bins
	              are laid out; see AGENTS.md.
	  noise       white Gaussian, at the SNR stated in a 3 kHz bandwidth, added
	              to the real output because that is the only part a receiver
	              can hear.

	Nothing here knows about video frames or about Speed. Every process is
	defined per sample of the 11025 Hz signal, so running the signal faster
	changes nothing about what a sample sees.
*/
class Channel
{
public:
	struct Params
	{
		double snrDb           = 25.0;
		double fadeDepth       = 0.0;///< 0 wire .. 1 Rayleigh
		double fadeRateHz      = 0.3;///< Doppler spread
		double multipathSeconds = 0.0;
		double multipathLevel  = 0.0;
		double qrmHz           = 1900.0;
		double qrmLevel        = 0.0;
		double audioLevel      = 0.0;
		int binSpacing         = 1;///< 0 linear to Nyquist, 1 logarithmic 20 Hz..20 kHz
	};

	/// The Clarke model's ray count. Eight is the smallest number that gives
	/// an envelope whose distribution is close to Rayleigh; Jakes used it.
	static constexpr int kRays = 8;
	static constexpr int kBins = 64;
	/// The Bin Spacing assumptions, in Hz. Linear assumes a 44.1 kHz host.
	static constexpr double kLinearNyquistHz = 22050.0;
	static constexpr double kLogLowHz        = 20.0;
	static constexpr double kLogHighHz       = 20000.0;

	Channel();

	void SetParams( const Params& p );
	/// The host's bins. Ramped to the new values over `rampSamples` so a
	/// frame-rate update is a slope, not a click.
	void SetAudioBins( const float* bins, int count, int rampSamples );
	void Reset();

	/// One sample: the transmitter's analytic tone in, the real audio out.
	double Step( double re, double im );

	/// The current complex gain of the direct path, for the harness.
	void DirectGain( double& re, double& im ) const;

	const Params& Current() const { return params; }

private:
	struct Phasor
	{
		double re = 1.0, im = 0.0;
		double stepRe = 1.0, stepIm = 0.0;
		void SetRateHz( double hz );
		void Advance()
		{
			const double r = re * stepRe - im * stepIm;
			im             = re * stepIm + im * stepRe;
			re             = r;
		}
		void Renormalise();
	};

	struct Fader
	{
		Phasor rays[ kRays ];
		double phaseRe[ kRays ] = {};
		double phaseIm[ kRays ] = {};
		void Seed( Rng& rng, double dopplerHz );
		void SetDoppler( double dopplerHz );
		void Gain( double& re, double& im ) const;
		void Advance();
	private:
		double angles[ kRays ] = {};
	};

	Params params;
	Rng rng;
	Fader direct;
	Fader reflected;
	Phasor qrm;

	//The second path's delay line, 256 samples: 23 ms, more than the control
	//can ask for.
	static constexpr int kDelayMask = 255;
	std::vector< double > delayRe, delayIm;
	int delayWrite  = 0;
	int delaySamples = 0;

	Phasor audio[ kBins ];
	double audioAmp[ kBins ]    = {};
	double audioTarget[ kBins ] = {};
	double audioSlope[ kBins ]  = {};
	int audioRamp               = 0;
	bool audioActive[ kBins ]   = {};

	double noiseSigma    = 0.0;
	int sinceRenormalise = 0;
	bool binsInitialised = false;
};

} // namespace slowscan::sstv
