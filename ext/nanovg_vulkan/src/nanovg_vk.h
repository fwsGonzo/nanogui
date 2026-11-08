//
// NanoVG Vulkan renderer. Mirrors nanovg_gl.h's GL3 path: the same shader
// (SPIR-V of a direct port, see shaders/), the same uniform block, stencil
// fills with the convex fast path, NVGVK_ANTIALIAS and NVGVK_STENCIL_STROKES.
//
// It owns no frame of its own. A host (the application's Vulkan backend) hands
// it the command buffer of the pass being recorded at each flush, runs uploads
// outside rendering, and destroys resources once the GPU is done with them.
// Clip space and winding are GL's: the host is expected not to flip Y (NDC
// y=-1 lands in memory row 0), and front faces are VK_FRONT_FACE_CLOCKWISE.
// A pass without a stencil attachment draws as GL3 does on a framebuffer
// without stencil bits: stencil tests pass and write nothing.
//
// zlib license, as nanovg_gl.h (Copyright (c) 2009-2013 Mikko Mononen).
//
#ifndef NANOVG_VK_H
#define NANOVG_VK_H

#include <vulkan/vulkan.h>
#include "nanovg.h"

#ifdef __cplusplus
extern "C" {
#endif

// Same bits as nanovg_gl.h's NVG_ANTIALIAS / NVG_STENCIL_STROKES / NVG_DEBUG,
// renamed so both headers can be included together.
enum NVGVKcreateFlags {
	NVGVK_ANTIALIAS       = 1 << 0,
	NVGVK_STENCIL_STROKES = 1 << 1,
	NVGVK_DEBUG           = 1 << 2,
};

// On top of NVGimageFlags; same bit as nanovg_gl.h's NVG_IMAGE_NODELETE.
enum NVGVKimageFlags {
	NVGVK_IMAGE_NODELETE = 1 << 16,
};

// The pass the host is recording, for building pipelines.
typedef struct NVGVKpass {
	VkFormat colorFormats[4];
	uint32_t colorCount;
	VkFormat depthFormat;          // also the stencil format when hasStencil
	int hasStencil;
	VkSampleCountFlagBits samples;
	uint32_t width, height;
} NVGVKpass;

typedef struct NVGVKhost {
	void* user;
	// The command buffer inside the current pass, with that pass's formats.
	// VK_NULL_HANDLE skips the flush.
	VkCommandBuffer (*renderingCommandBuffer)(void* user, NVGVKpass* pass);
	// Calls record(arg, cmd) now, with a command buffer outside any rendering
	// scope, ordered before whatever the host records next.
	void (*outsideRendering)(void* user, void (*record)(void* arg, VkCommandBuffer cmd), void* arg);
	// Someone else recorded into the host's command buffer.
	void (*invalidateBoundState)(void* user);
	// Calls fn(arg) once the GPU has finished everything submitted so far.
	void (*deferDestroy)(void* user, void (*fn)(void* arg), void* arg);
} NVGVKhost;

typedef struct NVGVKcreateInfo {
	VkPhysicalDevice physicalDevice;
	VkDevice device;               // Vulkan 1.3 (dynamic rendering, dynamic topology)
	NVGVKhost host;
} NVGVKcreateInfo;

NVGcontext* nvgCreateVk(const NVGVKcreateInfo* info, int flags);
void nvgDeleteVk(NVGcontext* ctx);

// An image drawing a texture the caller owns (never destroyed by NanoVG).
// sampler may be VK_NULL_HANDLE: NanoVG then samples by imageFlags.
int nvgvkCreateImageFromView(NVGcontext* ctx, VkImageView view, VkSampler sampler,
	VkImageLayout layout, int w, int h, int imageFlags);

// As above, but the view is looked up at every flush that draws it, so the
// caller's texture may be recreated underneath. resolve returns 0 when the
// texture has nothing to show (the dummy texture is drawn instead).
typedef int (*NVGVKresolveImage)(void* arg, VkImageView* view, VkSampler* sampler, VkImageLayout* layout);
int nvgvkCreateImageResolved(NVGcontext* ctx, NVGVKresolveImage resolve, void* arg,
	int w, int h, int imageFlags);

// Viewport for the following flushes in GL numbers (bottom-left origin).
// w <= 0 or h <= 0 restores the default: the whole pass.
void nvgvkViewport(NVGcontext* ctx, int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif

#endif // NANOVG_VK_H
