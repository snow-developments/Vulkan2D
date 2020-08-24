/// \file Renderer.c
/// \author Paolo Mazzon
#include <vulkan/vulkan.h>
#include <SDL2/SDL_vulkan.h>
#include "VK2D/Renderer.h"
#include "VK2D/BuildOptions.h"
#include "VK2D/Validation.h"
#include "VK2D/Initializers.h"
#include "VK2D/Constants.h"
#include "VK2D/PhysicalDevice.h"
#include "VK2D/LogicalDevice.h"
#include "VK2D/Image.h"
#include "VK2D/Texture.h"
#include "VK2D/Pipeline.h"
#include "VK2D/Blobs.h"
#include "VK2D/Buffer.h"
#include "VK2D/DescriptorControl.h"
#include "VK2D/Polygon.h"
#include "VK2D/Math.h"

/******************************* Globals *******************************/

// For debugging
PFN_vkCreateDebugReportCallbackEXT fvkCreateDebugReportCallbackEXT;
PFN_vkDestroyDebugReportCallbackEXT fvkDestroyDebugReportCallbackEXT;

// For everything
VK2DRenderer gRenderer = NULL;

// For testing purposes
VK2DBuffer gTestUBO = NULL;

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

/******************************* Internal functions *******************************/

static void _vk2dRendererCreateDemos() {
#ifdef VK2D_ENABLE_DEBUG
	VK2DUniformBufferObject ubo;
	identityMatrix(ubo.model);

	vec3 turnAxis = {0, 0, 1};
	rotateMatrix(ubo.model, turnAxis, 0);

	vec3 eyes = {0, 0, 2};
	vec3 center = {0, 0, 0};
	vec3 up = {0, 1, 0};
	cameraMatrix(ubo.view, eyes, center, up);

	orthographicMatrix(ubo.proj, 2, gRenderer->surfaceWidth / gRenderer->surfaceHeight, 0.1, 10);
	gTestUBO = vk2dBufferLoad(sizeof(VK2DUniformBufferObject), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, gRenderer->ld, &ubo);
#endif //VK2D_ENABLE_DEBUG
}

static void _vk2dRendererDestroyDemos() {
#ifdef VK2D_ENABLE_DEBUG
	vk2dBufferFree(gTestUBO);
#endif //VK2D_ENABLE_DEBUG
}

static VkCommandBuffer _vk2dRendererGetNextCommandBuffer() {
	if (gRenderer->drawListSize[gRenderer->drawCommandPool] == gRenderer->drawCommandBuffers[gRenderer->drawCommandPool]) {
		VkCommandBuffer *newList = realloc(gRenderer->draws[gRenderer->drawCommandPool], (gRenderer->drawListSize[gRenderer->drawCommandPool] + VK2D_DEFAULT_ARRAY_EXTENSION) * sizeof(VkCommandBuffer*));
		if (vk2dPointerCheck(newList)) {
			gRenderer->draws[gRenderer->drawCommandPool] = newList;
			gRenderer->drawListSize[gRenderer->drawCommandPool] += VK2D_DEFAULT_ARRAY_EXTENSION;
			vk2dLogicalDeviceGetCommandBuffers(gRenderer->ld, gRenderer->drawCommandPool, false, VK2D_DEFAULT_ARRAY_EXTENSION, &gRenderer->draws[gRenderer->drawCommandPool][gRenderer->drawCommandBuffers[gRenderer->drawCommandPool]]);
		}
	}

	// Just get the next command buffer in the list
	VkCommandBuffer out = gRenderer->draws[gRenderer->drawCommandPool][gRenderer->drawCommandBuffers[gRenderer->drawCommandPool]];
	gRenderer->drawCommandBuffers[gRenderer->drawCommandPool]++;
	return out;
}

// Ends the render pass in the current primary buffer
static void _vk2dRendererEndRenderPass() {
	if (gRenderer->drawCommandBuffers[gRenderer->drawCommandPool] > 0)
		vkCmdExecuteCommands(
				gRenderer->primaryBuffer[gRenderer->drawCommandPool],
				gRenderer->drawCommandBuffers[gRenderer->drawCommandPool] - gRenderer->drawOffset,
				&gRenderer->draws[gRenderer->drawCommandPool][gRenderer->drawOffset]);
	vkCmdEndRenderPass(gRenderer->primaryBuffer[gRenderer->drawCommandPool]);
}

