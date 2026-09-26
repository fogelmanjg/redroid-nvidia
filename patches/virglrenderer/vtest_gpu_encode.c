/* See vtest_gpu_encode.h. Vulkan, CUDA and NVENC are all loaded lazily via
 * dlopen so the vtest server keeps working (minus this command) on hosts
 * without an NVIDIA driver - same philosophy as vtest_gpu_alloc.c. */

#include "vtest_gpu_encode.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

/* Minimal CUDA driver API surface - just enough for external-memory interop,
 * mirroring what ffnvcodec's dynlink_cuda.h declares (not depended on here
 * to avoid a hard build-time requirement on that project's headers). */
typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef void *CUexternalMemory;
typedef void *CUmipmappedArray;
typedef void *CUarray;
#define CUDA_SUCCESS 0
#define CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD 1
#define CU_AD_FORMAT_UNSIGNED_INT8 0x01
#define CUDA_ARRAY3D_SURFACE_LDST 0x02
#define CUDA_ARRAY3D_VIDEO_ENCODE_DECODE 0x100

typedef struct {
   int type;
   union {
      int fd;
      struct { void *handle; const void *name; } win32;
   } handle;
   unsigned long long size;
   unsigned int flags;
   unsigned int reserved[16];
} cuda_ext_mem_handle_desc;

typedef struct {
   size_t Width, Height, Depth;
   int Format;
   unsigned int NumChannels;
   unsigned int Flags;
} cuda_array3d_descriptor;

typedef struct {
   unsigned long long offset;
   cuda_array3d_descriptor arrayDesc;
   unsigned int numLevels;
   unsigned int reserved[16];
} cuda_ext_mem_mipmap_desc;

/* NVENC's own struct/function surface is much larger; rather than duplicate
 * it here too, this module treats it as fully opaque and reaches it only
 * through nvEncodeAPI.h at build time IF present (checked by meson), same
 * spirit as the CUDA subset above but too large to hand-roll safely. */
#include <ffnvcodec/nvEncodeAPI.h>

#define ENCODE_W_MAX 3840
#define ENCODE_H_MAX 2160
#define BITSTREAM_BUF_SIZE (8 * 1024 * 1024)

struct encode_state {
   /* Vulkan: a private device, deliberately independent from
    * vtest_gpu_alloc.c's alloc_vk - see the "spike 2 vs spike 3" story in
    * redroid-nvidia's DEVLOG for why a shared/re-exported allocation across
    * devices doesn't work but an imported-then-copied one does. */
   void *vk_lib;
   PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
   VkInstance instance;
   VkPhysicalDevice phys;
   VkDevice dev;
   VkPhysicalDeviceMemoryProperties mem_props;
   VkQueue queue;
   VkCommandPool pool;
   VkCommandBuffer cmd;

   PFN_vkCreateImage CreateImage;
   PFN_vkDestroyImage DestroyImage;
   PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
   PFN_vkAllocateMemory AllocateMemory;
   PFN_vkFreeMemory FreeMemory;
   PFN_vkBindImageMemory BindImageMemory;
   PFN_vkGetMemoryFdKHR GetMemoryFdKHR;
   PFN_vkCreateCommandPool CreateCommandPool;
   PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
   PFN_vkBeginCommandBuffer BeginCommandBuffer;
   PFN_vkEndCommandBuffer EndCommandBuffer;
   PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
   PFN_vkCmdCopyImage CmdCopyImage;
   PFN_vkQueueSubmit QueueSubmit;
   PFN_vkQueueWaitIdle QueueWaitIdle;
   PFN_vkGetDeviceQueue GetDeviceQueue;
   PFN_vkResetCommandBuffer ResetCommandBuffer;

   /* CUDA */
   void *cuda_lib;
   CUresult (*cuInit)(unsigned int);
   CUresult (*cuDeviceGet)(CUdevice *, int);
   CUresult (*cuCtxCreate_v2)(CUcontext *, unsigned int, CUdevice);
   CUresult (*cuCtxPopCurrent_v2)(CUcontext *);
   CUresult (*cuCtxSetCurrent)(CUcontext);
   CUresult (*cuImportExternalMemory)(CUexternalMemory *, const cuda_ext_mem_handle_desc *);
   CUresult (*cuExternalMemoryGetMappedMipmappedArray)(CUmipmappedArray *, CUexternalMemory, const cuda_ext_mem_mipmap_desc *);
   CUresult (*cuMipmappedArrayGetLevel)(CUarray *, CUmipmappedArray, unsigned int);
   CUresult (*cuGetErrorString)(CUresult, const char **);
   CUcontext cuctx;

