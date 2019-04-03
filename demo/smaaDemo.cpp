/*
Copyright (c) 2015-2019 Alternative Games Ltd / Turo Lamminen

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/


#include <cassert>
#include <cfloat>
#include <cinttypes>
#include <cstdio>

#include <thread>
#include <chrono>

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
#include <unordered_set>

// TODO: use std::variant if the compiler has C++17
#include <boost/variant/variant.hpp>
#include <boost/variant/apply_visitor.hpp>
#include <boost/variant/static_visitor.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <tclap/CmdLine.h>

#include <pcg_random.hpp>

#include "renderer/Renderer.h"
#include "utils/Utils.h"

#include "AreaTex.h"
#include "SearchTex.h"

// AFTER Renderer.h because it sets GLM_FORCE_* macros which affect these
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>


// mingw fuckery...
#if defined(__GNUC__) && defined(_WIN32)

#include <mingw.thread.h>

#endif  // defined(__GNUC__) && defined(_WIN32)


namespace ShaderDefines {

using namespace glm;

#include "../shaderDefines.h"

}  // namespace ShaderDefines


using namespace renderer;


class SMAADemo;


enum class AAMethod : uint8_t {
	  MSAA
	, FXAA
	, SMAA
	, SMAA2X
	, LAST = SMAA2X
};


const char *name(AAMethod m) {
	switch (m) {
	case AAMethod::MSAA:
		return "MSAA";
		break;

	case AAMethod::FXAA:
		return "FXAA";
		break;

	case AAMethod::SMAA:
		return "SMAA";
		break;

	case AAMethod::SMAA2X:
		return "SMAA2X";
		break;
	}

	UNREACHABLE();
}


const char *smaaDebugModes[3] = { "None", "Edges", "Weights" };

static const unsigned int inputTextBufferSize = 1024;


const char* GetClipboardText(void* user_data) {
	char *clipboard = SDL_GetClipboardText();
	if (clipboard) {
		char *text = static_cast<char*>(user_data);
		size_t length = strnlen(clipboard, inputTextBufferSize - 1);
		strncpy(text, clipboard, length);
		text[length] = '\0';
		SDL_free(clipboard);
		return text;
	} else {
		return nullptr;
	}
}


void SetClipboardText(void* /* user_data */, const char* text) {
	SDL_SetClipboardText(text);
}


class RandomGen {
	pcg32 rng;

	RandomGen(const RandomGen &) = delete;
	RandomGen &operator=(const RandomGen &) = delete;
	RandomGen(RandomGen &&) = delete;
	RandomGen &operator=(RandomGen &&) = delete;

public:

	explicit RandomGen(uint64_t seed)
	: rng(seed)
	{
	}


	float randFloat() {
		uint32_t u = randU32();
		// because 24 bits mantissa
		u &= 0x00FFFFFFU;
		return float(u) / 0x00FFFFFFU;
	}


	uint32_t randU32() {
		return rng();
	}


	// min inclusive
	// max exclusive
	uint32_t range(uint32_t min, uint32_t max) {
		uint32_t range = max - min;
		uint32_t size = std::numeric_limits<uint32_t>::max() / range;
		uint32_t discard = size * range;

		uint32_t r;
		do {
			r = rng();
		} while (r >= discard);

		return min + r / size;
	}
};


static const char *const msaaQualityLevels[] =
{ "2x", "4x", "8x", "16x", "32x", "64x" };


static unsigned int msaaSamplesToQuality(unsigned int q) {
	assert(q > 1);
	assert(isPow2(q));

	// TODO: have a more specific function for this
	unsigned int retval = 0;
	unsigned int count = 0;
	forEachSetBit(q, [&] (uint32_t bit, uint32_t /* mask */) {
		assert(bit > 0);
		retval = bit - 1;
		count++;
	});

	assert(count == 1);
	return retval;
}


static unsigned int msaaQualityToSamples(unsigned int n) {
	return (1 << (n + 1));
}


static const char *fxaaQualityLevels[] =
{ "10", "15", "20", "29", "39" };


static const unsigned int maxFXAAQuality = sizeof(fxaaQualityLevels) / sizeof(fxaaQualityLevels[0]);


static const char *smaaQualityLevels[] =
{ "CUSTOM", "LOW", "MEDIUM", "HIGH", "ULTRA" };


static const unsigned int maxSMAAQuality = sizeof(smaaQualityLevels) / sizeof(smaaQualityLevels[0]);


static const std::array<ShaderDefines::SMAAParameters, maxSMAAQuality> defaultSMAAParameters =
{ {
	  { 0.05f, 0.1f * 0.15f, 32u, 16u, 25u, 0u, 0u, 0u }  // custom
	, { 0.15f, 0.1f * 0.15f,  1u,  8u, 25u, 0u, 0u, 0u }  // low
	, { 0.10f, 0.1f * 0.10f,  1u,  8u, 25u, 0u, 0u, 0u }  // medium
	, { 0.10f, 0.1f * 0.10f, 16u,  8u, 25u, 0u, 0u, 0u }  // high
	, { 0.05f, 0.1f * 0.05f, 32u, 16u, 25u, 0u, 0u, 0u }  // ultra
} };


enum class SMAAEdgeMethod : uint8_t {
	  Color
	, Luma
	, Depth
};


struct Image {
	std::string    filename;
	std::string    shortName;
	TextureHandle  tex;
	unsigned int   width, height;


	Image()
	: width(0)
	, height(0)
	{
	}


	Image(const Image &)             = default;
	Image(Image &&) noexcept            = default;

	Image &operator=(const Image &)  = default;
	Image &operator=(Image &&) noexcept = default;

	~Image() {}
};


enum class Rendertargets : uint32_t {
	  Invalid
	, MainColor
	, MainDepth
	, Velocity
	, VelocityMS
	, Edges
	, BlendWeights
	, TemporalPrevious
	, TemporalCurrent
	, Subsample1
	, Subsample2
	, FinalRender
};


static const char *to_string(Rendertargets r) {

	switch (r) {
	case Rendertargets::MainColor:
		return "MainColor";

	case Rendertargets::MainDepth:
		return "MainDepth";

	case Rendertargets::Velocity:
		return "Velocity";

	case Rendertargets::VelocityMS:
		return "VelocityMS";

	case Rendertargets::Edges:
		return "Edges";

	case Rendertargets::BlendWeights:
		return "BlendWeights";

	case Rendertargets::TemporalPrevious:
		return "TemporalPrevious";

	case Rendertargets::TemporalCurrent:
		return "TemporalCurrent";

	case Rendertargets::Subsample1:
		return "Subsample1";

	case Rendertargets::Subsample2:
		return "Subsample2";

	case Rendertargets::FinalRender:
		return "FinalRender";

	case Rendertargets::Invalid:
		return "Invalid";

	}

	UNREACHABLE();
	return "";
}


enum class RenderPasses : uint32_t {
	  Invalid
	, Scene
	, Final
	, GUI
	, FXAA
	, Separate
	, SMAAEdges
	, SMAAWeights
	, SMAABlend
	, SMAAEdges2
	, SMAAWeights2
	, SMAA2XBlend1
	, SMAA2XBlend2
};


static const char *to_string(RenderPasses r) {
	switch (r) {
	case RenderPasses::Scene:
		return "Scene";

	case RenderPasses::Final:
		return "Final";

	case RenderPasses::GUI:
		return "GUI";

	case RenderPasses::FXAA:
		return "FXAA";

	case RenderPasses::Separate:
		return "Separate";

	case RenderPasses::SMAAEdges:
		return "SMAAEdges";

	case RenderPasses::SMAAWeights:
		return "SMAAweights";

	case RenderPasses::SMAABlend:
		return "SMAABlend";

	case RenderPasses::SMAAEdges2:
		return "SMAAEdges2";

	case RenderPasses::SMAAWeights2:
		return "SMAAWeights2";

	case RenderPasses::SMAA2XBlend1:
		return "SMAA2XBlend1";

	case RenderPasses::SMAA2XBlend2:
		return "SMAA2XBlend2";

	case RenderPasses::Invalid:
		return "Invalid";
	}

	UNREACHABLE();
	return "";
}


namespace std {


template <> struct hash<RenderPasses> {
	size_t operator()(const RenderPasses &k) const {
		return hash<size_t>()(static_cast<size_t>(k));
	}

};


template <> struct hash<Rendertargets> {
	size_t operator()(const Rendertargets &k) const {
		return hash<size_t>()(static_cast<size_t>(k));
	}

};


template <> struct hash<Format> {
	size_t operator()(const Format &k) const {
		return hash<size_t>()(static_cast<size_t>(k));
	}
};


template <> struct hash<std::pair<Rendertargets, Format> > {
	size_t operator()(const std::pair<Rendertargets, Format> &k) const {
		size_t a = hash<Rendertargets>()(k.first);
		size_t b = hash<Format>()(k.second);

		// TODO: put this in a helper function
		a ^= b + 0x9e3779b9 + (a << 6) + (a >> 2);

		return a;
	}
};


} // namespace std


struct SMAAPipelines {
	PipelineHandle  edgePipeline;
	PipelineHandle  blendWeightPipeline;
	std::array<PipelineHandle, 2>  neighborPipelines;
};


class RenderGraph {

public:

	class PassResources {

		std::unordered_map<std::pair<Rendertargets, Format>, TextureHandle>  rendertargets;

		// TODO: buffers

	public:

		TextureHandle get(Rendertargets rt, Format fmt = Format::Invalid) const {
			std::pair<Rendertargets, Format> p;
			p.first  = rt;
			p.second = fmt;

			auto it = rendertargets.find(p);
			// failing this means the rendertarget was not correctly declared as input
			assert(it != rendertargets.end());

			return it->second;
		}

		friend class RenderGraph;
	};


	struct PassDesc {
		PassDesc()
		: depthStencil_(Rendertargets::Invalid)
		, numSamples_(1)
		, clearDepthAttachment(false)
		, depthClearValue(1.0f)
		{
			for (auto &rt : colorRTs_) {
				rt.id            = Rendertargets::Invalid;
				rt.passBegin     = PassBegin::DontCare;
				rt.clearValue    = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
			}
		}

		~PassDesc() { }

		PassDesc(const PassDesc &)                = default;
		PassDesc(PassDesc &&) noexcept            = default;

		PassDesc &operator=(const PassDesc &)     = default;
		PassDesc &operator=(PassDesc &&) noexcept = default;

		PassDesc &depthStencil(Rendertargets ds, PassBegin) {
			depthStencil_ = ds;
			return *this;
		}

		PassDesc &color(unsigned int index, Rendertargets id, PassBegin pb, glm::vec4 clear = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f)) {
			assert(index < MAX_COLOR_RENDERTARGETS);
			assert(id != Rendertargets::Invalid);
			colorRTs_[index].id             = id;
			colorRTs_[index].passBegin      = pb;
			if (pb == PassBegin::Clear) {
				colorRTs_[index].clearValue = clear;
			}
			return *this;
		}

		PassDesc &clearDepth(float v) {
			clearDepthAttachment  = true;
			depthClearValue       = v;
			return *this;
		}

		PassDesc &name(const std::string &str) {
			name_ = str;
			return *this;
		}

		PassDesc &numSamples(unsigned int n) {
			numSamples_ = n;
			return *this;
		}

		PassDesc &inputRendertarget(Rendertargets id) {
			auto success UNUSED = inputRendertargets.emplace(id);
			assert(success.second);
			return *this;
		}

		struct RTInfo {
			Rendertargets  id;
			PassBegin                     passBegin;
			glm::vec4                     clearValue;
		};

		Rendertargets                 depthStencil_;
		std::array<RTInfo, MAX_COLOR_RENDERTARGETS>  colorRTs_;
		std::unordered_set<Rendertargets>  inputRendertargets;
		unsigned int                                 numSamples_;
		std::string                                  name_;
		bool                                         clearDepthAttachment;
		float                                        depthClearValue;
	};

	typedef  std::function<void(RenderPasses, PassResources &)>  RenderPassFunc;


private:

	enum class State : uint8_t {
		  Invalid
		, Building
		, Ready
		, Rendering
	};


	struct RenderPass {
		RenderPassHandle   handle;
		FramebufferHandle  fb;
		RenderPassFunc     func;
		PassDesc           desc;
		RenderPassDesc     rpDesc;
	};


	struct InternalRT {
		RenderTargetHandle  handle;
		RenderTargetDesc    desc;
	};


	struct ExternalRT {
		Format              format;
		Layout              initialLayout;
		Layout              finalLayout;

		// not owned by us
		// only valid during frame
		RenderTargetHandle  handle;

		// TODO: map of renderpasses which use this
	};


	typedef boost::variant<ExternalRT, InternalRT> Rendertarget;


	template <typename FE, typename FI>
	static void visitRendertarget(Rendertarget &rt, FE &&fe, FI &&fi) {
		struct FuncVisitor final : public boost::static_visitor<void> {
			FE fe;
			FI fi;

			explicit FuncVisitor(FE &&fe_, FI &&fi_)
			: fe(std::move(fe_))
			, fi(std::move(fi_))
			{
			}

			void operator()(ExternalRT &e) const {
				fe(e);
			}

			void operator()(InternalRT &i) const {
				fi(i);
			}
		};

		return boost::apply_visitor(FuncVisitor(std::move(fe), std::move(fi)), rt);
	}


	template <typename FE, typename FI>
	static void visitRendertarget(const Rendertarget &rt, FE &&fe, FI &&fi) {
		struct FuncVisitor final : public boost::static_visitor<void> {
			FE fe;
			FI fi;

			explicit FuncVisitor(FE &&fe_, FI &&fi_)
			: fe(std::move(fe_))
			, fi(std::move(fi_))
			{
			}

			void operator()(const ExternalRT &e) const {
				fe(e);
			}

			void operator()(const InternalRT &i) const {
				fi(i);
			}
		};

		return boost::apply_visitor(FuncVisitor(std::move(fe), std::move(fi)), rt);
	}


	template <typename T, typename FE, typename FI>
	static T visitRendertargetValue(Rendertarget &rt, FE &&fe, FI &&fi) {
		struct FuncVisitor final : public boost::static_visitor<T> {
			FE fe;
			FI fi;

			explicit FuncVisitor(FE &&fe_, FI &&fi_)
			: fe(std::move(fe_))
			, fi(std::move(fi_))
			{
			}

			T operator()(ExternalRT &e) const {
				return fe(e);
			}

			T operator()(InternalRT &i) const {
				return fi(i);
			}
		};

		return boost::apply_visitor(FuncVisitor(std::move(fe), std::move(fi)), rt);
	}


	template <typename T, typename FE, typename FI>
	static T visitRendertargetValue(const Rendertarget &rt, FE &&fe, FI &&fi) {
		struct FuncVisitor final : public boost::static_visitor<T> {
			FE fe;
			FI fi;

			explicit FuncVisitor(FE &&fe_, FI &&fi_)
			: fe(std::move(fe_))
			, fi(std::move(fi_))
			{
			}

			T operator()(const ExternalRT &e) const {
				return fe(e);
			}

			T operator()(const InternalRT &i) const {
				return fi(i);
			}
		};

		return boost::apply_visitor(FuncVisitor(std::move(fe), std::move(fi)), rt);
	}


	static void nopExternal(const ExternalRT & /* e */) { }
	static void nopInternal(const InternalRT & /* i */) { }


	static Format getFormat(const Rendertarget &rt) {
		return visitRendertargetValue<Format>(rt
		                       , [] (const ExternalRT &e) { return e.format;        }
		                       , [] (const InternalRT &i) { return i.desc.format(); }
		                        );
	}


	static Format getAdditionalViewFormat(const Rendertarget &rt) {
		return visitRendertargetValue<Format>(rt
		                       , [] (const ExternalRT & /* e */) { return Format::Invalid;               }
		                       , [] (const InternalRT &i)        { return i.desc.additionalViewFormat(); }
		                        );
	}


	static RenderTargetHandle getHandle(const Rendertarget &rt) {
		return visitRendertargetValue<RenderTargetHandle>(rt
		                       , [] (const ExternalRT &e) { return e.handle; }
		                       , [] (const InternalRT &i) { return i.handle; }
		                        );
	}


	static bool isExternal(const Rendertarget &rt) {
		return visitRendertargetValue<bool>(rt
		                       , [] (const ExternalRT & /* e */) { return true;  }
		                       , [] (const InternalRT & /* i */) { return false; }
		                        );
	}


	void buildRenderPassFramebuffer(Renderer &renderer, RenderPass &rp) {
		const auto &desc = rp.desc;

		FramebufferDesc fbDesc;
		fbDesc.renderPass(rp.handle)
			  .name(desc.name_);

		if (desc.depthStencil_ != Rendertargets::Invalid) {
			auto it = rendertargets.find(desc.depthStencil_);
			assert(it != rendertargets.end());
			fbDesc.depthStencil(getHandle(it->second));
		}

		for (unsigned int i = 0; i < MAX_COLOR_RENDERTARGETS; i++) {
			const auto &rt = desc.colorRTs_[i];
			if (rt.id != Rendertargets::Invalid) {
				auto it = rendertargets.find(rt.id);
				assert(it != rendertargets.end());
				fbDesc.color(i, getHandle(it->second));
			}
		}

		// TODO: cache framebuffers
		auto fbHandle = renderer.createFramebuffer(fbDesc);
		assert(fbHandle);
		rp.fb = fbHandle;
	}


	struct Blit {
		Rendertargets  source;
		Rendertargets  dest;
		Layout         finalLayout;
	};

	struct ResolveMSAA {
		Rendertargets  source;
		Rendertargets  dest;
		Layout         finalLayout;
	};

	struct Pipeline {
		PipelineDesc    desc;
		PipelineHandle  handle;
	};

	typedef boost::variant<Blit, RenderPasses, ResolveMSAA> Operation;


	State                                          state;
	bool                                           hasExternalRTs;
	RenderPasses                     currentRP;
	std::vector<Operation>                         operations;
	Rendertargets                   finalTarget;

	std::unordered_map<Rendertargets, Rendertarget>  rendertargets;

	// TODO: use hash map
	std::vector<Pipeline>                          pipelines;

	std::unordered_map<RenderPasses, RenderPass> renderPasses;
	std::unordered_set<RenderPasses>     renderpassesWithExternalRTs;


	RenderGraph(const RenderGraph &)                = delete;
	RenderGraph(RenderGraph &&) noexcept            = delete;

	RenderGraph &operator=(const RenderGraph &)     = delete;
	RenderGraph &operator=(RenderGraph &&) noexcept = delete;