static void _vk2dRendererCreateDebug() {
#ifdef VK2D_ENABLE_DEBUG
	fvkCreateDebugReportCallbackEXT = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(gRenderer->vk, "vkCreateDebugReportCallbackEXT");
	fvkDestroyDebugReportCallbackEXT = (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(gRenderer->vk, "vkDestroyDebugReportCallbackEXT");

	if (vk2dPointerCheck(fvkCreateDebugReportCallbackEXT) && vk2dPointerCheck(fvkDestroyDebugReportCallbackEXT)) {
		VkDebugReportCallbackCreateInfoEXT callbackCreateInfoEXT = vk2dInitDebugReportCallbackCreateInfoEXT(_vk2dDebugCallback);
		fvkCreateDebugReportCallbackEXT(gRenderer->vk, &callbackCreateInfoEXT, VK_NULL_HANDLE, &gRenderer->dr);
	}
#endif // VK2D_ENABLE_DEBUG
}

static void _vk2dRendererDestroyDebug() {
#ifdef VK2D_ENABLE_DEBUG
	fvkDestroyDebugReportCallbackEXT(gRenderer->vk, gRenderer->dr, VK_NULL_HANDLE);
#endif // VK2D_ENABLE_DEBUG
}

// Grabs a preferred present mode if available returning FIFO if its unavailable
static VkPresentModeKHR _vk2dRendererGetPresentMode(VkPresentModeKHR mode) {
	uint32_t i;
	for (i = 0; i < gRenderer->presentModeCount; i++)
		if (gRenderer->presentModes[i] == mode)
			return mode;
	return VK_PRESENT_MODE_FIFO_KHR;
}

static void _vk2dRendererGetSurfaceSize() {
	if (gRenderer->surfaceCapabilities.currentExtent.width == UINT32_MAX || gRenderer->surfaceCapabilities.currentExtent.height == UINT32_MAX) {
		SDL_Vulkan_GetDrawableSize(gRenderer->window, (void*)&gRenderer->surfaceWidth, (void*)&gRenderer->surfaceHeight);
	} else {
		gRenderer->surfaceWidth = gRenderer->surfaceCapabilities.currentExtent.width;
		gRenderer->surfaceHeight = gRenderer->surfaceCapabilities.currentExtent.height;
	}
}

static void _vk2dRendererCreateWindowSurface() {
	// Create the surface then load up surface relevant values
	vk2dErrorCheck(SDL_Vulkan_CreateSurface(gRenderer->window, gRenderer->vk, &gRenderer->surface) == SDL_TRUE ? VK_SUCCESS : -1);
	vk2dErrorCheck(vkGetPhysicalDeviceSurfacePresentModesKHR(gRenderer->pd->dev, gRenderer->surface, &gRenderer->presentModeCount, VK_NULL_HANDLE));
	gRenderer->presentModes = malloc(sizeof(VkPresentModeKHR) * gRenderer->presentModeCount);

	if (vk2dPointerCheck(gRenderer->presentModes)) {
		vk2dErrorCheck(vkGetPhysicalDeviceSurfacePresentModesKHR(gRenderer->pd->dev, gRenderer->surface, &gRenderer->presentModeCount, gRenderer->presentModes));
		vk2dErrorCheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gRenderer->pd->dev, gRenderer->surface, &gRenderer->surfaceCapabilities));
		// You may want to search for a different format, but according to the Vulkan hardware database, 100% of systems support VK_FORMAT_B8G8R8A8_SRGB
		gRenderer->surfaceFormat.format = VK_FORMAT_B8G8R8A8_SRGB;
		gRenderer->surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		_vk2dRendererGetSurfaceSize();
	}
}

static void _vk2dRendererDestroyWindowSurface() {
	vkDestroySurfaceKHR(gRenderer->vk, gRenderer->surface, VK_NULL_HANDLE);
	free(gRenderer->presentModes);
}

static void _vk2dRendererCreateSwapchain() {
	uint32_t i;

	gRenderer->config.screenMode = (VK2DScreenMode)_vk2dRendererGetPresentMode((VkPresentModeKHR)gRenderer->config.screenMode);
	VkSwapchainCreateInfoKHR  swapchainCreateInfoKHR = vk2dInitSwapchainCreateInfoKHR(
			gRenderer->surface,
			gRenderer->surfaceCapabilities,
			gRenderer->surfaceFormat,
			gRenderer->surfaceWidth,
			gRenderer->surfaceHeight,
			(VkPresentModeKHR)gRenderer->config.screenMode,
			VK_NULL_HANDLE,
			gRenderer->surfaceCapabilities.minImageCount + (gRenderer->config.screenMode == sm_TripleBuffer ? 1 : 0)
			);
	VkBool32 supported;
	vkGetPhysicalDeviceSurfaceSupportKHR(gRenderer->pd->dev, gRenderer->pd->QueueFamily.graphicsFamily, gRenderer->surface, &supported);
	if (vk2dErrorInline(supported != VK_TRUE ? -1 : VK_SUCCESS))
		vkCreateSwapchainKHR(gRenderer->ld->dev, &swapchainCreateInfoKHR, VK_NULL_HANDLE, &gRenderer->swapchain);

	vk2dErrorCheck(vkGetSwapchainImagesKHR(gRenderer->ld->dev, gRenderer->swapchain, &gRenderer->swapchainImageCount, VK_NULL_HANDLE));
	gRenderer->swapchainImageViews = malloc(gRenderer->swapchainImageCount * sizeof(VkImageView));
	gRenderer->swapchainImages = malloc(gRenderer->swapchainImageCount * sizeof(VkImage));
	if (vk2dPointerCheck(gRenderer->swapchainImageViews) && vk2dPointerCheck(gRenderer->swapchainImages)) {
		vk2dErrorCheck(vkGetSwapchainImagesKHR(gRenderer->ld->dev, gRenderer->swapchain, &gRenderer->swapchainImageCount, gRenderer->swapchainImages));

		for (i = 0; i < gRenderer->swapchainImageCount; i++) {
			VkImageViewCreateInfo imageViewCreateInfo = vk2dInitImageViewCreateInfo(gRenderer->swapchainImages[i], gRenderer->surfaceFormat.format, VK_IMAGE_ASPECT_COLOR_BIT, 1);
			vk2dErrorCheck(vkCreateImageView(gRenderer->ld->dev, &imageViewCreateInfo, VK_NULL_HANDLE, &gRenderer->swapchainImageViews[i]));
		}
	}

	vk2dLogMessage("Swapchain (%i images) initialized...", swapchainCreateInfoKHR.minImageCount);
}

static void _vk2dRendererDestroySwapchain() {
	uint32_t i;
	for (i = 0; i < gRenderer->swapchainImageCount; i++)
		vkDestroyImageView(gRenderer->ld->dev, gRenderer->swapchainImageViews[i], VK_NULL_HANDLE);

	vkDestroySwapchainKHR(gRenderer->ld->dev, gRenderer->swapchain, VK_NULL_HANDLE);
}

