#include "../Shaders.h"

namespace slowscan::shaders
{
/// The clip, down onto the transmitter's grid, upside down.
///
/// A box filter over each destination pixel's footprint, not a point sample:
/// a 1080p frame point-sampled onto 256 lines keeps one row in four and
/// throws the rest away, and thin detail crawls as it drifts across the kept
/// rows. Averaging is what a station's scan converter did.
///
/// Written with y flipped, on purpose. glReadPixels returns the bottom row
/// first, and the transmitter wants row 0 to be the top of the picture --
/// SSTV sends the top line first -- so the flip is done here, once, and the
/// CPU never has to reorder a buffer.
///
/// Straight colour out. The tone for a pixel is its value, and a
/// premultiplied pixel that is dark only because it is transparent would be
/// sent as black.
const char* const kReadbackFragment = R"(#version 410 core
uniform sampler2D InputTexture;
uniform vec2 MaxUV;
uniform vec2 InputSize;
uniform vec2 TargetSize;

in vec2 uv;

out vec4 fragColor;

void main()
{
	vec2 flipped = vec2( uv.x, MaxUV.y - uv.y );
	vec2 ratio = InputSize / max( TargetSize, vec2( 1.0 ) );

	// One tap per source texel covered, capped so a 4K composition costs a
	// bounded amount. 8 taps each way is a 2560x2048 source onto 320x256.
	ivec2 taps = ivec2( clamp( ceil( ratio ), vec2( 1.0 ), vec2( 8.0 ) ) );
	vec2 texel = MaxUV / max( InputSize, vec2( 1.0 ) );

	vec4 sum = vec4( 0.0 );
	for( int y = 0; y < taps.y; ++y )
	{
		for( int x = 0; x < taps.x; ++x )
		{
			vec2 f = ( vec2( x, y ) + 0.5 ) / vec2( taps ) - 0.5;
			sum += texture( InputTexture, flipped + f * ratio * texel );
		}
	}

	vec4 color = sum / float( taps.x * taps.y );
	if( color.a > 0.0 )
		color.rgb /= color.a;

	fragColor = vec4( color.rgb, 1.0 );
}
)";
} // namespace slowscan::shaders