public:


	RenderGraph()
	: state(State::Invalid)
	, currentRP(RenderPasses::Invalid)
	{
	}


	~RenderGraph() {
	}


	void reset(Renderer &renderer, std::function<void(void)> processEvents);

	void renderTarget(Rendertargets rt, const RenderTargetDesc &desc);

	void externalRenderTarget(Rendertargets rt, Format format, Layout initialLayout, Layout finalLayout);

	void renderPass(RenderPasses rp, const PassDesc &desc, RenderPassFunc f);

	void resolveMSAA(Rendertargets source, Rendertargets dest);

	void blit(Rendertargets source, Rendertargets dest);

	void presentRenderTarget(Rendertargets rt);

	void build(Renderer &renderer);

	void bindExternalRT(Rendertargets rt, RenderTargetHandle handle);

	void render(Renderer &renderer);

	PipelineHandle createPipeline(Renderer &renderer, RenderPasses rp, PipelineDesc &desc);
};


class SMAADemo {
	RendererDesc                                      rendererDesc;
	glm::uvec2                                        renderSize;
	RenderGraph                                       renderGraph;

	// command line things
	std::vector<std::string>                          imageFiles;

	bool                                              recreateSwapchain;
	bool                                              rebuildRG;
	bool                                              keepGoing;

	// aa things
	bool                                              antialiasing;
	AAMethod                                          aaMethod;
	bool                                              temporalAA;
	bool                                              temporalAAFirstFrame;
	unsigned int                                      temporalFrame;
	bool                                              temporalReproject;
	float                                             reprojectionWeightScale;
	// number of samples in current scene fb
	// 1 or 2 if SMAA
	// 2.. if MSAA
	unsigned int                                      numSamples;
	unsigned int                                      debugMode;
	unsigned int                                      fxaaQuality;
	unsigned int                                      msaaQuality;
	unsigned int                                      maxMSAAQuality;

	unsigned int                                      smaaQuality;
	SMAAEdgeMethod                                    smaaEdgeMethod;
	bool                                              smaaPredication;
	ShaderDefines::SMAAParameters                     smaaParameters;

	float                                             predicationThreshold;
	float                                             predicationScale;
	float                                             predicationStrength;

	// timing things
	bool                                              fpsLimitActive;
	uint32_t                                          fpsLimit;
	uint64_t                                          sleepFudge;
	uint64_t                                          tickBase;
	uint64_t                                          lastTime;
	uint64_t                                          freqMult;
	uint64_t                                          freqDiv;

	// scene things
	// 0 for cubes
	// 1.. for images
	unsigned int                                      activeScene;
	unsigned int                                      cubesPerSide;
	unsigned int                                      colorMode;
	bool                                              rotateCubes;
	bool                                              visualizeCubeOrder;
	unsigned int                                      cubeOrderNum;
	float                                             cameraRotation;
	float                                             cameraDistance;
	uint64_t                                          rotationTime;
	unsigned int                                      rotationPeriodSeconds;
	RandomGen                                         random;
	std::vector<Image>                                images;
	std::vector<ShaderDefines::Cube>                  cubes;

	glm::mat4                                         currViewProj;
	glm::mat4                                         prevViewProj;
	std::array<glm::vec4, 2>                          subsampleIndices;

	Renderer                                          renderer;
	Format                                            depthFormat;

	std::array<RenderTargetHandle, 2>                 temporalRTs;

	PipelineHandle                                    cubePipeline;
	PipelineHandle                                    imagePipeline;
	PipelineHandle                                    blitPipeline;
	PipelineHandle                                    guiPipeline;
	PipelineHandle                                    separatePipeline;
	std::array<PipelineHandle, 2>                     temporalAAPipelines;
	PipelineHandle                                    fxaaPipeline;

	BufferHandle                                      cubeVBO;
	BufferHandle                                      cubeIBO;

	SamplerHandle                                     linearSampler;
	SamplerHandle                                     nearestSampler;

	SMAAPipelines                                     smaaPipelines;
	TextureHandle                                     areaTex;
	TextureHandle                                     searchTex;

	// gui / input things
	TextureHandle                                     imguiFontsTex;
	ImGuiContext                                      *imGuiContext;
	bool                                              textInputActive;
	bool                                              rightShift, leftShift;
	bool                                              rightAlt,   leftAlt;
	bool                                              rightCtrl,  leftCtrl;
	char                                              imageFileName[inputTextBufferSize];
	char                                              clipboardText[inputTextBufferSize];


	SMAADemo(const SMAADemo &) = delete;
	SMAADemo &operator=(const SMAADemo &) = delete;
	SMAADemo(SMAADemo &&) = delete;
	SMAADemo &operator=(SMAADemo &&) = delete;

	void renderFXAA(RenderPasses rp, RenderGraph::PassResources &r);

	void renderSeparate(RenderPasses rp, RenderGraph::PassResources &r);

	void renderSMAAEdges(RenderPasses rp, RenderGraph::PassResources &r, Rendertargets input, int pass);

	void renderSMAAWeights(RenderPasses rp, RenderGraph::PassResources &r, int pass);

	void renderSMAABlend(RenderPasses rp, RenderGraph::PassResources &r, Rendertargets input, int pass);

	void renderSMAADebug(RenderPasses rp, RenderGraph::PassResources &r, Rendertargets rt);

	void renderTemporalAA(RenderPasses rp, RenderGraph::PassResources &r);

	void updateGUI(uint64_t elapsed);

	void renderGUI(RenderPasses rp, RenderGraph::PassResources &r);

	void renderCubeScene(RenderPasses rp, RenderGraph::PassResources &r);

	void renderImageScene(RenderPasses rp, RenderGraph::PassResources &r);

	void loadImage(const std::string &filename);

	uint64_t getNanoseconds() {
		return (SDL_GetPerformanceCounter() - tickBase) * freqMult / freqDiv;
	}

	void shuffleCubeRendering();

	void reorderCubeRendering();

	void colorCubes();

	void setAntialiasing(bool enabled);

	void setTemporalAA(bool enabled);


public:

	SMAADemo();

	~SMAADemo();

	void parseCommandLine(int argc, char *argv[]);

	void initRender();

	void rebuildRenderGraph();

	void createCubes();

	void mainLoopIteration();

	void processInput();

	bool shouldKeepGoing() const {
		return keepGoing;
	}

	void render();
};


SMAADemo::SMAADemo()
: recreateSwapchain(false)
, rebuildRG(true)
, keepGoing(true)

, antialiasing(true)
, aaMethod(AAMethod::SMAA)
, temporalAA(false)
, temporalAAFirstFrame(false)
, temporalFrame(0)
, temporalReproject(true)
, reprojectionWeightScale(30.0f)
, numSamples(1)
, debugMode(0)
, fxaaQuality(maxFXAAQuality - 1)
, msaaQuality(0)
, maxMSAAQuality(1)
, predicationThreshold(0.01f)
, predicationScale(2.0f)
, predicationStrength(0.4f)

, fpsLimitActive(true)
, fpsLimit(0)
, sleepFudge(0)
, tickBase(0)
, lastTime(0)
, freqMult(0)
, freqDiv(0)

, activeScene(0)
, cubesPerSide(8)
, colorMode(0)
, rotateCubes(false)
, visualizeCubeOrder(false)
, cubeOrderNum(1)
, cameraRotation(0.0f)
, cameraDistance(25.0f)
, rotationTime(0)
, rotationPeriodSeconds(30)
, random(1)

, depthFormat(Format::Invalid)

, imGuiContext(nullptr)
, textInputActive(false)
, rightShift(false)
, leftShift(false)
, rightAlt(false)
, leftAlt(false)
, rightCtrl(false)
, leftCtrl(false)
{
	rendererDesc.swapchain.width  = 1280;
	rendererDesc.swapchain.height = 720;

	smaaQuality = maxSMAAQuality - 1;
	smaaEdgeMethod  = SMAAEdgeMethod::Color;
	smaaPredication = false;
	smaaParameters  = defaultSMAAParameters[smaaQuality];

	uint64_t freq = SDL_GetPerformanceFrequency();
	tickBase      = SDL_GetPerformanceCounter();

	freqMult   = 1000000000ULL;
	freqDiv    = freq;
	uint64_t g = gcd(freqMult, freqDiv);
	freqMult  /= g;
	freqDiv   /= g;
	LOG("freqMult: %" PRIu64 "\n", freqMult);
	LOG("freqDiv: %"  PRIu64 "\n", freqDiv);

	lastTime = getNanoseconds();

	// measure minimum sleep length and use it as fudge factor
	sleepFudge = 1000ULL * 1000ULL;
	for (unsigned int i = 0; i < 8; i++) {
		std::this_thread::sleep_for(std::chrono::nanoseconds(1));
		uint64_t ticks = getNanoseconds();
		uint64_t diff  = ticks - lastTime;
		sleepFudge     = std::min(sleepFudge, diff);
		lastTime       = ticks;
	}

	LOG("sleep fudge (nanoseconds): %" PRIu64 "\n", sleepFudge);

	memset(imageFileName, 0, inputTextBufferSize);
	memset(clipboardText, 0, inputTextBufferSize);
}


SMAADemo::~SMAADemo() {
	if (imGuiContext) {
		ImGui::DestroyContext(imGuiContext);
		imGuiContext = nullptr;
	}

	if (temporalRTs[0]) {
		assert(temporalRTs[1]);
		renderer.deleteRenderTarget(temporalRTs[0]);
		renderer.deleteRenderTarget(temporalRTs[1]);
	}

	renderGraph.reset(renderer, SDL_PumpEvents);

	if (cubeVBO) {
		renderer.deleteBuffer(cubeVBO);
		cubeVBO = BufferHandle();

		renderer.deleteBuffer(cubeIBO);
		cubeIBO = BufferHandle();
	}

	if (linearSampler) {
		renderer.deleteSampler(linearSampler);
		linearSampler = SamplerHandle();

		renderer.deleteSampler(nearestSampler);
		nearestSampler = SamplerHandle();
	}

	if (areaTex) {
		renderer.deleteTexture(areaTex);
		areaTex = TextureHandle();

		renderer.deleteTexture(searchTex);
		searchTex = TextureHandle();
	}
}


struct Vertex {
	float x, y, z;
};


const float coord = sqrtf(3.0f) / 2.0f;


static const Vertex vertices[] =
{
	  { -coord , -coord, -coord }
	, { -coord ,  coord, -coord }
	, {  coord , -coord, -coord }
	, {  coord ,  coord, -coord }
	, { -coord , -coord,  coord }
	, { -coord ,  coord,  coord }
	, {  coord , -coord,  coord }
	, {  coord ,  coord,  coord }
};


static const uint32_t indices[] =
{
	// top
	  1, 3, 5
	, 5, 3, 7

	// front
	, 0, 2, 1
	, 1, 2, 3

	// back
	, 7, 6, 5
	, 5, 6, 4

	// left
	, 0, 1, 4
	, 4, 1, 5

	// right
	, 2, 6, 3
	, 3, 6, 7

	// bottom
	, 2, 0, 6
	, 6, 0, 4
};


#define VBO_OFFSETOF(st, member) reinterpret_cast<GLvoid *>(offsetof(st, member))


void SMAADemo::parseCommandLine(int argc, char *argv[]) {
	try {
		TCLAP::CmdLine cmd("SMAA demo", ' ', "1.0");

		TCLAP::SwitchArg                       debugSwitch("",        "debug",      "Enable renderer debugging",     cmd, false);
		TCLAP::SwitchArg                       robustSwitch("",       "robust",     "Enable renderer robustness",    cmd, false);
		TCLAP::SwitchArg                       tracingSwitch("",      "trace",      "Enable renderer tracing",       cmd, false);
		TCLAP::SwitchArg                       noCacheSwitch("",      "nocache",    "Don't load shaders from cache", cmd, false);
		TCLAP::SwitchArg                       noOptSwitch("",        "noopt",      "Don't optimize shaders",        cmd, false);
		TCLAP::SwitchArg                       validateSwitch("",     "validate",   "Validate shader SPIR-V",        cmd, false);
		TCLAP::SwitchArg                       fullscreenSwitch("f",  "fullscreen", "Start in fullscreen mode",      cmd, false);
		TCLAP::SwitchArg                       noVsyncSwitch("",      "novsync",    "Disable vsync",                 cmd, false);
		TCLAP::SwitchArg                       noTransferQSwitch("",  "no-transfer-queue", "Disable transfer queue", cmd, false);

		TCLAP::ValueArg<unsigned int>          windowWidthSwitch("",  "width",      "Window width",  false, rendererDesc.swapchain.width,  "width",  cmd);
		TCLAP::ValueArg<unsigned int>          windowHeightSwitch("", "height",     "Window height", false, rendererDesc.swapchain.height, "height", cmd);

		TCLAP::ValueArg<unsigned int>          rotateSwitch("",       "rotate",     "Rotation period", false, 0,          "seconds", cmd);

		TCLAP::ValueArg<std::string>           aaMethodSwitch("m",    "method",     "AA Method",     false, "SMAA",        "SMAA/FXAA/MSAA", cmd);
		TCLAP::ValueArg<std::string>           aaQualitySwitch("q",   "quality",    "AA Quality",    false, "",            "", cmd);
		TCLAP::SwitchArg                       temporalAASwitch("t",  "temporal",   "Temporal AA", cmd, false);

		TCLAP::UnlabeledMultiArg<std::string>  imagesArg("images",    "image files", false, "image file", cmd, true, nullptr);

		cmd.parse(argc, argv);

		rendererDesc.debug                 = debugSwitch.getValue();
		rendererDesc.robustness            = robustSwitch.getValue();
		rendererDesc.tracing               = tracingSwitch.getValue();
		rendererDesc.skipShaderCache       = noCacheSwitch.getValue();
		rendererDesc.optimizeShaders       = !noOptSwitch.getValue();
		rendererDesc.validateShaders       = validateSwitch.getValue();
		rendererDesc.transferQueue         = !noTransferQSwitch.getValue();
		rendererDesc.swapchain.fullscreen  = fullscreenSwitch.getValue();
		rendererDesc.swapchain.width       = windowWidthSwitch.getValue();
		rendererDesc.swapchain.height      = windowHeightSwitch.getValue();
		rendererDesc.swapchain.vsync       = noVsyncSwitch.getValue() ? VSync::Off : VSync::On;

		unsigned int r = rotateSwitch.getValue();
		if (r != 0) {
			rotateCubes           = true;
			rotationPeriodSeconds = std::max(1U, std::min(r, 60U));
		}

		std::string aaMethodStr = aaMethodSwitch.getValue();
		std::transform(aaMethodStr.begin(), aaMethodStr.end(), aaMethodStr.begin(), ::toupper);
		std::string aaQualityStr = aaQualitySwitch.getValue();
		if (aaMethodStr == "SMAA") {
			aaMethod = AAMethod::SMAA;

			if (!aaQualityStr.empty()) {
				std::transform(aaQualityStr.begin(), aaQualityStr.end(), aaQualityStr.begin(), ::toupper);
				for (unsigned int i = 0; i < maxSMAAQuality; i++) {
					if (aaQualityStr == smaaQualityLevels[i]) {
						smaaQuality = i;
						break;
					}
				}
			}
		} else if (aaMethodStr == "SMAA2X") {
			aaMethod = AAMethod::SMAA2X;

			if (!aaQualityStr.empty()) {
				std::transform(aaQualityStr.begin(), aaQualityStr.end(), aaQualityStr.begin(), ::toupper);
				for (unsigned int i = 0; i < maxSMAAQuality; i++) {
					if (aaQualityStr == smaaQualityLevels[i]) {
						smaaQuality = i;
						break;
					}
				}
			}
		} else if (aaMethodStr == "FXAA") {
			aaMethod = AAMethod::FXAA;

			if (!aaQualityStr.empty()) {
				std::transform(aaQualityStr.begin(), aaQualityStr.end(), aaQualityStr.begin(), ::toupper);
				for (unsigned int i = 0; i < maxFXAAQuality; i++) {
					if (aaQualityStr == fxaaQualityLevels[i]) {
						fxaaQuality = i;
						break;
					}
				}
			}
		} else if (aaMethodStr == "MSAA") {
			aaMethod = AAMethod::MSAA;

			int n = atoi(aaQualityStr.c_str());
			if (n > 0) {
				if (!isPow2(n)) {
					n = nextPow2(n);
				}

				msaaQuality = msaaSamplesToQuality(n);
			}

		} else {
			LOG("Bad AA method \"%s\"\n", aaMethodStr.c_str());
			fprintf(stderr, "Bad AA method \"%s\"\n", aaMethodStr.c_str());
			exit(1);
		}

		temporalAA = temporalAASwitch.getValue();

		imageFiles    = imagesArg.getValue();

	} catch (TCLAP::ArgException &e) {
		LOG("parseCommandLine exception: %s for arg %s\n", e.error().c_str(), e.argId().c_str());
	} catch (...) {
		LOG("parseCommandLine: unknown exception\n");
	}
}


