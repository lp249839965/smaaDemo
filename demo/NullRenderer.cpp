#ifdef RENDERER_NULL

#include "RendererInternal.h"
#include "Utils.h"


Buffer::Buffer()
: ringBufferAlloc(false)
, size(0)
{
}


Buffer::~Buffer() {
}


RendererImpl::RendererImpl(const RendererDesc &desc)
: swapchainDesc(desc.swapchain)
, savePreprocessedShaders(false)
, frameNum(0)
, ringBufSize(desc.ephemeralRingBufSize)
, ringBufPtr(0)
, numBuffers(0)
, numSamplers(0)
, numTextures(0)
, inFrame(false)
, inRenderPass(false)
, validPipeline(false)
, pipelineDrawn(false)
, scissorSet(false)
{
	SDL_Init(SDL_INIT_EVENTS);

	ringBuffer.resize(ringBufSize, 0);
	// TODO: use valgrind to make sure we only write to intended parts of ring buffer
}


RendererImpl::~RendererImpl() {
	SDL_Quit();
}


BufferHandle RendererImpl::createBuffer(uint32_t size, const void *contents) {
	assert(size != 0);
	assert(contents != nullptr);

	// TODO: check desc

	numBuffers++;
	return numBuffers;
}


BufferHandle RendererImpl::createEphemeralBuffer(uint32_t size, const void *contents) {
	assert(size != 0);
	assert(contents != nullptr);

	// sub-allocate from persistent coherent buffer
	// round current pointer up to necessary alignment
	const unsigned int align = 8;
	const unsigned int add   = (1 << align) - 1;
	const unsigned int mask  = ~add;
	unsigned int alignedPtr  = (ringBufPtr + add) & mask;
	assert(ringBufPtr <= alignedPtr);
	// TODO: ring buffer size should be pow2, se should use add & mask here too
	unsigned int beginPtr    =  alignedPtr % ringBufSize;

	if (beginPtr + size >= ringBufSize) {
		// we went past the end and have to go back to beginning
		// TODO: add and mask here too
		ringBufPtr = (ringBufPtr / ringBufSize + 1) * ringBufSize;
		assert((ringBufPtr & ~mask) == 0);
		alignedPtr  = (ringBufPtr + add) & mask;
		beginPtr    =  alignedPtr % ringBufSize;
		assert(beginPtr + size < ringBufSize);
		assert(beginPtr == 0);
	}
	ringBufPtr = alignedPtr + size;

	// TODO: use valgrind to enforce we only write to intended parts of ring buffer
	memcpy(&ringBuffer[beginPtr], contents, size);

	auto result    = buffers.add();
	Buffer &buffer = result.first;
	buffer.ringBufferAlloc = true;
	buffer.beginOffs       = beginPtr;
	buffer.size            = size;

	ephemeralBuffers.push_back(result.second);

	return result.second;
}


RenderPassHandle RendererImpl::createRenderPass(const RenderPassDesc & /* desc */) {
	return RenderPassHandle(0);
}


PipelineHandle RendererImpl::createPipeline(const PipelineDesc &desc) {
	auto result = pipelines.add();
	result.first = desc;
	return result.second;
}


RenderTargetHandle RendererImpl::createRenderTarget(const RenderTargetDesc &desc) {
	assert(desc.width_  > 0);
	assert(desc.height_ > 0);
	assert(desc.format_ != Invalid);

	return 0;
}


SamplerHandle RendererImpl::createSampler(const SamplerDesc & /* desc */) {
	// TODO: check desc

	numSamplers++;
	return numSamplers;
}


VertexShaderHandle RendererImpl::createVertexShader(const std::string & /* name */, const ShaderMacros & /* macros */) {
	return VertexShaderHandle (0);
}


FragmentShaderHandle RendererImpl::createFragmentShader(const std::string & /* name */, const ShaderMacros & /* macros */) {
	return FragmentShaderHandle (0);
}


TextureHandle RendererImpl::createTexture(const TextureDesc &desc) {
	assert(desc.width_   > 0);
	assert(desc.height_  > 0);
	assert(desc.numMips_ > 0);

	// TODO: check data

	numTextures++;
	return numTextures;
}