   /* NVENC */
   void *nvenc_lib;
   NVENCSTATUS (NVENCAPI *NvEncodeAPICreateInstance)(NV_ENCODE_API_FUNCTION_LIST *);
   NVENCSTATUS (NVENCAPI *NvEncodeAPIGetMaxSupportedVersion)(uint32_t *);
   NV_ENCODE_API_FUNCTION_LIST fn;
   uint32_t drv_api_version;
   void *encoder;

   /* Persistent per-resolution state: the copy target image/memory NVENC is
    * registered against, and the encoder session itself. Torn down and
    * recreated whenever the requested width/height changes. */
   uint32_t cur_w, cur_h;
   VkImage copy_image;
   VkDeviceMemory copy_memory;
   NV_ENC_REGISTERED_PTR registered;
   void *mapped;
   void *bitstream_buf;
   uint8_t bitstream_out[BITSTREAM_BUF_SIZE];
};

static struct encode_state enc;
static pthread_mutex_t enc_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool enc_failed;

#define MKVER(structver) (enc.drv_api_version | ((uint32_t)(structver) << 16) | (0x7u << 28))

static VkFormat
drm_format_to_vk(uint32_t drm_format)
{
   /* mirrors vtest_gpu_alloc.c's drm_format_to_vk - kept local since these
    * two modules are independent and neither should depend on the other's
    * internals for something this small. */
   switch (drm_format) {
   case 0x20203852: return VK_FORMAT_R8_UNORM;             /* R8   */
   case 0x36314752: return VK_FORMAT_R5G6B5_UNORM_PACK16;  /* RG16 */
   case 0x34325258:                                        /* XR24 */
   case 0x34325241: return VK_FORMAT_B8G8R8A8_UNORM;       /* AR24 */
   case 0x34324258:                                        /* XB24 */
   case 0x34324241: return VK_FORMAT_R8G8B8A8_UNORM;       /* AB24 */
   case 0x30334241: return VK_FORMAT_A2B10G10R10_UNORM_PACK32; /* AB30 */
   case 0x48344241: return VK_FORMAT_R16G16B16A16_SFLOAT;  /* AB4H */
   default: return VK_FORMAT_UNDEFINED;
   }
}

static int
encode_vk_init_locked(void)
{
   enc.vk_lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
   if (!enc.vk_lib)
      return -ENODEV;
   *(void **)&enc.GetInstanceProcAddr = dlsym(enc.vk_lib, "vkGetInstanceProcAddr");
   if (!enc.GetInstanceProcAddr)
      return -ENODEV;

#define GET_GLOBAL(name) PFN_vk##name name = (PFN_vk##name)enc.GetInstanceProcAddr(NULL, "vk" #name)
#define GET_INST(name) PFN_vk##name name = (PFN_vk##name)enc.GetInstanceProcAddr(enc.instance, "vk" #name)

   GET_GLOBAL(CreateInstance);
   if (!CreateInstance)
      return -ENODEV;

   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "vtest-gpu-encode",
      .apiVersion = VK_API_VERSION_1_1,
   };
   const VkInstanceCreateInfo inst_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   if (CreateInstance(&inst_info, NULL, &enc.instance) != VK_SUCCESS)
      return -ENODEV;

   GET_INST(EnumeratePhysicalDevices);
   GET_INST(GetPhysicalDeviceProperties);
   GET_INST(EnumerateDeviceExtensionProperties);
   GET_INST(GetPhysicalDeviceMemoryProperties);
   GET_INST(CreateDevice);
   GET_INST(GetDeviceProcAddr);

   VkPhysicalDevice devices[16];
   uint32_t count = 16;
   if (EnumeratePhysicalDevices(enc.instance, &count, devices) < 0 || !count)
      return -ENODEV;

   const char *want = getenv("VTEST_ALLOC_GPU");
   VkPhysicalDevice best = VK_NULL_HANDLE;
   int best_score = -1;
   for (uint32_t i = 0; i < count; i++) {
      VkPhysicalDeviceProperties props;
      GetPhysicalDeviceProperties(devices[i], &props);

      bool has_dma_buf = false, has_modifier = false;
      VkExtensionProperties exts[512];
      uint32_t ext_count = 512;
      EnumerateDeviceExtensionProperties(devices[i], NULL, &ext_count, exts);
      for (uint32_t j = 0; j < ext_count; j++) {
         if (!strcmp(exts[j].extensionName, "VK_EXT_external_memory_dma_buf"))
            has_dma_buf = true;
         if (!strcmp(exts[j].extensionName, "VK_EXT_image_drm_format_modifier"))
            has_modifier = true;
      }
      if (!has_dma_buf || !has_modifier)
         continue;

      int score = 0;
      if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
         score += 2;
      if (want && strstr(props.deviceName, want))
         score += 10;
      if (score > best_score) {
         best_score = score;
         best = devices[i];
      }
   }
   if (best == VK_NULL_HANDLE)
      return -ENODEV;
   enc.phys = best;
   GetPhysicalDeviceMemoryProperties(best, &enc.mem_props);

   const float prio = 1.0f;
   const VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio,
   };
   const char *dev_exts[] = {
      "VK_KHR_external_memory_fd",
      "VK_EXT_external_memory_dma_buf",
      "VK_EXT_image_drm_format_modifier",
   };
   const VkDeviceCreateInfo dev_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
      .enabledExtensionCount = 3, .ppEnabledExtensionNames = dev_exts,
   };
   if (CreateDevice(best, &dev_info, NULL, &enc.dev) != VK_SUCCESS)
      return -ENODEV;