struct GlobalDS {
	BufferHandle   globalUniforms;
	SamplerHandle  linearSampler;
	SamplerHandle  nearestSampler;


	static const DescriptorLayout layout[];
	static DSLayoutHandle layoutHandle;
};


const DescriptorLayout GlobalDS::layout[] = {
	  { DescriptorType::UniformBuffer,  offsetof(GlobalDS, globalUniforms) }
	, { DescriptorType::Sampler,        offsetof(GlobalDS, linearSampler ) }
	, { DescriptorType::Sampler,        offsetof(GlobalDS, nearestSampler) }
	, { DescriptorType::End,            0                                  }
};

DSLayoutHandle GlobalDS::layoutHandle;


struct CubeSceneDS {
	BufferHandle unused;
	BufferHandle instances;

	static const DescriptorLayout layout[];
	static DSLayoutHandle layoutHandle;
};


const DescriptorLayout CubeSceneDS::layout[] = {
	  { DescriptorType::UniformBuffer,  offsetof(CubeSceneDS, unused)    }
	, { DescriptorType::StorageBuffer,  offsetof(CubeSceneDS, instances) }
	, { DescriptorType::End,            0                                }
};

DSLayoutHandle CubeSceneDS::layoutHandle;


struct ColorCombinedDS {
	BufferHandle unused;
	CSampler color;

	static const DescriptorLayout layout[];
	static DSLayoutHandle layoutHandle;
};


const DescriptorLayout ColorCombinedDS::layout[] = {
	  { DescriptorType::UniformBuffer,    offsetof(ColorCombinedDS, unused) }
	, { DescriptorType::CombinedSampler,  offsetof(ColorCombinedDS, color)  }
	, { DescriptorType::End,              0,                                }
};

DSLayoutHandle ColorCombinedDS::layoutHandle;


struct ColorTexDS {
	BufferHandle unused;
	TextureHandle color;

	static const DescriptorLayout layout[];
	static DSLayoutHandle layoutHandle;
};


const DescriptorLayout ColorTexDS::layout[] = {
	  { DescriptorType::UniformBuffer,  offsetof(ColorTexDS, unused) }
	, { DescriptorType::Texture,        offsetof(ColorTexDS, color)  }
	, { DescriptorType::End,            0,                           }
};

DSLayoutHandle ColorTexDS::layoutHandle;


struct EdgeDetectionDS {
	BufferHandle  smaaUBO;
	CSampler color;
	CSampler predicationTex;

	static const DescriptorLayout layout[];
	static DSLayoutHandle layoutHandle;
};


const DescriptorLayout EdgeDetectionDS::layout[] = {
	  { DescriptorType::UniformBuffer,    offsetof(EdgeDetectionDS, smaaUBO)        }
	, { DescriptorType::CombinedSampler,  offsetof(EdgeDetectionDS, color)          }
	, { DescriptorType::CombinedSampler,  offsetof(EdgeDetectionDS, predicationTex) }
	, { DescriptorType::End,              0,                                        }
};

DSLayoutHandle EdgeDetectionDS::layoutHandle;


struct BlendWeightDS {
	BufferHandle  smaaUBO;
	CSampler edgesTex;
	CSampler areaTex;
	CSampler searchTex;

	static const DescriptorLayout layout[];
	static DSLayoutHandle layoutHandle;
};


const DescriptorLayout BlendWeightDS::layout[] = {
	  { DescriptorType::UniformBuffer,    offsetof(BlendWeightDS, smaaUBO)   }
	, { DescriptorType::CombinedSampler,  offsetof(BlendWeightDS, edgesTex)  }
	, { DescriptorType::CombinedSampler,  offsetof(BlendWeightDS, areaTex)   }
	, { DescriptorType::CombinedSampler,  offsetof(BlendWeightDS, searchTex) }
	, { DescriptorType::End,              0,                                 }
};

DSLayoutHandle BlendWeightDS::layoutHandle;


struct NeighborBlendDS {
	BufferHandle  smaaUBO;
	CSampler color;
	CSampler blendweights;

	static const DescriptorLayout layout[];
	static DSLayoutHandle layoutHandle;
};


const DescriptorLayout NeighborBlendDS::layout[] = {
	  { DescriptorType::UniformBuffer,    offsetof(NeighborBlendDS, smaaUBO)      }
	, { DescriptorType::CombinedSampler,  offsetof(NeighborBlendDS, color)        }
	, { DescriptorType::CombinedSampler,  offsetof(NeighborBlendDS, blendweights) }
	, { DescriptorType::End,              0                                       }
};

DSLayoutHandle NeighborBlendDS::layoutHandle;


struct TemporalAADS {
	BufferHandle  smaaUBO;
	CSampler currentTex;
	CSampler previousTex;
	CSampler velocityTex;

	static const DescriptorLayout layout[];
	static DSLayoutHandle layoutHandle;
};


const DescriptorLayout TemporalAADS::layout[] = {
	  { DescriptorType::UniformBuffer,    offsetof(TemporalAADS, smaaUBO)     }
	, { DescriptorType::CombinedSampler,  offsetof(TemporalAADS, currentTex)  }
	, { DescriptorType::CombinedSampler,  offsetof(TemporalAADS, previousTex) }
	, { DescriptorType::CombinedSampler,  offsetof(TemporalAADS, velocityTex) }
	, { DescriptorType::End,              0                                   }
};

DSLayoutHandle TemporalAADS::layoutHandle;



static const int numDepths = 5;
static const std::array<Format, numDepths> depths
  = { { Format::Depth24X8, Format::Depth24S8, Format::Depth32Float, Format::Depth16, Format::Depth16S8 } };


void SMAADemo::initRender() {
	renderer = Renderer::createRenderer(rendererDesc);
	renderSize = renderer.getDrawableSize();
	const auto &features = renderer.getFeatures();
	LOG("Max MSAA samples: %u\n",  features.maxMSAASamples);
	LOG("sRGB frame buffer: %s\n", features.sRGBFramebuffer ? "yes" : "no");
	LOG("SSBO support: %s\n",      features.SSBOSupported ? "yes" : "no");
	maxMSAAQuality = msaaSamplesToQuality(features.maxMSAASamples) + 1;
	if (msaaQuality >= maxMSAAQuality) {
		msaaQuality = maxMSAAQuality - 1;
	}

	unsigned int refreshRate = renderer.getCurrentRefreshRate();

	if (refreshRate == 0) {
		LOG("Failed to get current refresh rate, using max\n");
		refreshRate = renderer.getMaxRefreshRate();
	}

	if (refreshRate == 0) {
		LOG("Failed to get refresh rate, defaulting to 60\n");
		fpsLimit = 2 * 60;
	} else {
		fpsLimit = 2 * refreshRate;
	}

	for (auto depth : depths) {
		if (renderer.isRenderTargetFormatSupported(depth)) {
			depthFormat = depth;
			break;
		}
	}
	if (depthFormat == Format::Invalid) {
		throw std::runtime_error("no supported depth formats");
	}
	LOG("Using depth format %s\n", formatName(depthFormat));

	renderer.registerDescriptorSetLayout<GlobalDS>();
	renderer.registerDescriptorSetLayout<CubeSceneDS>();
	renderer.registerDescriptorSetLayout<ColorCombinedDS>();
	renderer.registerDescriptorSetLayout<ColorTexDS>();
	renderer.registerDescriptorSetLayout<EdgeDetectionDS>();
	renderer.registerDescriptorSetLayout<BlendWeightDS>();
	renderer.registerDescriptorSetLayout<NeighborBlendDS>();
	renderer.registerDescriptorSetLayout<TemporalAADS>();

	linearSampler  = renderer.createSampler(SamplerDesc().minFilter(FilterMode::Linear). magFilter(FilterMode::Linear) .name("linear"));
	nearestSampler = renderer.createSampler(SamplerDesc().minFilter(FilterMode::Nearest).magFilter(FilterMode::Nearest).name("nearest"));

	cubeVBO = renderer.createBuffer(BufferType::Vertex, sizeof(vertices), &vertices[0]);
	cubeIBO = renderer.createBuffer(BufferType::Index, sizeof(indices), &indices[0]);

#ifdef RENDERER_OPENGL

	const bool flipSMAATextures = true;

#else  // RENDERER_OPENGL

	const bool flipSMAATextures = false;

#endif  // RENDERER_OPENGL

	TextureDesc texDesc;
	texDesc.width(AREATEX_WIDTH)
	       .height(AREATEX_HEIGHT)
	       .format(Format::RG8);
	texDesc.name("SMAA area texture");

	if (flipSMAATextures) {
		std::vector<unsigned char> tempBuffer(AREATEX_SIZE);
		for (unsigned int y = 0; y < AREATEX_HEIGHT; y++) {
			unsigned int srcY = AREATEX_HEIGHT - 1 - y;
			//unsigned int srcY = y;
			memcpy(&tempBuffer[y * AREATEX_PITCH], areaTexBytes + srcY * AREATEX_PITCH, AREATEX_PITCH);
		}
		texDesc.mipLevelData(0, &tempBuffer[0], AREATEX_SIZE);
		areaTex = renderer.createTexture(texDesc);
	} else {
		texDesc.mipLevelData(0, areaTexBytes, AREATEX_SIZE);
		areaTex = renderer.createTexture(texDesc);
	}

	texDesc.width(SEARCHTEX_WIDTH)
	       .height(SEARCHTEX_HEIGHT)
	       .format(Format::R8);
	texDesc.name("SMAA search texture");
	if (flipSMAATextures) {
		std::vector<unsigned char> tempBuffer(SEARCHTEX_SIZE);
		for (unsigned int y = 0; y < SEARCHTEX_HEIGHT; y++) {
			unsigned int srcY = SEARCHTEX_HEIGHT - 1 - y;
			//unsigned int srcY = y;
			memcpy(&tempBuffer[y * SEARCHTEX_PITCH], searchTexBytes + srcY * SEARCHTEX_PITCH, SEARCHTEX_PITCH);
		}
		texDesc.mipLevelData(0, &tempBuffer[0], SEARCHTEX_SIZE);
		searchTex = renderer.createTexture(texDesc);
	} else {
		texDesc.mipLevelData(0, searchTexBytes, SEARCHTEX_SIZE);
		searchTex = renderer.createTexture(texDesc);
	}

	images.reserve(imageFiles.size());
	for (const auto &filename : imageFiles) {
		loadImage(filename);
	}

	// imgui setup
	{
		imGuiContext = ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename                 = nullptr;
		io.KeyMap[ImGuiKey_Tab]        = SDL_SCANCODE_TAB;                     // Keyboard mapping. ImGui will use those indices to peek into the io.KeyDown[] array.
		io.KeyMap[ImGuiKey_LeftArrow]  = SDL_SCANCODE_LEFT;
		io.KeyMap[ImGuiKey_RightArrow] = SDL_SCANCODE_RIGHT;
		io.KeyMap[ImGuiKey_UpArrow]    = SDL_SCANCODE_UP;
		io.KeyMap[ImGuiKey_DownArrow]  = SDL_SCANCODE_DOWN;
		io.KeyMap[ImGuiKey_PageUp]     = SDL_SCANCODE_PAGEUP;
		io.KeyMap[ImGuiKey_PageDown]   = SDL_SCANCODE_PAGEDOWN;
		io.KeyMap[ImGuiKey_Home]       = SDL_SCANCODE_HOME;
		io.KeyMap[ImGuiKey_End]        = SDL_SCANCODE_END;
		io.KeyMap[ImGuiKey_Delete]     = SDL_SCANCODE_DELETE;
		io.KeyMap[ImGuiKey_Backspace]  = SDL_SCANCODE_BACKSPACE;
		io.KeyMap[ImGuiKey_Enter]      = SDL_SCANCODE_RETURN;
		io.KeyMap[ImGuiKey_Escape]     = SDL_SCANCODE_ESCAPE;
		io.KeyMap[ImGuiKey_A]          = SDL_SCANCODE_A;
		io.KeyMap[ImGuiKey_C]          = SDL_SCANCODE_C;
		io.KeyMap[ImGuiKey_V]          = SDL_SCANCODE_V;
		io.KeyMap[ImGuiKey_X]          = SDL_SCANCODE_X;
		io.KeyMap[ImGuiKey_Y]          = SDL_SCANCODE_Y;
		io.KeyMap[ImGuiKey_Z]          = SDL_SCANCODE_Z;

		// TODO: clipboard
		io.SetClipboardTextFn = SetClipboardText;
		io.GetClipboardTextFn = GetClipboardText;
		io.ClipboardUserData  = clipboardText;

		// Build texture atlas
		unsigned char *pixels = nullptr;
		int width = 0, height = 0;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

		texDesc.width(width)
		       .height(height)
		       .format(Format::sRGBA8)
		       .name("GUI")
		       .mipLevelData(0, pixels, width * height * 4);
		imguiFontsTex = renderer.createTexture(texDesc);
		io.Fonts->TexID = nullptr;
	}
}


