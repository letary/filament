/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "VulkanPipelineCache.h"

#include "VulkanConstants.h"
#include "VulkanHandles.h"

#include "vulkan/utils/Conversion.h"

#include <private/utils/Tracing.h>

#include <utils/compiler.h>
#include <utils/JobSystem.h>
#include <utils/Log.h>

#include <chrono>
#include <utils/Panic.h>
#if defined(__clang__)
// Vulkan functions often immediately dereference pointers, so it's fine to pass in a pointer
// to a stack-allocated variable.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-stack-address"
#endif

using namespace bluevk;

namespace filament::backend {

namespace {

using utils::JobSystem;

#if FVK_ENABLED(FVK_DEBUG_SHADER_MODULE)
void printPipelineFeedbackInfo(VkPipelineCreationFeedbackCreateInfo const& feedbackInfo) {
    VkPipelineCreationFeedback const& pipelineInfo = *feedbackInfo.pPipelineCreationFeedback;
    if (!(pipelineInfo.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT)) {
        return;
    }

    bool const isCacheHit =
            (pipelineInfo.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT);
    FVK_LOGD << "Pipeline build stats - Cache hit: " << isCacheHit
             << ", Time: " << pipelineInfo.duration / 1000000.0 << "ms";

    for (uint32_t i = 0; i < feedbackInfo.pipelineStageCreationFeedbackCount; ++i) {
        VkPipelineCreationFeedback const& stageInfo = feedbackInfo.pPipelineStageCreationFeedbacks[i];
        if (!(stageInfo.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT)) {
            continue;
        }

        bool const isVertexShader = (i == 0);
        bool const isCacheHit = (stageInfo.flags &
                                 VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT);
        FVK_LOGD << (isVertexShader ? "Vertex" : "Fragment")
                 << " shader build stats - Cache hit: " << isCacheHit
                 << ", Time: " << stageInfo.duration / 1000000.0 << "ms";
    }
}
#endif

} // namespace

VulkanPipelineCache::VulkanPipelineCache(DriverBase& driver, VkDevice device, VulkanContext const& context)
        : mDevice(device),
          mCallbackManager(driver),
          mContext(context) {
    VkPipelineCacheCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
    };
    bluevk::vkCreatePipelineCache(mDevice, &createInfo, VKALLOC, &mPipelineCache);

    // lecodes 0028: say which way the prewarming decision went — it is silent otherwise, and its
    // three inputs (feature flag, vertex-input dynamic state, dynamic rendering) all fail quietly.
    FVK_LOGI << "VulkanPipelineCache: pipeline prewarming "
             << (mContext.shouldUsePipelineCachePrewarming() ? "ON" : "OFF")
             << " (flag " << mContext.asyncPipelineCachePrewarmingEnabled()
             << ", vertexInputDynamicState " << mContext.isVertexInputDynamicStateSupported()
             << ", dynamicRendering " << mContext.isDynamicRenderingSupported()
             << ", parallelCompileDisabled " << mContext.parallelShaderCompilationDisabled() << ")";

    if (mContext.shouldUsePipelineCachePrewarming()) {
        mCompilerThreadPool.init(
            /*threadCount=*/1,
            []() {
                JobSystem::setThreadName("CompilerThreadPool");
                // This thread should be lower priority than the main thread.
                JobSystem::setThreadPriority(JobSystem::Priority::DISPLAY);
            }, []() {
                // No cleanup required.
            });
    }
}

void VulkanPipelineCache::bindLayout(VkPipelineLayout layout) noexcept {
    mPipelineRequirements.layout = layout;
}

