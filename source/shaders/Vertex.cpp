#include "../Shaders.h"

namespace slowscan::shaders
{
/// Shared by both passes: draws the screen quad and scales UVs by MaxUV so
/// the same program works against a host texture with padding and against
/// our own framebuffers, which have none.
const char* const kVertex = R"(#version 410 core
uniform vec2 MaxUV;

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV * MaxUV;
}
)";
} // namespace slowscan::shaders