static void _vk2dRendererCreateColourResources() {
	if (gRenderer->config.msaa != msaa_1x) {
		gRenderer->msaaImage = vk2dImageCreate(
				gRenderer->ld,
				gRenderer->surfaceWidth,
				gRenderer->surfaceHeight,
				gRenderer->surfaceFormat.format,
				VK_IMAGE_ASPECT_COLOR_BIT,
				VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
				(VkSampleCountFlagBits) gRenderer->config.msaa);
		vk2dLogMessage("Colour resources initialized...");
	} else {
		vk2dLogMessage("Colour resources not enabled...");
	}
}

static void _vk2dRendererDestroyColourResources() {
	if (gRenderer->msaaImage != NULL)
		vk2dImageFree(gRenderer->msaaImage);
	gRenderer->msaaImage = NULL;
}

static void _vk2dRendererCreateDepthStencilImage() {
	// First order of business - find a good stencil format
	VkFormat formatAttempts[] = {
			VK_FORMAT_D32_SFLOAT,
			VK_FORMAT_D32_SFLOAT_S8_UINT,
			VK_FORMAT_D24_UNORM_S8_UINT
	};
	const uint32_t count = 3;
	uint32_t i;
	gRenderer->dsiAvailable = false;

	for (i = 0; i < count && !gRenderer->dsiAvailable; i++) {
		VkFormatProperties formatProperties;
		vkGetPhysicalDeviceFormatProperties(gRenderer->pd->dev, formatAttempts[i], &formatProperties);

		if (formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
			gRenderer->dsiFormat = formatAttempts[i];
			gRenderer->dsiAvailable = true;
		}
	}
	if (vk2dErrorInline(gRenderer->dsiAvailable == false ? -1 : 0)) {
		// Create the image itself
		gRenderer->dsi = vk2dImageCreate(
				gRenderer->ld,
				gRenderer->surfaceWidth,
				gRenderer->surfaceHeight,
				gRenderer->dsiFormat,
				VK_IMAGE_ASPECT_DEPTH_BIT,
				VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
				(VkSampleCountFlagBits)gRenderer->config.msaa);
		vk2dLogMessage("Depth stencil image initialized...");
	} else {
		vk2dLogMessage("Depth stencil image unavailable...");
	}
}

static void _vk2dRendererDestroyDepthStencilImage() {
	if (gRenderer->dsiAvailable)
		vk2dImageFree(gRenderer->dsi);
}

static void _vk2dRendererCreateRenderPass() {
	uint32_t attachCount;
	if (gRenderer->config.msaa != 1) {
		attachCount = 3; // Depth, colour, resolve
	} else {
		attachCount = 2; // Depth, colour
	}
	VkAttachmentReference resolveAttachment;
	VkAttachmentDescription attachments[attachCount];
	memset(attachments, 0, sizeof(VkAttachmentDescription) * attachCount);
	attachments[0].format = gRenderer->dsiFormat;
	attachments[0].samples = (VkSampleCountFlagBits)gRenderer->config.msaa;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	attachments[1].format = gRenderer->surfaceFormat.format;
	attachments[1].samples = (VkSampleCountFlagBits)gRenderer->config.msaa;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout = gRenderer->config.msaa > 1 ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	if (gRenderer->config.msaa != 1) {
		attachments[2].format = gRenderer->surfaceFormat.format;
		attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachments[2].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		resolveAttachment.attachment = 2;
		resolveAttachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}

	// Set up subpass color attachment
	const uint32_t colourAttachCount = 1;
	VkAttachmentReference subpassColourAttachments0[colourAttachCount];
	uint32_t i;
	for (i = 0; i < colourAttachCount; i++) {
		subpassColourAttachments0[i].attachment = 1;
		subpassColourAttachments0[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}

	VkAttachmentReference subpassDepthStencilAttachment0 = {};
	subpassDepthStencilAttachment0.attachment = 0;
	subpassDepthStencilAttachment0.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	// Set up subpass
	const uint32_t subpassCount = 1;
	VkSubpassDescription subpasses[1] = {};
	subpasses[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpasses[0].colorAttachmentCount = colourAttachCount;
	subpasses[0].pColorAttachments = subpassColourAttachments0;
	subpasses[0].pDepthStencilAttachment = &subpassDepthStencilAttachment0;
	subpasses[0].pResolveAttachments = gRenderer->config.msaa > 1 ? &resolveAttachment : VK_NULL_HANDLE;

	// Subpass dependency
	VkSubpassDependency subpassDependency = {};
	subpassDependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	subpassDependency.dstSubpass = 0;
	subpassDependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	subpassDependency.srcAccessMask = 0;
	subpassDependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	subpassDependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	VkRenderPassCreateInfo renderPassCreateInfo = vk2dInitRenderPassCreateInfo(attachments, attachCount, subpasses, subpassCount, &subpassDependency, 1);
	vk2dErrorCheck(vkCreateRenderPass(gRenderer->ld->dev, &renderPassCreateInfo, VK_NULL_HANDLE, &gRenderer->renderPass));

	// Two more render passes for mid-frame render pass reset to swapchain and rendering to textures
	if (gRenderer->config.msaa != 1) {
		attachments[1].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachments[1].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachments[2].initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		attachments[2].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	} else {
		attachments[1].initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		attachments[1].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	}
	vk2dErrorCheck(vkCreateRenderPass(gRenderer->ld->dev, &renderPassCreateInfo, VK_NULL_HANDLE, &gRenderer->midFrameSwapRenderPass));

	if (gRenderer->config.msaa != 1) {
		attachments[1].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachments[1].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachments[2].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachments[2].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	} else {
		attachments[1].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachments[1].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}
	vk2dErrorCheck(vkCreateRenderPass(gRenderer->ld->dev, &renderPassCreateInfo, VK_NULL_HANDLE, &gRenderer->externalTargetRenderPass));

	vk2dLogMessage("Render pass initialized...");
}

static void _vk2dRendererDestroyRenderPass() {
	vkDestroyRenderPass(gRenderer->ld->dev, gRenderer->renderPass, VK_NULL_HANDLE);
	vkDestroyRenderPass(gRenderer->ld->dev, gRenderer->externalTargetRenderPass, VK_NULL_HANDLE);
	vkDestroyRenderPass(gRenderer->ld->dev, gRenderer->midFrameSwapRenderPass, VK_NULL_HANDLE);
}

static void _vk2dRendererCreateDescriptorSetLayout() {
	// For textures
	const uint32_t layoutCount = 2;
	VkDescriptorSetLayoutBinding descriptorSetLayoutBinding[layoutCount];
	descriptorSetLayoutBinding[0] = vk2dInitDescriptorSetLayoutBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, VK_NULL_HANDLE);
	descriptorSetLayoutBinding[1] = vk2dInitDescriptorSetLayoutBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, VK_NULL_HANDLE);
	VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo = vk2dInitDescriptorSetLayoutCreateInfo(descriptorSetLayoutBinding, layoutCount);
	vk2dErrorCheck(vkCreateDescriptorSetLayout(gRenderer->ld->dev, &descriptorSetLayoutCreateInfo, VK_NULL_HANDLE, &gRenderer->duslt));

	// For shapes
	const uint32_t shapeLayoutCount = 1;
	VkDescriptorSetLayoutBinding descriptorSetLayoutBindingShapes[shapeLayoutCount];
	descriptorSetLayoutBindingShapes[0] = vk2dInitDescriptorSetLayoutBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, VK_NULL_HANDLE);
	VkDescriptorSetLayoutCreateInfo shapesDescriptorSetLayoutCreateInfo = vk2dInitDescriptorSetLayoutCreateInfo(descriptorSetLayoutBindingShapes, shapeLayoutCount);
	vk2dErrorCheck(vkCreateDescriptorSetLayout(gRenderer->ld->dev, &shapesDescriptorSetLayoutCreateInfo, VK_NULL_HANDLE, &gRenderer->dusls));

	vk2dLogMessage("Descriptor set layout initialized...");
}