VulkanPipelineCache::PipelineCacheEntry* VulkanPipelineCache::getOrCreatePipeline() noexcept {
    // If a cached object exists, re-use it, otherwise create a new one.
    if (PipelineMap::iterator pipelineIter = mPipelines.find(mPipelineRequirements);
            pipelineIter != mPipelines.end()) {
        auto& pipeline = pipelineIter.value();
        pipeline.lastUsed = mCurrentTime;
        FILAMENT_TRACING_EVENT(FILAMENT_TRACING_CATEGORY_FILAMENT,
                "Pipeline(Hit)", "program", mBoundProgram.c_str_safe());
        // lecodes 0028: keep the classes drawn with this render pass fresh (handle compare only).
        for (auto& c : mClasses) {
            if (c.key.renderPass == mPipelineRequirements.renderPass) c.lastSeen = mCurrentTime;
        }
        return &pipeline;
    }
    FILAMENT_TRACING_EVENT(FILAMENT_TRACING_CATEGORY_FILAMENT,
            "Pipeline(Miss)", "program", mBoundProgram.c_str_safe());
    auto const t0 = std::chrono::steady_clock::now();
    PipelineCacheEntry cacheEntry {
        .handle = createPipeline(mPipelineRequirements),
        .lastUsed = mCurrentTime,
    };
    double const ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    ++mFlushMisses; mFlushMissMs += ms;
    ++mTotalMisses; mTotalMissMs += ms;
    recordClass(mPipelineRequirements);
    if (ms >= 5.0) {
        // One line per draw-time build that cost real time: which program, how long, and the state
        // class that decides whether a prewarm pipeline could have been a driver-cache hit for it.
        auto const& rs = mPipelineRequirements.rasterState;
        FVK_LOGI << "VulkanPipelineCache: built \"" << mBoundProgram.c_str_safe() << "\" in " << ms
                 << " ms (fs " << (mPipelineRequirements.shaders[1] != VK_NULL_HANDLE)
                 << ", blend " << (int) rs.blendEnable << ", depthWrite " << (int) rs.depthWriteEnable
                 << ", writeMask " << (int) rs.colorWriteMask << ", cull " << (int) rs.cullMode
                 << ", targets " << (int) rs.colorTargetCount << ", samples " << (int) rs.rasterizationSamples
                 << ")";
    }
    assert_invariant(cacheEntry.handle != VK_NULL_HANDLE && "Pipeline handle is VK_NULL_HANDLE");
    return &mPipelines.emplace(mPipelineRequirements, cacheEntry).first.value();
}

void VulkanPipelineCache::recordClass(PipelineKey const& real) noexcept {
    PipelineKey k = real;
    k.shaders[0] = VK_NULL_HANDLE;
    k.shaders[1] = VK_NULL_HANDLE;
    k.layout = VK_NULL_HANDLE;
    static PipelineEqual const equal;
    PipelineClass* cls = nullptr;
    for (auto& c : mClasses) {
        if (equal(c.key, k)) { cls = &c; break; }
    }
    if (cls == nullptr) {
        if (mClasses.size() < MAX_CLASSES) {
            mClasses.push_back({ k, mCurrentTime, {} });
            cls = &mClasses.back();
        } else {
            // Full: the least recently seen class gives way.
            auto oldest = mClasses.begin();
            for (auto it = mClasses.begin(); it != mClasses.end(); ++it) {
                if (it->lastSeen < oldest->lastSeen) oldest = it;
            }
            *oldest = { k, mCurrentTime, {} };
            cls = &*oldest;
        }
    }
    cls->lastSeen = mCurrentTime;
    for (auto l : cls->layouts) {
        if (l == real.layout) return;   // a known pairing: nothing new to prewarm
    }
    cls->layouts.push_back(real.layout);

    // This class was just drawn with this layout for the first time: every live program of the
    // same layout gets its pipeline for it now, in the background — except the one that just
    // built it at draw time.
    for (size_t i = mPrograms.size(); i-- > 0;) {
        auto const& p = mPrograms[i];
        if (p.program->isParallelCompilationCanceled()) {
            mPrograms.erase(mPrograms.begin() + i);
            continue;
        }
        if (p.layout != real.layout) continue;
        if (p.program->getVertexShader() == real.shaders[0]
                && p.program->getFragmentShader() == real.shaders[1]) continue;
        prewarmClass(*cls, p.program, p.layout, CompilerPriorityQueue::LOW);
    }
}

