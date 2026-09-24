/* Tier 7 spike: does an OPTIMAL-tiled, DRM-modifier Vulkan image - exported as
 * a dma_buf exactly the way redroid-nvidia's own vtest_gpu_alloc_gpu() already
 * does for real, GPU-only-tiled buffers - import cleanly into CUDA (as
 * OPAQUE_FD) and then into NVENC as a CUDAARRAY input, with zero CPU access
 * to the pixel data anywhere in the chain?
 *
 * If yes: encoding a real Codec2 HW_VIDEO_ENCODER-usage buffer through NVENC
 * never needs Tier 5's CPU-readback fix at all, because NVENC (a hardware
 * block) would read the driver's native tiled layout directly, the same way
 * a CPU pointer cannot. Confirmed on real hardware - see README.md.
 *
 * Build (needs FFmpeg's nv-codec-headers installed to /usr/local/include -
 * `git clone https://github.com/FFmpeg/nv-codec-headers && cd $_ && sudo make install`):
 *   gcc -O0 -g -Wall -I/usr/local/include/ffnvcodec \
 *       -o tier7_nvenc_dualexport_spike tier7_nvenc_dualexport_spike.c \
 *       -lvulkan -l:libcuda.so.1 -l:libnvidia-encode.so.1
 *
 * Run: ./tier7_nvenc_dualexport_spike - writes /tmp/tier7_spike_out.h264 on success.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vulkan/vulkan.h>
#include "dynlink_cuda.h"
#include "nvEncodeAPI.h"

#define W 1280
#define H 720
#define CHK_VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "VK FAIL %s: %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)
#define CHK_CU(x) do { CUresult r_ = (x); if (r_ != CUDA_SUCCESS) { const char *s_ = NULL; cuGetErrorString(r_, &s_); fprintf(stderr, "CUDA FAIL %s: %d (%s) (line %d)\n", #x, r_, s_ ? s_ : "?", __LINE__); exit(1); } } while (0)
static void *g_encoder_for_diag = NULL;
static NV_ENCODE_API_FUNCTION_LIST *g_fnlist_for_diag = NULL;
#define CHK_NV(x) do { NVENCSTATUS r_ = (x); if (r_ != NV_ENC_SUCCESS) { \
    fprintf(stderr, "NVENC FAIL %s: %d (line %d)\n", #x, r_, __LINE__); \
    if (g_encoder_for_diag && g_fnlist_for_diag && g_fnlist_for_diag->nvEncGetLastErrorString) \
        fprintf(stderr, "  nvEncGetLastErrorString: %s\n", g_fnlist_for_diag->nvEncGetLastErrorString(g_encoder_for_diag)); \
    exit(1); } } while (0)

extern CUresult cuInit(unsigned int);
extern CUresult cuDeviceGet(CUdevice *, int);
extern CUresult cuDeviceGetCount(int *);
extern CUresult cuCtxCreate_v2(CUcontext *, unsigned int, CUdevice);
extern CUresult cuCtxSetCurrent(CUcontext);
extern CUresult cuCtxPopCurrent_v2(CUcontext *);
extern CUresult cuImportExternalMemory(CUexternalMemory *, const CUDA_EXTERNAL_MEMORY_HANDLE_DESC *);
extern CUresult cuExternalMemoryGetMappedMipmappedArray(CUmipmappedArray *, CUexternalMemory, const CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC *);
extern CUresult cuMipmappedArrayGetLevel(CUarray *, CUmipmappedArray, unsigned int);
extern CUresult cuGetErrorString(CUresult, const char **);

int main(void)
{
    /* ---- 1. Vulkan: real device, matching vtest_gpu_alloc_image's setup ---- */
    VkInstance instance;
    const VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "tier7-spike", .apiVersion = VK_API_VERSION_1_1 };
    const VkInstanceCreateInfo inst_info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    CHK_VK(vkCreateInstance(&inst_info, NULL, &instance));

    VkPhysicalDevice devices[16];
    uint32_t count = 16;
    CHK_VK(vkEnumeratePhysicalDevices(instance, &count, devices));
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);
        fprintf(stderr, "GPU %u: %s (type %d)\n", i, props.deviceName, props.deviceType);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            phys = devices[i];
    }
    if (!phys) phys = devices[0];

    const float prio = 1.0f;
    const VkDeviceQueueCreateInfo qinfo = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    const char *dev_exts[] = { "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier" };
    const VkDeviceCreateInfo dinfo = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qinfo, .enabledExtensionCount = 3, .ppEnabledExtensionNames = dev_exts };
    VkDevice dev;
    CHK_VK(vkCreateDevice(phys, &dinfo, NULL, &dev));

    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);

    VkQueue queue;
    vkGetDeviceQueue(dev, 0, 0, &queue);

    /* ---- 2. Create the OPTIMAL-tiled image: same shape as vtest_gpu_alloc_gpu() ---- */
    const VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkDrmFormatModifierPropertiesListEXT mod_list = { .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT };
    VkFormatProperties2 fmt_props = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &mod_list };
    vkGetPhysicalDeviceFormatProperties2(phys, format, &fmt_props);
    VkDrmFormatModifierPropertiesEXT mod_props[64];
    mod_list.drmFormatModifierCount = mod_list.drmFormatModifierCount < 64 ? mod_list.drmFormatModifierCount : 64;
    mod_list.pDrmFormatModifierProperties = mod_props;
    vkGetPhysicalDeviceFormatProperties2(phys, format, &fmt_props);

    uint64_t mod_candidates[64];
    uint32_t mod_count = 0;
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    for (uint32_t i = 0; i < mod_list.drmFormatModifierCount; i++)
        if (mod_props[i].drmFormatModifierPlaneCount == 1 &&
            (mod_props[i].drmFormatModifierTilingFeatures & need) == need &&
            mod_props[i].drmFormatModifier != 0)
            mod_candidates[mod_count++] = mod_props[i].drmFormatModifier;
    fprintf(stderr, "found %u non-linear single-plane modifier(s) for B8G8R8A8_UNORM\n", mod_count);
    if (!mod_count) { fprintf(stderr, "no usable modifier - aborting\n"); return 1; }

    const VkExternalMemoryHandleTypeFlags both_handles = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT | VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;
    const VkExternalMemoryImageCreateInfo ext_info = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .handleTypes = both_handles };
    const VkImageDrmFormatModifierListCreateInfoEXT modinfo = { .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT, .pNext = &ext_info, .drmFormatModifierCount = mod_count, .pDrmFormatModifiers = mod_candidates };
    const VkImageCreateInfo imginfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &modinfo,
        .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = { W, H, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image;
    CHK_VK(vkCreateImage(dev, &imginfo, NULL, &image));

    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(dev, image, &reqs);
    uint32_t mem_type = UINT32_MAX;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
        if ((reqs.memoryTypeBits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            { mem_type = i; break; }
    if (mem_type == UINT32_MAX) mem_type = __builtin_ffs(reqs.memoryTypeBits) - 1;

    const VkExportMemoryAllocateInfo exp_info = { .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .handleTypes = both_handles };
    const VkMemoryDedicatedAllocateInfo ded_info = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &exp_info, .image = image };
    const VkMemoryAllocateInfo alloc_info = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ded_info, .allocationSize = reqs.size, .memoryTypeIndex = mem_type };
    VkDeviceMemory memory;
    CHK_VK(vkAllocateMemory(dev, &alloc_info, NULL, &memory));
    CHK_VK(vkBindImageMemory(dev, image, memory, 0));
    fprintf(stderr, "image allocated: %u bytes, DEVICE_LOCAL, DRM_FORMAT_MODIFIER tiling (OPTIMAL-equivalent)\n", (unsigned)reqs.size);

    /* ---- 3. Clear it to a known color via a real GPU command (no CPU touch) ---- */
    VkCommandPool pool;
    const VkCommandPoolCreateInfo poolinfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
    CHK_VK(vkCreateCommandPool(dev, &poolinfo, NULL, &pool));
    VkCommandBuffer cmd;
    const VkCommandBufferAllocateInfo cbinfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    CHK_VK(vkAllocateCommandBuffers(dev, &cbinfo, &cmd));

    const VkCommandBufferBeginInfo begininfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHK_VK(vkBeginCommandBuffer(cmd, &begininfo));
    VkImageMemoryBarrier to_dst = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_dst);
    /* distinctive color: B=40,G=180,R=220,A=255 as bytes (B8G8R8A8) -> visually a warm orange */
    const VkClearColorValue clear = { .float32 = { 220.0f/255, 180.0f/255, 40.0f/255, 1.0f } };
    const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    VkImageMemoryBarrier to_general = to_dst;
    to_general.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_general.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; to_general.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &to_general);
    CHK_VK(vkEndCommandBuffer(cmd));

    const VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    CHK_VK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    CHK_VK(vkQueueWaitIdle(queue));
    fprintf(stderr, "cleared to (220,180,40) via vkCmdClearColorImage, GPU-side only\n");

    /* ---- 4. Export as dma_buf exactly like vtest_gpu_alloc_gpu() does ---- */
    PFN_vkGetMemoryFdKHR GetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
    const VkMemoryGetFdInfoKHR fdinfo = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = memory, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    int dmabuf_fd = -1;
    CHK_VK(GetMemoryFdKHR(dev, &fdinfo, &dmabuf_fd));
    fprintf(stderr, "exported dma_buf fd=%d (VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT), size=%u\n", dmabuf_fd, (unsigned)reqs.size);

    const VkMemoryGetFdInfoKHR fdinfo2 = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = memory, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR };
    int opaque_fd = -1;
    VkResult opaque_export = GetMemoryFdKHR(dev, &fdinfo2, &opaque_fd);
    if (opaque_export != VK_SUCCESS) {
        fprintf(stderr, "second export as OPAQUE_FD_BIT FAILED: %d - driver doesn't allow dual-handle-type export for this image, falling back to the DMA_BUF fd for the CUDA import test\n", opaque_export);
        opaque_fd = dmabuf_fd;
    } else {
        fprintf(stderr, "ALSO exported the SAME memory as opaque_fd=%d (VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR)\n", opaque_fd);
    }

    /* ---- 5. THE SPIKE: import that exact fd into CUDA as OPAQUE_FD ---- */
    CHK_CU(cuInit(0));
    int ndev = 0;
    CHK_CU(cuDeviceGetCount(&ndev));
    CUdevice cudev;
    CHK_CU(cuDeviceGet(&cudev, 0));
    CUcontext cuctx;
    CHK_CU(cuCtxCreate_v2(&cuctx, 0, cudev));

    CUDA_EXTERNAL_MEMORY_HANDLE_DESC memdesc;
    memset(&memdesc, 0, sizeof(memdesc));
    memdesc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    memdesc.handle.fd = opaque_fd; /* CUDA takes ownership of the fd on success */
    memdesc.size = reqs.size;
    CUexternalMemory extmem;
    CUresult impres = cuImportExternalMemory(&extmem, &memdesc);
    if (impres != CUDA_SUCCESS) {
        const char *s = NULL; cuGetErrorString(impres, &s);
        fprintf(stderr, "\n*** RESULT: cuImportExternalMemory(OPAQUE_FD) on fd=%d FAILED: %d (%s) ***\n", opaque_fd, impres, s ? s : "?");
        fprintf(stderr, "Neither the plain DMA_BUF fd nor a same-allocation OPAQUE_FD-tagged fd import into CUDA here.\n");
        return 2;
    }
    fprintf(stderr, "cuImportExternalMemory OK on fd=%d\n", opaque_fd);

    CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC arrdesc;
    memset(&arrdesc, 0, sizeof(arrdesc));
    arrdesc.arrayDesc.Width = W;
    arrdesc.arrayDesc.Height = H;
    arrdesc.arrayDesc.Depth = 0;
    arrdesc.arrayDesc.Format = CU_AD_FORMAT_UNSIGNED_INT8;
    arrdesc.arrayDesc.NumChannels = 4;
    arrdesc.arrayDesc.Flags = CUDA_ARRAY3D_SURFACE_LDST | CUDA_ARRAY3D_VIDEO_ENCODE_DECODE;
    arrdesc.numLevels = 1;
    CUmipmappedArray mip;
    CUresult mapres = cuExternalMemoryGetMappedMipmappedArray(&mip, extmem, &arrdesc);
    if (mapres != CUDA_SUCCESS) {
        const char *s = NULL; cuGetErrorString(mapres, &s);
        fprintf(stderr, "\n*** RESULT: cuExternalMemoryGetMappedMipmappedArray FAILED: %d (%s) ***\n", mapres, s ? s : "?");
        fprintf(stderr, "The fd imported, but the driver rejects treating this OPTIMAL/tiled allocation as a plain\n");
        fprintf(stderr, "linear array-backed external memory object - likely needs the modifier surfaced explicitly.\n");
        return 3;
    }
    CUarray cuarr;
    CHK_CU(cuMipmappedArrayGetLevel(&cuarr, mip, 0));
    fprintf(stderr, "cuExternalMemoryGetMappedMipmappedArray + cuMipmappedArrayGetLevel OK -> real CUarray\n");

    /* ---- 6. Feed that CUarray straight into NVENC, no CPU access anywhere ---- */
    uint32_t max_supported = 0;
    CHK_NV(NvEncodeAPIGetMaxSupportedVersion(&max_supported));
    uint32_t drv_major = max_supported >> 4, drv_minor = max_supported & 0xF;
    fprintf(stderr, "driver max supported NVENC API: %u.%u (header built against %u.%u)\n",
            drv_major, drv_minor, (unsigned)NVENCAPI_MAJOR_VERSION, (unsigned)NVENCAPI_MINOR_VERSION);
    const uint32_t drv_api_version = drv_major | (drv_minor << 24);
    /* Rebuild struct-version tags against the driver's own reported minor version instead
     * of the header's (newer, 13.1) - the header's NV_ENC_*_VER macros bake in NVENCAPI_VERSION
     * at compile time, which this driver's libnvidia-encode (13.0) rejects with
     * NV_ENC_ERR_INVALID_VERSION. Also drop the extra (1u<<31) "extended" bit some 13.1 macros
     * add - not present in 13.0's own versioning scheme. */