static void _vk2dRendererDestroyDescriptorSetLayout() {
	vkDestroyDescriptorSetLayout(gRenderer->ld->dev, gRenderer->duslt, VK_NULL_HANDLE);
	vkDestroyDescriptorSetLayout(gRenderer->ld->dev, gRenderer->dusls, VK_NULL_HANDLE);
}

VkPipelineVertexInputStateCreateInfo _vk2dGetTextureVertexInputState();
VkPipelineVertexInputStateCreateInfo _vk2dGetColourVertexInputState();
static void _vk2dRendererCreatePipelines() {
	uint32_t i;
	VkPipelineVertexInputStateCreateInfo textureVertexInfo = _vk2dGetTextureVertexInputState();
	VkPipelineVertexInputStateCreateInfo colourVertexInfo = _vk2dGetColourVertexInputState();

	// Texture pipeline
	gRenderer->texPipe = vk2dPipelineCreate(
			gRenderer->ld,
			gRenderer->renderPass,
			gRenderer->surfaceWidth,
			gRenderer->surfaceHeight,
			(void*)VK2DVertTex,
			sizeof(VK2DVertTex),
			(void*)VK2DFragTex,
			sizeof(VK2DFragTex),
			gRenderer->duslt,
			&textureVertexInfo,
			true,
			gRenderer->config.msaa);

	// Polygon pipelines
	gRenderer->primFillPipe = vk2dPipelineCreate(
			gRenderer->ld,
			gRenderer->renderPass,
			gRenderer->surfaceWidth,
			gRenderer->surfaceHeight,
			(void*)VK2DVertColour,
			sizeof(VK2DVertColour),
			(void*)VK2DFragColour,
			sizeof(VK2DFragColour),
			gRenderer->dusls,
			&colourVertexInfo,
			true,
			gRenderer->config.msaa);
	gRenderer->primLinePipe = vk2dPipelineCreate(
			gRenderer->ld,
			gRenderer->renderPass,
			gRenderer->surfaceWidth,
			gRenderer->surfaceHeight,
			(void*)VK2DVertColour,
			sizeof(VK2DVertColour),
			(void*)VK2DFragColour,
			sizeof(VK2DFragColour),
			gRenderer->dusls,
			&colourVertexInfo,
			false,
			gRenderer->config.msaa);

	if (gRenderer->customPipeInfo != NULL) {
		for (i = 0; i < gRenderer->pipeCount; i++)
			gRenderer->customPipes[i] = vk2dPipelineCreate(
					gRenderer->ld,
					gRenderer->renderPass,
					gRenderer->surfaceWidth,
					gRenderer->surfaceWidth,
					gRenderer->customPipeInfo[i].vertBuffer,
					gRenderer->customPipeInfo[i].vertBufferSize,
					gRenderer->customPipeInfo[i].fragBuffer,
					gRenderer->customPipeInfo[i].fragBufferSize,
					gRenderer->customPipeInfo[i].descriptorSetLayout,
					&gRenderer->customPipeInfo[i].vertexInfo,
					gRenderer->customPipeInfo[i].fill,
					gRenderer->config.msaa
					);
	}

	vk2dLogMessage("Pipelines initialized...");
}

