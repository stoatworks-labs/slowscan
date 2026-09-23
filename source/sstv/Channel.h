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
	  audio       the host's 64 FFT bins as NOISE whose spectrum they shape.
	              Resolume gives a spectrum and not a waveform, so there is no
	              waveform to put on the air; what can be put there honestly
	              is Gaussian noise through a filter whose magnitude response
	              is the bins, at the frequencies the Bin Spacing assumption
	              gives them. Nobody has measured how those bins are laid out;
	              see AGENTS.md. Anything the bins put above the channel's
	              Nyquist is outside the channel and is dropped, not aliased.
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

	/// The noise standard deviation, per real sample, that puts `snrDb` of
	/// signal-to-noise in a 3 kHz bandwidth against a unit-amplitude tone.
	static double NoiseSigma( double snrDb );

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
	/// The host's bins, read as MAGNITUDES. The interference filter is
	/// redesigned from them; the noise running through it carries on.
	void SetAudioBins( const float* bins, int count );

	/// The interference filter's taps, for the harness.
	static constexpr int kAudioTaps = 64;
	const double* AudioTaps() const { return audioTaps; }
	/// The frequency range, in Hz, bin `i` covers under a spacing.
	static void BinRangeHz( int spacing, int i, double& lowHz, double& highHz );

	/// Negative control: state the SNR 3 dB wrong -- in the 1.5 kHz the
	/// receiver's filter passes rather than in 3 kHz. `--threshold` must see it.
	void DebugSnrOffsetDb( double db ) { debugSnrOffsetDb = db; SetParams( params ); }
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

	//The interference: white Gaussian noise through a 64-tap linear-phase
	//FIR whose response is the bins. One Gaussian and 64 multiply-adds a
	//sample, whatever the bins hold.
	float bins[ kBins ]            = {};
	double audioTaps[ kAudioTaps ] = {};
	double audioNoise[ kAudioTaps ] = {};
	int audioWrite                 = 0;
	bool audioActive               = false;
	Rng audioRng;
	void designAudioFilter();
	double debugSnrOffsetDb = 0.0;

	double noiseSigma    = 0.0;
	int sinceRenormalise = 0;
	bool binsInitialised = false;
};

} // namespace slowscan::sstv
