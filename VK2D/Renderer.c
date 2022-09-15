/// \file Renderer.c
/// \author Paolo Mazzon
#include <vulkan/vulkan.h>
#include <SDL2/SDL_vulkan.h>
#include "VK2D/RendererMeta.h"
#include "VK2D/Renderer.h"
#include "VK2D/BuildOptions.h"
#include "VK2D/Validation.h"
#include "VK2D/Initializers.h"
#include "VK2D/Constants.h"
#include "VK2D/PhysicalDevice.h"
#include "VK2D/LogicalDevice.h"
#include "VK2D/Texture.h"
#include "VK2D/Shader.h"
#include "VK2D/Image.h"

// To set DPI awareness
#ifdef WIN32
#include <Windows.h>
#endif // WIN32

/******************************* Forward declarations *******************************/

bool _vk2dFileExists(const char *filename);
unsigned char* _vk2dLoadFile(const char *filename, uint32_t *size);

/******************************* Globals *******************************/

// For everything
VK2DRenderer gRenderer = NULL;

#ifdef VK2D_ENABLE_DEBUG
static const char* EXTENSIONS[] = {
		VK_EXT_DEBUG_REPORT_EXTENSION_NAME
};
static const char* LAYERS[] = {
		"VK_LAYER_KHRONOS_validation"
};
static const int LAYER_COUNT = 1;
static const int EXTENSION_COUNT = 1;
#else // VK2D_ENABLE_DEBUG
static const char* EXTENSIONS[] = {

};
static const char* LAYERS[] = {

};
static const int LAYER_COUNT = 0;
static const int EXTENSION_COUNT = 0;
#endif // VK2D_ENABLE_DEBUG


/******************************* User-visible functions *******************************/

int32_t vk2dRendererInit(SDL_Window *window, VK2DRendererConfig config) {
	gRenderer = calloc(1, sizeof(struct VK2DRenderer));
	int32_t errorCode = 0;
	uint32_t totalExtensionCount, i, sdlExtensions;
	const char** totalExtensions;

	// Windows 10 dpi settings don't play nice
#ifdef WIN32
	SetProcessDPIAware();
	SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
#endif // WIN32

	// Print all available layers
	VkLayerProperties *systemLayers;
	uint32_t systemLayerCount;
	vkEnumerateInstanceLayerProperties(&systemLayerCount, VK_NULL_HANDLE);
	systemLayers = malloc(sizeof(VkLayerProperties) * systemLayerCount);
	vkEnumerateInstanceLayerProperties(&systemLayerCount, systemLayers);
	vk2dLogMessage("Available layers: ");
	for (i = 0; i < systemLayerCount; i++)
		vk2dLogMessage("  - %s", systemLayers[i].layerName);
	vk2dLogMessage("");
	free(systemLayers);

	// Find number of total number of extensions
	SDL_Vulkan_GetInstanceExtensions(window, &sdlExtensions, VK_NULL_HANDLE);
	totalExtensionCount = sdlExtensions + EXTENSION_COUNT;
	totalExtensions = malloc(totalExtensionCount * sizeof(char*));

	if (vk2dPointerCheck(gRenderer)) {
		// Load extensions
		SDL_Vulkan_GetInstanceExtensions(window, &sdlExtensions, totalExtensions);
		for (i = sdlExtensions; i < totalExtensionCount; i++) totalExtensions[i] = EXTENSIONS[i - sdlExtensions];

		// Log all used extensions
		vk2dLogMessage("Vulkan Enabled Extensions: ");
		for (i = 0; i < totalExtensionCount; i++)
			vk2dLogMessage(" - %s", totalExtensions[i]);
		vk2dLogMessage(""); // Newline

		// Create instance, physical, and logical device
		VkInstanceCreateInfo instanceCreateInfo = vk2dInitInstanceCreateInfo((void*)&VK2D_DEFAULT_CONFIG, LAYERS, LAYER_COUNT, totalExtensions, totalExtensionCount);
		vkCreateInstance(&instanceCreateInfo, VK_NULL_HANDLE, &gRenderer->vk);
		gRenderer->pd = vk2dPhysicalDeviceFind(gRenderer->vk, VK2D_DEVICE_BEST_FIT);
		gRenderer->ld = vk2dLogicalDeviceCreate(gRenderer->pd, false, true);
		gRenderer->window = window;

		// Assign user settings, except for screen mode which will be handled later
		VK2DMSAA maxMSAA = vk2dPhysicalDeviceGetMSAA(gRenderer->pd);
		gRenderer->config = config;
		gRenderer->config.msaa = maxMSAA >= config.msaa ? config.msaa : maxMSAA;
		gRenderer->newConfig = gRenderer->config;

		// Create the VMA
		VmaAllocatorCreateInfo allocatorCreateInfo = {};
		allocatorCreateInfo.device = gRenderer->ld->dev;
		allocatorCreateInfo.physicalDevice = gRenderer->pd->dev;
		allocatorCreateInfo.instance = gRenderer->vk;
		allocatorCreateInfo.vulkanApiVersion = VK_MAKE_VERSION(1, 1, 0);
		vmaCreateAllocator(&allocatorCreateInfo, &gRenderer->vma);

		// Initialize subsystems
		_vk2dRendererCreateDebug();
		_vk2dRendererCreateWindowSurface();
		_vk2dRendererCreateSwapchain();
		_vk2dRendererCreateColourResources();
		_vk2dRendererCreateRenderPass();
		_vk2dRendererCreateDescriptorSetLayouts();
		_vk2dRendererCreatePipelines();
		_vk2dRendererCreateFrameBuffer();
		_vk2dRendererCreateDescriptorPool(false);
		_vk2dRendererCreateUniformBuffers(true);
		_vk2dRendererCreateSampler();
		_vk2dRendererCreateUnits();
		_vk2dRendererCreateSynchronization();

		vk2dRendererSetColourMod((void*)VK2D_DEFAULT_COLOUR_MOD);
		gRenderer->viewport.x = 0;
		gRenderer->viewport.y = 0;
		gRenderer->viewport.width = gRenderer->surfaceWidth;
		gRenderer->viewport.height = gRenderer->surfaceHeight;
		gRenderer->viewport.minDepth = 0;
		gRenderer->viewport.maxDepth = 1;
	} else {
		errorCode = -1;
	}

	return errorCode;
}