static void _vk2dRendererDestroyPipelines(bool preserveCustomPipes) {
	uint32_t i;
	vk2dPipelineFree(gRenderer->primLinePipe);
	vk2dPipelineFree(gRenderer->primFillPipe);
	vk2dPipelineFree(gRenderer->texPipe);

	for (i = 0; i < gRenderer->pipeCount; i++)
		vk2dPipelineFree(gRenderer->customPipes[i]);

	if (!preserveCustomPipes) {
		for (i = 0; i < gRenderer->pipeCount; i++) {
			free(gRenderer->customPipeInfo[i].fragBuffer);
			free(gRenderer->customPipeInfo[i].vertBuffer);
		}
		free(gRenderer->customPipeInfo);
		free(gRenderer->customPipes);
	}
}

static void _vk2dRendererCreateFrameBuffer() {
	uint32_t i;
	gRenderer->framebuffers = malloc(sizeof(VkFramebuffer) * gRenderer->swapchainImageCount);

	if (vk2dPointerCheck(gRenderer->framebuffers)) {
		for (i = 0; i < gRenderer->swapchainImageCount; i++) {
			// There is no 3rd attachment if msaa is disabled
			const int attachCount = gRenderer->config.msaa > 1 ? 3 : 2;
			VkImageView attachments[attachCount];
			if (gRenderer->config.msaa > 1) {
				attachments[0] = gRenderer->dsi->view;
				attachments[1] = gRenderer->msaaImage->view;
				attachments[2] = gRenderer->swapchainImageViews[i];
			} else {
				attachments[0] = gRenderer->dsi->view;
				attachments[1] = gRenderer->swapchainImageViews[i];
			}

			VkFramebufferCreateInfo framebufferCreateInfo = vk2dInitFramebufferCreateInfo(gRenderer->renderPass, gRenderer->surfaceWidth, gRenderer->surfaceHeight, attachments, attachCount);
			vk2dErrorCheck(vkCreateFramebuffer(gRenderer->ld->dev, &framebufferCreateInfo, VK_NULL_HANDLE, &gRenderer->framebuffers[i]));
			vk2dLogMessage("Framebuffer[%i] ready...", i);
		}
	}

	vk2dLogMessage("Framebuffers initialized...");
}

static void _vk2dRendererDestroyFrameBuffer() {
	uint32_t i;
	for (i = 0; i < gRenderer->swapchainImageCount; i++)
		vkDestroyFramebuffer(gRenderer->ld->dev, gRenderer->framebuffers[i], VK_NULL_HANDLE);
	free(gRenderer->framebuffers);
}

static void _vk2dRendererCreateUniformBuffers() {
	gRenderer->ubos = calloc(1, sizeof(VK2DUniformBufferObject) * gRenderer->swapchainImageCount);
	gRenderer->uboBuffers = malloc(sizeof(VK2DBuffer) * gRenderer->swapchainImageCount);
	uint32_t i;

	if (vk2dPointerCheck(gRenderer->ubos) && vk2dPointerCheck(gRenderer->uboBuffers)) {
		for (i = 0; i < gRenderer->swapchainImageCount; i++)
			gRenderer->uboBuffers[i] = vk2dBufferCreate(sizeof(VK2DUniformBufferObject), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, gRenderer->ld);
	}
	vk2dLogMessage("UBO initialized...");
}

static void _vk2dRendererDestroyUniformBuffers() {
	uint32_t i;
	for (i = 0; i < gRenderer->swapchainImageCount; i++)
		vk2dBufferFree(gRenderer->uboBuffers[i]);
	free(gRenderer->ubos);
	free(gRenderer->uboBuffers);
}

static void _vk2dRendererCreateDescriptorPool() {
	gRenderer->descConTex = malloc(sizeof(VK2DDescCon) * gRenderer->swapchainImageCount);
	gRenderer->descConPrim = malloc(sizeof(VK2DDescCon) * gRenderer->swapchainImageCount);
	uint32_t i;

	if (vk2dPointerCheck(gRenderer->descConPrim) && vk2dPointerCheck(gRenderer->descConTex)) {
		for (i = 0; i < gRenderer->swapchainImageCount; i++) {
			gRenderer->descConTex[i] = vk2dDescConCreate(gRenderer->ld, gRenderer->duslt, 0, 1);
			gRenderer->descConPrim[i] = vk2dDescConCreate(gRenderer->ld, gRenderer->dusls, 0, VK2D_NO_LOCATION);
		}
	} // TODO: Custom pipeline descriptor controllers
	vk2dLogMessage("Descriptor pool initialized...");
}

static void _vk2dRendererDestroyDescriptorPool() {
	uint32_t i;
	for (i = 0; i < gRenderer->swapchainImageCount; i++) {
		vk2dDescConFree(gRenderer->descConTex[i]);
		vk2dDescConFree(gRenderer->descConPrim[i]);
	} // TODO: Custom pipeline descriptor controllers
}