#define MKVER(structver) (drv_api_version | ((uint32_t)(structver) << 16) | (0x7u << 28))

    /* NVENC manages pushing/popping the context itself - it must be handed a
     * floating context, not one left current on this thread (NVIDIA's own
     * NvEncoderCuda sample does exactly this after finishing CUDA-side setup). */
    CUcontext popped;
    CHK_CU(cuCtxPopCurrent_v2(&popped));

    NV_ENCODE_API_FUNCTION_LIST fnlist;
    memset(&fnlist, 0, sizeof(fnlist));
    fnlist.version = MKVER(2);
    CHK_NV(NvEncodeAPICreateInstance(&fnlist));

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sess;
    memset(&sess, 0, sizeof(sess));
    sess.version = MKVER(1);
    sess.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    sess.device = cuctx;
    sess.apiVersion = drv_api_version;
    void *encoder = NULL;
    CHK_NV(fnlist.nvEncOpenEncodeSessionEx(&sess, &encoder));
    g_encoder_for_diag = encoder;
    g_fnlist_for_diag = &fnlist;
    fprintf(stderr, "NVENC session opened against the same CUDA context\n");

    NV_ENC_PRESET_CONFIG presetcfg;
    memset(&presetcfg, 0, sizeof(presetcfg));
    presetcfg.version = MKVER(5) | (1u<<31);
    presetcfg.presetCfg.version = MKVER(9) | (1u<<31);
    CHK_NV(fnlist.nvEncGetEncodePresetConfigEx(encoder, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P3_GUID, NV_ENC_TUNING_INFO_LOW_LATENCY, &presetcfg));

    NV_ENC_INITIALIZE_PARAMS init;
    memset(&init, 0, sizeof(init));
    init.version = MKVER(7) | (1u<<31);
    init.encodeGUID = NV_ENC_CODEC_H264_GUID;
    init.presetGUID = NV_ENC_PRESET_P3_GUID;
    init.encodeWidth = W;
    init.encodeHeight = H;
    init.darWidth = W;
    init.darHeight = H;
    init.frameRateNum = 30;
    init.frameRateDen = 1;
    init.enablePTD = 1;
    init.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    init.encodeConfig = &presetcfg.presetCfg;
    CHK_NV(fnlist.nvEncInitializeEncoder(encoder, &init));
    fprintf(stderr, "NVENC encoder initialized: H264, %dx%d\n", W, H);

    NV_ENC_REGISTER_RESOURCE reg;
    memset(&reg, 0, sizeof(reg));
    reg.version = MKVER(5);
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY;
    reg.width = W;
    reg.height = H;
    reg.pitch = W * 4;
    reg.resourceToRegister = cuarr;
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;
    CHK_NV(fnlist.nvEncRegisterResource(encoder, &reg));
    fprintf(stderr, "NVENC registered the CUarray as input resource - no readback, no CPU copy\n");

    NV_ENC_MAP_INPUT_RESOURCE map;
    memset(&map, 0, sizeof(map));
    map.version = MKVER(4);
    map.registeredResource = reg.registeredResource;
    CHK_NV(fnlist.nvEncMapInputResource(encoder, &map));

    NV_ENC_CREATE_BITSTREAM_BUFFER bsbuf;
    memset(&bsbuf, 0, sizeof(bsbuf));
    bsbuf.version = MKVER(1);
    CHK_NV(fnlist.nvEncCreateBitstreamBuffer(encoder, &bsbuf));

    NV_ENC_PIC_PARAMS pic;
    memset(&pic, 0, sizeof(pic));
    pic.version = MKVER(7) | (1u<<31);
    pic.inputWidth = W;
    pic.inputHeight = H;
    pic.inputPitch = W * 4;
    pic.inputBuffer = map.mappedResource;
    pic.outputBitstream = bsbuf.bitstreamBuffer;
    pic.bufferFmt = map.mappedBufferFmt;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    CHK_NV(fnlist.nvEncEncodePicture(encoder, &pic));
    fprintf(stderr, "nvEncEncodePicture OK\n");

    NV_ENC_LOCK_BITSTREAM lock;
    memset(&lock, 0, sizeof(lock));
    lock.version = MKVER(2) | (1u<<31);
    lock.outputBitstream = bsbuf.bitstreamBuffer;
    CHK_NV(fnlist.nvEncLockBitstream(encoder, &lock));
    fprintf(stderr, "encoded %u bytes of H.264\n", lock.bitstreamSizeInBytes);

    FILE *f = fopen("/tmp/tier7_spike_out.h264", "wb");
    fwrite(lock.bitstreamBufferPtr, 1, lock.bitstreamSizeInBytes, f);
    fclose(f);
    CHK_NV(fnlist.nvEncUnlockBitstream(encoder, bsbuf.bitstreamBuffer));

    fprintf(stderr, "\n*** RESULT: full path OK - Vulkan OPTIMAL/tiled dma_buf -> CUDA OPAQUE_FD import ->\n");
    fprintf(stderr, "CUarray -> NVENC CUDAARRAY input -> real H.264, with zero CPU reads of pixel data. ***\n");
    fprintf(stderr, "Wrote /tmp/tier7_spike_out.h264 - decode it and check the color is (220,180,40).\n");

    /* Leaving the NVENC session/CUDA context open at process exit hangs the
     * driver's teardown (observed directly on real hardware) - destroy it
     * explicitly rather than relying on process exit to clean up. */
    fnlist.nvEncDestroyEncoder(encoder);
    return 0;
}