#define GET_DEV(name) enc.name = (PFN_vk##name)GetDeviceProcAddr(enc.dev, "vk" #name)
   GET_DEV(CreateImage);
   GET_DEV(DestroyImage);
   GET_DEV(GetImageMemoryRequirements);
   GET_DEV(AllocateMemory);
   GET_DEV(FreeMemory);
   GET_DEV(BindImageMemory);
   GET_DEV(GetMemoryFdKHR);
   GET_DEV(CreateCommandPool);
   GET_DEV(AllocateCommandBuffers);
   GET_DEV(BeginCommandBuffer);
   GET_DEV(EndCommandBuffer);
   GET_DEV(CmdPipelineBarrier);
   GET_DEV(CmdCopyImage);
   GET_DEV(QueueSubmit);
   GET_DEV(QueueWaitIdle);
   GET_DEV(GetDeviceQueue);
   GET_DEV(ResetCommandBuffer);
#undef GET_DEV
#undef GET_INST
#undef GET_GLOBAL

   if (!enc.GetMemoryFdKHR || !enc.CmdCopyImage)
      return -ENODEV;

   enc.GetDeviceQueue(enc.dev, 0, 0, &enc.queue);

   const VkCommandPoolCreateInfo poolinfo = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0,
   };
   if (enc.CreateCommandPool(enc.dev, &poolinfo, NULL, &enc.pool) != VK_SUCCESS)
      return -ENODEV;
   const VkCommandBufferAllocateInfo cbinfo = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = enc.pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
   };
   if (enc.AllocateCommandBuffers(enc.dev, &cbinfo, &enc.cmd) != VK_SUCCESS)
      return -ENODEV;

   return 0;
}

