#pragma once

#include <cmath>
#include <cstdint>

namespace slowscan::sstv
{
/**
	xorshift128+, seeded. The same generator vectrix's signal engine uses (see
	ATTRIBUTIONS.md), and for the same reason: the offline harness has to be
	able to run the same channel twice. A noise process that cannot be
	reproduced cannot tell a regression from the weather, and the threshold
	check measures a variance against a stated SNR -- it needs the noise to be
	the noise it says it is.
*/
class Rng
{
public:
	explicit Rng( uint64_t seed = 0x9E3779B97F4A7C15ull )
	{
		Seed( seed );
	}

	void Seed( uint64_t seed )
	{
		//SplitMix64 to fill the state: seeding xorshift with a small integer
		//directly gives a first few hundred outputs with visible structure.
		state[ 0 ] = splitMix( seed );
		state[ 1 ] = splitMix( seed );
		if( state[ 0 ] == 0 && state[ 1 ] == 0 )
			state[ 0 ] = 0x9E3779B97F4A7C15ull;
		spare    = 0.0;
		hasSpare = false;
	}

	uint64_t Next()
	{
		uint64_t s1       = state[ 0 ];
		const uint64_t s0 = state[ 1 ];
		state[ 0 ]        = s0;
		s1 ^= s1 << 23;
		state[ 1 ] = s1 ^ s0 ^ ( s1 >> 18 ) ^ ( s0 >> 5 );
		return state[ 1 ] + s0;
	}

	/// Uniform in [0, 1). 53 bits, so a double's whole mantissa.
	double Unit()
	{
		return static_cast< double >( Next() >> 11 ) * ( 1.0 / 9007199254740992.0 );
	}

	/// Standard normal, by Box-Muller. Two per pair; the second is kept.
	double Gaussian()
	{
		if( hasSpare )
		{
			hasSpare = false;
			return spare;
		}
		double u1 = Unit();
		//log( 0 ) is -inf; a zero draw is one in 2^53 but it only has to
		//happen once to put a NaN through the whole receiver for ever.
		if( u1 < 1e-300 )
			u1 = 1e-300;
		const double u2 = Unit();
		const double r  = std::sqrt( -2.0 * std::log( u1 ) );
		const double a  = 6.283185307179586 * u2;
		spare           = r * std::sin( a );
		hasSpare        = true;
		return r * std::cos( a );
	}

private:
	static uint64_t splitMix( uint64_t& x )
	{
		x += 0x9E3779B97F4A7C15ull;
		uint64_t z = x;
		z          = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
		z          = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBull;
		return z ^ ( z >> 31 );
	}

	uint64_t state[ 2 ] = { 0, 0 };
	double spare        = 0.0;
	bool hasSpare       = false;
};

} // namespace slowscan::sstv