void VulkanPipelineCache::prewarmClass(PipelineClass const& cls,
        resource_ptr<VulkanProgram> const& vprogram, VkPipelineLayout layout,
        CompilerPriorityQueue priority) {
    PipelineKey k = cls.key;
    k.shaders[0] = vprogram->getVertexShader();
    k.shaders[1] = vprogram->getFragmentShader();
    k.layout = layout;
    if (mPipelines.find(k) != mPipelines.end()) return;

    CallbackManager::Handle cmh = mCallbackManager.get();
    auto token = std::make_shared<ProgramToken>();
    // vprogram is held by the job so the shader modules outlive createPipeline (as upstream does).
    mCompilerThreadPool.queue(priority, token, [this, vprogram, k, cmh]() mutable {
        if (!vprogram->isParallelCompilationCanceled()) {
            VkPipeline pipeline = createPipeline(k, {});
            if (pipeline != VK_NULL_HANDLE) {
                std::lock_guard<std::mutex> const lock(mPrewarmedLock);
                mPrewarmed.push_back({ k, pipeline });
                mPrewarmBuilt.fetch_add(1, std::memory_order_relaxed);
            } else {
                mPrewarmFailed.fetch_add(1, std::memory_order_relaxed);
            }
        }
        mCallbackManager.put(cmh);
    });
}

void VulkanPipelineCache::bindPipeline(VulkanCommandBuffer* commands) {
    VkCommandBuffer const cmdbuffer = commands->buffer();
    PipelineCacheEntry* cacheEntry = getOrCreatePipeline();

    // If an error occurred, allow higher levels to handle it gracefully.
    assert_invariant(cacheEntry != nullptr && "Failed to create/find pipeline");


    static PipelineEqual equal;
    if (!equal(mBoundPipeline, mPipelineRequirements)) {
        mBoundPipeline = mPipelineRequirements;
        vkCmdBindPipeline(cmdbuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, cacheEntry->handle);
    }
}