static int
encode_cuda_nvenc_init_locked(void)
{
   enc.cuda_lib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
   if (!enc.cuda_lib)
      return -ENODEV;
#define GET_CU(name) *(void **)&enc.name = dlsym(enc.cuda_lib, #name)
   GET_CU(cuInit);
   GET_CU(cuDeviceGet);
   GET_CU(cuCtxCreate_v2);
   GET_CU(cuCtxPopCurrent_v2);
   GET_CU(cuCtxSetCurrent);
   GET_CU(cuImportExternalMemory);
   GET_CU(cuExternalMemoryGetMappedMipmappedArray);
   GET_CU(cuMipmappedArrayGetLevel);
   GET_CU(cuGetErrorString);
#undef GET_CU
   if (!enc.cuInit || !enc.cuImportExternalMemory)
      return -ENODEV;

   if (enc.cuInit(0) != CUDA_SUCCESS)
      return -ENODEV;
   CUdevice cudev;
   if (enc.cuDeviceGet(&cudev, 0) != CUDA_SUCCESS)
      return -ENODEV;
   if (enc.cuCtxCreate_v2(&enc.cuctx, 0, cudev) != CUDA_SUCCESS)
      return -ENODEV;

   enc.nvenc_lib = dlopen("libnvidia-encode.so.1", RTLD_NOW | RTLD_LOCAL);
   if (!enc.nvenc_lib)
      return -ENODEV;
   *(void **)&enc.NvEncodeAPICreateInstance = dlsym(enc.nvenc_lib, "NvEncodeAPICreateInstance");
   *(void **)&enc.NvEncodeAPIGetMaxSupportedVersion = dlsym(enc.nvenc_lib, "NvEncodeAPIGetMaxSupportedVersion");
   if (!enc.NvEncodeAPICreateInstance || !enc.NvEncodeAPIGetMaxSupportedVersion)
      return -ENODEV;

   uint32_t max_supported = 0;
   if (enc.NvEncodeAPIGetMaxSupportedVersion(&max_supported) != NV_ENC_SUCCESS)
      return -ENODEV;
   /* NV_ENC_*_VER macros bake in this header's own major.minor at compile
    * time; rebuild every version tag from what the installed driver's
    * libnvidia-encode actually reports instead - see redroid-nvidia's
    * DEVLOG (2026-09-25) for the NV_ENC_ERR_INVALID_VERSION this avoids. */
   enc.drv_api_version = (max_supported >> 4) | ((max_supported & 0xF) << 24);

   /* NVENC pushes/pops the CUDA context itself and rejects one left current
    * on the calling thread - see the same DEVLOG entry. */
   CUcontext popped;
   enc.cuCtxPopCurrent_v2(&popped);

   memset(&enc.fn, 0, sizeof(enc.fn));
   enc.fn.version = MKVER(2);
   if (enc.NvEncodeAPICreateInstance(&enc.fn) != NV_ENC_SUCCESS)
      return -ENODEV;

   NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sess;
   memset(&sess, 0, sizeof(sess));
   sess.version = MKVER(1);
   sess.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
   sess.device = enc.cuctx;
   sess.apiVersion = enc.drv_api_version;
   if (enc.fn.nvEncOpenEncodeSessionEx(&sess, &enc.encoder) != NV_ENC_SUCCESS)
      return -ENODEV;

   return 0;
}

static int
encode_init_locked(void)
{
   if (enc.encoder)
      return 0;
   if (enc_failed)
      return -ENODEV;
   enc_failed = true;

   int ret = encode_vk_init_locked();
   if (ret)
      return ret;
   ret = encode_cuda_nvenc_init_locked();
   if (ret)
      return ret;

   enc_failed = false;
   return 0;
}

/* (Re)creates the persistent copy target + NVENC registration for a new
 * resolution. Tears down the previous one first, if any. */
