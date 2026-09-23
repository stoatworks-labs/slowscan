#pragma once

#include <FFGLSDK.h>

namespace slowscan
{
/**
    An off-screen buffer for one stage of the chain.

    Two things on top of the SDK's FFGLFBO.

    **It reallocates only when it has to.** The readback buffer is sized by
    the SSTV mode (320x256 or 320x240), not by the composition, so it keeps
    its size across a resolution change and only reallocates when the mode
    changes. Ensure() is called every frame and is a no-op in nearly all.

    **It actually frees its colour texture.** `ffglex::FFGLFBO::Release()`
    deletes the framebuffer and the depth renderbuffer, then tests
    `depthBufferID` a second time where it plainly meant `colorTextureID` --
    so the colour texture is leaked on every release (SDK b1afaf9,
    `FFGLFBO.cpp`). One leak would not matter; this plugin drops and rebuilds
    every buffer whenever the system or the composition size changes, and
    somebody auditioning NTSC against PAL a few times should not be paying
    megabytes of texture memory for each comparison.
*/
class PassBuffer : public ffglex::FFGLFBO
{
public:
	~PassBuffer();

	/// Allocate at this size and format, reusing the existing buffer if it
	/// already matches. Newly allocated buffers are cleared, because a pass that
	/// reads its own history reads this on its first frame.
	bool Ensure( GLsizei requestedWidth, GLsizei requestedHeight, GLint format );

	/// Release everything, including the colour texture the SDK forgets.
	void Destroy();

	bool IsValid() const
	{
		return GetGLID() != 0;
	}
};

} // namespace slowscan
