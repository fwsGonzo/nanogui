//
// NanoVG Vulkan renderer; see nanovg_vk.h. Structure and semantics follow
// nanovg_gl.h's GL3 path call for call (glnvg__fill, __convexFill, __stroke,
// __triangles, __convertPaint), with GL state turned into pipelines.
//
// zlib license, as nanovg_gl.h (Copyright (c) 2009-2013 Mikko Mononen).
//
#if defined(__GNUC__) || defined(__clang__)
// Vulkan structs are zero-filled through `T x{sType}`.
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "nanovg_vk.h"
#include "nanovg_vk_shaders.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{

enum VKNVGshaderType {
	NSVG_SHADER_FILLGRAD,
	NSVG_SHADER_FILLIMG,
	NSVG_SHADER_SIMPLE,
	NSVG_SHADER_IMG
};

enum VKNVGcallType {
	VKNVG_NONE = 0,
	VKNVG_FILL,
	VKNVG_CONVEXFILL,
	VKNVG_STROKE,
	VKNVG_TRIANGLES,
};

// GL state of each draw in nanovg_gl.h's flush, as one pipeline each.
enum VKNVGvariant : uint8_t {
	VAR_NORMAL,         // cull back, no stencil (convex fill, plain strokes, triangles)
	VAR_FILL_STENCIL,   // no colour, no cull, front INCR_WRAP / back DECR_WRAP
	VAR_STENCIL_EQ0,    // stencil EQUAL 0, KEEP (fill fringes, stroke AA)
	VAR_FILL_COVER,     // stencil NOTEQUAL 0, ZERO
	VAR_STROKE_BASE,    // stencil EQUAL 0, pass INCR
	VAR_STROKE_CLEAR,   // no colour, ALWAYS, ZERO
	VAR_COUNT
};

// std140 layout of the fragment shader's `frag` block (GL3's GLNVGfragUniforms).
struct VKNVGfragUniforms {
	float scissorMat[12]; // matrices are actually 3 vec4s
	float paintMat[12];
	NVGcolor innerCol;
	NVGcolor outerCol;
	float scissorExt[2];
	float scissorScale[2];
	float extent[2];
	float radius;
	float feather;
	float strokeMult;
	float strokeThr;
	int lineStyle;
	int texType;
	int type;
};
constexpr VkDeviceSize FRAG_RANGE = (sizeof(VKNVGfragUniforms) + 15) & ~VkDeviceSize(15);

struct VKNVGtexture {
	int id = 0;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;     // external sampler; null = by flags
	VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	NVGVKresolveImage resolve = nullptr;
	void* resolveArg = nullptr;
	int width = 0, height = 0;
	int type = 0;
	int flags = 0;
	uint32_t mips = 1;
	bool owned = false;                     // image/memory/view are ours
};

struct VKNVGblend {
	VkBlendFactor srcRGB, dstRGB, srcAlpha, dstAlpha;
};

struct VKNVGcall {
	int type;
	int image;
	int pathOffset;
	int pathCount;
	int triangleOffset;
	int triangleCount;
	int uniformOffset;
	VKNVGblend blendFunc;
};

struct VKNVGpath {
	int fillOffset;
	int fillCount;
	int strokeOffset;
	int strokeCount;
};

struct PipeKey {
	uint32_t variant;
	uint32_t blend;            // four 8-bit VkBlendFactors
	VkFormat colors[4];
	uint32_t colorCount;
	VkFormat depth;
	uint32_t stencil;
	uint32_t samples;
	bool operator<(const PipeKey& o) const { return std::memcmp(this, &o, sizeof(PipeKey)) < 0; }
};

// A host-visible buffer for one flush (vertices + uniforms) or one upload
// (staging), with the descriptor sets made for it. Returned to the pool's
// free list by the host's deferDestroy once the GPU is done with it.
struct Pool;
struct Chunk {
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void* mapped = nullptr;
	VkDeviceSize size = 0;
	std::vector<VkDescriptorPool> descPools;
	size_t descPoolActive = 0;
	std::map<std::pair<uint64_t, uint64_t>, VkDescriptorSet> sets;
};

struct Pool {
	VkDevice device = VK_NULL_HANDLE;
	bool dead = false;
	std::vector<Chunk*> free;

	void destroy(Chunk* c) {
		for (VkDescriptorPool p : c->descPools) vkDestroyDescriptorPool(device, p, nullptr);
		if (c->buffer) vkDestroyBuffer(device, c->buffer, nullptr);
		if (c->memory) vkFreeMemory(device, c->memory, nullptr);
		delete c;
	}
	void release(Chunk* c) {
		if (dead || free.size() >= 16) { destroy(c); return; }
		for (VkDescriptorPool p : c->descPools) vkResetDescriptorPool(device, p, 0);
		c->descPoolActive = 0;
		c->sets.clear();
		free.push_back(c);
	}
};

struct VKNVGcontext {
	NVGVKhost host{};
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkPhysicalDeviceMemoryProperties memProps{};
	VkDeviceSize uboAlign = 256;
	int flags = 0;

	VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
	std::map<PipeKey, VkPipeline> pipelines;
	VkSampler samplers[16] = {};           // by samplerIndex(flags)

	std::vector<VKNVGtexture> textures;
	int textureId = 0;
	int dummyTex = 0;
	std::shared_ptr<Pool> pool;

	float view[2] = {0, 0};
	int viewport[4] = {0, 0, 0, 0};
	int fragSize = 0;

	// Per frame buffers
	std::vector<VKNVGcall> calls;
	std::vector<VKNVGpath> paths;
	std::vector<NVGvertex> verts;
	std::vector<unsigned char> uniforms;
	int nuniforms = 0;

	// recording state of one flush
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	NVGVKpass pass{};
	Chunk* chunk = nullptr;
	VkPipeline boundPipeline = VK_NULL_HANDLE;
	VkDescriptorSet boundSet = VK_NULL_HANDLE;
	uint32_t boundOffset = ~0u;
	VkPrimitiveTopology boundTopology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
	VKNVGblend blend{};
};

template <class F>
void outsideRendering(VKNVGcontext* vk, F&& fn)
{
	using Fn = std::remove_reference_t<F>;
	vk->host.outsideRendering(vk->host.user,
		[](void* arg, VkCommandBuffer cmd) { (*static_cast<Fn*>(arg))(cmd); }, &fn);
}

template <class F>
void deferDestroy(VKNVGcontext* vk, F fn)
{
	auto* heap = new F(std::move(fn));
	vk->host.deferDestroy(vk->host.user,
		[](void* arg) { auto* f = static_cast<F*>(arg); (*f)(); delete f; }, heap);
}

VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

uint32_t findMemoryType(VKNVGcontext* vk, uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < vk->memProps.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (vk->memProps.memoryTypes[i].propertyFlags & want) == want)
			return i;
	return UINT32_MAX;
}