static int
encode_reconfigure_locked(uint32_t width, uint32_t height, uint64_t modifier,
                          const uint64_t *mod_candidates, uint32_t mod_count)
{
   if (enc.registered) {
      NV_ENC_MAP_INPUT_RESOURCE unmap; /* nvEncUnmapInputResource takes the mapped ptr, tracked below */
      (void)unmap;
      if (enc.mapped)
         enc.fn.nvEncUnmapInputResource(enc.encoder, enc.mapped);
      enc.fn.nvEncUnregisterResource(enc.encoder, enc.registered);
      enc.registered = NULL;
      enc.mapped = NULL;
   }
   if (enc.bitstream_buf) {
      enc.fn.nvEncDestroyBitstreamBuffer(enc.encoder, enc.bitstream_buf);
      enc.bitstream_buf = NULL;
   }
   if (enc.copy_image) {
      enc.DestroyImage(enc.dev, enc.copy_image, NULL);
      enc.copy_image = VK_NULL_HANDLE;
   }
   if (enc.copy_memory) {
      enc.FreeMemory(enc.dev, enc.copy_memory, NULL);
      enc.copy_memory = VK_NULL_HANDLE;
   }

   const VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
   const VkExternalMemoryImageCreateInfo ext_info = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT |
                     VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
   };
   const VkImageDrmFormatModifierListCreateInfoEXT modinfo = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
      .pNext = &ext_info, .drmFormatModifierCount = mod_count, .pDrmFormatModifiers = mod_candidates,
   };
   const VkImageCreateInfo imginfo = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &modinfo,
      .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = { width, height, 1 },
      .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkResult rvr = enc.CreateImage(enc.dev, &imginfo, NULL, &enc.copy_image);
   if (rvr != VK_SUCCESS)
      return -EINVAL;

   VkMemoryRequirements reqs;
   enc.GetImageMemoryRequirements(enc.dev, enc.copy_image, &reqs);
   uint32_t mem_type = UINT32_MAX;
   for (uint32_t i = 0; i < enc.mem_props.memoryTypeCount; i++)
      if ((reqs.memoryTypeBits & (1u << i)) &&
          (enc.mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
         { mem_type = i; break; }
   if (mem_type == UINT32_MAX)
      mem_type = ffs(reqs.memoryTypeBits) - 1;

   const VkExportMemoryAllocateInfo exp_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT |
                     VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
   };
   const VkMemoryDedicatedAllocateInfo ded_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &exp_info, .image = enc.copy_image,
   };
   const VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ded_info,
      .allocationSize = reqs.size, .memoryTypeIndex = mem_type,
   };
   rvr = enc.AllocateMemory(enc.dev, &alloc_info, NULL, &enc.copy_memory);
   if (rvr != VK_SUCCESS)
      return -EINVAL;
   rvr = enc.BindImageMemory(enc.dev, enc.copy_image, enc.copy_memory, 0);
   if (rvr != VK_SUCCESS)
      return -EINVAL;

   const VkMemoryGetFdInfoKHR fdinfo = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = enc.copy_memory,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
   };
   int opaque_fd = -1;
   rvr = enc.GetMemoryFdKHR(enc.dev, &fdinfo, &opaque_fd);
   if (rvr != VK_SUCCESS || opaque_fd < 0)
      return -EIO;

   /* The CUDA context was popped after encode_init_locked()'s one-time NVENC
    * session setup (NVENC needs it floating - see the DEVLOG entry on
    * NV_ENC_ERR_INVALID_ENCODERDEVICE); every later CUDA-side call in this
    * function needs it current on this thread again, or fails with
    * CUDA_ERROR_INVALID_CONTEXT (201). */
   enc.cuCtxSetCurrent(enc.cuctx);

   cuda_ext_mem_handle_desc memdesc;
   memset(&memdesc, 0, sizeof(memdesc));
   memdesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
   memdesc.handle.fd = opaque_fd; /* CUDA owns it now */
   memdesc.size = reqs.size;
   CUexternalMemory extmem;
   CUresult cr = enc.cuImportExternalMemory(&extmem, &memdesc);
   if (cr != CUDA_SUCCESS)
      return -EIO;

   cuda_ext_mem_mipmap_desc arrdesc;
   memset(&arrdesc, 0, sizeof(arrdesc));
   arrdesc.arrayDesc.Width = width;
   arrdesc.arrayDesc.Height = height;
   arrdesc.arrayDesc.Format = CU_AD_FORMAT_UNSIGNED_INT8;
   arrdesc.arrayDesc.NumChannels = 4;
   arrdesc.arrayDesc.Flags = CUDA_ARRAY3D_SURFACE_LDST | CUDA_ARRAY3D_VIDEO_ENCODE_DECODE;
   arrdesc.numLevels = 1;
   CUmipmappedArray mip;
   cr = enc.cuExternalMemoryGetMappedMipmappedArray(&mip, extmem, &arrdesc);
   if (cr != CUDA_SUCCESS)
      return -EIO;
   CUarray cuarr;
   cr = enc.cuMipmappedArrayGetLevel(&cuarr, mip, 0);
   if (cr != CUDA_SUCCESS)
      return -EIO;

   /* Done with CUDA calls for this reconfigure - pop again before any NVENC
    * call, same reason as encode_cuda_nvenc_init_locked(). */
   {
      CUcontext popped;
      enc.cuCtxPopCurrent_v2(&popped);
   }

   NV_ENC_PRESET_CONFIG presetcfg;
   memset(&presetcfg, 0, sizeof(presetcfg));
   presetcfg.version = MKVER(5) | (1u << 31);
   presetcfg.presetCfg.version = MKVER(9) | (1u << 31);
   NVENCSTATUS nvst = enc.fn.nvEncGetEncodePresetConfigEx(enc.encoder, NV_ENC_CODEC_H264_GUID,
                                           NV_ENC_PRESET_P3_GUID, NV_ENC_TUNING_INFO_LOW_LATENCY,
                                           &presetcfg);
   if (nvst != NV_ENC_SUCCESS)
      return -EIO;
   /* This module encodes one on-demand frame per VCMD_ENCODE_RESOURCE call,
    * not a real temporal sequence it controls the cadence of - force P-only
    * (no B-frame reordering) so every NvEncEncodePicture has an immediately
    * lockable bitstream, never NV_ENC_ERR_NEED_MORE_INPUT. */
   presetcfg.presetCfg.frameIntervalP = 1;
   /* NVENC's default (repeatSPSPPS=0) only emits SPS/PPS once, out-of-band,
    * retrievable separately via nvEncGetSequenceParams() - not useful here,
    * since each caller (VCMD_ENCODE_RESOURCE, the SCM_RIGHTS listener) reads
    * one self-contained Annex-B buffer per call with no shared decoder state
    * across calls. Force every IDR to carry its own SPS/PPS inline instead,
    * so each response is independently decodable - confirmed necessary: a
    * real client (scrcpy) rejected the very first response with "the first
    * video packet is not a config packet" until this was set. */
   presetcfg.presetCfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;

   NV_ENC_INITIALIZE_PARAMS init;
   memset(&init, 0, sizeof(init));
   init.version = MKVER(7) | (1u << 31);
   init.encodeGUID = NV_ENC_CODEC_H264_GUID;
   init.presetGUID = NV_ENC_PRESET_P3_GUID;
   init.encodeWidth = width;
   init.encodeHeight = height;
   init.darWidth = width;
   init.darHeight = height;
   init.frameRateNum = 30;
   init.frameRateDen = 1;
   init.enablePTD = 1;
   init.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
   init.encodeConfig = &presetcfg.presetCfg;
   nvst = enc.fn.nvEncInitializeEncoder(enc.encoder, &init);
   if (nvst != NV_ENC_SUCCESS)
      return -EIO;

   NV_ENC_REGISTER_RESOURCE reg;
   memset(&reg, 0, sizeof(reg));
   reg.version = MKVER(5);
   reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY;
   reg.width = width;
   reg.height = height;
   reg.pitch = width * 4;
   reg.resourceToRegister = cuarr;
   reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
   reg.bufferUsage = NV_ENC_INPUT_IMAGE;
   nvst = enc.fn.nvEncRegisterResource(enc.encoder, &reg);
   if (nvst != NV_ENC_SUCCESS)
      return -EIO;
   enc.registered = reg.registeredResource;

   NV_ENC_CREATE_BITSTREAM_BUFFER bsbuf;
   memset(&bsbuf, 0, sizeof(bsbuf));
   bsbuf.version = MKVER(1);
   nvst = enc.fn.nvEncCreateBitstreamBuffer(enc.encoder, &bsbuf);
   if (nvst != NV_ENC_SUCCESS)
      return -EIO;
   enc.bitstream_buf = bsbuf.bitstreamBuffer;

   enc.cur_w = width;
   enc.cur_h = height;
   (void)modifier;
   return 0;
}