void VulkanPipelineCache::asyncPrewarmCache(
        resource_ptr<VulkanProgram> vprogram,
        VkPipelineLayout layout,
        StereoscopicType stereoscopicType,
        uint8_t stereoscopicViewCount,
        CompilerPriorityQueue priority) {
    PipelineKey key {
        .shaders = {
            vprogram->getVertexShader(),
            vprogram->getFragmentShader(),
        },
        // We're using dynamic rendering, so this should be empty.
        .renderPass = VK_NULL_HANDLE,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .subpassIndex = 0,
        // We're using vertex input dynamic state, so these should be empty.
        .vertexAttributes = {},
        .vertexBuffers = {},
        // lecodes 0028: the stand-in raster state is the COMMON OPAQUE one — colour writes on, depth
        // test + write, back-face culling — instead of upstream's "write nothing" state. Measured on
        // NVIDIA 616 (cold driver cache): with colorWriteMask 0 / no depth write the driver compiled
        // 87 prewarm pipelines and the 20 first-draw pipelines still cost 400 ms (394 ms with the
        // prewarm off) — a fragment shader whose outputs are masked is dead-code-eliminated into a
        // different binary, so nothing the prewarm built was ever a cache hit.
        .rasterState = {
            .cullMode = VK_CULL_MODE_BACK_BIT,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .depthBiasEnable = VK_FALSE,
            .blendEnable = VK_FALSE,
            .depthWriteEnable = VK_TRUE,
            .alphaToCoverageEnable = VK_FALSE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .colorWriteMask = 0xf,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            .depthClamp = VK_FALSE,
            .colorTargetCount = 1,
            .colorBlendOp = BlendEquation::ADD,
            .alphaBlendOp = BlendEquation::ADD,
            .depthCompareOp = SamplerCompareFunc::GE,
            .depthBiasConstantFactor = 0.f,
            .depthBiasSlopeFactor = 0.f,
        },
        .stencilState = {},
        .layout = layout,
    };
    PipelineDynamicOptions dynamicOptions {
        .useDynamicVertexInputState = true,
        .useDynamicRenderPasses = true,
        .stereoscopicType = stereoscopicType,
        .stereoscopicViewCount = stereoscopicViewCount,
    };

    // lecodes 0028: the REAL pipelines this program will be asked for — every fresh class (the
    // draw-time keys seen so far, minus their program) with this program's shaders and layout,
    // built exactly as the draw would (static vertex input, real render pass) and adopted into
    // mPipelines by gc(). Falls back to upstream's stand-in pipeline while no class exists yet
    // (programs created before the first frame).
    bool registered = false;
    for (auto const& p : mPrograms) {
        if (p.program.get() == vprogram.get()) { registered = true; break; }
    }
    if (!registered) mPrograms.push_back({ vprogram, layout });
    size_t matched = 0;
    for (auto const& c : mClasses) {
        if (c.lastSeen + CLASS_FRESH_FLUSHES < mCurrentTime) continue;
        bool sameLayout = false;
        for (auto l : c.layouts) if (l == layout) { sameLayout = true; break; }
        if (!sameLayout) continue;
        prewarmClass(c, vprogram, layout, priority);
        ++matched;
    }
    if (matched == 0 && !mClasses.empty()) {
        // A layout no class was drawn with yet — a new KIND of material (the first particle
        // material, say). Its exact pipeline cannot be known, but the driver caches per shader
        // binary under the real render pass: measured on NVIDIA, the same program under another
        // cull / blend state costs microseconds once one real pipeline exists. So build it under
        // every fresh class; the draw's own key then hits the driver, not the compiler.
        for (auto const& c : mClasses) {
            if (c.lastSeen + CLASS_FRESH_FLUSHES < mCurrentTime) continue;
            prewarmClass(c, vprogram, layout, priority);
            ++matched;
        }
    }
    if (matched > 0) return;

    CallbackManager::Handle cmh = mCallbackManager.get();
    auto token = std::make_shared<ProgramToken>();
    // Note: we keep a ref to vprogram in this lambda so that the shader modules don't get
    // destroyed before we call createPipeline(). We can catch this in some cases, to avoid
    // compiling too many unnecessary already-destroyed-materials.
    mCompilerThreadPool.queue(
        priority, token, [this, vprogram, key, dynamicOptions, cmh, priority]() mutable {
            UTILS_UNUSED_WITHOUT_TRACING const CompilerPriorityQueue priorityQueue = priority;
            FILAMENT_TRACING_EVENT(FILAMENT_TRACING_CATEGORY_FILAMENT,
                    "asyncPrewarmCache(Job)",
                    "program", vprogram->programString.c_str_safe(),
                    "priority", static_cast<uint32_t>(priorityQueue));
            if (vprogram->isParallelCompilationCanceled()) {
                FVK_LOGD << "Skipping prewarm for a program that has been destroyed already.";
                mCallbackManager.put(cmh);
                return;
            }

            VkPipeline pipeline = createPipeline(key, dynamicOptions);
            mCallbackManager.put(cmh);
            // We don't actually need this pipeline, we just wanted to force the
            // driver to cache the pipeline's information.
            if (pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(mDevice, pipeline, VKALLOC);
                mPrewarmBuilt.fetch_add(1, std::memory_order_relaxed);
            } else {
                mPrewarmFailed.fetch_add(1, std::memory_order_relaxed);
                FVK_LOGW << "Failed to create a pipeline during prewarming, draw-time pipeline "
                            "creation may fail.";
            }
        });
}