void vk2dRendererQuit() {
	if (gRenderer != NULL) {
		vkQueueWaitIdle(gRenderer->ld->queue);

		// Destroy subsystems
		_vk2dRendererDestroySynchronization();
		_vk2dRendererDestroyTargetsList();
		_vk2dRendererDestroyUnits();
		_vk2dRendererDestroySampler();
		_vk2dRendererDestroyDescriptorPool(false);
		_vk2dRendererDestroyUniformBuffers();
		_vk2dRendererDestroyFrameBuffer();
		_vk2dRendererDestroyPipelines(false);
		_vk2dRendererDestroyDescriptorSetLayout();
		_vk2dRendererDestroyRenderPass();
		_vk2dRendererDestroyColourResources();
		_vk2dRendererDestroySwapchain();
		_vk2dRendererDestroyWindowSurface();
		_vk2dRendererDestroyDebug();

		vmaDestroyAllocator(gRenderer->vma);

		// Destroy core bits
		vk2dLogicalDeviceFree(gRenderer->ld);
		vk2dPhysicalDeviceFree(gRenderer->pd);

		free(gRenderer);
		gRenderer = NULL;

		vk2dLogMessage("VK2D has been uninitialized.");
	}
}

void vk2dRendererWait() {
	if (gRenderer != NULL)
		vkQueueWaitIdle(gRenderer->ld->queue);
	else
		vk2dLogMessage("Renderer is not initialized");
}

VK2DRenderer vk2dRendererGetPointer() {
	return gRenderer;
}

void vk2dRendererResetSwapchain() {
	if (gRenderer != NULL)
		gRenderer->resetSwapchain = true;
	else
		vk2dLogMessage("Renderer is not initialized");
}

VK2DRendererConfig vk2dRendererGetConfig() {
	if (gRenderer != NULL)
		return gRenderer->config;
	else
		vk2dLogMessage("Renderer is not initialized");
	VK2DRendererConfig c = {};
	return c;
}

void vk2dRendererSetConfig(VK2DRendererConfig config) {
	if (gRenderer != NULL) {
		gRenderer->newConfig = config;
		VK2DMSAA maxMSAA = vk2dPhysicalDeviceGetMSAA(gRenderer->pd);
		gRenderer->newConfig.msaa = maxMSAA >= config.msaa ? config.msaa : maxMSAA;
		vk2dRendererResetSwapchain();
	} else {
		vk2dLogMessage("Renderer is not initialized");
	}
}