/*
 * Discovers the driver's own real modifier for a format, the same way
 * vtest_gpu_alloc.c's own allocator does when it first creates a buffer
 * (enumerate what the format supports, non-linear, single-plane,
 * renderable+samplable) - used for the SCM_RIGHTS path (see
 * nvenc_scm_listener.c), where the sending side (a Codec2 component reading
 * an opaque, non-cros_gralloc native_handle_t it can't fully decode) has no
 * reliable way to report the buffer's true modifier itself. Matches
 * redroid-hwenc's own VA-API daemon philosophy exactly: let the host
 * determine its own real modifier by asking its own driver, rather than
 * trust a value the sender can't actually know. Deliberately does not
 * create an image to read back GetImageDrmFormatModifierPropertiesEXT's
 * "chosen" value the way the allocator does - in practice this driver
 * exposes exactly one non-linear candidate per format, so the first match
 * from the enumeration is that same answer without the extra round trip.
 * Returns 0 (DRM_FORMAT_MOD_LINEAR) if nothing better is found - not a
 * likely correct answer for a real composited surface, but a safe,
 * deterministic fallback that fails the subsequent import cleanly instead
 * of silently using a stale value from a previous call.
 */
uint64_t
vtest_gpu_encode_discover_modifier(uint32_t drm_format)
{
   const VkFormat format = drm_format_to_vk(drm_format);
   if (format == VK_FORMAT_UNDEFINED)
      return 0;

   pthread_mutex_lock(&enc_mutex);
   int ret = encode_init_locked();
   if (ret) {
      pthread_mutex_unlock(&enc_mutex);
      return 0;
   }

#define GET_INST(name) PFN_vk##name name = (PFN_vk##name)enc.GetInstanceProcAddr(enc.instance, "vk" #name)
   GET_INST(GetPhysicalDeviceFormatProperties2);
#undef GET_INST
   uint64_t chosen = 0;
   if (GetPhysicalDeviceFormatProperties2) {
      VkDrmFormatModifierPropertiesListEXT mod_list = {
         .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
      };
      VkFormatProperties2 fmt_props = {
         .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
         .pNext = &mod_list,
      };
      GetPhysicalDeviceFormatProperties2(enc.phys, format, &fmt_props);
      VkDrmFormatModifierPropertiesEXT props[64];
      mod_list.drmFormatModifierCount =
         mod_list.drmFormatModifierCount < 64 ? mod_list.drmFormatModifierCount : 64;
      mod_list.pDrmFormatModifierProperties = props;
      GetPhysicalDeviceFormatProperties2(enc.phys, format, &fmt_props);

      const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
      for (uint32_t i = 0; i < mod_list.drmFormatModifierCount; i++) {
         if (props[i].drmFormatModifierPlaneCount == 1 &&
             (props[i].drmFormatModifierTilingFeatures & need) == need &&
             props[i].drmFormatModifier != 0) {
            chosen = props[i].drmFormatModifier;
            break;
         }
      }
   }
   pthread_mutex_unlock(&enc_mutex);
   return chosen;
}