void SMAADemo::rebuildRenderGraph() {
	assert(rebuildRG);

	if (temporalRTs[0]) {
		assert(temporalRTs[1]);
		renderer.deleteRenderTarget(temporalRTs[0]);
		temporalRTs[0] = RenderTargetHandle();
		renderer.deleteRenderTarget(temporalRTs[1]);
		temporalRTs[1] = RenderTargetHandle();
	}

	renderGraph.reset(renderer, std::bind(std::mem_fn(&SMAADemo::processInput), this));

	if (antialiasing && aaMethod == AAMethod::MSAA) {
		numSamples = msaaQualityToSamples(msaaQuality);
		assert(numSamples > 1);
	} else if (antialiasing && aaMethod == AAMethod::SMAA2X) {
		numSamples = 2;
	} else {
		numSamples = 1;
	}

	renderSize = renderer.getDrawableSize();
	LOG("drawable size: %ux%u\n", renderSize.x, renderSize.y);

	const unsigned int windowWidth  = renderSize.x;
	const unsigned int windowHeight = renderSize.y;

	LOG("create framebuffers at size %ux%u\n", windowWidth, windowHeight);
	logFlush();

	if (activeScene == 0) {
		// cube scene

		{
			RenderTargetDesc rtDesc;
			rtDesc.name("main color")
				  .numSamples(numSamples)
				  .format(Format::sRGBA8)
				  .additionalViewFormat(Format::RGBA8)
				  .width(windowWidth)
				  .height(windowHeight);
			renderGraph.renderTarget(Rendertargets::MainColor, rtDesc);
		}

		// TODO: only create velocity buffer when doing temporal AA
		{
			RenderTargetDesc rtDesc;
			rtDesc.name("velocity")
				  .numSamples(1)
				  .format(Format::RG16Float)
				  .width(windowWidth)
				  .height(windowHeight);
			renderGraph.renderTarget(Rendertargets::Velocity, rtDesc);
		}

		auto velocityRT = Rendertargets::Velocity;
		if (numSamples > 1) {
			RenderTargetDesc rtDesc;
			rtDesc.name("velocity multisample")
				  .numSamples(numSamples)
				  .format(Format::RG16Float)
				  .width(windowWidth)
				  .height(windowHeight);
			renderGraph.renderTarget(Rendertargets::VelocityMS, rtDesc);

			velocityRT = Rendertargets::VelocityMS;
		}

		{
			RenderTargetDesc rtDesc;
			rtDesc.name("main depth")
				  .numSamples(numSamples)
				  .format(depthFormat)
				  .width(windowWidth)
				  .height(windowHeight);
			renderGraph.renderTarget(Rendertargets::MainDepth, rtDesc);
		}

		RenderGraph::PassDesc desc;
		desc.color(0, Rendertargets::MainColor, PassBegin::Clear)
		    .color(1, velocityRT,               PassBegin::Clear)
		    .depthStencil(Rendertargets::MainDepth,  PassBegin::Clear)
		    .clearDepth(1.0f)
		    .name("Scene")
		    .numSamples(numSamples);

		renderGraph.renderPass(RenderPasses::Scene, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderCubeScene(rp, r); } );
	} else {
		// image scene

		{
			RenderTargetDesc rtDesc;
			rtDesc.name("main color")
				  .numSamples(numSamples)
				  .format(Format::sRGBA8)
				  .additionalViewFormat(Format::RGBA8)
				  .width(windowWidth)
				  .height(windowHeight);
			renderGraph.renderTarget(Rendertargets::MainColor, rtDesc);
		}

		// TODO: don't use velocity buffer
		{
			RenderTargetDesc rtDesc;
			rtDesc.name("velocity")
				  .numSamples(1)
				  .format(Format::RG16Float)
				  .width(windowWidth)
				  .height(windowHeight);
			renderGraph.renderTarget(Rendertargets::Velocity, rtDesc);
		}

		auto velocityRT = Rendertargets::Velocity;
		if (numSamples > 1) {
			RenderTargetDesc rtDesc;
			rtDesc.name("velocity multisample")
				  .numSamples(numSamples)
				  .format(Format::RG16Float)
				  .width(windowWidth)
				  .height(windowHeight);
			renderGraph.renderTarget(Rendertargets::VelocityMS, rtDesc);
			velocityRT = Rendertargets::VelocityMS;
		}

		{
			RenderTargetDesc rtDesc;
			rtDesc.name("main depth")
				  .numSamples(numSamples)
				  .format(depthFormat)
				  .width(windowWidth)
				  .height(windowHeight);
			renderGraph.renderTarget(Rendertargets::MainDepth, rtDesc);
		}

		RenderGraph::PassDesc desc;
		desc.color(0, Rendertargets::MainColor, PassBegin::Clear)
		    .color(1, velocityRT,               PassBegin::Clear)
		    .depthStencil(Rendertargets::MainDepth,  PassBegin::Clear)
		    .clearDepth(1.0f)
		    .name("Scene")
		    .numSamples(numSamples);

		renderGraph.renderPass(RenderPasses::Scene, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderImageScene(rp, r); } );
	}

	{
		RenderTargetDesc rtDesc;
		rtDesc.name("final")
		      .format(Format::sRGBA8)
		      .width(windowWidth)
		      .height(windowHeight);
		renderGraph.renderTarget(Rendertargets::FinalRender, rtDesc);
	}

	if (antialiasing) {
		if (temporalAA) {
			{
				RenderTargetDesc rtDesc;
				rtDesc.name("Temporal resolve 1")
				      .format(Format::sRGBA8)  // TODO: not right?
				      .width(windowWidth)
				      .height(windowHeight);
				temporalRTs[0] = renderer.createRenderTarget(rtDesc);

				rtDesc.name("Temporal resolve 2");
				temporalRTs[1] = renderer.createRenderTarget(rtDesc);

				renderGraph.externalRenderTarget(Rendertargets::TemporalPrevious, Format::sRGBA8, Layout::ShaderRead, Layout::ShaderRead);
				renderGraph.externalRenderTarget(Rendertargets::TemporalCurrent,  Format::sRGBA8, Layout::Undefined,  Layout::ShaderRead);
			}

			if (numSamples > 1) {
				renderGraph.resolveMSAA(Rendertargets::VelocityMS, Rendertargets::Velocity);
			}

			switch (aaMethod) {
			case AAMethod::MSAA: {
				renderGraph.resolveMSAA(Rendertargets::MainColor, Rendertargets::TemporalCurrent);
			} break;

			case AAMethod::FXAA: {
				{
					RenderGraph::PassDesc desc;
					desc.color(0, Rendertargets::TemporalCurrent, PassBegin::Clear)
					    .inputRendertarget(Rendertargets::MainColor)
                        .name("FXAA temporal");

					renderGraph.renderPass(RenderPasses::FXAA, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderFXAA(rp, r); } );
				}
			} break;

			case AAMethod::SMAA: {
				// edges pass
				{
					RenderTargetDesc rtDesc;
					rtDesc.name("SMAA edges")
						  .format(Format::RGBA8)
						  .width(windowWidth)
						  .height(windowHeight);
					renderGraph.renderTarget(Rendertargets::Edges, rtDesc);
				}

				{
					// TODO: only add MainDepth when using predication
					RenderGraph::PassDesc desc;
					desc.color(0, Rendertargets::Edges, PassBegin::Clear)
					    .inputRendertarget(Rendertargets::MainColor)
					    .inputRendertarget(Rendertargets::MainDepth)
						.name("SMAA edges");

					renderGraph.renderPass(RenderPasses::SMAAEdges, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSMAAEdges(rp, r, Rendertargets::MainColor, 0); } );
				}

				// blendweights pass
				{
					RenderTargetDesc rtDesc;
					rtDesc.name("SMAA weights")
						  .format(Format::RGBA8)
						  .width(windowWidth)
						  .height(windowHeight);
					renderGraph.renderTarget(Rendertargets::BlendWeights, rtDesc);

					RenderGraph::PassDesc desc;
					desc.color(0, Rendertargets::BlendWeights, PassBegin::Clear)
					    .inputRendertarget(Rendertargets::Edges)
						.name("SMAA weights");

					renderGraph.renderPass(RenderPasses::SMAAWeights, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSMAAWeights(rp, r, 0); } );
				}

				// full effect
				{
					RenderGraph::PassDesc desc;
					desc.color(0, Rendertargets::TemporalCurrent, PassBegin::Clear)
					    .inputRendertarget(Rendertargets::MainColor)
					    .inputRendertarget(Rendertargets::BlendWeights)
						.name("SMAA blend");

					renderGraph.renderPass(RenderPasses::SMAABlend, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSMAABlend(rp, r, Rendertargets::MainColor, 0); } );
				}
			} break;

			case AAMethod::SMAA2X: {
				{
					RenderTargetDesc rtDesc;
					rtDesc.format(Format::sRGBA8)
						  .additionalViewFormat(Format::RGBA8)
						  .width(windowWidth)
						  .height(windowHeight);

					rtDesc.name("Subsample separate 1");
					renderGraph.renderTarget(Rendertargets::Subsample1, rtDesc);

					rtDesc.name("Subsample separate 2");
					renderGraph.renderTarget(Rendertargets::Subsample2, rtDesc);
				}

				{
					RenderGraph::PassDesc desc;
					desc.color(0, Rendertargets::Subsample1, PassBegin::DontCare)
					    .color(1, Rendertargets::Subsample2, PassBegin::DontCare)
					    .inputRendertarget(Rendertargets::MainColor)
					    .name("Subsample separate");

					renderGraph.renderPass(RenderPasses::Separate, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSeparate(rp, r); } );
				}

				// TODO: clean up the renderpass mess

				// edges pass
				{
					RenderTargetDesc rtDesc;
					rtDesc.name("SMAA edges")
						  .format(Format::RGBA8)
						  .width(windowWidth)
						  .height(windowHeight);
					renderGraph.renderTarget(Rendertargets::Edges, rtDesc);
				}

				{
					// TODO: only add MainDepth when using predication
					RenderGraph::PassDesc desc;
					desc.color(0, Rendertargets::Edges, PassBegin::Clear)
					    .inputRendertarget(Rendertargets::Subsample1)
					    .inputRendertarget(Rendertargets::MainDepth)
						.name("SMAA edges");

					renderGraph.renderPass(RenderPasses::SMAAEdges, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSMAAEdges(rp, r, Rendertargets::Subsample1, 0); } );
				}

				// blendweights pass
				{
					RenderTargetDesc rtDesc;
					rtDesc.name("SMAA weights")
						  .format(Format::RGBA8)
						  .width(windowWidth)
						  .height(windowHeight);
					renderGraph.renderTarget(Rendertargets::BlendWeights, rtDesc);

					RenderGraph::PassDesc desc;
					desc.color(0, Rendertargets::BlendWeights, PassBegin::Clear)
					    .inputRendertarget(Rendertargets::Edges)
						.name("SMAA weights");

					renderGraph.renderPass(RenderPasses::SMAAWeights, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSMAAWeights(rp, r, 0); } );
				}

				// full effect
				{
					RenderGraph::PassDesc desc;
					desc.color(0, Rendertargets::TemporalCurrent, PassBegin::Clear)
					    .inputRendertarget(Rendertargets::Subsample1)
					    .inputRendertarget(Rendertargets::BlendWeights)
						.name("SMAA2x blend 1");

					renderGraph.renderPass(RenderPasses::SMAA2XBlend1, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSMAABlend(rp, r, Rendertargets::Subsample1, 0); } );
				}

				// edges pass
				{
					// TODO: only add MainDepth when using predication
					RenderGraph::PassDesc desc;
					desc.color(0, Rendertargets::Edges, PassBegin::Clear)
					    .inputRendertarget(Rendertargets::Subsample2)
					    .inputRendertarget(Rendertargets::MainDepth)
						.name("SMAA edges");

					renderGraph.renderPass(RenderPasses::SMAAEdges2, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSMAAEdges(rp, r, Rendertargets::Subsample2, 1); } );
				}

				// blendweights pass
				{
					RenderGraph::PassDesc desc;
					desc.color(0, Rendertargets::BlendWeights, PassBegin::Clear)
					    .inputRendertarget(Rendertargets::Edges)
						.name("SMAA weights");

					renderGraph.renderPass(RenderPasses::SMAAWeights2, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSMAAWeights(rp, r, 1); } );
				}

				// full effect
				{
					RenderGraph::PassDesc desc;
					desc.color(0, Rendertargets::TemporalCurrent, PassBegin::Keep)
					    .inputRendertarget(Rendertargets::Subsample2)
					    .inputRendertarget(Rendertargets::BlendWeights)
						.name("SMAA2x blend 2");

					renderGraph.renderPass(RenderPasses::SMAA2XBlend2, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSMAABlend(rp, r, Rendertargets::Subsample2, 1); } );
				}
			} break;
			}

			{
				RenderGraph::PassDesc desc;
				desc.color(0, Rendertargets::FinalRender, PassBegin::Clear)
					.inputRendertarget(Rendertargets::TemporalPrevious)
					.inputRendertarget(Rendertargets::TemporalCurrent)
					.inputRendertarget(Rendertargets::Velocity)
					.name("Temporal AA");

				renderGraph.renderPass(RenderPasses::Final, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderTemporalAA(rp, r); } );
			}
		} else {
			// no temporal AA
			switch (aaMethod) {
			case AAMethod::MSAA: {
				renderGraph.resolveMSAA(Rendertargets::MainColor, Rendertargets::FinalRender);
			} break;

			case AAMethod::FXAA: {
				RenderGraph::PassDesc desc;
				desc.color(0, Rendertargets::FinalRender, PassBegin::Clear)
				    .inputRendertarget(Rendertargets::MainColor)
					.name("FXAA");

				renderGraph.renderPass(RenderPasses::FXAA, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderFXAA(rp, r); } );
			} break;

			case AAMethod::SMAA: {
				// edges pass
				{
					RenderTargetDesc rtDesc;
					rtDesc.name("SMAA edges")
						  .format(Format::RGBA8)
						  .width(windowWidth)
						  .height(windowHeight);
					renderGraph.renderTarget(Rendertargets::Edges, rtDesc);
				}

				{
					// TODO: only add MainDepth when using predication
					RenderGraph::PassDesc desc;
					desc.color(0, Rendertargets::Edges, PassBegin::Clear)
					    .inputRendertarget(Rendertargets::MainColor)
					    .inputRendertarget(Rendertargets::MainDepth)
						.name("SMAA edges");

					renderGraph.renderPass(RenderPasses::SMAAEdges, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSMAAEdges(rp, r, Rendertargets::MainColor, 0); } );
				}

				switch (debugMode) {
				case 0:
					// blendweights pass
					{
						RenderTargetDesc rtDesc;
						rtDesc.name("SMAA weights")
							  .format(Format::RGBA8)
							  .width(windowWidth)
							  .height(windowHeight);
						renderGraph.renderTarget(Rendertargets::BlendWeights, rtDesc);

						RenderGraph::PassDesc desc;
						desc.color(0, Rendertargets::BlendWeights, PassBegin::Clear)
						    .inputRendertarget(Rendertargets::Edges)
							.name("SMAA weights");

						renderGraph.renderPass(RenderPasses::SMAAWeights, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSMAAWeights(rp, r, 0); } );
					}

					// full effect
					{
						RenderGraph::PassDesc desc;
						desc.color(0, Rendertargets::FinalRender, PassBegin::Clear)
						    .inputRendertarget(Rendertargets::MainColor)
						    .inputRendertarget(Rendertargets::BlendWeights)
							.name("SMAA blend");

						renderGraph.renderPass(RenderPasses::SMAABlend, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSMAABlend(rp, r, Rendertargets::MainColor, 0); } );
					}

					break;

				case 1:
					// visualize edges
					{
						RenderGraph::PassDesc desc;
						desc.color(0, Rendertargets::FinalRender, PassBegin::Clear)
						    .inputRendertarget(Rendertargets::Edges)
							.name("Visualize edges");

						renderGraph.renderPass(RenderPasses::Final, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSMAADebug(rp, r, Rendertargets::Edges); } );
					}

					break;

				case 2:
					// blendweights pass
					{
						RenderTargetDesc rtDesc;
						rtDesc.name("SMAA weights")
							  .format(Format::RGBA8)
							  .width(windowWidth)
							  .height(windowHeight);
						renderGraph.renderTarget(Rendertargets::BlendWeights, rtDesc);

						RenderGraph::PassDesc desc;
						desc.color(0, Rendertargets::BlendWeights, PassBegin::Clear)
						    .inputRendertarget(Rendertargets::Edges)
							.name("SMAA weights");

						renderGraph.renderPass(RenderPasses::SMAAWeights, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSMAAWeights(rp, r, 0); } );
					}

					// visualize blend weights
					{
						RenderGraph::PassDesc desc;
						desc.color(0, Rendertargets::FinalRender, PassBegin::Clear)
						    .inputRendertarget(Rendertargets::BlendWeights)
							.name("Visualize blend weights");

						renderGraph.renderPass(RenderPasses::Final, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSMAADebug(rp, r, Rendertargets::BlendWeights); } );
					}

					break;
				}
			} break;

			case AAMethod::SMAA2X: {
				{
					RenderTargetDesc rtDesc;
					rtDesc.format(Format::sRGBA8)
						  .additionalViewFormat(Format::RGBA8)
						  .width(windowWidth)
						  .height(windowHeight);

					rtDesc.name("Subsample separate 1");
					renderGraph.renderTarget(Rendertargets::Subsample1, rtDesc);

					rtDesc.name("Subsample separate 2");
					renderGraph.renderTarget(Rendertargets::Subsample2, rtDesc);

					RenderGraph::PassDesc desc;
					desc.color(0, Rendertargets::Subsample1, PassBegin::DontCare)
					    .color(1, Rendertargets::Subsample2, PassBegin::DontCare)
					    .inputRendertarget(Rendertargets::MainColor)
					    .name("Subsample separate");

					renderGraph.renderPass(RenderPasses::Separate, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSeparate(rp, r); } );
				}

				// edges pass
				{
					RenderTargetDesc rtDesc;
					rtDesc.name("SMAA edges")
						  .format(Format::RGBA8)
						  .width(windowWidth)
						  .height(windowHeight);
					renderGraph.renderTarget(Rendertargets::Edges, rtDesc);

					// TODO: only add MainDepth when using predication
					RenderGraph::PassDesc desc;
					desc.color(0, Rendertargets::Edges, PassBegin::Clear)
					    .inputRendertarget(Rendertargets::Subsample1)
					    .inputRendertarget(Rendertargets::MainDepth)
						.name("SMAA edges");

					renderGraph.renderPass(RenderPasses::SMAAEdges, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSMAAEdges(rp, r, Rendertargets::Subsample1, 0); } );
				}

				// blendweights pass
				{
					RenderTargetDesc rtDesc;
					rtDesc.name("SMAA weights")
						  .format(Format::RGBA8)
						  .width(windowWidth)
						  .height(windowHeight);
					renderGraph.renderTarget(Rendertargets::BlendWeights, rtDesc);

					RenderGraph::PassDesc desc;
					desc.color(0, Rendertargets::BlendWeights, PassBegin::Clear)
					    .inputRendertarget(Rendertargets::Edges)
						.name("SMAA weights");

					renderGraph.renderPass(RenderPasses::SMAAWeights, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSMAAWeights(rp, r, 0); } );
				}

				// final blend pass
				{
					RenderGraph::PassDesc desc;
					desc.color(0, Rendertargets::FinalRender, PassBegin::Clear)
					    .inputRendertarget(Rendertargets::Subsample1)
					    .inputRendertarget(Rendertargets::BlendWeights)
						.name("SMAA2x blend 1");

					renderGraph.renderPass(RenderPasses::SMAA2XBlend1, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSMAABlend(rp, r, Rendertargets::Subsample1, 0); } );
				}

				// second pass
				// edges pass
				{
					// TODO: only add MainDepth when using predication
					RenderGraph::PassDesc desc;
					desc.color(0, Rendertargets::Edges, PassBegin::Clear)
					    .inputRendertarget(Rendertargets::Subsample2)
					    .inputRendertarget(Rendertargets::MainDepth)
						.name("SMAA edges");

					renderGraph.renderPass(RenderPasses::SMAAEdges2, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSMAAEdges(rp, r, Rendertargets::Subsample2, 1); } );
				}

				// blendweights pass
				{
					RenderGraph::PassDesc desc;
					desc.color(0, Rendertargets::BlendWeights, PassBegin::Clear)
					    .inputRendertarget(Rendertargets::Edges)
						.name("SMAA weights");

					renderGraph.renderPass(RenderPasses::SMAAWeights2, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSMAAWeights(rp, r, 1); } );
				}

				// final blend pass
				{
					RenderGraph::PassDesc desc;
					desc.color(0, Rendertargets::FinalRender, PassBegin::Keep)
					    .inputRendertarget(Rendertargets::Subsample2)
					    .inputRendertarget(Rendertargets::BlendWeights)
						.name("SMAA2x blend 2");

					renderGraph.renderPass(RenderPasses::SMAA2XBlend2, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderSMAABlend(rp, r, Rendertargets::Subsample2, 1); } );
				}

			} break;
			}
		}
	} else {
		renderGraph.blit(Rendertargets::MainColor, Rendertargets::FinalRender);
	}

	{
		RenderGraph::PassDesc desc;
		desc.color(0, Rendertargets::FinalRender, PassBegin::Keep)
			.name("GUI");

		renderGraph.renderPass(RenderPasses::GUI, desc, [this] (RenderPasses rp, RenderGraph::PassResources &r) { this->renderGUI(rp, r); } );
	}

	renderGraph.presentRenderTarget(Rendertargets::FinalRender);

	renderGraph.build(renderer);

	cubePipeline           = PipelineHandle();
	imagePipeline          = PipelineHandle();
	blitPipeline           = PipelineHandle();
	guiPipeline            = PipelineHandle();
	separatePipeline       = PipelineHandle();
	temporalAAPipelines[0] = PipelineHandle();
	temporalAAPipelines[1] = PipelineHandle();
	fxaaPipeline           = PipelineHandle();

	smaaPipelines.edgePipeline         = PipelineHandle();
	smaaPipelines.blendWeightPipeline  = PipelineHandle();
	smaaPipelines.neighborPipelines[0] = PipelineHandle();
	smaaPipelines.neighborPipelines[1] = PipelineHandle();

	for (unsigned int i = 0; i < 2; i++) {
		temporalAAPipelines[i] = PipelineHandle();
	}

	separatePipeline = PipelineHandle();

	rebuildRG = false;
}