void vk2dRendererStartFrame(vec4 clearColour) {
	if (gRenderer != NULL) {
		if (!gRenderer->procedStartFrame) {
			gRenderer->procedStartFrame = true;

			/*********** Get image and synchronization ***********/

			gRenderer->previousTime = SDL_GetPerformanceCounter();

			// Wait for previous rendering to be finished
			vkWaitForFences(gRenderer->ld->dev, 1, &gRenderer->inFlightFences[gRenderer->currentFrame], VK_TRUE,
							UINT64_MAX);

			// Acquire image
			vkAcquireNextImageKHR(gRenderer->ld->dev, gRenderer->swapchain, UINT64_MAX,
								  gRenderer->imageAvailableSemaphores[gRenderer->currentFrame], VK_NULL_HANDLE,
								  &gRenderer->scImageIndex);

			if (gRenderer->imagesInFlight[gRenderer->scImageIndex] != VK_NULL_HANDLE) {
				vkWaitForFences(gRenderer->ld->dev, 1, &gRenderer->imagesInFlight[gRenderer->scImageIndex], VK_TRUE,
								UINT64_MAX);
			}
			gRenderer->imagesInFlight[gRenderer->scImageIndex] = gRenderer->inFlightFences[gRenderer->currentFrame];

			/*********** Start-of-frame tasks ***********/

			// Reset currently bound items
			_vk2dRendererResetBoundPointers();

			// Reset current render targets
			gRenderer->targetFrameBuffer = gRenderer->framebuffers[gRenderer->scImageIndex];
			gRenderer->targetRenderPass = gRenderer->renderPass;
			gRenderer->targetSubPass = 0;
			gRenderer->targetImage = gRenderer->swapchainImages[gRenderer->scImageIndex];
			gRenderer->targetUBOSet = gRenderer->cameras[0].uboSets[gRenderer->scImageIndex]; // TODO: Should prob be reworked
			gRenderer->target = VK2D_TARGET_SCREEN;

			// Flush the current ubo into its buffer for the frame
			for (int i = 0; i < VK2D_MAX_CAMERAS; i++) {
				if (gRenderer->cameras[i].state == cs_Normal) {
					_vk2dCameraUpdateUBO(&gRenderer->cameras[i].ubos[gRenderer->scImageIndex],
										 &gRenderer->cameras[i].spec);
					_vk2dRendererFlushUBOBuffer(gRenderer->scImageIndex, i);
				}
			}

			// Start the render pass
			VkCommandBufferBeginInfo beginInfo = vk2dInitCommandBufferBeginInfo(
					VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
					VK_NULL_HANDLE);
			vkResetCommandBuffer(gRenderer->commandBuffer[gRenderer->scImageIndex], 0);
			vk2dErrorCheck(vkBeginCommandBuffer(gRenderer->commandBuffer[gRenderer->scImageIndex], &beginInfo));

			// Setup render pass
			VkRect2D rect = {};
			rect.extent.width = gRenderer->surfaceWidth;
			rect.extent.height = gRenderer->surfaceHeight;
			const uint32_t clearCount = 1;
			VkClearValue clearValues[1] = {};
			clearValues[0].color.float32[0] = clearColour[0];
			clearValues[0].color.float32[1] = clearColour[1];
			clearValues[0].color.float32[2] = clearColour[2];
			clearValues[0].color.float32[3] = clearColour[3];
			VkRenderPassBeginInfo renderPassBeginInfo = vk2dInitRenderPassBeginInfo(
					gRenderer->renderPass,
					gRenderer->framebuffers[gRenderer->scImageIndex],
					rect,
					clearValues,
					clearCount);

			vkCmdBeginRenderPass(gRenderer->commandBuffer[gRenderer->scImageIndex], &renderPassBeginInfo,
								 VK_SUBPASS_CONTENTS_INLINE);
		}
	} else {
		vk2dLogMessage("Renderer is not initialized");
	}
}

