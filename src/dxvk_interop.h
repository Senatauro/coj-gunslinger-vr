// Interfaces de interop D3D9 -> Vulkan do DXVK 3.1.1 (de src/d3d9/d3d9_interfaces.h, licença zlib). Só o que o
// mod usa, com os tipos Vulkan mínimos para compilar sem os headers do Vulkan (em 32 bits os handles não
// despacháveis são uint64_t).
#pragma once
#include <d3d9.h>
#include <stdint.h>

typedef struct VkInstance_T* VkInstance;
typedef struct VkPhysicalDevice_T* VkPhysicalDevice;
typedef struct VkDevice_T* VkDevice;
typedef struct VkQueue_T* VkQueue;
typedef uint64_t VkImage;
typedef int VkImageLayout;
enum { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = 14, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL = 6, VK_IMAGE_ASPECT_COLOR_BIT = 1 };

struct VkImageCreateInfo {
	int sType;
	const void* pNext;
	uint32_t flags;
	int imageType, format;
	uint32_t width, height, depth, mipLevels, arrayLayers;
	int samples, tiling;
	uint32_t usage;
	int sharingMode;
	uint32_t queueFamilyIndexCount;
	const uint32_t* pQueueFamilyIndices;
	int initialLayout;
};
struct VkImageSubresourceRange {
	uint32_t aspectMask, baseMipLevel, levelCount, baseArrayLayer, layerCount;
};

struct ID3D9VkInteropTexture : IUnknown {
	virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(VkImage* image, VkImageLayout* layout, VkImageCreateInfo* info) = 0;
};

struct ID3D9VkInteropDevice : IUnknown {
	virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance*, VkPhysicalDevice*, VkDevice*) = 0;
	virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue*, uint32_t* queueIndex, uint32_t* queueFamilyIndex) = 0;
	virtual void STDMETHODCALLTYPE TransitionTextureLayout(ID3D9VkInteropTexture*, const VkImageSubresourceRange*,
	                                                       VkImageLayout oldLayout, VkImageLayout newLayout) = 0;
	virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
	virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
	virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
	// LockDevice, UnlockDevice, WaitForResource e CreateImage vêm depois na vtable e não são usados; omiti-los não
	// desloca os métodos acima.
};

static const GUID IID_ID3D9VkInteropDevice = {0x2eaa4b89, 0x0107, 0x4bdb, {0x87, 0xf7, 0x0f, 0x54, 0x1c, 0x49, 0x3c, 0xe0}};
static const GUID IID_ID3D9VkInteropTexture = {0xd56344f5, 0x8d35, 0x46fd, {0x80, 0x6d, 0x94, 0xc3, 0x51, 0xb4, 0x72, 0xc1}};