Chunk* acquireChunk(VKNVGcontext* vk, VkDeviceSize need)
{
	Pool& pool = *vk->pool;
	for (size_t i = 0; i < pool.free.size(); i++) {
		if (pool.free[i]->size >= need) {
			Chunk* c = pool.free[i];
			pool.free.erase(pool.free.begin() + i);
			return c;
		}
	}
	VkDeviceSize size = 256 * 1024;
	while (size < need) size *= 2;

	Chunk* c = new Chunk();
	c->size = size;
	VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = size;
	bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(vk->device, &bi, nullptr, &c->buffer) != VK_SUCCESS) { pool.destroy(c); return nullptr; }
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(vk->device, c->buffer, &req);
	VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = findMemoryType(vk, req.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (ai.memoryTypeIndex == UINT32_MAX ||
		vkAllocateMemory(vk->device, &ai, nullptr, &c->memory) != VK_SUCCESS ||
		vkBindBufferMemory(vk->device, c->buffer, c->memory, 0) != VK_SUCCESS ||
		vkMapMemory(vk->device, c->memory, 0, VK_WHOLE_SIZE, 0, &c->mapped) != VK_SUCCESS) {
		pool.destroy(c);
		return nullptr;
	}
	return c;
}

void releaseChunk(VKNVGcontext* vk, Chunk* c)
{
	std::shared_ptr<Pool> pool = vk->pool;
	deferDestroy(vk, [pool, c]() { pool->release(c); });
}

VKNVGtexture* allocTexture(VKNVGcontext* vk)
{
	VKNVGtexture* tex = nullptr;
	for (auto& t : vk->textures)
		if (t.id == 0) { tex = &t; break; }
	if (tex == nullptr) {
		vk->textures.emplace_back();
		tex = &vk->textures.back();
	}
	*tex = VKNVGtexture{};
	tex->id = ++vk->textureId;
	return tex;
}

VKNVGtexture* findTexture(VKNVGcontext* vk, int id)
{
	if (id == 0) return nullptr;
	for (auto& t : vk->textures)
		if (t.id == id) return &t;
	return nullptr;
}

void destroyTextureLater(VKNVGcontext* vk, const VKNVGtexture& t)
{
	if (!t.owned || (t.flags & NVGVK_IMAGE_NODELETE)) return;
	VkDevice device = vk->device;
	VkImage image = t.image;
	VkImageView view = t.view;
	VkDeviceMemory memory = t.memory;
	deferDestroy(vk, [device, image, view, memory]() {
		if (view) vkDestroyImageView(device, view, nullptr);
		if (image) vkDestroyImage(device, image, nullptr);
		if (memory) vkFreeMemory(device, memory, nullptr);
	});
}

int samplerIndex(int imageFlags, bool mips)
{
	return ((imageFlags & NVG_IMAGE_NEAREST) ? 1 : 0)
		| ((imageFlags & NVG_IMAGE_REPEATX) ? 2 : 0)
		| ((imageFlags & NVG_IMAGE_REPEATY) ? 4 : 0)
		| (mips ? 8 : 0);
}

VkSampler samplerFor(VKNVGcontext* vk, int imageFlags, bool mips)
{
	const int i = samplerIndex(imageFlags, mips);
	if (vk->samplers[i]) return vk->samplers[i];
	const bool nearest = (i & 1) != 0;
	VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	si.magFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
	si.minFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
	si.mipmapMode = nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
	si.addressModeU = (i & 2) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	si.addressModeV = (i & 4) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	si.minLod = 0.0f;
	si.maxLod = (i & 8) ? VK_LOD_CLAMP_NONE : 0.0f;
	if (vkCreateSampler(vk->device, &si, nullptr, &vk->samplers[i]) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return vk->samplers[i];
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, uint32_t baseMip, uint32_t mipCount,
	VkImageLayout from, VkImageLayout to,
	VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
	VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = srcAccess;
	b.dstAccessMask = dstAccess;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, 1};
	vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// ---------------------------------------------------------------------------
// NVGparams

int vknvg__renderCreateTexture(void* uptr, int type, int w, int h, int imageFlags, const unsigned char* data);

int vknvg__renderCreate(void* uptr)
{
	auto* vk = static_cast<VKNVGcontext*>(uptr);

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(vk->physicalDevice, &props);
	vkGetPhysicalDeviceMemoryProperties(vk->physicalDevice, &vk->memProps);
	vk->uboAlign = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 16);
	vk->fragSize = int(alignUp(FRAG_RANGE, vk->uboAlign));

	VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	mi.codeSize = sizeof(nvgvk_vert_spv);
	mi.pCode = nvgvk_vert_spv;
	if (vkCreateShaderModule(vk->device, &mi, nullptr, &vk->vert) != VK_SUCCESS) return 0;
	mi.codeSize = sizeof(nvgvk_frag_spv);
	mi.pCode = nvgvk_frag_spv;
	if (vkCreateShaderModule(vk->device, &mi, nullptr, &vk->frag) != VK_SUCCESS) return 0;

	VkDescriptorSetLayoutBinding bindings[2] = {};
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	li.bindingCount = 2;
	li.pBindings = bindings;
	if (vkCreateDescriptorSetLayout(vk->device, &li, nullptr, &vk->setLayout) != VK_SUCCESS) return 0;

	VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 2};
	VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &vk->setLayout;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &push;
	if (vkCreatePipelineLayout(vk->device, &pli, nullptr, &vk->pipelineLayout) != VK_SUCCESS) return 0;

	vk->pool = std::make_shared<Pool>();
	vk->pool->device = vk->device;

	// Some platforms does not allow to have samples to unset textures.
	// Create empty one which is bound when there's no texture specified.
	vk->dummyTex = vknvg__renderCreateTexture(vk, NVG_TEXTURE_ALPHA, 1, 1, 0, nullptr);
	return vk->dummyTex != 0;
}