static void _vk2dRendererCreateSynchronization() {
	uint32_t i;
	VkSemaphoreCreateInfo semaphoreCreateInfo = vk2dInitSemaphoreCreateInfo(0);
	VkFenceCreateInfo fenceCreateInfo = vk2dInitFenceCreateInfo(VK_FENCE_CREATE_SIGNALED_BIT);
	gRenderer->imageAvailableSemaphores = malloc(sizeof(VkSemaphore) * VK2D_MAX_FRAMES_IN_FLIGHT);
	gRenderer->renderFinishedSemaphores = malloc(sizeof(VkSemaphore) * VK2D_MAX_FRAMES_IN_FLIGHT);
	gRenderer->inFlightFences = malloc(sizeof(VkFence) * VK2D_MAX_FRAMES_IN_FLIGHT);
	gRenderer->imagesInFlight = calloc(1, sizeof(VkFence) * gRenderer->swapchainImageCount);

	if (vk2dPointerCheck(gRenderer->imageAvailableSemaphores) && vk2dPointerCheck(gRenderer->renderFinishedSemaphores)
		&& vk2dPointerCheck(gRenderer->inFlightFences) && vk2dPointerCheck(gRenderer->imagesInFlight)) {
		for (i = 0; i < VK2D_MAX_FRAMES_IN_FLIGHT; i++) {
			vk2dErrorCheck(vkCreateSemaphore(gRenderer->ld->dev, &semaphoreCreateInfo, VK_NULL_HANDLE, &gRenderer->imageAvailableSemaphores[i]));
			vk2dErrorCheck(vkCreateSemaphore(gRenderer->ld->dev, &semaphoreCreateInfo, VK_NULL_HANDLE, &gRenderer->renderFinishedSemaphores[i]));
			vk2dErrorCheck(vkCreateFence(gRenderer->ld->dev, &fenceCreateInfo, VK_NULL_HANDLE, &gRenderer->inFlightFences[i]));
		}
	}

	// Drawing command pool synchronization
	gRenderer->draws = calloc(VK2D_DEVICE_COMMAND_POOLS, sizeof(VkCommandBuffer*));
	gRenderer->drawListSize = calloc(VK2D_DEVICE_COMMAND_POOLS, sizeof(uint32_t));
	gRenderer->drawCommandBuffers = calloc(VK2D_DEVICE_COMMAND_POOLS, sizeof(uint32_t));
	gRenderer->primaryBuffer = calloc(VK2D_DEVICE_COMMAND_POOLS, sizeof(VkCommandBuffer));

	if (vk2dPointerCheck(gRenderer->primaryBuffer)) {
		for (i = 0; i < VK2D_DEVICE_COMMAND_POOLS; i++)
			gRenderer->primaryBuffer[i] = vk2dLogicalDeviceGetCommandBuffer(gRenderer->ld, i, true);
	}

	vk2dLogMessage("Synchronization initialized...");
}

static void _vk2dRendererDestroySynchronization() {
	uint32_t i;

	for (i = 0; i < VK2D_MAX_FRAMES_IN_FLIGHT; i++) {
		vkDestroySemaphore(gRenderer->ld->dev, gRenderer->renderFinishedSemaphores[i], VK_NULL_HANDLE);
		vkDestroySemaphore(gRenderer->ld->dev, gRenderer->imageAvailableSemaphores[i], VK_NULL_HANDLE);
		vkDestroyFence(gRenderer->ld->dev, gRenderer->inFlightFences[i], VK_NULL_HANDLE);
	}
	free(gRenderer->imagesInFlight);
	free(gRenderer->inFlightFences);
	free(gRenderer->imageAvailableSemaphores);
	free(gRenderer->renderFinishedSemaphores);

	for (i = 0; i < VK2D_DEVICE_COMMAND_POOLS; i++)
		free(gRenderer->draws[i]);
	free(gRenderer->draws);
	free(gRenderer->drawListSize);
	free(gRenderer->drawCommandBuffers);
	free(gRenderer->primaryBuffer);
}

static void _vk2dRendererCreateSampler() {
	VkSamplerCreateInfo samplerCreateInfo = vk2dInitSamplerCreateInfo(gRenderer->config.filterMode, gRenderer->config.msaa, 1);
	vk2dErrorCheck(vkCreateSampler(gRenderer->ld->dev, &samplerCreateInfo, VK_NULL_HANDLE, &gRenderer->textureSampler));
}

static void _vk2dRendererDestroySampler() {
	vkDestroySampler(gRenderer->ld->dev, gRenderer->textureSampler, VK_NULL_HANDLE);
}

// If the window is resized or minimized or whatever
static void _vk2dRendererResetSwapchain() {
	// Hang while minimized
	SDL_WindowFlags flags;
	flags = SDL_GetWindowFlags(gRenderer->window);
	while (flags & SDL_WINDOW_MINIMIZED) {
		flags = SDL_GetWindowFlags(gRenderer->window);
		SDL_PumpEvents();
	}
	vkDeviceWaitIdle(gRenderer->ld->dev);

	// Free swapchain
	_vk2dRendererDestroySynchronization();
	_vk2dRendererDestroySampler();
	_vk2dRendererDestroyDescriptorPool();
	_vk2dRendererDestroyUniformBuffers();
	_vk2dRendererDestroyFrameBuffer();
	_vk2dRendererDestroyPipelines(true);
	_vk2dRendererDestroyRenderPass();
	_vk2dRendererDestroyDepthStencilImage();
	_vk2dRendererDestroyColourResources();
	_vk2dRendererDestroySwapchain();

	// Swap out configs in case they were changed
	gRenderer->config = gRenderer->newConfig;

	// Restart swapchain
	_vk2dRendererGetSurfaceSize();
	_vk2dRendererCreateSwapchain();
	_vk2dRendererCreateColourResources();
	_vk2dRendererCreateDepthStencilImage();
	_vk2dRendererCreateRenderPass();
	_vk2dRendererCreatePipelines();
	_vk2dRendererCreateFrameBuffer();
	_vk2dRendererCreateUniformBuffers();
	_vk2dRendererCreateDescriptorPool();
	_vk2dRendererCreateSampler();
	_vk2dRendererCreateSynchronization();
}

/******************************* User-visible functions *******************************/

