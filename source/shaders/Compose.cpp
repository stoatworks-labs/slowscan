#include "../Shaders.h"

namespace slowscan::shaders
{
/// The output: the received picture at the mode's aspect, the cursor, the
/// mix.
///
/// Two coordinate systems meet here:
///
///   uv         0..1 across the output, y = 0 at the BOTTOM (GL's order).
///   (px, py)   picture pixels, y = 0 at the TOP, which is the order the
///              lines were received in and the order the CPU uploads them.
///
/// The picture texture is uploaded top row first, so a top-down py reads it
/// directly: the one flip is `1.0 - inner.y`, and there is no other.
///
/// Outside the picture's rectangle the output is transparent black, so the
/// letterbox of a 4:3 picture on a 16:9 composition shows the layer below.
/// `mix( clip, pic, 0.0 )` is `clip * 1 + pic * 0`, exact in GLSL: Mix 0 is
/// the clip, byte for byte, which `sstest --render` asserts.
///
/// The cursor marks every output pixel whose vertical footprint holds the
/// centre of the cursor row, as well as the row the pixel shows. On an
/// output with fewer rows than the picture (320x180 holds 256 lines in 180)
/// nearest sampling skips a third of the rows, and a one-row cursor would
/// blink in and out of existence as it walked down the frame.
const char* const kComposeFragment = R"(#version 410 core
uniform sampler2D InputTexture;
uniform sampler2D PictureTexture;

uniform vec2 InputMaxUV;
uniform vec2 RectOrigin;//bottom-left of the picture, in output uv
uniform vec2 RectSize;
uniform ivec2 PictureSize;
uniform int CursorRow;//picture row from the top to mark, or -1
uniform float RowsPerPixel;//picture rows one output pixel spans
uniform vec3 CursorColour;
uniform float Mix;

in vec2 uv;

out vec4 fragColor;

void main()
{
	vec4 clip = texture( InputTexture, uv * InputMaxUV );

	vec2 inner = ( uv - RectOrigin ) / max( RectSize, vec2( 1e-6 ) );

	vec4 pic;
	if( inner.x < 0.0 || inner.x >= 1.0 || inner.y < 0.0 || inner.y >= 1.0 )
	{
		pic = vec4( 0.0 );
	}
	else
	{
		int px = clamp( int( inner.x * float( PictureSize.x ) ), 0, PictureSize.x - 1 );
		float rowY = ( 1.0 - inner.y ) * float( PictureSize.y );
		int py = clamp( int( rowY ), 0, PictureSize.y - 1 );
		vec3 rgb = texelFetch( PictureTexture, ivec2( px, py ), 0 ).rgb;
		float cursorCentre = float( CursorRow ) + 0.5;
		float halfSpan = 0.5 * RowsPerPixel;
		if( CursorRow >= 0 && ( py == CursorRow || ( cursorCentre > rowY - halfSpan && cursorCentre <= rowY + halfSpan ) ) )
			rgb = CursorColour;
		pic = vec4( rgb, 1.0 );
	}

	fragColor = mix( clip, pic, Mix );
}
)";
} // namespace slowscan::shaders