void SMAADemo::loadImage(const std::string &filename) {
	int width = 0, height = 0;
	unsigned char *imageData = stbi_load(filename.c_str(), &width, &height, NULL, 4);
	LOG(" %s : %p  %dx%d\n", filename.c_str(), imageData, width, height);
	if (!imageData) {
		LOG("Bad image: %s\n", stbi_failure_reason());
		return;
	}

	images.push_back(Image());
	auto &img      = images.back();
	img.filename   = filename;
	auto lastSlash = filename.rfind('/');
	if (lastSlash != std::string::npos) {
		img.shortName = filename.substr(lastSlash + 1);
	}
	else {
		img.shortName = filename;
	}

	TextureDesc texDesc;
	texDesc.width(width)
	       .height(height)
	       .name(img.shortName)
	       .format(Format::sRGBA8);

	texDesc.mipLevelData(0, imageData, width * height * 4);
	img.width  = width;
	img.height = height;
	img.tex    = renderer.createTexture(texDesc);

	stbi_image_free(imageData);

	activeScene = static_cast<unsigned int>(images.size());
}


void SMAADemo::createCubes() {
	// cube of cubes, n^3 cubes total
	const unsigned int numCubes = static_cast<unsigned int>(pow(cubesPerSide, 3));

	const float cubeDiameter = sqrtf(3.0f);
	const float cubeDistance = cubeDiameter + 1.0f;

	const float bigCubeSide = cubeDistance * cubesPerSide;

	cubes.clear();
	cubes.reserve(numCubes);

	unsigned int order = 0;
	for (unsigned int x = 0; x < cubesPerSide; x++) {
		for (unsigned int y = 0; y < cubesPerSide; y++) {
			for (unsigned int z = 0; z < cubesPerSide; z++) {
				float qx = random.randFloat();
				float qy = random.randFloat();
				float qz = random.randFloat();
				float qw = random.randFloat();
				float reciprocLen = 1.0f / sqrtf(qx*qx + qy*qy + qz*qz + qw*qw);
				qx *= reciprocLen;
				qy *= reciprocLen;
				qz *= reciprocLen;
				qw *= reciprocLen;

				ShaderDefines::Cube cube;
				cube.position = glm::vec3((x * cubeDistance) - (bigCubeSide / 2.0f)
				                        , (y * cubeDistance) - (bigCubeSide / 2.0f)
				                        , (z * cubeDistance) - (bigCubeSide / 2.0f));

				cube.order    = order;
				order++;

				cube.rotation = glm::vec4(qx, qy, qz, qw);
				cube.color    = glm::vec3(1.0f, 1.0f, 1.0f);
				cubes.emplace_back(cube);
			}
		}
	}

	colorCubes();
}


void SMAADemo::shuffleCubeRendering() {
	const unsigned int numCubes = cubes.size();
	for (unsigned int i = 0; i < numCubes - 1; i++) {
		unsigned int victim = random.range(i, numCubes);
		std::swap(cubes[i], cubes[victim]);
	}
}


void SMAADemo::reorderCubeRendering() {
	auto cubeCompare = [] (const ShaderDefines::Cube &a, const ShaderDefines::Cube &b) {
		return a.order < b.order;
	};
	std::sort(cubes.begin(), cubes.end(), cubeCompare);
}


static float sRGB2linear(float v) {
    if (v <= 0.04045f) {
        return v / 12.92f;
    } else {
        return powf((v + 0.055f) / 1.055f, 2.4f);
    }
}


void SMAADemo::colorCubes() {
	if (colorMode == 0) {
		for (auto &cube : cubes) {
			// random RGB
			cube.color.x = sRGB2linear(random.randFloat());
			cube.color.y = sRGB2linear(random.randFloat());
			cube.color.z = sRGB2linear(random.randFloat());
		}
	} else {
		for (auto &cube : cubes) {
			// YCbCr, fixed luma, random chroma, alpha = 1.0
			// worst case scenario for luma edge detection
			// TODO: use the same luma as shader

			float y = 0.3f;
			const float c_red   = 0.299f
			          , c_green = 0.587f
			          , c_blue  = 0.114f;
			float cb = random.randFloat() * 2.0f - 1.0f;
			float cr = random.randFloat() * 2.0f - 1.0f;

			float r = cr * (2 - 2 * c_red) + y;
			float g = (y - c_blue * cb - c_red * cr) / c_green;
			float b = cb * (2 - 2 * c_blue) + y;

			cube.color.x = sRGB2linear(r);
			cube.color.y = sRGB2linear(g);
			cube.color.z = sRGB2linear(b);
		}
	}
}


void SMAADemo::setAntialiasing(bool enabled) {
	antialiasing = enabled;
	rebuildRG    = true;

	if (enabled && temporalAA) {
		temporalAAFirstFrame = true;
	}
}


void SMAADemo::setTemporalAA(bool enabled) {
	if (!temporalAA && enabled) {
		temporalAAFirstFrame = true;
	}

	temporalAA = enabled;
	rebuildRG = true;
}


static void printHelp() {
	printf(" a                - toggle antialiasing on/off\n");
	printf(" c                - re-color cubes\n");
	printf(" d                - cycle through debug visualizations\n");
	printf(" f                - toggle fullscreen\n");
	printf(" h                - print help\n");
	printf(" m                - change antialiasing method\n");
	printf(" q                - cycle through AA quality levels\n");
	printf(" t                - toggle temporal antialiasing on/off\n");
	printf(" v                - toggle vsync\n");
	printf(" LEFT/RIGHT ARROW - cycle through scenes\n");
	printf(" SPACE            - toggle cube rotation\n");
	printf(" ESC              - quit\n");
}