VkPipeline VulkanPipelineCache::createPipeline(
        const PipelineKey& key, const PipelineDynamicOptions& dynamicOptions) noexcept {
    assert_invariant(key.shaders[0] && "Vertex shader is not bound.");
    assert_invariant(key.layout && "No pipeline layout specified");

    VkPipelineShaderStageCreateInfo shaderStages[SHADER_MODULE_COUNT];
    shaderStages[0] = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = key.shaders[0],
        .pName = "main",
    };
    shaderStages[1] = shaderStages[0];
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = key.shaders[1];

    bool const hasFragmentShader = shaderStages[1].module != VK_NULL_HANDLE;

    VkPipelineColorBlendAttachmentState colorBlendAttachments[MRT::MAX_SUPPORTED_RENDER_TARGET_COUNT];
    VkPipelineColorBlendStateCreateInfo colorBlendState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = key.rasterState.colorTargetCount,
        .pAttachments = colorBlendAttachments,
    };

    VkPipelineVertexInputStateCreateInfo vertexInputState;
    VkVertexInputAttributeDescription vertexAttributes[VERTEX_ATTRIBUTE_COUNT];
    VkVertexInputBindingDescription vertexBuffers[VERTEX_ATTRIBUTE_COUNT];
    if (!dynamicOptions.useDynamicVertexInputState) {
        uint32_t numVertexAttribs = 0;
        uint32_t numVertexBuffers = 0;

        for (uint32_t i = 0; i < VERTEX_ATTRIBUTE_COUNT; i++) {
            if (key.vertexAttributes[i].format > 0) {
                vertexAttributes[numVertexAttribs++] = key.vertexAttributes[i];
            }
            if (key.vertexBuffers[i].stride > 0) {
                vertexBuffers[numVertexBuffers++] = key.vertexBuffers[i];
            }
        }
        vertexInputState = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = numVertexBuffers,
            .pVertexBindingDescriptions = vertexBuffers,
            .vertexAttributeDescriptionCount = numVertexAttribs,
            .pVertexAttributeDescriptions = vertexAttributes,
        };
    }

    VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = (VkPrimitiveTopology) key.topology,
    };
    VkPipelineViewportStateCreateInfo viewportState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    constexpr size_t maxDynamicStates = 3;
    size_t numDynamicStates = 2;
    VkDynamicState enabledDynamicStates[maxDynamicStates] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    if (dynamicOptions.useDynamicVertexInputState) {
        enabledDynamicStates[numDynamicStates++] = VK_DYNAMIC_STATE_VERTEX_INPUT_EXT;
    }
    VkPipelineDynamicStateCreateInfo dynamicState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<uint32_t>(numDynamicStates),
        .pDynamicStates = enabledDynamicStates,
    };

    auto const& raster = key.rasterState;
    VkPipelineRasterizationStateCreateInfo vkRaster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = raster.depthClamp,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = raster.cullMode,
        .frontFace = raster.frontFace,
        .depthBiasEnable = raster.depthBiasEnable,
        .depthBiasConstantFactor = raster.depthBiasConstantFactor,
        .depthBiasClamp = 0.0f,
        .depthBiasSlopeFactor = raster.depthBiasSlopeFactor,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo vkMs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = (VkSampleCountFlagBits) raster.rasterizationSamples,
        .sampleShadingEnable = VK_FALSE,
        .minSampleShading = 0.0f,
        .alphaToCoverageEnable = raster.alphaToCoverageEnable,
        .alphaToOneEnable = VK_FALSE,
    };
    bool const enableDepthTest =
        raster.depthCompareOp != SamplerCompareFunc::A ||
        raster.depthWriteEnable;
    // Stencil must be enabled if we're testing OR writing to the stencil buffer.
    auto const& stencil = key.stencilState;
    bool const enableStencilTest =
        stencil.front.stencilFunc != StencilState::StencilFunction::A ||
        stencil.back.stencilFunc != StencilState::StencilFunction::A ||
        stencil.front.stencilOpDepthFail != StencilOperation::KEEP ||
        stencil.back.stencilOpDepthFail != StencilOperation::KEEP ||
        stencil.front.stencilOpStencilFail != StencilOperation::KEEP ||
        stencil.back.stencilOpStencilFail != StencilOperation::KEEP ||
        stencil.front.stencilOpDepthStencilPass != StencilOperation::KEEP ||
        stencil.back.stencilOpDepthStencilPass != StencilOperation::KEEP ||
        stencil.stencilWrite;
    VkPipelineDepthStencilStateCreateInfo vkDs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = enableDepthTest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = raster.depthWriteEnable,
        .depthCompareOp = fvkutils::getCompareOp(raster.depthCompareOp),
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = enableStencilTest ? VK_TRUE : VK_FALSE,
        .minDepthBounds = 0.0f,
        .maxDepthBounds = 0.0f,
    };
    vkDs.front = {
        .failOp = fvkutils::getStencilOp(stencil.front.stencilOpStencilFail),
        .passOp = fvkutils::getStencilOp(stencil.front.stencilOpDepthStencilPass),
        .depthFailOp = fvkutils::getStencilOp(stencil.front.stencilOpDepthFail),
        .compareOp = fvkutils::getCompareOp(stencil.front.stencilFunc),
        .compareMask = stencil.front.readMask,
        .writeMask = (uint32_t) (stencil.stencilWrite ? stencil.front.writeMask : 0u),
        .reference = (uint32_t) stencil.front.ref,
    };
    vkDs.back = {
        .failOp = fvkutils::getStencilOp(stencil.back.stencilOpStencilFail),
        .passOp = fvkutils::getStencilOp(stencil.back.stencilOpDepthStencilPass),
        .depthFailOp = fvkutils::getStencilOp(stencil.back.stencilOpDepthFail),
        .compareOp = fvkutils::getCompareOp(stencil.back.stencilFunc),
        .compareMask = stencil.back.readMask,
        .writeMask = (uint32_t) (stencil.stencilWrite ? stencil.back.writeMask : 0u),
        .reference = (uint32_t) stencil.back.ref,
    };

    VkGraphicsPipelineCreateInfo pipelineCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = hasFragmentShader ? SHADER_MODULE_COUNT : 1,
        .pStages = shaderStages,
        .pVertexInputState = dynamicOptions.useDynamicVertexInputState ? nullptr : &vertexInputState,
        .pInputAssemblyState = &inputAssemblyState,
        .pViewportState = &viewportState,
        .pRasterizationState = &vkRaster,
        .pMultisampleState = &vkMs,
        .pDepthStencilState = &vkDs,
        .pColorBlendState = &colorBlendState,
        .pDynamicState = &dynamicState,
        .layout = key.layout,
        .renderPass = key.renderPass,
        .subpass = key.subpassIndex,
    };

    // There are no color attachments if there is no bound fragment shader.  (e.g. shadow map gen)
    // TODO: This should be handled in a higher layer.
    if (!hasFragmentShader) {
        colorBlendState.attachmentCount = 0;
    } else {
        // Filament assumes consistent blend state across all color attachments.
        colorBlendAttachments[0] = {
            .blendEnable = raster.blendEnable,
            .srcColorBlendFactor = raster.srcColorBlendFactor,
            .dstColorBlendFactor = raster.dstColorBlendFactor,
            .colorBlendOp = (VkBlendOp) raster.colorBlendOp,
            .srcAlphaBlendFactor = raster.srcAlphaBlendFactor,
            .dstAlphaBlendFactor = raster.dstAlphaBlendFactor,
            .alphaBlendOp = (VkBlendOp) raster.alphaBlendOp,
            .colorWriteMask = raster.colorWriteMask,
        };
        for (uint8_t i = 1; i < colorBlendState.attachmentCount; ++i) {
            colorBlendAttachments[i] = colorBlendAttachments[0];
        }
    }

    VkPipelineRenderingCreateInfoKHR renderingInfo {};
    VkFormat pipelineRenderingColorFormats[] = {VK_FORMAT_UNDEFINED};
    if (dynamicOptions.useDynamicRenderPasses) {
        renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        renderingInfo.pNext = pipelineCreateInfo.pNext;
        // Fill values in with empty values where we do not already have values.
        renderingInfo.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
        renderingInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
        // If multiview, create a bitmask with bits representing each enabled view set to 1;
        // otherwise, set the view mask to 0.
        renderingInfo.viewMask = dynamicOptions.stereoscopicType == StereoscopicType::MULTIVIEW ?
            (1 << dynamicOptions.stereoscopicViewCount) - 1 : 0;

        if (hasFragmentShader) {
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachmentFormats = pipelineRenderingColorFormats;
        }

        pipelineCreateInfo.pNext = &renderingInfo;
    }