int
vtest_gpu_encode_dmabuf(int fd, uint32_t width, uint32_t height,
                        uint32_t drm_format, uint64_t modifier,
                        uint32_t stride, const uint8_t **out_buf, uint32_t *out_len)
{
   if (width == 0 || height == 0 || width > ENCODE_W_MAX || height > ENCODE_H_MAX) {
      close(fd);
      return -EINVAL;
   }
   const VkFormat format = drm_format_to_vk(drm_format);
   if (format == VK_FORMAT_UNDEFINED) {
      close(fd);
      return -EINVAL;
   }

   pthread_mutex_lock(&enc_mutex);
   int ret = encode_init_locked();
   if (ret) {
      pthread_mutex_unlock(&enc_mutex);
      close(fd);
      return ret;
   }

   /* Step 1: plain-import the foreign fd - self-compatible, no export
    * chained (see DEVLOG for why chaining one here silently corrupts data
    * on this driver instead of failing outright). */
   const VkSubresourceLayout layout = { .offset = 0, .rowPitch = stride, .size = 0, .arrayPitch = 0, .depthPitch = 0 };
   const VkExternalMemoryImageCreateInfo imported_ext = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   const VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_mod = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .pNext = &imported_ext, .drmFormatModifier = modifier,
      .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &layout,
   };
   const VkImageCreateInfo imported_imginfo = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &explicit_mod,
      .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = { width, height, 1 },
      .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage imported_image = VK_NULL_HANDLE;
   VkDeviceMemory imported_memory = VK_NULL_HANDLE;
   ret = -EIO;
   VkResult vr = enc.CreateImage(enc.dev, &imported_imginfo, NULL, &imported_image);
   if (vr != VK_SUCCESS) {
      close(fd); /* not yet handed to Vulkan */
      goto out;
   }

   VkMemoryRequirements imported_reqs;
   enc.GetImageMemoryRequirements(enc.dev, imported_image, &imported_reqs);
   uint32_t imported_mem_type = UINT32_MAX;
   for (uint32_t i = 0; i < enc.mem_props.memoryTypeCount; i++)
      if ((imported_reqs.memoryTypeBits & (1u << i)) &&
          (enc.mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
         { imported_mem_type = i; break; }
   if (imported_mem_type == UINT32_MAX)
      imported_mem_type = ffs(imported_reqs.memoryTypeBits) - 1;

   const VkImportMemoryFdInfoKHR import_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = fd,
   };
   const VkMemoryDedicatedAllocateInfo imported_ded = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &import_info, .image = imported_image,
   };
   const VkMemoryAllocateInfo imported_alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imported_ded,
      .allocationSize = imported_reqs.size, .memoryTypeIndex = imported_mem_type,
   };
   vr = enc.AllocateMemory(enc.dev, &imported_alloc, NULL, &imported_memory);
   if (vr != VK_SUCCESS) {
      /* fd ownership only transfers to Vulkan on success */
      close(fd);
      goto out;
   }
   vr = enc.BindImageMemory(enc.dev, imported_image, imported_memory, 0);
   if (vr != VK_SUCCESS)
      goto out;

   /* Step 2: (re)configure the persistent copy target if the resolution
    * changed since the last call. */
   if (width != enc.cur_w || height != enc.cur_h) {
      uint64_t mods[1] = { modifier };
      ret = encode_reconfigure_locked(width, height, modifier, mods, 1);
      if (ret)
         goto out;
   }

   /* Step 3: real GPU-to-GPU copy, both images OPTIMAL/tiled throughout. */
   enc.ResetCommandBuffer(enc.cmd, 0);
   const VkCommandBufferBeginInfo begininfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   enc.BeginCommandBuffer(enc.cmd, &begininfo);

   VkImageMemoryBarrier imported_to_src = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = imported_image, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
   };
   VkImageMemoryBarrier copy_to_dst = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = enc.copy_image, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
   };
   VkImageMemoryBarrier both[2] = { imported_to_src, copy_to_dst };
   enc.CmdPipelineBarrier(enc.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, NULL, 0, NULL, 2, both);

   const VkImageCopy region = {
      .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .srcOffset = { 0, 0, 0 },
      .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstOffset = { 0, 0, 0 },
      .extent = { width, height, 1 },
   };
   enc.CmdCopyImage(enc.cmd, imported_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    enc.copy_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

   VkImageMemoryBarrier copy_to_general = copy_to_dst;
   copy_to_general.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   copy_to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
   copy_to_general.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   copy_to_general.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
   enc.CmdPipelineBarrier(enc.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          0, 0, NULL, 0, NULL, 1, &copy_to_general);
   enc.EndCommandBuffer(enc.cmd);

   const VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &enc.cmd };
   if (enc.QueueSubmit(enc.queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
      goto out;
   enc.QueueWaitIdle(enc.queue);

   /* Step 4: encode the (persistent, already-registered) copy target. */
   NV_ENC_MAP_INPUT_RESOURCE map;
   memset(&map, 0, sizeof(map));
   map.version = MKVER(4);
   map.registeredResource = enc.registered;
   if (enc.fn.nvEncMapInputResource(enc.encoder, &map) != NV_ENC_SUCCESS)
      goto out;
   enc.mapped = map.mappedResource;

   NV_ENC_PIC_PARAMS pic;
   memset(&pic, 0, sizeof(pic));
   pic.version = MKVER(7) | (1u << 31);
   pic.inputWidth = width;
   pic.inputHeight = height;
   pic.inputPitch = width * 4;
   pic.inputBuffer = map.mappedResource;
   pic.outputBitstream = enc.bitstream_buf;
   pic.bufferFmt = map.mappedBufferFmt;
   pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
   NVENCSTATUS enc_status = enc.fn.nvEncEncodePicture(enc.encoder, &pic);
   if (enc_status != NV_ENC_SUCCESS && enc_status != NV_ENC_ERR_NEED_MORE_INPUT) {
      enc.fn.nvEncUnmapInputResource(enc.encoder, enc.mapped);
      enc.mapped = NULL;
      goto out;
   }

   NV_ENC_LOCK_BITSTREAM lock;
   memset(&lock, 0, sizeof(lock));
   lock.version = MKVER(2) | (1u << 31);
   lock.outputBitstream = enc.bitstream_buf;
   if (enc.fn.nvEncLockBitstream(enc.encoder, &lock) != NV_ENC_SUCCESS) {
      enc.fn.nvEncUnmapInputResource(enc.encoder, enc.mapped);
      enc.mapped = NULL;
      goto out;
   }
   uint32_t len = lock.bitstreamSizeInBytes;
   if (len > sizeof(enc.bitstream_out))
      len = sizeof(enc.bitstream_out);
   memcpy(enc.bitstream_out, lock.bitstreamBufferPtr, len);
   enc.fn.nvEncUnlockBitstream(enc.encoder, enc.bitstream_buf);
   enc.fn.nvEncUnmapInputResource(enc.encoder, enc.mapped);
   enc.mapped = NULL;

   *out_buf = enc.bitstream_out;
   *out_len = len;
   ret = 0;

out:
   if (imported_image)
      enc.DestroyImage(enc.dev, imported_image, NULL);
   if (imported_memory)
      enc.FreeMemory(enc.dev, imported_memory, NULL);
   pthread_mutex_unlock(&enc_mutex);
   return ret;
}