bool uploadRegion(VKNVGcontext* vk, VKNVGtexture* tex, int x, int y, int w, int h,
	const unsigned char* data, bool initial)
{
	const int bpp = tex->type == NVG_TEXTURE_RGBA ? 4 : 1;
	Chunk* staging = nullptr;
	if (data) {
		const VkDeviceSize bytes = VkDeviceSize(w) * h * bpp;
		staging = acquireChunk(vk, bytes);
		if (staging == nullptr) return false;
		// Rows of the region out of the whole image (GL_UNPACK_ROW_LENGTH = width).
		auto* dst = static_cast<unsigned char*>(staging->mapped);
		for (int row = 0; row < h; row++)
			std::memcpy(dst + size_t(row) * w * bpp,
				data + (size_t(y + row) * tex->width + x) * bpp, size_t(w) * bpp);
	}

	VkImage image = tex->image;
	const uint32_t mips = initial ? tex->mips : 1;
	const int tw = tex->width, th = tex->height;
	VkBuffer src = staging ? staging->buffer : VK_NULL_HANDLE;
	outsideRendering(vk, [&](VkCommandBuffer cmd) {
		if (initial)
			imageBarrier(cmd, image, 0, mips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
		else
			imageBarrier(cmd, image, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

		if (src) {
			VkBufferImageCopy region{};
			region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			region.imageOffset = {x, y, 0};
			region.imageExtent = {uint32_t(w), uint32_t(h), 1};
			vkCmdCopyBufferToImage(cmd, src, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		} else {
			// GL leaves a data-less texture undefined; start it transparent.
			VkClearColorValue clear{};
			VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
			vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
		}

		// glGenerateMipmap
		int mw = tw, mh = th;
		for (uint32_t level = 1; level < mips && src; level++) {
			imageBarrier(cmd, image, level - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
			const int nw = std::max(1, mw / 2), nh = std::max(1, mh / 2);
			VkImageBlit blit{};
			blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
			blit.srcOffsets[1] = {mw, mh, 1};
			blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
			blit.dstOffsets[1] = {nw, nh, 1};
			vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
			imageBarrier(cmd, image, level - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
			mw = nw; mh = nh;
		}
		const uint32_t last = (src && mips > 1) ? mips - 1 : 0;
		const uint32_t count = (src && mips > 1) ? 1 : mips;
		imageBarrier(cmd, image, last, count, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	});
	if (staging) releaseChunk(vk, staging);
	return true;
}

int vknvg__renderCreateTexture(void* uptr, int type, int w, int h, int imageFlags, const unsigned char* data)
{
	auto* vk = static_cast<VKNVGcontext*>(uptr);
	if (w <= 0 || h <= 0) return 0;
	VKNVGtexture* tex = allocTexture(vk);
	tex->width = w;
	tex->height = h;
	tex->type = type;
	tex->flags = imageFlags;
	tex->owned = true;
	tex->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if (imageFlags & NVG_IMAGE_GENERATE_MIPMAPS)
		tex->mips = uint32_t(std::floor(std::log2(double(std::max(w, h))))) + 1;
	const int id = tex->id;

	const VkFormat format = type == NVG_TEXTURE_RGBA ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8_UNORM;
	VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = format;
	ii.extent = {uint32_t(w), uint32_t(h), 1};
	ii.mipLevels = tex->mips;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
		| (tex->mips > 1 ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
	ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	auto fail = [&]() {
		destroyTextureLater(vk, *tex);
		*tex = VKNVGtexture{};
		return 0;
	};
	if (vkCreateImage(vk->device, &ii, nullptr, &tex->image) != VK_SUCCESS) return fail();
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(vk->device, tex->image, &req);
	VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = findMemoryType(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = findMemoryType(vk, req.memoryTypeBits, 0);
	if (vkAllocateMemory(vk->device, &ai, nullptr, &tex->memory) != VK_SUCCESS) return fail();
	if (vkBindImageMemory(vk->device, tex->image, tex->memory, 0) != VK_SUCCESS) return fail();

	VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = tex->image;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = format;
	vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, tex->mips, 0, 1};
	if (vkCreateImageView(vk->device, &vi, nullptr, &tex->view) != VK_SUCCESS) return fail();

	if (!uploadRegion(vk, tex, 0, 0, w, h, data, true)) return fail();
	return id;
}

int vknvg__renderDeleteTexture(void* uptr, int image)
{
	auto* vk = static_cast<VKNVGcontext*>(uptr);
	VKNVGtexture* tex = findTexture(vk, image);
	if (tex == nullptr) return 0;
	destroyTextureLater(vk, *tex);
	*tex = VKNVGtexture{};
	return 1;
}

int vknvg__renderUpdateTexture(void* uptr, int image, int x, int y, int w, int h, const unsigned char* data)
{
	auto* vk = static_cast<VKNVGcontext*>(uptr);
	VKNVGtexture* tex = findTexture(vk, image);
	if (tex == nullptr || !tex->owned || data == nullptr) return 0;
	x = std::clamp(x, 0, tex->width);
	y = std::clamp(y, 0, tex->height);
	w = std::min(w, tex->width - x);
	h = std::min(h, tex->height - y);
	if (w <= 0 || h <= 0) return 1;
	return uploadRegion(vk, tex, x, y, w, h, data, false) ? 1 : 0;
}

int vknvg__renderGetTextureSize(void* uptr, int image, int* w, int* h)
{
	auto* vk = static_cast<VKNVGcontext*>(uptr);
	VKNVGtexture* tex = findTexture(vk, image);
	if (tex == nullptr) return 0;
	*w = tex->width;
	*h = tex->height;
	return 1;
}

int vknvg__renderGetImageTextureId(void* uptr, int handle)
{
	auto* vk = static_cast<VKNVGcontext*>(uptr);
	return findTexture(vk, handle) ? handle : -1;
}

void xformToMat3x4(float* m3, float* t)
{
	m3[0] = t[0];
	m3[1] = t[1];
	m3[2] = 0.0f;
	m3[3] = 0.0f;
	m3[4] = t[2];
	m3[5] = t[3];
	m3[6] = 0.0f;
	m3[7] = 0.0f;
	m3[8] = t[4];
	m3[9] = t[5];
	m3[10] = 1.0f;
	m3[11] = 0.0f;
}

NVGcolor premulColor(NVGcolor c)
{
	c.r *= c.a;
	c.g *= c.a;
	c.b *= c.a;
	return c;
}

int convertPaint(VKNVGcontext* vk, VKNVGfragUniforms* frag, NVGpaint* paint,
	NVGscissor* scissor, float width, float fringe, float strokeThr, int lineStyle)
{
	VKNVGtexture* tex = nullptr;
	float invxform[6];

	std::memset(frag, 0, sizeof(*frag));

	frag->innerCol = premulColor(paint->innerColor);
	frag->outerCol = premulColor(paint->outerColor);
	frag->lineStyle = lineStyle;
	if (scissor->extent[0] < -0.5f || scissor->extent[1] < -0.5f) {
		std::memset(frag->scissorMat, 0, sizeof(frag->scissorMat));
		frag->scissorExt[0] = 1.0f;
		frag->scissorExt[1] = 1.0f;
		frag->scissorScale[0] = 1.0f;
		frag->scissorScale[1] = 1.0f;
	} else {
		nvgTransformInverse(invxform, scissor->xform);
		xformToMat3x4(frag->scissorMat, invxform);
		frag->scissorExt[0] = scissor->extent[0];
		frag->scissorExt[1] = scissor->extent[1];
		frag->scissorScale[0] = std::sqrt(scissor->xform[0]*scissor->xform[0] + scissor->xform[2]*scissor->xform[2]) / fringe;
		frag->scissorScale[1] = std::sqrt(scissor->xform[1]*scissor->xform[1] + scissor->xform[3]*scissor->xform[3]) / fringe;
	}

	std::memcpy(frag->extent, paint->extent, sizeof(frag->extent));
	frag->strokeMult = (width*0.5f + fringe*0.5f) / fringe;
	frag->strokeThr = strokeThr;

	if (paint->image != 0) {
		tex = findTexture(vk, paint->image);
		if (tex == nullptr) return 0;
		if ((tex->flags & NVG_IMAGE_FLIPY) != 0) {
			float m1[6], m2[6];
			nvgTransformTranslate(m1, 0.0f, frag->extent[1] * 0.5f);
			nvgTransformMultiply(m1, paint->xform);
			nvgTransformScale(m2, 1.0f, -1.0f);
			nvgTransformMultiply(m2, m1);
			nvgTransformTranslate(m1, 0.0f, -frag->extent[1] * 0.5f);
			nvgTransformMultiply(m1, m2);
			nvgTransformInverse(invxform, m1);
		} else {
			nvgTransformInverse(invxform, paint->xform);
		}
		frag->type = NSVG_SHADER_FILLIMG;

		if (tex->type == NVG_TEXTURE_RGBA)
			frag->texType = (tex->flags & NVG_IMAGE_PREMULTIPLIED) ? 0 : 1;
		else
			frag->texType = 2;
	} else {
		frag->type = NSVG_SHADER_FILLGRAD;
		frag->radius = paint->radius;
		frag->feather = paint->feather;
		nvgTransformInverse(invxform, paint->xform);
	}

	xformToMat3x4(frag->paintMat, invxform);

	return 1;
}

VKNVGfragUniforms* fragUniformPtr(VKNVGcontext* vk, int i)
{
	return reinterpret_cast<VKNVGfragUniforms*>(&vk->uniforms[size_t(i)]);
}

void vknvg__renderViewport(void* uptr, float width, float height, float devicePixelRatio)
{
	NVG_NOTUSED(devicePixelRatio);
	auto* vk = static_cast<VKNVGcontext*>(uptr);
	vk->view[0] = width;
	vk->view[1] = height;
}

// --- recording -------------------------------------------------------------

VkPipeline pipelineFor(VKNVGcontext* vk, VKNVGvariant variant)
{
	PipeKey key;
	std::memset(&key, 0, sizeof(key));
	key.variant = variant;
	key.blend = uint32_t(vk->blend.srcRGB) | uint32_t(vk->blend.dstRGB) << 8
		| uint32_t(vk->blend.srcAlpha) << 16 | uint32_t(vk->blend.dstAlpha) << 24;
	key.colorCount = std::min<uint32_t>(vk->pass.colorCount, 4);
	for (uint32_t i = 0; i < key.colorCount; i++) key.colors[i] = vk->pass.colorFormats[i];
	key.depth = vk->pass.depthFormat;
	key.stencil = vk->pass.hasStencil ? 1 : 0;
	key.samples = vk->pass.samples ? vk->pass.samples : VK_SAMPLE_COUNT_1_BIT;

	auto it = vk->pipelines.find(key);
	if (it != vk->pipelines.end()) return it->second;

	VkSpecializationMapEntry specEntry{0, 0, sizeof(VkBool32)};
	const VkBool32 edgeAA = (vk->flags & NVGVK_ANTIALIAS) ? VK_TRUE : VK_FALSE;
	VkSpecializationInfo spec{1, &specEntry, sizeof(VkBool32), &edgeAA};

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vk->vert;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = vk->frag;
	stages[1].pName = "main";
	stages[1].pSpecializationInfo = &spec;

	VkVertexInputBindingDescription binding{0, sizeof(NVGvertex), VK_VERTEX_INPUT_RATE_VERTEX};
	VkVertexInputAttributeDescription attrs[2] = {
		{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
		{1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 2 * sizeof(float)},
	};
	VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vin.vertexBindingDescriptionCount = 1;
	vin.pVertexBindingDescriptions = &binding;
	vin.vertexAttributeDescriptionCount = 2;
	vin.pVertexAttributeDescriptions = attrs;

	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // dynamic

	VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = 1;
	vp.scissorCount = 1;

	// GL: glEnable(GL_CULL_FACE), glCullFace(GL_BACK), glFrontFace(GL_CCW).
	// Un-flipped, GL's counter-clockwise is Vulkan's clockwise.
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = variant == VAR_FILL_STENCIL ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
	rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
	rs.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VkSampleCountFlagBits(key.samples);

	VkStencilOpState keep{};
	keep.failOp = keep.passOp = keep.depthFailOp = VK_STENCIL_OP_KEEP;
	keep.compareOp = VK_COMPARE_OP_ALWAYS;
	keep.compareMask = 0xff;
	keep.writeMask = 0xff;
	keep.reference = 0;
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	ds.depthTestEnable = VK_FALSE;
	ds.depthWriteEnable = VK_FALSE;
	ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
	ds.front = ds.back = keep;
	switch (variant) {
	case VAR_FILL_STENCIL:
		ds.stencilTestEnable = VK_TRUE;
		ds.front.passOp = VK_STENCIL_OP_INCREMENT_AND_WRAP;
		ds.back.passOp = VK_STENCIL_OP_DECREMENT_AND_WRAP;
		break;
	case VAR_STENCIL_EQ0:
		ds.stencilTestEnable = VK_TRUE;
		ds.front.compareOp = ds.back.compareOp = VK_COMPARE_OP_EQUAL;
		break;
	case VAR_FILL_COVER:
		ds.stencilTestEnable = VK_TRUE;
		ds.front.compareOp = VK_COMPARE_OP_NOT_EQUAL;
		ds.front.failOp = ds.front.depthFailOp = ds.front.passOp = VK_STENCIL_OP_ZERO;
		ds.back = ds.front;
		break;
	case VAR_STROKE_BASE:
		ds.stencilTestEnable = VK_TRUE;
		ds.front.compareOp = VK_COMPARE_OP_EQUAL;
		ds.front.passOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
		ds.back = ds.front;
		break;
	case VAR_STROKE_CLEAR:
		ds.stencilTestEnable = VK_TRUE;
		ds.front.failOp = ds.front.depthFailOp = ds.front.passOp = VK_STENCIL_OP_ZERO;
		ds.back = ds.front;
		break;
	default:
		break;
	}
	// GL on a target without a stencil buffer: the test passes, nothing is written.
	if (!key.stencil) ds.stencilTestEnable = VK_FALSE;

	const bool colorWrites = variant != VAR_FILL_STENCIL && variant != VAR_STROKE_CLEAR;
	VkPipelineColorBlendAttachmentState att[4] = {};
	att[0].blendEnable = VK_TRUE;
	att[0].srcColorBlendFactor = vk->blend.srcRGB;
	att[0].dstColorBlendFactor = vk->blend.dstRGB;
	att[0].colorBlendOp = VK_BLEND_OP_ADD;
	att[0].srcAlphaBlendFactor = vk->blend.srcAlpha;
	att[0].dstAlphaBlendFactor = vk->blend.dstAlpha;
	att[0].alphaBlendOp = VK_BLEND_OP_ADD;
	att[0].colorWriteMask = colorWrites ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
		| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT) : 0;
	// NanoVG writes location 0 only; further attachments stay untouched.
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = key.colorCount;
	cb.pAttachments = att;

	const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
		VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY};
	VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dy.dynamicStateCount = 3;
	dy.pDynamicStates = dyn;

	VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	ri.colorAttachmentCount = key.colorCount;
	ri.pColorAttachmentFormats = key.colors;
	ri.depthAttachmentFormat = key.depth == VK_FORMAT_S8_UINT ? VK_FORMAT_UNDEFINED : key.depth;
	ri.stencilAttachmentFormat = key.stencil ? key.depth : VK_FORMAT_UNDEFINED;

	VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pi.pNext = &ri;
	pi.stageCount = 2;
	pi.pStages = stages;
	pi.pVertexInputState = &vin;
	pi.pInputAssemblyState = &ia;
	pi.pViewportState = &vp;
	pi.pRasterizationState = &rs;
	pi.pMultisampleState = &ms;
	pi.pDepthStencilState = &ds;
	pi.pColorBlendState = &cb;
	pi.pDynamicState = &dy;
	pi.layout = vk->pipelineLayout;

	VkPipeline pipeline = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline) != VK_SUCCESS) {
		std::fprintf(stderr, "nanovg_vk: pipeline creation failed\n");
		pipeline = VK_NULL_HANDLE;
	}
	vk->pipelines[key] = pipeline;
	return pipeline;
}

bool bindPipeline(VKNVGcontext* vk, VKNVGvariant variant)
{
	VkPipeline p = pipelineFor(vk, variant);
	if (p == VK_NULL_HANDLE) return false;
	if (p != vk->boundPipeline) {
		vkCmdBindPipeline(vk->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
		vk->boundPipeline = p;
	}
	return true;
}

void setTopology(VKNVGcontext* vk, VkPrimitiveTopology t)
{
	if (t != vk->boundTopology) {
		vkCmdSetPrimitiveTopology(vk->cmd, t);
		vk->boundTopology = t;
	}
}

VkDescriptorSet descriptorSetFor(VKNVGcontext* vk, VkImageView view, VkSampler sampler, VkImageLayout layout)
{
	Chunk* c = vk->chunk;
	const auto key = std::make_pair(uint64_t(view), uint64_t(sampler));
	auto it = c->sets.find(key);
	if (it != c->sets.end()) return it->second;

	VkDescriptorSet set = VK_NULL_HANDLE;
	for (;;) {
		const bool fresh = c->descPoolActive == c->descPools.size();
		if (fresh) {
			VkDescriptorPoolSize sizes[2] = {
				{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 64},
				{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64},
			};
			VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			pi.maxSets = 64;
			pi.poolSizeCount = 2;
			pi.pPoolSizes = sizes;
			VkDescriptorPool p;
			if (vkCreateDescriptorPool(vk->device, &pi, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
			c->descPools.push_back(p);
		}
		VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		ai.descriptorPool = c->descPools[c->descPoolActive];
		ai.descriptorSetCount = 1;
		ai.pSetLayouts = &vk->setLayout;
		if (vkAllocateDescriptorSets(vk->device, &ai, &set) == VK_SUCCESS) break;
		if (fresh) return VK_NULL_HANDLE;
		c->descPoolActive++;
	}

	VkDescriptorBufferInfo bi{c->buffer, 0, FRAG_RANGE};
	VkDescriptorImageInfo ii{sampler, view, layout};
	VkWriteDescriptorSet w[2] = {};
	w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	w[0].dstSet = set;
	w[0].dstBinding = 0;
	w[0].descriptorCount = 1;
	w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	w[0].pBufferInfo = &bi;
	w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	w[1].dstSet = set;
	w[1].dstBinding = 1;
	w[1].descriptorCount = 1;
	w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	w[1].pImageInfo = &ii;
	vkUpdateDescriptorSets(vk->device, 2, w, 0, nullptr);
	c->sets[key] = set;
	return set;
}

bool textureBinding(VKNVGcontext* vk, VKNVGtexture* tex, VkImageView& view, VkSampler& sampler, VkImageLayout& layout)
{
	if (tex == nullptr) return false;
	view = tex->view;
	sampler = tex->sampler;
	layout = tex->layout;
	if (tex->resolve && !tex->resolve(tex->resolveArg, &view, &sampler, &layout))
		return false;
	if (view == VK_NULL_HANDLE) return false;
	if (sampler == VK_NULL_HANDLE) sampler = samplerFor(vk, tex->flags, tex->mips > 1);
	return sampler != VK_NULL_HANDLE;
}

// glnvg__setUniforms: the uniform block at `uniformOffset` and the image
// (the dummy texture when there is none).
void setUniforms(VKNVGcontext* vk, int uniformOffset, int image)
{
	VkImageView view;
	VkSampler sampler;
	VkImageLayout layout;
	if (!textureBinding(vk, findTexture(vk, image), view, sampler, layout))
		textureBinding(vk, findTexture(vk, vk->dummyTex), view, sampler, layout);
	VkDescriptorSet set = descriptorSetFor(vk, view, sampler, layout);
	const uint32_t offset = uint32_t(uniformOffset);
	if (set == VK_NULL_HANDLE) {
		vk->boundSet = VK_NULL_HANDLE;   // draws are skipped until a set binds
		return;
	}
	if (set != vk->boundSet || offset != vk->boundOffset) {
		vkCmdBindDescriptorSets(vk->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->pipelineLayout, 0, 1, &set, 1, &offset);
		vk->boundSet = set;
		vk->boundOffset = offset;
	}
}

void draw(VKNVGcontext* vk, int count, int first)
{
	if (count > 0 && vk->boundSet != VK_NULL_HANDLE) vkCmdDraw(vk->cmd, uint32_t(count), 1, uint32_t(first), 0);
}

void vknvg__fill(VKNVGcontext* vk, VKNVGcall* call)
{
	VKNVGpath* paths = &vk->paths[size_t(call->pathOffset)];
	int i, npaths = call->pathCount;

	// Draw shapes
	if (!bindPipeline(vk, VAR_FILL_STENCIL)) return;
	setUniforms(vk, call->uniformOffset, 0);
	setTopology(vk, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN);
	for (i = 0; i < npaths; i++)
		draw(vk, paths[i].fillCount, paths[i].fillOffset);

	// Draw anti-aliased pixels
	setUniforms(vk, call->uniformOffset + vk->fragSize, call->image);
	if (vk->flags & NVGVK_ANTIALIAS) {
		if (bindPipeline(vk, VAR_STENCIL_EQ0)) {
			setTopology(vk, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
			// Draw fringes
			for (i = 0; i < npaths; i++)
				draw(vk, paths[i].strokeCount, paths[i].strokeOffset);
		}
	}

	// Draw fill
	if (!bindPipeline(vk, VAR_FILL_COVER)) return;
	setTopology(vk, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
	draw(vk, call->triangleCount, call->triangleOffset);
}

void vknvg__convexFill(VKNVGcontext* vk, VKNVGcall* call)
{
	VKNVGpath* paths = &vk->paths[size_t(call->pathOffset)];
	int i, npaths = call->pathCount;

	if (!bindPipeline(vk, VAR_NORMAL)) return;
	setUniforms(vk, call->uniformOffset, call->image);

	for (i = 0; i < npaths; i++) {
		setTopology(vk, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN);
		draw(vk, paths[i].fillCount, paths[i].fillOffset);
		// Draw fringes
		if (paths[i].strokeCount > 0) {
			setTopology(vk, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
			draw(vk, paths[i].strokeCount, paths[i].strokeOffset);
		}
	}
}

void vknvg__stroke(VKNVGcontext* vk, VKNVGcall* call)
{
	VKNVGpath* paths = &vk->paths[size_t(call->pathOffset)];
	int npaths = call->pathCount, i;

	if (vk->flags & NVGVK_STENCIL_STROKES) {
		setTopology(vk, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);

		// Fill the stroke base without overlap
		if (!bindPipeline(vk, VAR_STROKE_BASE)) return;
		setUniforms(vk, call->uniformOffset + vk->fragSize, call->image);
		for (i = 0; i < npaths; i++)
			draw(vk, paths[i].strokeCount, paths[i].strokeOffset);

		// Draw anti-aliased pixels.
		if (!bindPipeline(vk, VAR_STENCIL_EQ0)) return;
		setUniforms(vk, call->uniformOffset, call->image);
		for (i = 0; i < npaths; i++)
			draw(vk, paths[i].strokeCount, paths[i].strokeOffset);

		// Clear stencil buffer.
		if (!bindPipeline(vk, VAR_STROKE_CLEAR)) return;
		for (i = 0; i < npaths; i++)
			draw(vk, paths[i].strokeCount, paths[i].strokeOffset);
	} else {
		if (!bindPipeline(vk, VAR_NORMAL)) return;
		setUniforms(vk, call->uniformOffset, call->image);
		setTopology(vk, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
		// Draw Strokes
		for (i = 0; i < npaths; i++)
			draw(vk, paths[i].strokeCount, paths[i].strokeOffset);
	}
}

void vknvg__triangles(VKNVGcontext* vk, VKNVGcall* call)
{
	if (!bindPipeline(vk, VAR_NORMAL)) return;
	setUniforms(vk, call->uniformOffset, call->image);
	setTopology(vk, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	draw(vk, call->triangleCount, call->triangleOffset);
}

void resetFrame(VKNVGcontext* vk)
{
	vk->verts.clear();
	vk->paths.clear();
	vk->calls.clear();
	vk->uniforms.clear();
	vk->nuniforms = 0;
}

void vknvg__renderCancel(void* uptr)
{
	resetFrame(static_cast<VKNVGcontext*>(uptr));
}

VkBlendFactor convertBlendFuncFactor(int factor, bool& ok)
{
	switch (factor) {
	case NVG_ZERO: return VK_BLEND_FACTOR_ZERO;
	case NVG_ONE: return VK_BLEND_FACTOR_ONE;
	case NVG_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
	case NVG_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	case NVG_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
	case NVG_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
	case NVG_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
	case NVG_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case NVG_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
	case NVG_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
	case NVG_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
	default: ok = false; return VK_BLEND_FACTOR_ZERO;
	}
}

VKNVGblend blendCompositeOperation(NVGcompositeOperationState op)
{
	bool ok = true;
	VKNVGblend blend;
	blend.srcRGB = convertBlendFuncFactor(op.srcRGB, ok);
	blend.dstRGB = convertBlendFuncFactor(op.dstRGB, ok);
	blend.srcAlpha = convertBlendFuncFactor(op.srcAlpha, ok);
	blend.dstAlpha = convertBlendFuncFactor(op.dstAlpha, ok);
	if (!ok) {
		blend.srcRGB = VK_BLEND_FACTOR_ONE;
		blend.dstRGB = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blend.srcAlpha = VK_BLEND_FACTOR_ONE;
		blend.dstAlpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	}
	return blend;
}

void vknvg__renderFlush(void* uptr)
{
	auto* vk = static_cast<VKNVGcontext*>(uptr);

	if (!vk->calls.empty()) {
		NVGVKpass pass{};
		pass.samples = VK_SAMPLE_COUNT_1_BIT;
		VkCommandBuffer cmd = vk->host.renderingCommandBuffer(vk->host.user, &pass);
		const VkDeviceSize ubytes = VkDeviceSize(vk->uniforms.size());
		const VkDeviceSize vofs = alignUp(std::max<VkDeviceSize>(ubytes, 1), 16);
		const VkDeviceSize vbytes = VkDeviceSize(vk->verts.size()) * sizeof(NVGvertex);
		Chunk* chunk = cmd ? acquireChunk(vk, vofs + vbytes) : nullptr;
		if (chunk) {
			if (ubytes) std::memcpy(chunk->mapped, vk->uniforms.data(), size_t(ubytes));
			if (vbytes) std::memcpy(static_cast<unsigned char*>(chunk->mapped) + vofs, vk->verts.data(), size_t(vbytes));

			vk->cmd = cmd;
			vk->pass = pass;
			vk->chunk = chunk;
			vk->boundPipeline = VK_NULL_HANDLE;
			vk->boundSet = VK_NULL_HANDLE;
			vk->boundOffset = ~0u;
			vk->boundTopology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;

			// GL leaves the viewport to the caller (the pass's); scissor test off.
			VkViewport viewport;
			if (vk->viewport[2] > 0 && vk->viewport[3] > 0)
				viewport = {float(vk->viewport[0]), float(vk->viewport[1]),
					float(vk->viewport[2]), float(vk->viewport[3]), 0.0f, 1.0f};
			else
				viewport = {0.0f, 0.0f, float(pass.width), float(pass.height), 0.0f, 1.0f};
			vkCmdSetViewport(cmd, 0, 1, &viewport);
			VkRect2D scissor{{0, 0}, {pass.width, pass.height}};
			vkCmdSetScissor(cmd, 0, 1, &scissor);
			VkDeviceSize offset = vofs;
			vkCmdBindVertexBuffers(cmd, 0, 1, &chunk->buffer, &offset);
			vkCmdPushConstants(cmd, vk->pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(vk->view), vk->view);

			for (VKNVGcall& call : vk->calls) {
				vk->blend = call.blendFunc;
				if (call.type == VKNVG_FILL)
					vknvg__fill(vk, &call);
				else if (call.type == VKNVG_CONVEXFILL)
					vknvg__convexFill(vk, &call);
				else if (call.type == VKNVG_STROKE)
					vknvg__stroke(vk, &call);
				else if (call.type == VKNVG_TRIANGLES)
					vknvg__triangles(vk, &call);
			}

			vk->host.invalidateBoundState(vk->host.user);
			releaseChunk(vk, chunk);
			vk->cmd = VK_NULL_HANDLE;
			vk->chunk = nullptr;
		}
	}

	// Reset calls
	resetFrame(vk);
}

int maxVertCount(const NVGpath* paths, int npaths)
{
	int i, count = 0;
	for (i = 0; i < npaths; i++) {
		count += paths[i].nfill;
		count += paths[i].nstroke;
	}
	return count;
}

VKNVGcall* allocCall(VKNVGcontext* vk)
{
	vk->calls.emplace_back();
	VKNVGcall* ret = &vk->calls.back();
	std::memset(ret, 0, sizeof(VKNVGcall));
	return ret;
}

int allocPaths(VKNVGcontext* vk, int n)
{
	const int ret = int(vk->paths.size());
	vk->paths.resize(vk->paths.size() + size_t(n));
	return ret;
}

int allocVerts(VKNVGcontext* vk, int n)
{
	const int ret = int(vk->verts.size());
	vk->verts.resize(vk->verts.size() + size_t(n));
	return ret;
}

int allocFragUniforms(VKNVGcontext* vk, int n)
{
	const int ret = vk->nuniforms * vk->fragSize;
	vk->nuniforms += n;
	vk->uniforms.resize(size_t(vk->nuniforms) * size_t(vk->fragSize));
	return ret;
}

void vset(NVGvertex* vtx, float x, float y, float u, float v)
{
	vtx->x = x;
	vtx->y = y;
	vtx->u = u;
	vtx->v = v;
	vtx->s = 0.0f;
	vtx->t = 0.0f;
}

void vknvg__renderFill(void* uptr, NVGpaint* paint, NVGcompositeOperationState compositeOperation, NVGscissor* scissor, float fringe,
	const float* bounds, const NVGpath* paths, int npaths)
{
	auto* vk = static_cast<VKNVGcontext*>(uptr);
	VKNVGcall* call = allocCall(vk);
	NVGvertex* quad;
	VKNVGfragUniforms* frag;
	int i, maxverts, offset;

	call->type = VKNVG_FILL;
	call->triangleCount = 4;
	call->pathOffset = allocPaths(vk, npaths);
	call->pathCount = npaths;
	call->image = paint->image;
	call->blendFunc = blendCompositeOperation(compositeOperation);

	if (npaths == 1 && paths[0].convex)
	{
		call->type = VKNVG_CONVEXFILL;
		call->triangleCount = 0;	// Bounding box fill quad not needed for convex fill
	}

	// Allocate vertices for all the paths.
	maxverts = maxVertCount(paths, npaths) + call->triangleCount;
	offset = allocVerts(vk, maxverts);

	for (i = 0; i < npaths; i++) {
		VKNVGpath* copy = &vk->paths[size_t(call->pathOffset + i)];
		const NVGpath* path = &paths[i];
		std::memset(copy, 0, sizeof(VKNVGpath));
		if (path->nfill > 0) {
			copy->fillOffset = offset;
			copy->fillCount = path->nfill;
			std::memcpy(&vk->verts[size_t(offset)], path->fill, sizeof(NVGvertex) * size_t(path->nfill));
			offset += path->nfill;
		}
		if (path->nstroke > 0) {
			copy->strokeOffset = offset;
			copy->strokeCount = path->nstroke;
			std::memcpy(&vk->verts[size_t(offset)], path->stroke, sizeof(NVGvertex) * size_t(path->nstroke));
			offset += path->nstroke;
		}
	}

	// Setup uniforms for draw calls
	if (call->type == VKNVG_FILL) {
		// Quad
		call->triangleOffset = offset;
		quad = &vk->verts[size_t(call->triangleOffset)];
		vset(&quad[0], bounds[2], bounds[3], 0.5f, 1.0f);
		vset(&quad[1], bounds[2], bounds[1], 0.5f, 1.0f);
		vset(&quad[2], bounds[0], bounds[3], 0.5f, 1.0f);
		vset(&quad[3], bounds[0], bounds[1], 0.5f, 1.0f);

		call->uniformOffset = allocFragUniforms(vk, 2);
		// Simple shader for stencil
		frag = fragUniformPtr(vk, call->uniformOffset);
		std::memset(frag, 0, sizeof(*frag));
		frag->strokeThr = -1.0f;
		frag->type = NSVG_SHADER_SIMPLE;
		// Fill shader
		if (!convertPaint(vk, fragUniformPtr(vk, call->uniformOffset + vk->fragSize), paint, scissor, fringe, fringe, -1.0f, 0))
			goto error;
	} else {
		call->uniformOffset = allocFragUniforms(vk, 1);
		// Fill shader
		if (!convertPaint(vk, fragUniformPtr(vk, call->uniformOffset), paint, scissor, fringe, fringe, -1.0f, 0))
			goto error;
	}

	return;

error:
	// Roll back the last call to prevent drawing it.
	vk->calls.pop_back();
}

void vknvg__renderStroke(void* uptr, NVGpaint* paint, NVGcompositeOperationState compositeOperation, NVGscissor* scissor, float fringe,
	float strokeWidth, int lineStyle, const NVGpath* paths, int npaths)
{
	auto* vk = static_cast<VKNVGcontext*>(uptr);
	VKNVGcall* call = allocCall(vk);
	int i, maxverts, offset;

	call->type = VKNVG_STROKE;
	call->pathOffset = allocPaths(vk, npaths);
	call->pathCount = npaths;
	call->image = paint->image;
	call->blendFunc = blendCompositeOperation(compositeOperation);

	// Allocate vertices for all the paths.
	maxverts = maxVertCount(paths, npaths);
	offset = allocVerts(vk, maxverts);

	for (i = 0; i < npaths; i++) {
		VKNVGpath* copy = &vk->paths[size_t(call->pathOffset + i)];
		const NVGpath* path = &paths[i];
		std::memset(copy, 0, sizeof(VKNVGpath));
		if (path->nstroke) {
			copy->strokeOffset = offset;
			copy->strokeCount = path->nstroke;
			std::memcpy(&vk->verts[size_t(offset)], path->stroke, sizeof(NVGvertex) * size_t(path->nstroke));
			offset += path->nstroke;
		}
	}

	if (vk->flags & NVGVK_STENCIL_STROKES) {
		// Fill shader
		call->uniformOffset = allocFragUniforms(vk, 2);
		if (!convertPaint(vk, fragUniformPtr(vk, call->uniformOffset), paint, scissor, strokeWidth, fringe, -1.0f, lineStyle))
			goto error;
		if (!convertPaint(vk, fragUniformPtr(vk, call->uniformOffset + vk->fragSize), paint, scissor, strokeWidth, fringe, 1.0f - 0.5f/255.0f, lineStyle))
			goto error;
	} else {
		// Fill shader
		call->uniformOffset = allocFragUniforms(vk, 1);
		if (!convertPaint(vk, fragUniformPtr(vk, call->uniformOffset), paint, scissor, strokeWidth, fringe, -1.0f, lineStyle))
			goto error;
	}

	return;

error:
	vk->calls.pop_back();
}

void vknvg__renderTriangles(void* uptr, NVGpaint* paint, NVGcompositeOperationState compositeOperation, NVGscissor* scissor,
	const NVGvertex* verts, int nverts, float fringe)
{
	auto* vk = static_cast<VKNVGcontext*>(uptr);
	VKNVGcall* call = allocCall(vk);
	VKNVGfragUniforms* frag;

	call->type = VKNVG_TRIANGLES;
	call->image = paint->image;
	call->blendFunc = blendCompositeOperation(compositeOperation);

	// Allocate vertices for all the paths.
	call->triangleOffset = allocVerts(vk, nverts);
	call->triangleCount = nverts;

	std::memcpy(&vk->verts[size_t(call->triangleOffset)], verts, sizeof(NVGvertex) * size_t(nverts));

	// Fill shader
	call->uniformOffset = allocFragUniforms(vk, 1);
	frag = fragUniformPtr(vk, call->uniformOffset);
	if (!convertPaint(vk, frag, paint, scissor, 1.0f, fringe, -1.0f, 0)) {
		vk->calls.pop_back();
		return;
	}
	frag->type = NSVG_SHADER_IMG;
}

void vknvg__renderDelete(void* uptr)
{
	auto* vk = static_cast<VKNVGcontext*>(uptr);
	if (vk == nullptr) return;

	if (vk->device != VK_NULL_HANDLE) {
		for (const auto& t : vk->textures)
			if (t.id != 0) destroyTextureLater(vk, t);

		struct Graveyard {
			VkDevice device;
			VkShaderModule vert, frag;
			VkDescriptorSetLayout setLayout;
			VkPipelineLayout pipelineLayout;
			std::vector<VkPipeline> pipelines;
			std::vector<VkSampler> samplers;
		};
		auto g = std::make_shared<Graveyard>();
		g->device = vk->device;
		g->vert = vk->vert;
		g->frag = vk->frag;
		g->setLayout = vk->setLayout;
		g->pipelineLayout = vk->pipelineLayout;
		for (auto& kv : vk->pipelines) if (kv.second) g->pipelines.push_back(kv.second);
		for (VkSampler s : vk->samplers) if (s) g->samplers.push_back(s);
		if (vk->pool) {
			vk->pool->dead = true;
			for (Chunk* c : vk->pool->free) vk->pool->destroy(c);
			vk->pool->free.clear();
		}
		if (vk->host.deferDestroy) {
			deferDestroy(vk, [g]() {
				for (VkPipeline p : g->pipelines) vkDestroyPipeline(g->device, p, nullptr);
				for (VkSampler s : g->samplers) vkDestroySampler(g->device, s, nullptr);
				if (g->pipelineLayout) vkDestroyPipelineLayout(g->device, g->pipelineLayout, nullptr);
				if (g->setLayout) vkDestroyDescriptorSetLayout(g->device, g->setLayout, nullptr);
				if (g->vert) vkDestroyShaderModule(g->device, g->vert, nullptr);
				if (g->frag) vkDestroyShaderModule(g->device, g->frag, nullptr);
			});
		}
	}
	delete vk;
}

} // namespace

extern "C" {

NVGcontext* nvgCreateVk(const NVGVKcreateInfo* info, int flags)
{
	if (info == nullptr || info->device == VK_NULL_HANDLE || info->physicalDevice == VK_NULL_HANDLE
		|| !info->host.renderingCommandBuffer || !info->host.outsideRendering
		|| !info->host.invalidateBoundState || !info->host.deferDestroy)
		return nullptr;

	auto* vk = new VKNVGcontext();
	vk->host = info->host;
	vk->physicalDevice = info->physicalDevice;
	vk->device = info->device;
	vk->flags = flags;

	NVGparams params;
	std::memset(&params, 0, sizeof(params));
	params.renderCreate = vknvg__renderCreate;
	params.renderCreateTexture = vknvg__renderCreateTexture;
	params.renderDeleteTexture = vknvg__renderDeleteTexture;
	params.renderUpdateTexture = vknvg__renderUpdateTexture;
	params.renderGetTextureSize = vknvg__renderGetTextureSize;
	params.renderGetImageTextureId = vknvg__renderGetImageTextureId;
	params.renderViewport = vknvg__renderViewport;
	params.renderCancel = vknvg__renderCancel;
	params.renderFlush = vknvg__renderFlush;
	params.renderFill = vknvg__renderFill;
	params.renderStroke = vknvg__renderStroke;
	params.renderTriangles = vknvg__renderTriangles;
	params.renderDelete = vknvg__renderDelete;
	params.userPtr = vk;
	params.edgeAntiAlias = flags & NVGVK_ANTIALIAS ? 1 : 0;

	// 'vk' is freed by nvgDeleteInternal on failure.
	return nvgCreateInternal(&params);
}

void nvgDeleteVk(NVGcontext* ctx)
{
	nvgDeleteInternal(ctx);
}

int nvgvkCreateImageFromView(NVGcontext* ctx, VkImageView view, VkSampler sampler,
	VkImageLayout layout, int w, int h, int imageFlags)
{
	auto* vk = static_cast<VKNVGcontext*>(nvgInternalParams(ctx)->userPtr);
	VKNVGtexture* tex = allocTexture(vk);
	tex->type = NVG_TEXTURE_RGBA;
	tex->view = view;
	tex->sampler = sampler;
	tex->layout = layout;
	tex->flags = imageFlags;
	tex->width = w;
	tex->height = h;
	return tex->id;
}

int nvgvkCreateImageResolved(NVGcontext* ctx, NVGVKresolveImage resolve, void* arg,
	int w, int h, int imageFlags)
{
	auto* vk = static_cast<VKNVGcontext*>(nvgInternalParams(ctx)->userPtr);
	VKNVGtexture* tex = allocTexture(vk);
	tex->type = NVG_TEXTURE_RGBA;
	tex->resolve = resolve;
	tex->resolveArg = arg;
	tex->flags = imageFlags;
	tex->width = w;
	tex->height = h;
	return tex->id;
}

void nvgvkViewport(NVGcontext* ctx, int x, int y, int w, int h)
{
	auto* vk = static_cast<VKNVGcontext*>(nvgInternalParams(ctx)->userPtr);
	vk->viewport[0] = x;
	vk->viewport[1] = y;
	vk->viewport[2] = w;
	vk->viewport[3] = h;
}

} // extern "C"