#if FVK_ENABLED(FVK_DEBUG_SHADER_MODULE)
    FVK_LOGD << "vkCreateGraphicsPipelines with shaders = (" << shaderStages[0].module << ", "
             << shaderStages[1].module << ")";

    VkPipelineCreationFeedback stageFeedbacks[SHADER_MODULE_COUNT] = {};
    VkPipelineCreationFeedback pipelineFeedback = {};
    VkPipelineCreationFeedbackCreateInfo feedbackInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO_EXT,
        .pNext = nullptr,
        .pPipelineCreationFeedback = &pipelineFeedback,
        .pipelineStageCreationFeedbackCount = hasFragmentShader ? SHADER_MODULE_COUNT : 1,
        .pPipelineStageCreationFeedbacks = stageFeedbacks,
    };

    if (mContext.pipelineCreationFeedbackSupported()) {
        feedbackInfo.pNext = pipelineCreateInfo.pNext;
        pipelineCreateInfo.pNext = &feedbackInfo;
    }
#endif
    VkPipeline pipeline;
    VkResult error = vkCreateGraphicsPipelines(mDevice, mPipelineCache, 1, &pipelineCreateInfo,
            VKALLOC, &pipeline);

#if FVK_ENABLED(FVK_DEBUG_SHADER_MODULE)
    FVK_LOGD << "vkCreateGraphicsPipelines with shaders = (" << shaderStages[0].module << ", "
             << shaderStages[1].module << ")";

    if (mContext.pipelineCreationFeedbackSupported()) {
        printPipelineFeedbackInfo(feedbackInfo);
    }