void SMAADemo::processInput() {
	ImGuiIO& io = ImGui::GetIO();

	SDL_Event event;
	memset(&event, 0, sizeof(SDL_Event));
	while (SDL_PollEvent(&event)) {
		int sceneIncrement = 1;
		switch (event.type) {
		case SDL_QUIT:
			keepGoing = false;
			break;

		case SDL_KEYDOWN:
			io.KeysDown[event.key.keysym.scancode] = true;

			switch (event.key.keysym.scancode) {
			case SDL_SCANCODE_LSHIFT:
				leftShift = true;
				break;

			case SDL_SCANCODE_RSHIFT:
				rightShift = true;
				break;

			case SDL_SCANCODE_LALT:
				leftAlt = true;
				break;

			case SDL_SCANCODE_RALT:
				rightAlt = true;
				break;

			case SDL_SCANCODE_LCTRL:
				leftCtrl = true;
				break;

			case SDL_SCANCODE_RCTRL:
				rightCtrl = true;
				break;

			default:
				break;
			}

			if (textInputActive) {
				break;
			}

			switch (event.key.keysym.scancode) {
			case SDL_SCANCODE_ESCAPE:
				keepGoing = false;
				break;

			case SDL_SCANCODE_SPACE:
				rotateCubes = !rotateCubes;
				break;

			case SDL_SCANCODE_A:
				setAntialiasing(!antialiasing);
				break;

			case SDL_SCANCODE_C:
				if (rightShift || leftShift) {
					colorMode = (colorMode + 1) % 2;
				}
				colorCubes();
				break;

			case SDL_SCANCODE_D:
				if (antialiasing && aaMethod == AAMethod::SMAA) {
					if (leftShift || rightShift) {
						debugMode = (debugMode + 3 - 1) % 3;
					} else {
						debugMode = (debugMode + 1) % 3;
					}
					rebuildRG = true;
				}
				break;

			case SDL_SCANCODE_H:
				printHelp();
				break;

			case SDL_SCANCODE_M:
				if (leftShift || rightShift) {
					aaMethod = AAMethod((int(aaMethod) + int(AAMethod::LAST)) % (int(AAMethod::LAST) + 1));
				} else {
					aaMethod = AAMethod((int(aaMethod) + 1) % (int(AAMethod::LAST) + 1));
				}
				rebuildRG = true;
				break;

			case SDL_SCANCODE_Q:
				switch (aaMethod) {
				case AAMethod::MSAA:
					if (leftShift || rightShift) {
						msaaQuality = msaaQuality + maxSMAAQuality - 1;
					} else {
						msaaQuality = msaaQuality + 1;
					}
					msaaQuality = msaaQuality % maxMSAAQuality;
					rebuildRG            = true;
					break;

				case AAMethod::FXAA:
					if (leftShift || rightShift) {
						fxaaQuality = fxaaQuality + maxFXAAQuality - 1;
					} else {
						fxaaQuality = fxaaQuality + 1;
					}
					fxaaQuality  = fxaaQuality % maxFXAAQuality;
					fxaaPipeline = PipelineHandle();
					break;

				case AAMethod::SMAA:
				case AAMethod::SMAA2X:
					if (leftShift || rightShift) {
						smaaQuality = smaaQuality + maxSMAAQuality - 1;
					} else {
						smaaQuality = smaaQuality + 1;
					}
					smaaQuality     = smaaQuality % maxSMAAQuality;
					smaaParameters  = defaultSMAAParameters[smaaQuality];

					smaaPipelines.edgePipeline         = PipelineHandle();
					smaaPipelines.blendWeightPipeline  = PipelineHandle();
					smaaPipelines.neighborPipelines[0] = PipelineHandle();
					smaaPipelines.neighborPipelines[1] = PipelineHandle();

					break;

				}
				break;

			case SDL_SCANCODE_T:
				setTemporalAA(!temporalAA);
				break;

			case SDL_SCANCODE_V:
				switch (rendererDesc.swapchain.vsync) {
				case VSync::On:
					rendererDesc.swapchain.vsync = VSync::LateSwapTear;
					break;

				case VSync::LateSwapTear:
					rendererDesc.swapchain.vsync = VSync::Off;
					break;

				case VSync::Off:
					rendererDesc.swapchain.vsync = VSync::On;
					break;
				}
				recreateSwapchain = true;
				break;

			case SDL_SCANCODE_F:
				rendererDesc.swapchain.fullscreen = !rendererDesc.swapchain.fullscreen;
				recreateSwapchain = true;
				break;

			case SDL_SCANCODE_LEFT:
				sceneIncrement = -1;
				// fallthrough
			case SDL_SCANCODE_RIGHT:
				{
					// if old or new scene is cubes we must rebuild RG
					if (activeScene == 0) {
						rebuildRG = true;
					}

					// all images + cubes scene
					unsigned int numScenes = static_cast<unsigned int>(images.size()) + 1;
					activeScene = (activeScene + sceneIncrement + numScenes) % numScenes;

					if (activeScene == 0) {
						rebuildRG = true;
					}
				}
				break;

			default:
				break;
			}

			break;

		case SDL_KEYUP:
			io.KeysDown[event.key.keysym.scancode] = false;

			switch (event.key.keysym.scancode) {
			case SDL_SCANCODE_LSHIFT:
				leftShift = false;
				break;

			case SDL_SCANCODE_RSHIFT:
				rightShift = false;
				break;

			case SDL_SCANCODE_LALT:
				leftAlt = false;
				break;

			case SDL_SCANCODE_RALT:
				rightAlt = false;
				break;

			case SDL_SCANCODE_LCTRL:
				leftCtrl = false;
				break;

			case SDL_SCANCODE_RCTRL:
				rightCtrl = false;
				break;

			default:
				break;
			}

			break;

		case SDL_TEXTINPUT:
			io.AddInputCharactersUTF8(event.text.text);
			break;

		case SDL_WINDOWEVENT:
			switch (event.window.event) {
			case SDL_WINDOWEVENT_SIZE_CHANGED:
			case SDL_WINDOWEVENT_RESIZED:
				rendererDesc.swapchain.width  = event.window.data1;
				rendererDesc.swapchain.height = event.window.data2;
				recreateSwapchain = true;
				LOG("window resize to %ux%u\n", rendererDesc.swapchain.width, rendererDesc.swapchain.height);
				logFlush();
				break;
			default:
				break;
			}
			break;

		case SDL_MOUSEMOTION:
			io.MousePos = ImVec2(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
			break;

		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			if (event.button.button < 6) {
				// SDL and imgui have left and middle in different order
				static const uint8_t SDLMouseLookup[5] = { 0, 2, 1, 3, 4 };
				io.MouseDown[SDLMouseLookup[event.button.button - 1]] = (event.button.state == SDL_PRESSED);
			}
			break;

		case SDL_MOUSEWHEEL:
			io.MouseWheel = static_cast<float>(event.wheel.y);
			break;

		case SDL_DROPFILE: {
				char* droppedFile = event.drop.file;
				std::string filestring(droppedFile);
				SDL_free(droppedFile);
				loadImage(filestring);
			} break;

		}
	}

	io.KeyShift = leftShift || rightShift;
	io.KeyAlt   = leftAlt   || rightAlt;
	io.KeyCtrl  = leftCtrl  || rightCtrl;
}


void SMAADemo::mainLoopIteration() {
	uint64_t ticks   = getNanoseconds();
	uint64_t elapsed = ticks - lastTime;

	if (fpsLimitActive) {
		uint64_t nsLimit = 1000000000ULL / fpsLimit;
		while (elapsed + sleepFudge < nsLimit) {
			// limit reached, throttle
			uint64_t nsWait = nsLimit - (elapsed + sleepFudge);
			std::this_thread::sleep_for(std::chrono::nanoseconds(nsWait));
			ticks   = getNanoseconds();
			elapsed = ticks - lastTime;
		}
	}

	lastTime = ticks;

	processInput();

	updateGUI(elapsed);

	if (activeScene == 0 && rotateCubes) {
		rotationTime += elapsed;

		// TODO: increasing rotation period can make cubes spin backwards
		const uint64_t rotationPeriod = rotationPeriodSeconds * 1000000000ULL;
		rotationTime   = rotationTime % rotationPeriod;
		cameraRotation = float(M_PI * 2.0f * rotationTime) / rotationPeriod;
	}

	if (antialiasing && temporalAA) {
		temporalFrame = (temporalFrame + 1) % 2;

		switch (aaMethod) {
		case AAMethod::MSAA:
		case AAMethod::FXAA:
			// not used
			subsampleIndices[0] = glm::vec4(0.0f);
			subsampleIndices[1] = glm::vec4(0.0f);
			break;

		case AAMethod::SMAA: {
			float v       = float(temporalFrame + 1);
			subsampleIndices[0] = glm::vec4(v, v, v, 0.0f);
			subsampleIndices[1] = glm::vec4(0.0f);
		} break;

		case AAMethod::SMAA2X:
			if (temporalFrame == 0) {
				subsampleIndices[0] = glm::vec4(5.0f, 3.0f, 1.0f, 3.0f);
				subsampleIndices[1] = glm::vec4(4.0f, 6.0f, 2.0f, 3.0f);
			} else {
				assert(temporalFrame == 1);
				subsampleIndices[0] = glm::vec4(3.0f, 5.0f, 1.0f, 4.0f);
				subsampleIndices[1] = glm::vec4(6.0f, 4.0f, 2.0f, 4.0f);
			}
			break;

		}
	} else {
		if (aaMethod == AAMethod::SMAA2X) {
			subsampleIndices[0] = glm::vec4(1.0, 1.0, 1.0, 0.0f);
		} else {
			subsampleIndices[0] = glm::vec4(0.0f);
		}
		subsampleIndices[1] = glm::vec4(2.0f, 2.0f, 2.0f, 0.0f);
	}

	render();
}


void SMAADemo::render() {
	if (recreateSwapchain) {
		renderer.setSwapchainDesc(rendererDesc.swapchain);

		renderSize = renderer.getDrawableSize();
		LOG("drawable size: %ux%u\n", renderSize.x, renderSize.y);
		logFlush();

		// pump events
		processInput();

		rebuildRG = true;
		recreateSwapchain = false;
	}

	if (rebuildRG) {
		rebuildRenderGraph();
		assert(!rebuildRG);
	}

	// TODO: this should be in RenderGraph
	while (!renderer.beginFrame()) {
		// check if caused by swapchain out of date, recreate if so
		if (renderer.isSwapchainDirty() ) {
			recreateSwapchain = true;
			return;
		}

		// pump events
		processInput();
	}

	if (antialiasing && temporalAA) {
		assert(temporalRTs[0]);
		assert(temporalRTs[1]);
		renderGraph.bindExternalRT(Rendertargets::TemporalPrevious, temporalRTs[1 - temporalFrame]);
		renderGraph.bindExternalRT(Rendertargets::TemporalCurrent,  temporalRTs[    temporalFrame]);
	}

	renderGraph.render(renderer);
}


void RenderGraph::reset(Renderer &renderer, std::function<void(void)> processEvents) {
	assert(state == State::Invalid || state == State::Ready);
	state = State::Building;

	renderPasses.clear();
	renderpassesWithExternalRTs.clear();
	hasExternalRTs = false;

	for (auto &p : pipelines) {
		renderer.deletePipeline(p.handle);
		p.handle = PipelineHandle();
	}
	pipelines.clear();

	for (auto &rt : rendertargets) {
		assert(rt.first != Rendertargets::Invalid);

		visitRendertarget(rt.second
						  , nopExternal
						  , [&] (InternalRT &i) {
							  assert(i.handle);
							  renderer.deleteRenderTarget(i.handle);
							  i.handle = RenderTargetHandle();
						  }
						 );
	}
	rendertargets.clear();

	for (auto &p : renderPasses) {
		auto &rp = p.second;
		if (rp.handle) {
			renderer.deleteRenderPass(rp.handle);
			rp.handle = RenderPassHandle();
		}

		if (rp.fb) {
			renderer.deleteFramebuffer(rp.fb);
			rp.fb = FramebufferHandle();
		}
	}
	renderPasses.clear();

	operations.clear();

	while (!renderer.waitForDeviceIdle()) {
		processEvents();
	}
}


PipelineHandle RenderGraph::createPipeline(Renderer &renderer, RenderPasses rp, PipelineDesc &desc) {
	assert(state == State::Ready || state == State::Rendering);

	auto it = renderPasses.find(rp);
	assert(it != renderPasses.end());
	desc.renderPass(it->second.handle);

	// TODO: use hash map
	for (const auto &pipeline : pipelines) {
		if (pipeline.desc == desc) {
			return pipeline.handle;
		}
	}

	auto handle = renderer.createPipeline(desc);

	Pipeline pipeline;
	pipeline.desc   = desc;
	pipeline.handle = handle;
	pipelines.emplace_back(std::move(pipeline));

	return handle;
}


void RenderGraph::renderTarget(Rendertargets rt, const RenderTargetDesc &desc) {
	assert(state == State::Building);
	assert(rt != Rendertargets::Invalid);

	InternalRT temp1;
	temp1.desc   = desc;
	auto UNUSED temp2 = rendertargets.emplace(rt, temp1);
	assert(temp2.second);
}


void RenderGraph::externalRenderTarget(Rendertargets rt, Format format, Layout initialLayout, Layout finalLayout) {
	assert(state == State::Building);
	assert(rt != Rendertargets::Invalid);
	assert(rendertargets.find(rt) == rendertargets.end());

	hasExternalRTs = true;

	ExternalRT e;
	e.format = format;
	e.initialLayout = initialLayout;
	e.finalLayout   = finalLayout;
	// leave handle undefined, it's set later by bindExternalRT
	auto temp UNUSED = rendertargets.emplace(rt, e);
	assert(temp.second);
}


void RenderGraph::bindExternalRT(Rendertargets rt, RenderTargetHandle handle) {
	assert(state == State::Ready);
	assert(handle);

	auto it = rendertargets.find(rt);
	assert(it != rendertargets.end());
	visitRendertarget(it->second
					  , [&] (ExternalRT &e) {
						  assert(!e.handle);
						  e.handle = handle;
					  }
					  , nopInternal
					 );
}


void RenderGraph::renderPass(RenderPasses rp, const PassDesc &desc, RenderPassFunc f) {
	assert(state == State::Building);

	RenderPass temp1;
	temp1.desc   = desc;
	temp1.func   = f;

	auto temp2 UNUSED = renderPasses.emplace(rp, temp1);
	assert(temp2.second);

	operations.push_back(rp);
}


void RenderGraph::resolveMSAA(Rendertargets source, Rendertargets dest) {
	assert(state == State::Building);

	ResolveMSAA op;
	op.source = source;
	op.dest   = dest;
	operations.push_back(op);
}


void RenderGraph::blit(Rendertargets source, Rendertargets dest) {
	assert(state == State::Building);

	Blit op;
	op.source = source;
	op.dest   = dest;
	operations.push_back(op);
}


void RenderGraph::presentRenderTarget(Rendertargets rt) {
	assert(state == State::Building);
	assert(rt != Rendertargets::Invalid);

	finalTarget = rt;
}



void RenderGraph::build(Renderer &renderer) {
	assert(state == State::Building);
	state = State::Ready;

	assert(finalTarget != Rendertargets::Invalid);

	LOG("RenderGraph::build start\n");

	for (auto &p : rendertargets) {
		assert(p.first != Rendertargets::Invalid);

		visitRendertarget(p.second
						  , nopExternal
						  , [&] (InternalRT &i) {
							  i.handle = renderer.createRenderTarget(i.desc);
						  }
						 );
	}

	// automatically decide layouts
	{
		std::unordered_map<Rendertargets, Layout> currentLayouts;

		// initialize final render target to transfer src
		currentLayouts[finalTarget] = Layout::TransferSrc;

		// initialize external rendertargets final layouts
		for (const auto &rt : rendertargets) {
			visitRendertarget(rt.second
							  , [&rt, &currentLayouts] (const ExternalRT &e) {
								  currentLayouts[rt.first] = e.finalLayout;
							  }
							  , nopInternal
							 );
		}

		struct LayoutVisitor final : public boost::static_visitor<void> {
			std::unordered_map<Rendertargets, Layout> &currentLayouts;
			RenderGraph &rg;


			LayoutVisitor(std::unordered_map<Rendertargets, Layout> &currentLayouts_, RenderGraph &rg_)
			: currentLayouts(currentLayouts_)
			, rg(rg_)
			{
			}

			void operator()(Blit &b) const {
				b.finalLayout            = currentLayouts[b.dest];
				currentLayouts[b.source] = Layout::TransferSrc;
			}

			void operator()(RenderPasses &rpId) const {
				auto it = rg.renderPasses.find(rpId);
				assert(it != rg.renderPasses.end());

				auto &rp     = it->second;
				auto &rpDesc = rp.rpDesc;
				auto &desc   = rp.desc;

				rpDesc.name(desc.name_);
				rpDesc.numSamples(desc.numSamples_);

				if (desc.depthStencil_ != Rendertargets::Invalid) {
					auto rtIt = rg.rendertargets.find(desc.depthStencil_);
					assert(rtIt != rg.rendertargets.end());

					Format fmt = getFormat(rtIt->second);
					assert(fmt != Format::Invalid);

					rpDesc.depthStencil(fmt, PassBegin::DontCare);
					if (desc.clearDepthAttachment) {
						rpDesc.clearDepth(desc.depthClearValue);
					}
				}

				for (unsigned int i = 0; i < MAX_COLOR_RENDERTARGETS; i++) {
					auto rtId = desc.colorRTs_[i].id;
					if (rtId != Rendertargets::Invalid) {
						auto rtIt = rg.rendertargets.find(rtId);
						assert(rtIt != rg.rendertargets.end());

						// get format
						Format fmt = getFormat(rtIt->second);
						assert(fmt != Format::Invalid);

						auto pb = desc.colorRTs_[i].passBegin;
						// TODO: check this, might need a forward pass over operations
						Layout initial = Layout::Undefined;
						if (pb == PassBegin::Keep) {
							initial = Layout::ColorAttachment;
						}

						Layout final = Layout::ColorAttachment;
						auto layoutIt = currentLayouts.find(rtId);
						if (layoutIt == currentLayouts.end()) {
							// unused
							// TODO: remove it entirely
						} else {
							final = layoutIt->second;
						}
						assert(final != Layout::Undefined);
						assert(final != Layout::TransferDst);

						rpDesc.color(i, fmt, pb, initial, final, desc.colorRTs_[i].clearValue);
						currentLayouts[rtId] = initial;
					}
				}

				// mark input rt current layout as shader read
				for (Rendertargets inputRT : desc.inputRendertargets) {
					currentLayouts[inputRT] = Layout::ShaderRead;
				}
			}

			void operator()(ResolveMSAA &resolve) const {
				resolve.finalLayout            = currentLayouts[resolve.dest];
				currentLayouts[resolve.source] = Layout::TransferSrc;
			}
		};

		LayoutVisitor lv(currentLayouts, *this);
		for (auto it = operations.rbegin(); it != operations.rend(); it++) {
			boost::apply_visitor(lv, *it);
		}

	}

	for (auto &p : renderPasses) {
		auto &temp = p.second;
		const auto &desc = temp.desc;

		assert(!temp.handle);
		auto rpHandle = renderer.createRenderPass(temp.rpDesc);
		assert(rpHandle);
		temp.handle = rpHandle;

		assert(!temp.fb);

		// if this renderpass has external RTs we defer its creation
		bool hasExternal = false;
		for (const auto &rt : desc.colorRTs_) {
			if (rt.id != Rendertargets::Invalid) {
				auto it = rendertargets.find(rt.id);
				assert(it != rendertargets.end());
				if (isExternal(it->second)) {
					hasExternal = true;
					break;
				}
			}
		}
		// TODO: check depthStencil too

		if (!hasExternal) {
			buildRenderPassFramebuffer(renderer, temp);
		} else {
			auto result UNUSED = renderpassesWithExternalRTs.insert(p.first);
			assert(result.second);
		}
	}

	{
		struct DebugVisitor final : public boost::static_visitor<void> {
			RenderGraph &rg;


			DebugVisitor(RenderGraph &rg_)
			: rg(rg_)
			{
			}


			void operator()(const Blit &b) const {
				LOG("Blit %s -> %s\t%s\n", to_string(b.source), to_string(b.dest), layoutName(b.finalLayout));
			}

			void operator()(const RenderPasses &rpId) const {
				LOG("RenderPass %s\n", to_string(rpId));
				auto it = rg.renderPasses.find(rpId);
				assert(it != rg.renderPasses.end());
				const auto &desc   = it->second.desc;
				const auto &rpDesc = it->second.rpDesc;

				if (desc.depthStencil_ != Rendertargets::Invalid) {
					LOG(" depthStencil %s\n", to_string(desc.depthStencil_));
				}

				for (unsigned int i = 0; i < MAX_COLOR_RENDERTARGETS; i++) {
					if (desc.colorRTs_[i].id != Rendertargets::Invalid) {
						const auto &rt = rpDesc.color(i);
						LOG(" color %u: %s\t%s\t%s\t%s\n", i, to_string(desc.colorRTs_[i].id), passBeginName(rt.passBegin), layoutName(rt.initialLayout), layoutName(rt.finalLayout));
					}
				}

				if (!desc.inputRendertargets.empty()) {
					LOG(" inputs:\n");
					std::vector<Rendertargets> inputs;
					inputs.reserve(desc.inputRendertargets.size());
					for (auto i : desc.inputRendertargets) {
						inputs.push_back(i);
					}

					std::sort(inputs.begin(), inputs.end());
					for (auto i : inputs) {
						LOG("  %s\n", to_string(i));
					}
				}
			}

			void operator()(const ResolveMSAA &r) const {
				LOG("ResolveMSAA %s -> %s\t%s\n", to_string(r.source), to_string(r.dest), layoutName(r.finalLayout));
			}
		};

		DebugVisitor d(*this);
		for (const auto &op : operations) {
			boost::apply_visitor(d, op);
		}
	}
	LOG("RenderGraph::build end\n");
	logFlush();
}


void RenderGraph::render(Renderer &renderer) {
	assert(state == State::Ready);
	state = State::Rendering;

	if (hasExternalRTs) {
		bool hasExternal = false;
		for (const auto &p : rendertargets) {
			// if we have external RTs they must be bound by now
			visitRendertarget(p.second
							  , [&] (const ExternalRT &e UNUSED) { assert(e.handle); hasExternal = true; }
							  , nopInternal
							 );
		}
		assert(hasExternal);

		// build framebuffers
		for (auto rpName : renderpassesWithExternalRTs) {
			auto it = renderPasses.find(rpName);
			assert(it != renderPasses.end());
			auto &rp = it->second;

			assert(!rp.fb);
			buildRenderPassFramebuffer(renderer, rp);
			assert(rp.fb);
		}
	}

	struct OpVisitor final : public boost::static_visitor<void> {
		Renderer    &r;
		RenderGraph &rg;


		OpVisitor(Renderer &r_, RenderGraph &rg_)
		: r(r_)
		, rg(rg_)
		{
		}


		void operator()(const Blit &b) const {
			auto srcIt = rg.rendertargets.find(b.source);
			assert(srcIt != rg.rendertargets.end());
			RenderTargetHandle sourceHandle = getHandle(srcIt->second);

			auto destIt = rg.rendertargets.find(b.dest);
			assert(destIt != rg.rendertargets.end());
			RenderTargetHandle targetHandle = getHandle(destIt->second);

			r.layoutTransition(targetHandle, Layout::Undefined, Layout::TransferDst);
			r.blit(sourceHandle, targetHandle);
			r.layoutTransition(targetHandle, Layout::TransferDst, b.finalLayout);
		}

		void operator()(const RenderPasses &rp) const {
			assert(rg.currentRP == RenderPasses::Invalid);
			rg.currentRP = rp;

			auto it = rg.renderPasses.find(rp);
			assert(it != rg.renderPasses.end());

			r.beginRenderPass(it->second.handle, it->second.fb);

			PassResources res;
			// TODO: build ahead of time, fill here?
			for (Rendertargets inputRT : it->second.desc.inputRendertargets) {
				// get rendertarget desc
				auto rtIt = rg.rendertargets.find(inputRT);
				assert(rtIt != rg.rendertargets.end());

				// get format
				Format fmt = getFormat(rtIt->second);
				assert(fmt != Format::Invalid);

				{
					// get view from renderer, add to res
					TextureHandle view = r.getRenderTargetView(getHandle(rtIt->second), fmt);
					res.rendertargets.emplace(std::make_pair(inputRT, fmt), view);
					// also add it with Format::Invalid so default works easier
					res.rendertargets.emplace(std::make_pair(inputRT, Format::Invalid), view);
				}

				// do the same for additional view format if there is one
				Format additionalFmt = getAdditionalViewFormat(rtIt->second);
				if (additionalFmt != Format::Invalid) {
					assert(additionalFmt != fmt);
					TextureHandle view = r.getRenderTargetView(getHandle(rtIt->second), additionalFmt);
					res.rendertargets.emplace(std::make_pair(inputRT, additionalFmt), view);
				}
			}

			it->second.func(rp, res);
			r.endRenderPass();

			assert(rg.currentRP == rp);
			rg.currentRP = RenderPasses::Invalid;
		}

		void operator()(const ResolveMSAA &resolve) const {
			auto srcIt = rg.rendertargets.find(resolve.source);
			assert(srcIt != rg.rendertargets.end());
			RenderTargetHandle sourceHandle = getHandle(srcIt->second);

			auto destIt = rg.rendertargets.find(resolve.dest);
			assert(destIt != rg.rendertargets.end());
			RenderTargetHandle targetHandle = getHandle(destIt->second);

			r.layoutTransition(targetHandle, Layout::Undefined, Layout::TransferDst);
			r.resolveMSAA(sourceHandle, targetHandle);
			r.layoutTransition(targetHandle, Layout::TransferDst, resolve.finalLayout);
		}
	};

	for (const auto &op : operations) {
		boost::apply_visitor(OpVisitor(renderer, *this), op);
	}

	{
		auto it = rendertargets.find(finalTarget);
		assert(it != rendertargets.end());
		renderer.presentFrame(getHandle(it->second));
	}

	assert(state == State::Rendering);
	state = State::Ready;

	assert(currentRP == RenderPasses::Invalid);

	if (hasExternalRTs) {
		for (auto &p : rendertargets) {
			// clear the bindings
			visitRendertarget(p.second
							  , [&] (ExternalRT &e) { assert(e.handle); e.handle = RenderTargetHandle(); }
							  , nopInternal
							 );
		}

		// clear framebuffers
		for (auto rpName : renderpassesWithExternalRTs) {
			// clear framebuffers
			auto it = renderPasses.find(rpName);
			assert(it != renderPasses.end());
			auto &rp = it->second;

			assert(rp.fb);
			// TODO: cache them
			renderer.deleteFramebuffer(rp.fb);
			rp.fb = FramebufferHandle();
		}
	}
	assert(currentRP == RenderPasses::Invalid);
}


void SMAADemo::renderCubeScene(RenderPasses rp, RenderGraph::PassResources & /* r */) {
	if (!cubePipeline) {
		std::string name = "cubes";
		if (numSamples > 1) {
			name += " MSAA x" + std::to_string(numSamples);
		}

		PipelineDesc plDesc;
		plDesc.name(name)
		      .vertexShader("cube")
		      .fragmentShader("cube")
		      .numSamples(numSamples)
		      .descriptorSetLayout<GlobalDS>(0)
		      .descriptorSetLayout<CubeSceneDS>(1)
		      .vertexAttrib(ATTR_POS, 0, 3, VtxFormat::Float, 0)
		      .vertexBufferStride(ATTR_POS, sizeof(Vertex))
		      .depthWrite(true)
		      .depthTest(true)
		      .cullFaces(true);

		cubePipeline = renderGraph.createPipeline(renderer, rp, plDesc);
	}
    assert(cubePipeline);

	renderer.bindPipeline(cubePipeline);

	const unsigned int windowWidth  = rendererDesc.swapchain.width;
	const unsigned int windowHeight = rendererDesc.swapchain.height;

	ShaderDefines::Globals globals;
	globals.screenSize            = glm::vec4(1.0f / float(windowWidth), 1.0f / float(windowHeight), windowWidth, windowHeight);
	globals.guiOrtho              = glm::ortho(0.0f, float(windowWidth), float(windowHeight), 0.0f);

	// TODO: better calculation, and check cube size (side is sqrt(3) currently)
	const float cubeDiameter = sqrtf(3.0f);
	const float cubeDistance = cubeDiameter + 1.0f;

	float farPlane  = cameraDistance + cubeDistance * float(cubesPerSide + 1);
	float nearPlane = std::max(0.1f, cameraDistance - cubeDistance * float(cubesPerSide + 1));

	glm::mat4 model  = glm::rotate(glm::mat4(1.0f), cameraRotation, glm::vec3(0.0f, 1.0f, 0.0f));
	glm::mat4 view   = glm::lookAt(glm::vec3(cameraDistance, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	glm::mat4 proj   = glm::perspective(float(65.0f * M_PI * 2.0f / 360.0f), float(windowWidth) / windowHeight, nearPlane, farPlane);
	glm::mat4 viewProj = proj * view * model;

	// temporal jitter
	if (antialiasing && temporalAA) {
		glm::vec2 jitter;
		if (aaMethod == AAMethod::MSAA || aaMethod == AAMethod::SMAA2X) {
			const glm::vec2 jitters[2] = {
				  {  0.125f,  0.125f }
				, { -0.125f, -0.125f }
			};
			jitter = jitters[temporalFrame];
		} else {
			const glm::vec2 jitters[2] = {
				  { -0.25f,  0.25f }
				, { 0.25f,  -0.25f }
			};
			jitter = jitters[temporalFrame];
		}

		jitter = jitter * 2.0f * glm::vec2(globals.screenSize.x, globals.screenSize.y);
		glm::mat4 jitterMatrix = glm::translate(glm::identity<glm::mat4>(), glm::vec3(jitter, 0.0f));
		viewProj = jitterMatrix * viewProj;
	}

	prevViewProj         = currViewProj;
	currViewProj         = viewProj;
	globals.viewProj     = currViewProj;
	globals.prevViewProj = prevViewProj;

	renderer.setViewport(0, 0, windowWidth, windowHeight);

	GlobalDS globalDS;
	globalDS.globalUniforms = renderer.createEphemeralBuffer(BufferType::Uniform, sizeof(ShaderDefines::Globals), &globals);
	globalDS.linearSampler  = linearSampler;
	globalDS.nearestSampler = nearestSampler;
	renderer.bindDescriptorSet(0, globalDS);

	renderer.bindVertexBuffer(0, cubeVBO);
	renderer.bindIndexBuffer(cubeIBO, false);

	CubeSceneDS cubeDS;
	// FIXME: remove unused UBO hack
	uint32_t temp    = 0;
	cubeDS.unused    = renderer.createEphemeralBuffer(BufferType::Uniform, 4, &temp);
	cubeDS.instances = renderer.createEphemeralBuffer(BufferType::Storage, static_cast<uint32_t>(sizeof(ShaderDefines::Cube) * cubes.size()), &cubes[0]);
	renderer.bindDescriptorSet(1, cubeDS);

	unsigned int numCubes = static_cast<unsigned int>(cubes.size());
	if (visualizeCubeOrder) {
		cubeOrderNum = cubeOrderNum % numCubes;
		cubeOrderNum++;
		numCubes     = cubeOrderNum;
	}

	renderer.drawIndexedInstanced(3 * 2 * 6, numCubes);
}


void SMAADemo::renderImageScene(RenderPasses rp, RenderGraph::PassResources & /* r */) {
	if (!imagePipeline) {
		PipelineDesc plDesc;
		plDesc.numSamples(numSamples)
		      .descriptorSetLayout<GlobalDS>(0)
		      .descriptorSetLayout<ColorTexDS>(1)
		      .vertexShader("image")
		      .fragmentShader("image")
		      .name("image");

		imagePipeline = renderGraph.createPipeline(renderer, rp, plDesc);
	}

	renderer.bindPipeline(imagePipeline);

	assert(activeScene > 0);
	const auto &image = images.at(activeScene - 1);

	const unsigned int windowWidth  = rendererDesc.swapchain.width;
	const unsigned int windowHeight = rendererDesc.swapchain.height;

	renderer.setViewport(0, 0, windowWidth, windowHeight);

	ShaderDefines::Globals globals;
	globals.screenSize            = glm::vec4(1.0f / float(windowWidth), 1.0f / float(windowHeight), windowWidth, windowHeight);
	globals.guiOrtho              = glm::ortho(0.0f, float(windowWidth), float(windowHeight), 0.0f);

	GlobalDS globalDS;
	globalDS.globalUniforms  = renderer.createEphemeralBuffer(BufferType::Uniform, sizeof(ShaderDefines::Globals), &globals);
	globalDS.linearSampler   = linearSampler;
	globalDS.nearestSampler  = nearestSampler;
	renderer.bindDescriptorSet(0, globalDS);

	assert(activeScene - 1 < images.size());
	ColorTexDS colorDS;
	// FIXME: remove unused UBO hack
	uint32_t temp  = 0;
	colorDS.unused = renderer.createEphemeralBuffer(BufferType::Uniform, 4, &temp);
	colorDS.color = image.tex;
	renderer.bindDescriptorSet(1, colorDS);
	renderer.draw(0, 3);
}


void SMAADemo::renderFXAA(RenderPasses rp, RenderGraph::PassResources &r) {
	if (!fxaaPipeline) {
		std::string qualityString(fxaaQualityLevels[fxaaQuality]);

		ShaderMacros macros;
		macros.emplace("FXAA_QUALITY_PRESET", qualityString);

		PipelineDesc plDesc;
		plDesc.depthWrite(false)
		      .depthTest(false)
		      .cullFaces(true)
		      .descriptorSetLayout<GlobalDS>(0)
		      .descriptorSetLayout<ColorCombinedDS>(1)
		      .shaderMacros(macros)
		      .vertexShader("fxaa")
		      .fragmentShader("fxaa")
		      .name(std::string("FXAA ") + qualityString);

		fxaaPipeline = renderGraph.createPipeline(renderer, rp, plDesc);
	}
	assert(fxaaPipeline);

	renderer.bindPipeline(fxaaPipeline);
	ColorCombinedDS colorDS;
	// FIXME: remove unused UBO hack
	uint32_t temp         = 0;
	colorDS.unused        = renderer.createEphemeralBuffer(BufferType::Uniform, 4, &temp);
	colorDS.color.tex     = r.get(Rendertargets::MainColor);
	colorDS.color.sampler = linearSampler;
	renderer.bindDescriptorSet(1, colorDS);
	renderer.draw(0, 3);
}


void SMAADemo::renderSeparate(RenderPasses rp, RenderGraph::PassResources &r) {
	if (!separatePipeline) {
		PipelineDesc plDesc;
		plDesc.descriptorSetLayout<GlobalDS>(0)
			  .descriptorSetLayout<ColorCombinedDS>(1)  // TODO: does this need its own DS?
			  .vertexShader("temporal")
			  .fragmentShader("separate")
			  .name("subsample separate");

		separatePipeline = renderGraph.createPipeline(renderer, rp, plDesc);
	}

	renderer.bindPipeline(separatePipeline);
	ColorCombinedDS separateDS;
	// FIXME: remove unused UBO hack
	uint32_t temp            = 0;
	separateDS.unused        = renderer.createEphemeralBuffer(BufferType::Uniform, 4, &temp);
	separateDS.color.tex     = r.get(Rendertargets::MainColor);
	separateDS.color.sampler = nearestSampler;
	renderer.bindDescriptorSet(1, separateDS);
	renderer.draw(0, 3);
}


void SMAADemo::renderSMAAEdges(RenderPasses rp, RenderGraph::PassResources &r, Rendertargets input, int pass) {
	if (!smaaPipelines.edgePipeline) {
		ShaderMacros macros;
		std::string qualityString(std::string("SMAA_PRESET_") + smaaQualityLevels[smaaQuality]);
		macros.emplace(qualityString, "1");

		if (smaaEdgeMethod != SMAAEdgeMethod::Color) {
			macros.emplace("EDGEMETHOD", std::to_string(static_cast<uint8_t>(smaaEdgeMethod)));
		}

		if (smaaPredication && smaaEdgeMethod != SMAAEdgeMethod::Depth) {
			macros.emplace("SMAA_PREDICATION", "1");
		}

		PipelineDesc plDesc;
		plDesc.depthWrite(false)
		      .depthTest(false)
		      .cullFaces(true)
		      .descriptorSetLayout<GlobalDS>(0)
		      .shaderMacros(macros)
		      .descriptorSetLayout<EdgeDetectionDS>(1)
		      .vertexShader("smaaEdge")
		      .fragmentShader("smaaEdge")
		      .name(std::string("SMAA edges ") + std::to_string(smaaQuality));
		smaaPipelines.edgePipeline      = renderGraph.createPipeline(renderer, rp, plDesc);
	}

	renderer.bindPipeline(smaaPipelines.edgePipeline);

	// TODO: this is redundant, clean it up
	ShaderDefines::SMAAUBO smaaUBO;
	smaaUBO.smaaParameters        = smaaParameters;
	smaaUBO.predicationThreshold  = predicationThreshold;
	smaaUBO.predicationScale      = predicationScale;
	smaaUBO.predicationStrength   = predicationStrength;
	smaaUBO.reprojWeigthScale     = reprojectionWeightScale;
	smaaUBO.subsampleIndices      = subsampleIndices[pass];

	auto smaaUBOBuf = renderer.createEphemeralBuffer(BufferType::Uniform, sizeof(ShaderDefines::SMAAUBO), &smaaUBO);

	EdgeDetectionDS edgeDS;
	edgeDS.smaaUBO = smaaUBOBuf;
	if (smaaEdgeMethod == SMAAEdgeMethod::Depth) {
		edgeDS.color.tex     = r.get(Rendertargets::MainDepth);
	} else {
		edgeDS.color.tex     = r.get(input, Format::RGBA8);
	}
	edgeDS.color.sampler = nearestSampler;
	// TODO: only set when using predication
	edgeDS.predicationTex.tex     = r.get(Rendertargets::MainDepth);
	edgeDS.predicationTex.sampler = nearestSampler;
	renderer.bindDescriptorSet(1, edgeDS);
	renderer.draw(0, 3);
}


void SMAADemo::renderSMAAWeights(RenderPasses rp, RenderGraph::PassResources &r, int pass) {
	if (!smaaPipelines.blendWeightPipeline) {
		ShaderMacros macros;
		std::string qualityString(std::string("SMAA_PRESET_") + smaaQualityLevels[smaaQuality]);
		macros.emplace(qualityString, "1");

		PipelineDesc plDesc;
		plDesc.depthWrite(false)
		      .depthTest(false)
		      .cullFaces(true)
		      .descriptorSetLayout<GlobalDS>(0)
		      .descriptorSetLayout<BlendWeightDS>(1)
		      .shaderMacros(macros)
		      .vertexShader("smaaBlendWeight")
		      .fragmentShader("smaaBlendWeight")
		      .name(std::string("SMAA weights ") + std::to_string(smaaQuality));
		smaaPipelines.blendWeightPipeline = renderGraph.createPipeline(renderer, rp, plDesc);
	}

	// TODO: this is redundant, clean it up
	ShaderDefines::SMAAUBO smaaUBO;
	smaaUBO.smaaParameters        = smaaParameters;
	smaaUBO.predicationThreshold  = predicationThreshold;
	smaaUBO.predicationScale      = predicationScale;
	smaaUBO.predicationStrength   = predicationStrength;
	smaaUBO.reprojWeigthScale     = reprojectionWeightScale;
	smaaUBO.subsampleIndices      = subsampleIndices[pass];

	auto smaaUBOBuf = renderer.createEphemeralBuffer(BufferType::Uniform, sizeof(ShaderDefines::SMAAUBO), &smaaUBO);

	renderer.bindPipeline(smaaPipelines.blendWeightPipeline);
	BlendWeightDS blendWeightDS;
	blendWeightDS.smaaUBO           = smaaUBOBuf;
	blendWeightDS.edgesTex.tex      = r.get(Rendertargets::Edges);
	blendWeightDS.edgesTex.sampler  = linearSampler;
	blendWeightDS.areaTex.tex       = areaTex;
	blendWeightDS.areaTex.sampler   = linearSampler;
	blendWeightDS.searchTex.tex     = searchTex;
	blendWeightDS.searchTex.sampler = linearSampler;
	renderer.bindDescriptorSet(1, blendWeightDS);

	renderer.draw(0, 3);
}


void SMAADemo::renderSMAABlend(RenderPasses rp, RenderGraph::PassResources &r, Rendertargets input, int pass) {
	if (!smaaPipelines.neighborPipelines[pass]) {
		ShaderMacros macros;
		std::string qualityString(std::string("SMAA_PRESET_") + smaaQualityLevels[smaaQuality]);
		macros.emplace(qualityString, "1");

		PipelineDesc plDesc;
		plDesc.depthWrite(false)
		      .depthTest(false)
		      .cullFaces(true)
		      .descriptorSetLayout<GlobalDS>(0)
		      .descriptorSetLayout<NeighborBlendDS>(1)
		      .shaderMacros(macros)
		      .vertexShader("smaaNeighbor")
		      .fragmentShader("smaaNeighbor");

		if (pass == 0) {
			plDesc.name(std::string("SMAA blend ") + std::to_string(smaaQuality));
		} else {
			assert(pass == 1);
			plDesc.blending(true)
			      .sourceBlend(BlendFunc::Constant)
			      .destinationBlend(BlendFunc::Constant)
			      .name(std::string("SMAA blend (S2X) ") + std::to_string(smaaQuality));
		}

		smaaPipelines.neighborPipelines[pass] = renderGraph.createPipeline(renderer, rp, plDesc);
	}

	// TODO: this is redundant, clean it up
	ShaderDefines::SMAAUBO smaaUBO;
	smaaUBO.smaaParameters        = smaaParameters;
	smaaUBO.predicationThreshold  = predicationThreshold;
	smaaUBO.predicationScale      = predicationScale;
	smaaUBO.predicationStrength   = predicationStrength;
	smaaUBO.reprojWeigthScale     = reprojectionWeightScale;
	smaaUBO.subsampleIndices      = subsampleIndices[pass];

	auto smaaUBOBuf = renderer.createEphemeralBuffer(BufferType::Uniform, sizeof(ShaderDefines::SMAAUBO), &smaaUBO);

	// full effect
	renderer.bindPipeline(smaaPipelines.neighborPipelines[pass]);

	NeighborBlendDS neighborBlendDS;
	neighborBlendDS.smaaUBO              = smaaUBOBuf;
	neighborBlendDS.color.tex            = r.get(input);
	neighborBlendDS.color.sampler        = linearSampler;
	neighborBlendDS.blendweights.tex     = r.get(Rendertargets::BlendWeights);
	neighborBlendDS.blendweights.sampler = linearSampler;
	renderer.bindDescriptorSet(1, neighborBlendDS);

	renderer.draw(0, 3);
}


void SMAADemo::renderSMAADebug(RenderPasses rp, RenderGraph::PassResources &r, Rendertargets rt) {
	if (!blitPipeline) {
		PipelineDesc plDesc;
		plDesc.descriptorSetLayout<GlobalDS>(0)
		      .descriptorSetLayout<ColorTexDS>(1)
		      .vertexShader("blit")
		      .fragmentShader("blit")
		      .name("blit");

		blitPipeline = renderGraph.createPipeline(renderer, rp, plDesc);
	}

	ColorTexDS blitDS;
	renderer.bindPipeline(blitPipeline);
	// FIXME: remove unused UBO hack
	uint32_t temp  = 0;
	blitDS.unused  = renderer.createEphemeralBuffer(BufferType::Uniform, 4, &temp);
	blitDS.color   = r.get(rt);
	renderer.bindDescriptorSet(1, blitDS);

	renderer.draw(0, 3);
}


void SMAADemo::renderTemporalAA(RenderPasses rp, RenderGraph::PassResources &r) {
	if (!temporalAAPipelines[temporalReproject]) {
		ShaderMacros macros;
		macros.emplace("SMAA_REPROJECTION", std::to_string(temporalReproject));

		PipelineDesc plDesc;
		plDesc.descriptorSetLayout<GlobalDS>(0)
			  .descriptorSetLayout<TemporalAADS>(1)
			  .vertexShader("temporal")
			  .fragmentShader("temporal")
			  .shaderMacros(macros)
			  .name("temporal AA");

		temporalAAPipelines[temporalReproject] = renderGraph.createPipeline(renderer, rp, plDesc);
	}

	renderer.bindPipeline(temporalAAPipelines[temporalReproject]);

	ShaderDefines::SMAAUBO smaaUBO;
	smaaUBO.smaaParameters        = smaaParameters;
	smaaUBO.predicationThreshold  = predicationThreshold;
	smaaUBO.predicationScale      = predicationScale;
	smaaUBO.predicationStrength   = predicationStrength;
	smaaUBO.reprojWeigthScale     = reprojectionWeightScale;
	smaaUBO.subsampleIndices      = subsampleIndices[0];

	auto smaaUBOBuf = renderer.createEphemeralBuffer(BufferType::Uniform, sizeof(ShaderDefines::SMAAUBO), &smaaUBO);

	TemporalAADS temporalDS;
	temporalDS.smaaUBO             = smaaUBOBuf;
	temporalDS.currentTex.tex      = r.get(Rendertargets::TemporalCurrent);
	temporalDS.currentTex.sampler  = nearestSampler;
	if (temporalAAFirstFrame) {
		// to prevent flicker on first frame after enabling
		// TODO: should just do blit
		temporalDS.previousTex.tex     = r.get(Rendertargets::TemporalCurrent);
		temporalDS.previousTex.sampler = nearestSampler;
		temporalAAFirstFrame = false;
	} else {
		temporalDS.previousTex.tex     = r.get(Rendertargets::TemporalPrevious);
		temporalDS.previousTex.sampler = nearestSampler;
	}
	temporalDS.velocityTex.tex         = r.get(Rendertargets::Velocity);
	temporalDS.velocityTex.sampler     = nearestSampler;

	renderer.bindDescriptorSet(1, temporalDS);
	renderer.draw(0, 3);
}


void SMAADemo::updateGUI(uint64_t elapsed) {
	const unsigned int windowWidth  = rendererDesc.swapchain.width;
	const unsigned int windowHeight = rendererDesc.swapchain.height;

	ImGuiIO& io    = ImGui::GetIO();
	io.DeltaTime   = float(double(elapsed) / double(1000000000ULL));
	io.DisplaySize = ImVec2(static_cast<float>(windowWidth), static_cast<float>(windowHeight));
	io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

	ImGui::NewFrame();

	if (io.WantTextInput != textInputActive) {
		textInputActive = io.WantTextInput;
		if (textInputActive) {
			SDL_StartTextInput();
		} else {
			SDL_StopTextInput();
		}
	}

	bool windowVisible = true;
	int flags = 0;
	flags |= ImGuiWindowFlags_NoTitleBar;
	flags |= ImGuiWindowFlags_NoResize;
	flags |= ImGuiWindowFlags_AlwaysAutoResize;

	if (ImGui::Begin("SMAA", &windowVisible, flags)) {
		if (ImGui::CollapsingHeader("Antialiasing properties", ImGuiTreeNodeFlags_DefaultOpen)) {
			bool temp = antialiasing;
			bool aaChanged = ImGui::Checkbox("Antialiasing", &temp);
			if (aaChanged) {
				setAntialiasing(temp);
				assert(temp == antialiasing);
			}

			int aa = static_cast<int>(aaMethod);
			ImGui::RadioButton("MSAA", &aa, static_cast<int>(AAMethod::MSAA)); ImGui::SameLine();
			ImGui::RadioButton("FXAA", &aa, static_cast<int>(AAMethod::FXAA)); ImGui::SameLine();
			ImGui::RadioButton("SMAA", &aa, static_cast<int>(AAMethod::SMAA)); ImGui::SameLine();
			ImGui::RadioButton("SMAA2X", &aa, static_cast<int>(AAMethod::SMAA2X));

			{
				bool tempTAA = temporalAA;
				if (ImGui::Checkbox("Temporal AA", &tempTAA)) {
					rebuildRG            = true;
				}
				if (temporalAA != tempTAA) {
					setTemporalAA(tempTAA);
					assert(temporalAA == tempTAA);
				}

				// temporal reprojection only enabled when temporal AA is
				if (!temporalAA) {
					ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
					ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
				}
				ImGui::Checkbox("Temporal reprojection", &temporalReproject);
				if (!temporalAA) {
					ImGui::PopItemFlag();
					ImGui::PopStyleVar();
				}
			}

			float w = reprojectionWeightScale;
			ImGui::SliderFloat("Reprojection weight scale", &w, 0.0f, 80.0f);
			reprojectionWeightScale = w;

			ImGui::Separator();
			int msaaq = msaaQuality;
			bool msaaChanged = ImGui::Combo("MSAA quality", &msaaq, msaaQualityLevels, maxMSAAQuality);
			if (aaChanged || aa != static_cast<int>(aaMethod)) {
				aaMethod = static_cast<AAMethod>(aa);
				rebuildRG = true;
			}

			if (msaaChanged && aaMethod == AAMethod::MSAA) {
				assert(msaaq >= 0);
				msaaQuality = static_cast<unsigned int>(msaaq);
				rebuildRG            = true;
			}

			ImGui::Separator();
			int sq = smaaQuality;
			ImGui::Combo("SMAA quality", &sq, smaaQualityLevels, maxSMAAQuality);
			assert(sq >= 0);
			assert(sq < int(maxSMAAQuality));
			if (int(smaaQuality) != sq) {
				smaaQuality = sq;
				if (sq != 0) {
					smaaParameters  = defaultSMAAParameters[sq];
				}
				smaaPipelines.edgePipeline         = PipelineHandle();
				smaaPipelines.blendWeightPipeline  = PipelineHandle();
				smaaPipelines.neighborPipelines[0] = PipelineHandle();
				smaaPipelines.neighborPipelines[1] = PipelineHandle();
			}

			if (ImGui::CollapsingHeader("SMAA custom properties")) {
				// parameters can only be changed in custom mode
				// https://github.com/ocornut/imgui/issues/211
				if (smaaQuality != 0) {
					ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
					ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
				}

				ImGui::SliderFloat("SMAA color/luma edge threshold", &smaaParameters.threshold,      0.0f, 0.5f);
				ImGui::SliderFloat("SMAA depth edge threshold",      &smaaParameters.depthThreshold, 0.0f, 1.0f);

				int s = smaaParameters.maxSearchSteps;
				ImGui::SliderInt("Max search steps",  &s, 0, 112);
				smaaParameters.maxSearchSteps = s;

				s = smaaParameters.maxSearchStepsDiag;
				ImGui::SliderInt("Max diagonal search steps", &s, 0, 20);
				smaaParameters.maxSearchStepsDiag = s;

				s = smaaParameters.cornerRounding;
				ImGui::SliderInt("Corner rounding",   &s, 0, 100);
				smaaParameters.cornerRounding = s;

				if (smaaQuality != 0) {
					ImGui::PopItemFlag();
					ImGui::PopStyleVar();
				}
			}

			ImGui::Checkbox("Predicated thresholding", &smaaPredication);

			if (!smaaPredication) {
				ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
				ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
			}

			ImGui::SliderFloat("Predication threshold", &predicationThreshold, 0.0f, 1.0f, "%.4f", 3.0f);
			ImGui::SliderFloat("Predication scale",     &predicationScale,     1.0f, 5.0f);
			ImGui::SliderFloat("Predication strength",  &predicationStrength,  0.0f, 1.0f);
			if (ImGui::Button("Reset predication values")) {
				predicationThreshold = 0.01f;
				predicationScale     = 2.0f;
				predicationStrength  = 0.4f;
			}

			if (!smaaPredication) {
				ImGui::PopItemFlag();
				ImGui::PopStyleVar();
			}

			int em = static_cast<int>(smaaEdgeMethod);
			ImGui::Text("SMAA edge detection");
			ImGui::RadioButton("Color", &em, static_cast<int>(SMAAEdgeMethod::Color));
			ImGui::RadioButton("Luma",  &em, static_cast<int>(SMAAEdgeMethod::Luma));
			ImGui::RadioButton("Depth", &em, static_cast<int>(SMAAEdgeMethod::Depth));
			smaaEdgeMethod = static_cast<SMAAEdgeMethod>(em);

			int d = debugMode;
			ImGui::Combo("SMAA debug", &d, smaaDebugModes, 3);
			assert(d >= 0);
			assert(d < 3);
			if (int(debugMode) != d) {
				debugMode = d;
				rebuildRG = true;
			}

			int fq = fxaaQuality;
			ImGui::Separator();
			ImGui::Combo("FXAA quality", &fq, fxaaQualityLevels, maxFXAAQuality);
			assert(fq >= 0);
			assert(fq < int(maxFXAAQuality));
			if (fq != int(fxaaQuality)) {
				fxaaPipeline = PipelineHandle();
				fxaaQuality = fq;
			}
		}

		if (ImGui::CollapsingHeader("Scene properties", ImGuiTreeNodeFlags_DefaultOpen)) {
			// TODO: don't regenerate this on every frame
			std::vector<const char *> scenes;
			scenes.reserve(images.size() + 1);
			scenes.push_back("Cubes");
			for (const auto &img : images) {
				scenes.push_back(img.shortName.c_str());
			}
			assert(activeScene < scenes.size());
			int s = activeScene;
			ImGui::Combo("Scene", &s, &scenes[0], static_cast<int>(scenes.size()));
			assert(s >= 0);
			assert(s < int(scenes.size()));
			if (s != int(activeScene)) {
				// if old or new scene is cubes we must rebuild RG
				if (activeScene == 0 || s == 0) {
					rebuildRG = true;
				}
				activeScene = s;
			}

			ImGui::InputText("Load image", imageFileName, inputTextBufferSize);

			ImGui::Columns(2);

			if (ImGui::Button("Paste")) {
				char *clipboard = SDL_GetClipboardText();
				if (clipboard) {
					size_t length = strnlen(clipboard, inputTextBufferSize - 1);
					strncpy(imageFileName, clipboard, length);
					imageFileName[length] = '\0';
					SDL_free(clipboard);
				}
			}
			ImGui::NextColumn();
			if (ImGui::Button("Load")) {
				std::string filename(imageFileName);
				loadImage(filename);
			}

			ImGui::Columns(1);

			int m = cubesPerSide;
			bool changed = ImGui::InputInt("Cubes per side", &m);
			if (changed && m > 0 && m < 55) {
				cubesPerSide = m;
				createCubes();
			}

			float l = cameraDistance;
			if (ImGui::SliderFloat("Camera distance", &l, 1.0f, 256.0f, "%.1f")) {
				cameraDistance = l;
			}

			ImGui::Checkbox("Rotate cubes", &rotateCubes);
			int p = rotationPeriodSeconds;
			ImGui::SliderInt("Rotation period (sec)", &p, 1, 60);
			assert(p >= 1);
			assert(p <= 60);
			rotationPeriodSeconds = p;

			ImGui::Separator();
			ImGui::Text("Cube coloring mode");
			int newColorMode = colorMode;
			ImGui::RadioButton("RGB",   &newColorMode, 0);
			ImGui::RadioButton("YCbCr", &newColorMode, 1);

			if (int(colorMode) != newColorMode) {
				colorMode = newColorMode;
				colorCubes();
			}

			if (ImGui::Button("Re-color cubes")) {
				colorCubes();
			}

			if (ImGui::Button("Shuffle cube rendering order")) {
				shuffleCubeRendering();
				cubeOrderNum = 1;
			}

			if (ImGui::Button("Reorder cube rendering order")) {
				reorderCubeRendering();
				cubeOrderNum = 1;
			}

			ImGui::Checkbox("Visualize cube order", &visualizeCubeOrder);
		}

		if (ImGui::CollapsingHeader("Swapchain properties", ImGuiTreeNodeFlags_DefaultOpen)) {
			bool temp = ImGui::Checkbox("Fullscreen", &rendererDesc.swapchain.fullscreen);
            // don't nuke recreateSwapchain in case it was already true
			if (temp) {
				recreateSwapchain = true;
			}

			int vsyncTemp = static_cast<int>(rendererDesc.swapchain.vsync);
			ImGui::Text("V-Sync");
			ImGui::RadioButton("Off",            &vsyncTemp, 0);
			ImGui::RadioButton("On",             &vsyncTemp, 1);
			ImGui::RadioButton("Late swap tear", &vsyncTemp, 2);

			if (vsyncTemp != static_cast<int>(rendererDesc.swapchain.vsync)) {
				recreateSwapchain = true;
				rendererDesc.swapchain.vsync = static_cast<VSync>(vsyncTemp);
			}

			int n = rendererDesc.swapchain.numFrames;
			// TODO: ask Renderer for the limits
			if (ImGui::SliderInt("frames ahead", &n, 1, 16)) {
				rendererDesc.swapchain.numFrames = n;
				recreateSwapchain = true;
			}

			ImGui::Checkbox("FPS limit", &fpsLimitActive);

			int f   = fpsLimit;
			bool changed = ImGui::InputInt("Max FPS", &f);
			if (changed && f > 0) {
				fpsLimit = f;
			}

			ImGui::Separator();
			// TODO: measure actual GPU time
			ImGui::LabelText("FPS", "%.1f", io.Framerate);
			ImGui::LabelText("Frame time ms", "%.1f", 1000.0f / io.Framerate);

#ifdef RENDERER_VULKAN
			ImGui::Separator();
			// VMA memory allocation stats
			MemoryStats stats = renderer.getMemStats();
			float usedMegabytes = static_cast<float>(stats.usedBytes) / (1024.0f * 1024.0f);
			float totalMegabytes = static_cast<float>(stats.usedBytes + stats.unusedBytes) / (1024.0f * 1024.0f);
			ImGui::LabelText("Allocation count", "%u", stats.allocationCount);
			ImGui::LabelText("Suballocation count", "%u", stats.subAllocationCount);
			ImGui::LabelText("Used memory (MB)", "%.2f", usedMegabytes);
			ImGui::LabelText("Total memory (MB)", "%.2f", totalMegabytes);
#endif
		}

		if (ImGui::Button("Quit")) {
			keepGoing = false;
		}
	}

	// move the window to right edge of screen
	ImVec2 pos;
	pos.x =  windowWidth  - ImGui::GetWindowWidth();
	pos.y = (windowHeight - ImGui::GetWindowHeight()) / 2.0f;
	ImGui::SetWindowPos(pos);

	ImGui::End();

#if 0
	bool demoVisible = true;
	ImGui::ShowDemoWindow(&demoVisible);
#endif  // 0

	ImGui::Render();
}


void SMAADemo::renderGUI(RenderPasses rp, RenderGraph::PassResources & /* r */) {
	auto drawData = ImGui::GetDrawData();
	assert(drawData->Valid);

	if (drawData->CmdListsCount > 0) {
		assert(drawData->CmdLists      != nullptr);
		assert(drawData->TotalVtxCount >  0);
		assert(drawData->TotalIdxCount >  0);

        if (!guiPipeline) {
			PipelineDesc plDesc;
			plDesc.descriptorSetLayout<GlobalDS>(0)
				  .descriptorSetLayout<ColorTexDS>(1)
				  .vertexShader("gui")
				  .fragmentShader("gui")
				  .blending(true)
				  .sourceBlend(BlendFunc::SrcAlpha)
				  .destinationBlend(BlendFunc::OneMinusSrcAlpha)
				  .scissorTest(true)
				  .vertexAttrib(ATTR_POS,   0, 2, VtxFormat::Float,  offsetof(ImDrawVert, pos))
				  .vertexAttrib(ATTR_UV,    0, 2, VtxFormat::Float,  offsetof(ImDrawVert, uv))
				  .vertexAttrib(ATTR_COLOR, 0, 4, VtxFormat::UNorm8, offsetof(ImDrawVert, col))
				  .vertexBufferStride(ATTR_POS, sizeof(ImDrawVert))
				  .name("gui");

			guiPipeline = renderGraph.createPipeline(renderer, rp, plDesc);
		}

		renderer.bindPipeline(guiPipeline);
		ColorTexDS colorDS;
		// FIXME: remove unused UBO hack
		uint32_t temp = 0;
		colorDS.unused = renderer.createEphemeralBuffer(BufferType::Uniform, 4, &temp);
		colorDS.color = imguiFontsTex;
		renderer.bindDescriptorSet(1, colorDS);
		// TODO: upload all buffers first, render after
		// and one buffer each vertex/index

		for (int n = 0; n < drawData->CmdListsCount; n++) {
			const ImDrawList* cmd_list = drawData->CmdLists[n];

			BufferHandle vtxBuf = renderer.createEphemeralBuffer(BufferType::Vertex, cmd_list->VtxBuffer.Size * sizeof(ImDrawVert), cmd_list->VtxBuffer.Data);
			BufferHandle idxBuf = renderer.createEphemeralBuffer(BufferType::Index, cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx), cmd_list->IdxBuffer.Data);
			renderer.bindIndexBuffer(idxBuf, true);
			renderer.bindVertexBuffer(0, vtxBuf);

			unsigned int idx_buffer_offset = 0;
			for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++) {
				const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
				if (pcmd->UserCallback) {
					// TODO: this probably does nothing useful for us
					assert(false);

					pcmd->UserCallback(cmd_list, pcmd);
				} else {
					assert(pcmd->TextureId == 0);
					renderer.setScissorRect(static_cast<unsigned int>(pcmd->ClipRect.x), static_cast<unsigned int>(pcmd->ClipRect.y),
						static_cast<unsigned int>(pcmd->ClipRect.z - pcmd->ClipRect.x), static_cast<unsigned int>(pcmd->ClipRect.w - pcmd->ClipRect.y));
					renderer.drawIndexedOffset(pcmd->ElemCount, idx_buffer_offset, 0, cmd_list->VtxBuffer.Size);
				}
				idx_buffer_offset += pcmd->ElemCount;
			}
		}
#if 0
		LOG("CmdListsCount: %d\n", drawData->CmdListsCount);
		LOG("TotalVtxCount: %d\n", drawData->TotalVtxCount);
		LOG("TotalIdxCount: %d\n", drawData->TotalIdxCount);
#endif // 0
	} else {
		assert(drawData->CmdLists      == nullptr);
		assert(drawData->TotalVtxCount == 0);
		assert(drawData->TotalIdxCount == 0);
	}
}


int main(int argc, char *argv[]) {
	try {
		logInit();

		auto demo = std::make_unique<SMAADemo>();

		demo->parseCommandLine(argc, argv);

		demo->initRender();
		demo->createCubes();
		printHelp();

		while (demo->shouldKeepGoing()) {
			try {
				demo->mainLoopIteration();
			} catch (std::exception &e) {
				LOG("caught std::exception: \"%s\"\n", e.what());
				logFlush();
				break;
			} catch (...) {
				LOG("caught unknown exception\n");
				logFlush();
				break;
			}
		}
	} catch (std::exception &e) {
		LOG("caught std::exception \"%s\"\n", e.what());
#ifndef _MSC_VER
		logShutdown();
		// so native dumps core
		throw;
#endif
	} catch (...) {
		LOG("unknown exception\n");
#ifndef _MSC_VER
		logShutdown();
		// so native dumps core
		throw;
#endif
	}
	logShutdown();
	return 0;
}