int32_t vk2dRendererInit(SDL_Window *window, VK2DRendererConfig config) {
	gRenderer = calloc(1, sizeof(struct VK2DRenderer));
	int32_t errorCode = 0;
	uint32_t totalExtensionCount, i, sdlExtensions;
	const char** totalExtensions;

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

		// Initialize subsystems
		_vk2dRendererCreateDebug();
		_vk2dRendererCreateWindowSurface();
		_vk2dRendererCreateSwapchain();
		_vk2dRendererCreateColourResources();
		_vk2dRendererCreateDepthStencilImage();
		_vk2dRendererCreateRenderPass();
		_vk2dRendererCreateDescriptorSetLayout();
		_vk2dRendererCreatePipelines();
		_vk2dRendererCreateFrameBuffer();
		_vk2dRendererCreateUniformBuffers();
		_vk2dRendererCreateDescriptorPool();
		_vk2dRendererCreateSampler();
		_vk2dRendererCreateSynchronization();

		// Demos
		_vk2dRendererCreateDemos();
	} else {
		errorCode = -1;
	}

	return errorCode;
}

void vk2dRendererQuit() {
	if (gRenderer != NULL) {
		vkQueueWaitIdle(gRenderer->ld->queue);

		// Demos
		_vk2dRendererDestroyDemos();

		// Destroy subsystems
		_vk2dRendererDestroySynchronization();
		_vk2dRendererDestroySampler();
		_vk2dRendererDestroyDescriptorPool();
		_vk2dRendererDestroyUniformBuffers();
		_vk2dRendererDestroyFrameBuffer();
		_vk2dRendererDestroyPipelines(false);
		_vk2dRendererDestroyDescriptorSetLayout();
		_vk2dRendererDestroyRenderPass();
		_vk2dRendererDestroyDepthStencilImage();
		_vk2dRendererDestroyColourResources();
		_vk2dRendererDestroySwapchain();
		_vk2dRendererDestroyWindowSurface();
		_vk2dRendererDestroyDebug();

		// Destroy core bits
		vk2dLogicalDeviceFree(gRenderer->ld);
		vk2dPhysicalDeviceFree(gRenderer->pd);

		free(gRenderer);
		gRenderer = NULL;

		vk2dLogMessage("VK2D has been uninitialized.");
	}
}

void vk2dRendererWait() {
	vkQueueWaitIdle(gRenderer->ld->queue);
}

VK2DRenderer vk2dRendererGetPointer() {
	return gRenderer;
}

void vk2dRendererResetSwapchain() {
	gRenderer->resetSwapchain = true;
}

VK2DRendererConfig vk2dRendererGetConfig() {
	return gRenderer->config;
}

void vk2dRendererSetConfig(VK2DRendererConfig config) {
	gRenderer->newConfig = config;
	vk2dRendererResetSwapchain();
}