DescriptorSetLayoutHandle RendererImpl::createDescriptorSetLayout(const DescriptorLayout * /* layout */) {
	return 0;
}


void RendererImpl::deleteBuffer(BufferHandle /* handle */) {
}


void RendererImpl::deleteRenderPass(RenderPassHandle /* fbo */) {
}


void RendererImpl::deleteRenderTarget(RenderTargetHandle &) {
}


void RendererImpl::deleteSampler(SamplerHandle /* handle */) {
}


void RendererImpl::deleteTexture(TextureHandle /* handle */) {
}


void RendererImpl::recreateSwapchain(const SwapchainDesc & /* desc */) {
}


void RendererImpl::beginFrame() {
	assert(!inFrame);
	inFrame       = true;
	inRenderPass  = false;
	validPipeline = false;
	pipelineDrawn = true;
}


void RendererImpl::presentFrame(RenderTargetHandle /* rt */) {
	assert(inFrame);
	inFrame = false;

	// TODO: multiple frames, only delete after no longer in use by (simulated) GPU
	for (auto handle : ephemeralBuffers) {
		Buffer &buffer = buffers.get(handle);
		assert(buffer.ringBufferAlloc);
		assert(buffer.size   >  0);
		buffers.remove(handle);
	}
	ephemeralBuffers.clear();
}


void RendererImpl::beginRenderPass(RenderPassHandle /* pass */) {
	assert(inFrame);
	assert(!inRenderPass);
	inRenderPass  = true;
	validPipeline = false;
}


void RendererImpl::endRenderPass() {
	assert(inFrame);
	assert(inRenderPass);
	inRenderPass = false;
}


void RendererImpl::bindPipeline(PipelineHandle pipeline) {
	assert(inFrame);
	assert(pipeline != 0);
	assert(inRenderPass);
	assert(pipelineDrawn);
	pipelineDrawn = false;
	validPipeline = true;
	scissorSet = false;

	currentPipeline = pipelines.get(pipeline);
}


void RendererImpl::bindIndexBuffer(BufferHandle /* buffer */, bool /* bit16 */ ) {
	assert(inFrame);
	assert(validPipeline);
}


void RendererImpl::bindVertexBuffer(unsigned int /* binding */, BufferHandle /* buffer */) {
	assert(inFrame);
	assert(validPipeline);
}


void RendererImpl::bindDescriptorSet(unsigned int /* index */, DescriptorSetLayoutHandle /* layout */, const void * /* data_ */) {
	assert(validPipeline);
}


void RendererImpl::setViewport(unsigned int /* x */, unsigned int /* y */, unsigned int /* width */, unsigned int /* height */) {
	assert(inFrame);
}


void RendererImpl::setScissorRect(unsigned int /* x */, unsigned int /* y */, unsigned int /* width */, unsigned int /* height */) {
	assert(validPipeline);
	assert(currentPipeline.scissorTest_);
	scissorSet = true;
}


void RendererImpl::draw(unsigned int /* firstVertex */, unsigned int vertexCount) {
	assert(inRenderPass);
	assert(validPipeline);
	assert(vertexCount > 0);
	assert(!currentPipeline.scissorTest_ || scissorSet);
	pipelineDrawn = true;
}


void RendererImpl::drawIndexedInstanced(unsigned int vertexCount, unsigned int instanceCount) {
	assert(inRenderPass);
	assert(validPipeline);
	assert(vertexCount > 0);
	assert(instanceCount > 0);
	assert(!currentPipeline.scissorTest_ || scissorSet);
	pipelineDrawn = true;
}


void RendererImpl::drawIndexedOffset(unsigned int vertexCount, unsigned int /* firstIndex */) {
	assert(inRenderPass);
	assert(validPipeline);
	assert(vertexCount > 0);
	assert(!currentPipeline.scissorTest_ || scissorSet);
	pipelineDrawn = true;
}


#endif //  RENDERER_NULL
