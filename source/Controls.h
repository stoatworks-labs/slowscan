#pragma once

#include <algorithm>
#include <cmath>

/**
	Host parameters to physical units.

	Every ranged FFGL parameter this plugin declares is 0..1, because
	`SetParamInfo` clamps an `FF_TYPE_STANDARD` default into 0..1 *before*
	returning and `SetParamRange` can only be called afterwards -- so a ranged
	parameter cannot have a ranged default, and the honest way out is to keep
	the host side normalised and put every conversion here, where it can be
	read and tested. Anything that is genuinely a whole number is
	`FF_TYPE_OPTION`, which is exempt.

	Option parameters are mapped BY INDEX: an option's range reads back 0..1
	whatever its element count, so nothing here scales one.
*/
namespace slowscan::controls
{
inline float clamp01( float v )
{
	return std::clamp( v, 0.0f, 1.0f );
}

/// Speed: 1x to 120x, logarithmic. Real time is two minutes a picture,
/// which no show can wait for; the top of the range is a second.
inline constexpr double kSpeedMax = 120.0;
inline double Speed( float v )
{
	return std::pow( kSpeedMax, static_cast< double >( clamp01( v ) ) );
}

/// SNR in a 3 kHz bandwidth, -10 dB to +40 dB, linear in dB.
inline constexpr double kSnrMinDb = -10.0;
inline constexpr double kSnrMaxDb = 40.0;
inline double SnrDb( float v )
{
	return kSnrMinDb + ( kSnrMaxDb - kSnrMinDb ) * clamp01( v );
}

/// Fade Depth is the Rayleigh fraction directly: 0 a wire, 1 pure scatter.
inline double FadeDepth( float v )
{
	return clamp01( v );
}

/// Doppler spread, 0.02 Hz to 5 Hz, logarithmic. HF is usually 0.1..1 Hz.
inline constexpr double kFadeRateMinHz = 0.02;
inline constexpr double kFadeRateMaxHz = 5.0;
inline double FadeRateHz( float v )
{
	return kFadeRateMinHz * std::pow( kFadeRateMaxHz / kFadeRateMinHz, static_cast< double >( clamp01( v ) ) );
}

/// Second-path delay, 0 to 8 ms. An SSTV pixel is about half a millisecond,
/// so the top of the range is a ghost sixteen pixels to the right.
inline constexpr double kMultipathMaxMs = 8.0;
inline double MultipathSeconds( float v )
{
	return kMultipathMaxMs * 1e-3 * clamp01( v );
}

inline double MultipathLevel( float v )
{
	return clamp01( v );
}

/// Interfering carrier, 300 Hz to 3 kHz, logarithmic.
inline constexpr double kQrmMinHz = 300.0;
inline constexpr double kQrmMaxHz = 3000.0;
inline double QrmHz( float v )
{
	return kQrmMinHz * std::pow( kQrmMaxHz / kQrmMinHz, static_cast< double >( clamp01( v ) ) );
}

/// Carrier amplitude relative to the signal's: 1 is as loud as the picture.
inline double QrmLevel( float v )
{
	return clamp01( v );
}

/// The host's spectrum, as tones: 1 puts a full-scale bin at the signal's
/// own amplitude.
inline double AudioLevel( float v )
{
	return clamp01( v );
}

/// Post-discriminator lowpass, 100 Hz to 3 kHz, logarithmic.
inline constexpr double kRxBandwidthMinHz = 100.0;
inline constexpr double kRxBandwidthMaxHz = 3000.0;
inline double RxBandwidthHz( float v )
{
	return kRxBandwidthMinHz * std::pow( kRxBandwidthMaxHz / kRxBandwidthMinHz, static_cast< double >( clamp01( v ) ) );
}

/// Clock Error and Slant Correct: -300 to +300 ppm, centred. A sound card
/// is typically tens of ppm off; three hundred slants a Martin M1 by about
/// 75 pixels over the frame, which is as far as it is still a picture.
inline constexpr double kPpmRange = 300.0;
inline double Ppm( float v )
{
	return ( static_cast< double >( clamp01( v ) ) - 0.5 ) * 2.0 * kPpmRange;
}

} // namespace slowscan::controls