#endif

    assert_invariant(error == VK_SUCCESS);
    if (error != VK_SUCCESS) {
        FVK_LOGE << "vkCreateGraphicsPipelines error " << error;
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

void VulkanPipelineCache::bindProgram(fvkmemory::resource_ptr<VulkanProgram> program) noexcept {
    mPipelineRequirements.shaders[0] = program->getVertexShader();
    mPipelineRequirements.shaders[1] = program->getFragmentShader();
    mBoundProgram = program->programString;

    // If this is a debug build, validate the current shader.
#if FVK_ENABLED(FVK_DEBUG_SHADER_MODULE)
    if (mPipelineRequirements.shaders[0] == VK_NULL_HANDLE ||
            mPipelineRequirements.shaders[1] == VK_NULL_HANDLE) {
        FVK_LOGE << "Binding missing shader: " << program->name.c_str();
    }
#endif
}

void VulkanPipelineCache::bindRasterState(RasterState const& rasterState) noexcept {
    mPipelineRequirements.rasterState = rasterState;
}

void VulkanPipelineCache::bindStencilState(StencilState const& stencilState) noexcept {
    mPipelineRequirements.stencilState = stencilState;
}

void VulkanPipelineCache::bindRenderPass(
        fvkmemory::resource_ptr<VulkanRenderPass> renderPass,
        int subpassIndex) noexcept {
    mPipelineRequirements.renderPass = renderPass->getVkRenderPass();
    mPipelineRequirements.subpassIndex = subpassIndex;
}

void VulkanPipelineCache::bindPrimitiveTopology(VkPrimitiveTopology topology) noexcept {
    assert_invariant(uint32_t(topology) <= 0xffffu);
    mPipelineRequirements.topology = topology;
}

void VulkanPipelineCache::bindVertexArray(VkVertexInputAttributeDescription const* attribDesc,
        VkVertexInputBindingDescription const* bufferDesc, uint8_t count) {
    for (size_t i = 0; i < VERTEX_ATTRIBUTE_COUNT; i++) {
        if (i < count) {
            mPipelineRequirements.vertexAttributes[i] = attribDesc[i];
            mPipelineRequirements.vertexBuffers[i] = bufferDesc[i];
        } else {
            mPipelineRequirements.vertexAttributes[i] = VertexInputAttributeDescription{};
            mPipelineRequirements.vertexBuffers[i] = VertexInputBindingDescription{};
        }
    }
}

void VulkanPipelineCache::addCachePrewarmCallback(CallbackHandler* handler,
                                                  const CallbackHandler::Callback callback,
                                                  void* user) {
    if (callback) {
        mCallbackManager.setCallback(handler, callback, user);
    }
}

void VulkanPipelineCache::resetBoundPipeline() {
    mBoundPipeline = {};
}

void VulkanPipelineCache::terminate() noexcept {
    for (auto& iter : mPipelines) {
        vkDestroyPipeline(mDevice, iter.second.handle, VKALLOC);
    }
    mPipelines.clear();
    resetBoundPipeline();

    mCallbackManager.terminate();
    mCompilerThreadPool.terminate();

    mPrograms.clear();   // the registry's program refs, before the resource manager goes
    mClasses.clear();

    // Prewarmed pipelines nobody adopted (the pool finished after the last gc()).
    {
        std::lock_guard<std::mutex> const lock(mPrewarmedLock);
        for (auto const& p : mPrewarmed) vkDestroyPipeline(mDevice, p.handle, VKALLOC);
        mPrewarmed.clear();
    }

    FVK_LOGI << "VulkanPipelineCache: " << mTotalMisses << " pipelines built at draw time ("
             << mTotalMissMs << " ms), prewarmed " << mPrewarmBuilt.load() << " (adopted "
             << mPrewarmAdopted << ", failed " << mPrewarmFailed.load() << ")";

    vkDestroyPipelineCache(mDevice, mPipelineCache, VKALLOC);
}

void VulkanPipelineCache::gc() noexcept {
    // The timestamp associated with a given cache entry represents "time" as a count of flush
    // events since the cache was constructed. If any cache entry was most recently used over
    // FVK_MAX_PIPELINE_AGE flush events in the past, then we can be sure that it is no longer
    // being used by the GPU, and is therefore safe to destroy or reclaim.
    ++mCurrentTime;

    // lecodes 0028: a class not drawn for CLASS_FRESH_FLUSHES goes — its render pass may be evicted
    // by the FBO cache soon, and a prewarm job must never be handed a dead one.
    for (size_t i = 0; i < mClasses.size();) {
        if (mClasses[i].lastSeen + CLASS_FRESH_FLUSHES < mCurrentTime) {
            mClasses[i] = mClasses.back();
            mClasses.pop_back();
        } else {
            ++i;
        }
    }

    // lecodes 0028: adopt the pipelines the prewarm thread built against real classes — the draw
    // finds them in mPipelines and never builds one. A duplicate (the draw got there first) is freed.
    {
        std::vector<PrewarmedPipeline> ready;
        {
            std::lock_guard<std::mutex> const lock(mPrewarmedLock);
            ready.swap(mPrewarmed);
        }
        for (auto const& p : ready) {
            if (mPipelines.find(p.key) == mPipelines.end()) {
                mPipelines.emplace(p.key, PipelineCacheEntry{ p.handle, mCurrentTime });
                ++mPrewarmAdopted;
            } else {
                vkDestroyPipeline(mDevice, p.handle, VKALLOC);
            }
        }
    }

    // lecodes 0028: a flush that built pipelines at draw time is where a hitch comes from — say so
    // with the count and the cost (a prewarmed program's pipeline is meant to be a cache hit here).
    if (mFlushMisses > 0) {
        FVK_LOGI << "VulkanPipelineCache: flush " << mCurrentTime << " built " << mFlushMisses
                 << " pipelines at draw time in " << mFlushMissMs << " ms (prewarmed so far "
                 << mPrewarmBuilt.load() << ", adopted " << mPrewarmAdopted << ", failed "
                 << mPrewarmFailed.load() << ", classes " << mClasses.size() << ")";
        mFlushMisses = 0;
        mFlushMissMs = 0.0;
    }

    // The Vulkan spec says: "When a command buffer begins recording, all state in that command
    // buffer is undefined." Therefore, we need to clear all bindings at this time.
    resetBoundPipeline();

    // NOTE: Due to robin_map restrictions, we cannot use auto or range-based loops.

    // Evict any pipelines that have not been used in a while.
    // Any pipeline older than FVK_MAX_COMMAND_BUFFERS can be safely destroyed.
   using ConstPipeIterator = decltype(mPipelines)::const_iterator;
   for (ConstPipeIterator iter = mPipelines.begin(); iter != mPipelines.end();) {
       const PipelineCacheEntry& cacheEntry = iter.value();
       if (cacheEntry.lastUsed + FVK_MAX_PIPELINE_AGE < mCurrentTime) {
           vkDestroyPipeline(mDevice, iter->second.handle, VKALLOC);
           iter = mPipelines.erase(iter);
       } else {
           ++iter;
       }
   }
}

bool VulkanPipelineCache::PipelineEqual::operator()(const PipelineKey& k1,
        const PipelineKey& k2) const {
    return 0 == memcmp((const void*) &k1, (const void*) &k2, sizeof(k1));
}

} // namespace filament::backend

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