void vk2dRendererStartFrame() {
	// Wait for previous rendering to be finished
	vkWaitForFences(gRenderer->ld->dev, 1, &gRenderer->inFlightFences[gRenderer->currentFrame], VK_TRUE, UINT64_MAX);

	// Acquire image
	vkAcquireNextImageKHR(gRenderer->ld->dev, gRenderer->swapchain, UINT64_MAX, gRenderer->imageAvailableSemaphores[gRenderer->currentFrame], VK_NULL_HANDLE, &gRenderer->scImageIndex);

	if (gRenderer->imagesInFlight[gRenderer->scImageIndex] != VK_NULL_HANDLE) {
		vkWaitForFences(gRenderer->ld->dev, 1, &gRenderer->imagesInFlight[gRenderer->scImageIndex], VK_TRUE, UINT64_MAX);
	}
	gRenderer->imagesInFlight[gRenderer->scImageIndex] = gRenderer->inFlightFences[gRenderer->currentFrame];

	// Get the command pool and desc con ready
	gRenderer->drawCommandPool = (gRenderer->drawCommandPool + 1) % VK2D_DEVICE_COMMAND_POOLS;
	gRenderer->drawCommandBuffers[gRenderer->drawCommandPool] = 0;
	gRenderer->drawOffset = 0;
	vk2dLogicalDeviceResetPool(gRenderer->ld, gRenderer->drawCommandPool);
	vk2dDescConReset(gRenderer->descConPrim[gRenderer->scImageIndex]);
	vk2dDescConReset(gRenderer->descConTex[gRenderer->scImageIndex]);
	gRenderer->targetFrameBuffer = gRenderer->framebuffers[gRenderer->scImageIndex];
	gRenderer->targetRenderPass = gRenderer->renderPass;
	gRenderer->targetSubPass = 0;

	// Start the render pass
	VkCommandBufferBeginInfo beginInfo = vk2dInitCommandBufferBeginInfo(0, VK_NULL_HANDLE);
	vkResetCommandBuffer(gRenderer->primaryBuffer[gRenderer->drawCommandPool], 0);
	vk2dErrorCheck(vkBeginCommandBuffer(gRenderer->primaryBuffer[gRenderer->drawCommandPool], &beginInfo));

	// Setup render pass
	VkRect2D rect = {};
	rect.extent.width = gRenderer->surfaceWidth;
	rect.extent.height = gRenderer->surfaceHeight;
	const uint32_t clearCount = 1;
	VkClearValue clearValues[1] = {};
	clearValues[0].depthStencil.depth = 1;
	clearValues[0].depthStencil.stencil = 0;
	VkRenderPassBeginInfo renderPassBeginInfo = vk2dInitRenderPassBeginInfo(
			gRenderer->renderPass,
			gRenderer->framebuffers[gRenderer->scImageIndex],
			rect,
			clearValues,
			clearCount);

	vkCmdBeginRenderPass(gRenderer->primaryBuffer[gRenderer->drawCommandPool], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
}

void vk2dRendererEndFrame() {
	// Finish the primary command buffer, its time to PRESENT things
	_vk2dRendererEndRenderPass();
	vk2dErrorCheck(vkEndCommandBuffer(gRenderer->primaryBuffer[gRenderer->drawCommandPool]));

	// Wait for image before doing things
	VkPipelineStageFlags waitStage[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
	VkSubmitInfo submitInfo = vk2dInitSubmitInfo(
			&gRenderer->primaryBuffer[gRenderer->drawCommandPool],
			1,
			&gRenderer->renderFinishedSemaphores[gRenderer->currentFrame],
			1,
			&gRenderer->imageAvailableSemaphores[gRenderer->currentFrame],
			1,
			waitStage);

	// Submit
	vkResetFences(gRenderer->ld->dev, 1, &gRenderer->inFlightFences[gRenderer->currentFrame]);
	vk2dErrorCheck(vkQueueSubmit(gRenderer->ld->queue, 1, &submitInfo, gRenderer->inFlightFences[gRenderer->currentFrame]));

	// Final present info bit
	VkResult result;
	VkPresentInfoKHR presentInfo = vk2dInitPresentInfoKHR(&gRenderer->swapchain, 1, &gRenderer->scImageIndex, &result, &gRenderer->renderFinishedSemaphores[gRenderer->currentFrame], 1);
	vk2dErrorCheck(vkQueuePresentKHR(gRenderer->ld->queue, &presentInfo));
	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || gRenderer->resetSwapchain) {
		_vk2dRendererResetSwapchain();
		gRenderer->resetSwapchain = false;
	} else {
		vk2dErrorCheck(result);
	}

	gRenderer->currentFrame = (gRenderer->currentFrame + 1) % VK2D_MAX_FRAMES_IN_FLIGHT;
}

VK2DLogicalDevice vk2dRendererGetDevice() {
	return gRenderer->ld;
}

void vk2dRendererSetTarget(VK2DTexture target) {
	if (target->fbo != gRenderer->targetFrameBuffer) {
		// Figure out which render pass to use
		VkRenderPass pass = target == VK2D_TARGET_SCREEN ? gRenderer->midFrameSwapRenderPass : gRenderer->externalTargetRenderPass;
		VkFramebuffer framebuffer = target == VK2D_TARGET_SCREEN ? gRenderer->framebuffers[gRenderer->scImageIndex] : target->fbo;
		gRenderer->targetRenderPass = pass;
		gRenderer->targetFrameBuffer = framebuffer;

		// Setup render pass
		_vk2dRendererEndRenderPass();
		VkRect2D rect = {};
		rect.extent.width = gRenderer->surfaceWidth;
		rect.extent.height = gRenderer->surfaceHeight;
		const uint32_t clearCount = 1;
		VkClearValue clearValues[1] = {};
		clearValues[0].depthStencil.depth = 1;
		clearValues[0].depthStencil.stencil = 0;
		VkRenderPassBeginInfo renderPassBeginInfo = vk2dInitRenderPassBeginInfo(
				pass,
				framebuffer,
				rect,
				clearValues,
				clearCount);

		vkCmdBeginRenderPass(gRenderer->primaryBuffer[gRenderer->drawCommandPool], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
	}
}

void vk2dRendererDrawTex(VK2DTexture tex, float x, float y, float xscale, float yscale, float rot) {
	// TODO: This
}

void vk2dRendererDrawPolygon(VK2DPolygon polygon, bool filled, float x, float y, float xscale, float yscale, float rot) {
	// TODO: This (properly)
	// Necessary information
	VkCommandBufferInheritanceInfo inheritanceInfo = vk2dInitCommandBufferInheritanceInfo(gRenderer->targetRenderPass, gRenderer->targetSubPass, gRenderer->targetFrameBuffer);
	VkDescriptorSet set = vk2dDescConGetBufferSet(gRenderer->descConPrim[gRenderer->scImageIndex], gTestUBO); // TODO: Remove test thing here
	VkCommandBuffer buf = _vk2dRendererGetNextCommandBuffer();
	VkCommandBufferBeginInfo beginInfo = vk2dInitCommandBufferBeginInfo(VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT, &inheritanceInfo);
	VkViewport viewport = {};
	viewport.minDepth = 0;
	viewport.minDepth = 1;
	viewport.width = gRenderer->surfaceWidth;
	viewport.height = gRenderer->surfaceHeight;
	viewport.x = 0;
	viewport.y = 0;
	const float blendConstants[4] = {0.0, 0.0, 0.0, 0.0};

	// Recording the command buffer
	vk2dErrorCheck(vkBeginCommandBuffer(buf, &beginInfo));
	vkCmdBindPipeline(buf, VK_PIPELINE_BIND_POINT_GRAPHICS, filled ? gRenderer->primFillPipe->pipe : gRenderer->primLinePipe->pipe);
	vkCmdBindDescriptorSets(buf, VK_PIPELINE_BIND_POINT_GRAPHICS, filled ? gRenderer->primFillPipe->layout : gRenderer->primLinePipe->layout, 0, 1, &set, 0, VK_NULL_HANDLE);
	VkDeviceSize offsets[] = {0};
	vkCmdBindVertexBuffers(buf, 0, 1, &polygon->vertices->buf, offsets);
	vkCmdSetViewport(buf, 0, 1, &viewport);
	vkCmdSetBlendConstants(buf, blendConstants);
	vkCmdDraw(buf, polygon->vertexCount, 1, 0, 0);
	vk2dErrorCheck(vkEndCommandBuffer(buf));
}