void vk2dRendererEndFrame() {
	if (gRenderer != NULL) {
		if (gRenderer->procedStartFrame) {
			gRenderer->procedStartFrame = false;
			// Finish the primary command buffer, its time to PRESENT things
			vkCmdEndRenderPass(gRenderer->commandBuffer[gRenderer->scImageIndex]);
			vk2dErrorCheck(vkEndCommandBuffer(gRenderer->commandBuffer[gRenderer->scImageIndex]));

			// Wait for image before doing things
			VkPipelineStageFlags waitStage[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
			VkSubmitInfo submitInfo = vk2dInitSubmitInfo(
					&gRenderer->commandBuffer[gRenderer->scImageIndex],
					1,
					&gRenderer->renderFinishedSemaphores[gRenderer->currentFrame],
					1,
					&gRenderer->imageAvailableSemaphores[gRenderer->currentFrame],
					1,
					waitStage);

			// Submit
			vkResetFences(gRenderer->ld->dev, 1, &gRenderer->inFlightFences[gRenderer->currentFrame]);
			vk2dErrorCheck(vkQueueSubmit(gRenderer->ld->queue, 1, &submitInfo,
										 gRenderer->inFlightFences[gRenderer->currentFrame]));

			// Final present info bit
			VkResult result;
			VkPresentInfoKHR presentInfo = vk2dInitPresentInfoKHR(&gRenderer->swapchain, 1, &gRenderer->scImageIndex,
																  &result,
																  &gRenderer->renderFinishedSemaphores[gRenderer->currentFrame],
																  1);
			VkResult queueRes = vkQueuePresentKHR(gRenderer->ld->queue, &presentInfo);
			if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || gRenderer->resetSwapchain ||
				queueRes == VK_ERROR_OUT_OF_DATE_KHR) {
				_vk2dRendererResetSwapchain();
				gRenderer->resetSwapchain = false;
			} else {
				vk2dErrorCheck(result);
				vk2dErrorCheck(queueRes);
			}

			gRenderer->currentFrame = (gRenderer->currentFrame + 1) % VK2D_MAX_FRAMES_IN_FLIGHT;

			// Calculate time
			gRenderer->accumulatedTime += (((double) SDL_GetPerformanceCounter() - gRenderer->previousTime) /
										   (double) SDL_GetPerformanceFrequency()) * 1000;
			gRenderer->amountOfFrames++;
			if (gRenderer->accumulatedTime >= 1000) {
				gRenderer->frameTimeAverage = gRenderer->accumulatedTime / gRenderer->amountOfFrames;
				gRenderer->accumulatedTime = 0;
				gRenderer->amountOfFrames = 0;
			}
		}
	} else {
		vk2dLogMessage("Renderer is not initialized");
	}
}

VK2DLogicalDevice vk2dRendererGetDevice() {
	if (gRenderer != NULL)
		return gRenderer->ld;
	else
		vk2dLogMessage("Renderer is not initialized");
	return NULL;
}

void vk2dRendererSetTarget(VK2DTexture target) {
	if (gRenderer != NULL) {
		if (target != gRenderer->target) {
			gRenderer->target = target;
			// Figure out which render pass to use
			VkRenderPass pass = target == VK2D_TARGET_SCREEN ? gRenderer->midFrameSwapRenderPass
															 : gRenderer->externalTargetRenderPass;
			VkFramebuffer framebuffer =
					target == VK2D_TARGET_SCREEN ? gRenderer->framebuffers[gRenderer->scImageIndex] : target->fbo;
			VkImage image = target == VK2D_TARGET_SCREEN ? gRenderer->swapchainImages[gRenderer->scImageIndex]
														 : target->img->img;
			VkDescriptorSet buffer =
					target == VK2D_TARGET_SCREEN ? gRenderer->cameras[0].uboSets[gRenderer->scImageIndex]
												 : target->uboSet;

			vkCmdEndRenderPass(gRenderer->commandBuffer[gRenderer->scImageIndex]);

			// Now we either have to transition the image layout depending on whats going in and whats poppin out
			if (target == VK2D_TARGET_SCREEN)
				_vk2dTransitionImageLayout(gRenderer->targetImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
										   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			else
				_vk2dTransitionImageLayout(target->img->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
										   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

			// Assign new render targets
			gRenderer->targetRenderPass = pass;
			gRenderer->targetFrameBuffer = framebuffer;
			gRenderer->targetImage = image;
			gRenderer->targetUBOSet = buffer;

			// Setup new render pass
			VkRect2D rect = {};
			rect.extent.width = target == VK2D_TARGET_SCREEN ? gRenderer->surfaceWidth : target->img->width;
			rect.extent.height = target == VK2D_TARGET_SCREEN ? gRenderer->surfaceHeight : target->img->height;
			VkRenderPassBeginInfo renderPassBeginInfo = vk2dInitRenderPassBeginInfo(
					pass,
					framebuffer,
					rect,
					VK_NULL_HANDLE,
					0);

			vkCmdBeginRenderPass(gRenderer->commandBuffer[gRenderer->scImageIndex], &renderPassBeginInfo,
								 VK_SUBPASS_CONTENTS_INLINE);

			_vk2dRendererResetBoundPointers();
		}
	} else {
		vk2dLogMessage("Renderer is not initialized");
	}
}

void vk2dRendererSetColourMod(const vec4 mod) {
	if (gRenderer != NULL) {
		gRenderer->colourBlend[0] = mod[0];
		gRenderer->colourBlend[1] = mod[1];
		gRenderer->colourBlend[2] = mod[2];
		gRenderer->colourBlend[3] = mod[3];
	} else {
		vk2dLogMessage("Renderer is not initialized");
	}
}

void vk2dRendererGetColourMod(vec4 dst) {
	if (gRenderer != NULL) {
		dst[0] = gRenderer->colourBlend[0];
		dst[1] = gRenderer->colourBlend[1];
		dst[2] = gRenderer->colourBlend[2];
		dst[3] = gRenderer->colourBlend[3];
	} else {
		vk2dLogMessage("Renderer is not initialized");
	}
}

void vk2dRendererSetBlendMode(VK2DBlendMode blendMode) {
	if (gRenderer != NULL)
		gRenderer->blendMode = blendMode;
	else
		vk2dLogMessage("Renderer is not initialized");
}

VK2DBlendMode vk2dRendererGetBlendMode() {
	if (gRenderer != NULL)
		return gRenderer->blendMode;
	else
		vk2dLogMessage("Renderer is not initialized");
	return bm_None;
}

void vk2dRendererSetCamera(VK2DCameraSpec camera) {
	if (gRenderer != NULL) {
		gRenderer->cameras[VK2D_DEFAULT_CAMERA].spec = camera;
		gRenderer->cameras[VK2D_DEFAULT_CAMERA].spec.wOnScreen = gRenderer->surfaceWidth;
		gRenderer->cameras[VK2D_DEFAULT_CAMERA].spec.hOnScreen = gRenderer->surfaceHeight;
		gRenderer->cameras[VK2D_DEFAULT_CAMERA].spec.xOnScreen = 0;
		gRenderer->cameras[VK2D_DEFAULT_CAMERA].spec.yOnScreen = 0;
	} else {
		vk2dLogMessage("Renderer is not initialized");
	}
}

VK2DCameraSpec vk2dRendererGetCamera() {
	if (gRenderer != NULL)
		return gRenderer->defaultCameraSpec;
	else
		vk2dLogMessage("Renderer is not initialized");
	VK2DCameraSpec c = {};
	return c;
}

void vk2dRendererSetViewport(float x, float y, float w, float h) {
	if (gRenderer != NULL) {
		gRenderer->viewport.x = x;
		gRenderer->viewport.y = y;
		gRenderer->viewport.width = w;
		gRenderer->viewport.height = h;
	} else {
		vk2dLogMessage("Renderer is not initialized");
	}
}

void vk2dRendererGetViewport(float *x, float *y, float *w, float *h) {
	if (gRenderer != NULL) {
		*x = gRenderer->viewport.x;
		*y = gRenderer->viewport.y;
		*w = gRenderer->viewport.width;
		*h = gRenderer->viewport.height;
	} else {
		vk2dLogMessage("Renderer is not initialized");
	}
}

void vk2dRendererSetTextureCamera(bool useCameraOnTextures) {
	if (gRenderer != NULL)
		gRenderer->enableTextureCameraUBO = useCameraOnTextures;
	else
		vk2dLogMessage("Renderer is not initialized");
}

void vk2dRendererLockCameras(VK2DCameraIndex cam) {
	if (gRenderer != NULL)
		gRenderer->cameraLocked = cam;
	else
		vk2dLogMessage("Renderer is not initialized");
}

void vk2dRendererUnlockCameras() {
	if (gRenderer != NULL)
		gRenderer->cameraLocked = VK2D_INVALID_CAMERA;
	else
		vk2dLogMessage("Renderer is not initialized");
}

double vk2dRendererGetAverageFrameTime() {
	if (gRenderer != NULL)
		return gRenderer->frameTimeAverage;
	else
		vk2dLogMessage("Renderer is not initialized");
	return 0;
}

void vk2dRendererClear() {
	if (gRenderer != NULL) {
		VkDescriptorSet set = gRenderer->unitUBOSet;
		_vk2dRendererDrawRaw(&set, 1, gRenderer->unitSquare, gRenderer->primFillPipe, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0,
							 0, VK2D_INVALID_CAMERA);
	} else {
		vk2dLogMessage("Renderer is not initialized");
	}
}

void vk2dRendererDrawRectangle(float x, float y, float w, float h, float r, float ox, float oy) {
	if (gRenderer != NULL) {
#ifdef VK2D_UNIT_GENERATION
		vk2dRendererDrawPolygon(gRenderer->unitSquare, x, y, true, 1, w, h, r, ox / w, oy / h);
#endif // VK2D_UNIT_GENERATION
	} else {
		vk2dLogMessage("Renderer is not initialized");
	}
}

void vk2dRendererDrawRectangleOutline(float x, float y, float w, float h, float r, float ox, float oy, float lineWidth) {
	if (gRenderer != NULL) {
#ifdef VK2D_UNIT_GENERATION
		vk2dRendererDrawPolygon(gRenderer->unitSquareOutline, x, y, false, lineWidth, w, h, r, ox / w, oy / h);
#endif //  VK2D_UNIT_GENERATION
	} else {
		vk2dLogMessage("Renderer is not initialized");
	}
}

void vk2dRendererDrawCircle(float x, float y, float r) {
	if (gRenderer != NULL) {
#ifdef VK2D_UNIT_GENERATION
		vk2dRendererDrawPolygon(gRenderer->unitCircle, x, y, true, 1, r * 2, r * 2, 0, 0, 0);
#endif //  VK2D_UNIT_GENERATION
	} else {
		vk2dLogMessage("Renderer is not initialized");
	}
}

void vk2dRendererDrawCircleOutline(float x, float y, float r, float lineWidth) {
	if (gRenderer != NULL) {
#ifdef VK2D_UNIT_GENERATION
		vk2dRendererDrawPolygon(gRenderer->unitCircleOutline, x, y, false, lineWidth, r * 2, r * 2, 0, 0, 0);
#endif //  VK2D_UNIT_GENERATION
	} else {
		vk2dLogMessage("Renderer is not initialized");
	}
}

void vk2dRendererDrawLine(float x1, float y1, float x2, float y2) {
	if (gRenderer != NULL) {
#ifdef VK2D_UNIT_GENERATION
		float x = sqrtf(powf(y2 - y1, 2) + powf(x2 - x1, 2));
		float r = atan2f(y2 - y1, x2 - x1);
		vk2dRendererDrawPolygon(gRenderer->unitLine, x1, y1, false, 1, x, 1, r, 0, 0);
#endif //  VK2D_UNIT_GENERATION
	} else {
		vk2dLogMessage("Renderer is not initialized");
	}
}

void vk2dRendererDrawShader(VK2DShader shader, VK2DTexture tex, float x, float y, float xscale, float yscale, float rot, float originX, float originY, float xInTex, float yInTex, float texWidth, float texHeight) {
	if (gRenderer != NULL) {
		VkDescriptorSet sets[4];
		sets[1] = gRenderer->samplerSet;
		sets[2] = tex->img->set;
		sets[3] = shader->sets[shader->currentUniform];

		uint32_t setCount = shader->uniformSize == 0 ? 3 : 4;
		_vk2dRendererDraw(sets, setCount, NULL, shader->pipe, x, y, xscale, yscale, rot, originX, originY, 1, xInTex,
						  yInTex, texWidth, texHeight);
	} else {
		vk2dLogMessage("Renderer is not initialized");
	}
}

void vk2dRendererDrawTexture(VK2DTexture tex, float x, float y, float xscale, float yscale, float rot, float originX, float originY, float xInTex, float yInTex, float texWidth, float texHeight) {
	if (gRenderer != NULL) {
		VkDescriptorSet sets[3];
		sets[1] = gRenderer->samplerSet;
		sets[2] = tex->img->set;
		_vk2dRendererDraw(sets, 3, NULL, gRenderer->texPipe, x, y, xscale, yscale, rot, originX, originY, 1, xInTex,
						  yInTex, texWidth, texHeight);
	} else {
		vk2dLogMessage("Renderer is not initialized");
	}
}

void vk2dRendererDrawPolygon(VK2DPolygon polygon, float x, float y, bool filled, float lineWidth, float xscale, float yscale, float rot, float originX, float originY) {
	if (gRenderer != NULL) {
		VkDescriptorSet set;
		_vk2dRendererDraw(&set, 1, polygon, filled ? gRenderer->primFillPipe : gRenderer->primLinePipe, x, y, xscale,
						  yscale, rot, originX, originY, lineWidth, 0, 0, 0, 0);
	} else {
		vk2dLogMessage("Renderer is not initialized");
	}
}
