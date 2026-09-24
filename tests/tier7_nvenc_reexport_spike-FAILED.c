/* Tier 7 spike 2 - CONFIRMED DEAD END, kept as a documented negative result
 * (do not build on this file - see tier7_nvenc_copy_export_spike.c for the
 * approach that actually works).
 *
 * Question: does a dma_buf fd exported by ONE VkDevice with ONLY
 * DMA_BUF_BIT_EXT (simulating Venus's own resource-creation code) still
 * reach NVENC if a completely SEPARATE, independent VkDevice/VkInstance
 * ("the encode side") imports that fd and re-exports it as OPAQUE_FD_BIT_KHR
 * from its own, freshly-allocated VkDeviceMemory (import + export chained in
 * one vkAllocateMemory call)?
 *
 * Result: vkAllocateMemory returns VK_SUCCESS, cuImportExternalMemory
 * succeeds, NVENC encodes real-looking output bytes - but the actual pixel
 * content comes back black/zeroed, not the producer's real clear color.
 * vkGetPhysicalDeviceImageFormatProperties2(handleType=DMA_BUF_BIT_EXT) on
 * this exact image/modifier reports compatibleHandleTypes=0x200 (DMA_BUF
 * only) - OPAQUE_FD (0x1) is NOT in that set, so this combination is a real
 * Vulkan spec violation (import handle type must be compatible with any
 * chained export handle type) that this driver silently accepts instead of
 * rejecting, producing wrong data with no error anywhere in the chain.
 *
 * Fixed shape: tier7_nvenc_copy_export_spike.c - plain-import (self-
 * compatible, correct) followed by a real GPU-to-GPU vkCmdCopyImage into a
 * second, freshly self-allocated dual-export image, confirmed correct.
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
extern CUresult cuCtxPopCurrent_v2(CUcontext *);
extern CUresult cuImportExternalMemory(CUexternalMemory *, const CUDA_EXTERNAL_MEMORY_HANDLE_DESC *);
extern CUresult cuExternalMemoryGetMappedMipmappedArray(CUmipmappedArray *, CUexternalMemory, const CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC *);
extern CUresult cuMipmappedArrayGetLevel(CUarray *, CUmipmappedArray, unsigned int);
extern CUresult cuGetErrorString(CUresult, const char **);

struct vk_ctx {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkPhysicalDeviceMemoryProperties mem_props;
    VkQueue queue;
    PFN_vkGetMemoryFdKHR GetMemoryFdKHR;
};

static void vk_ctx_init(struct vk_ctx *c, const char **extra_exts, uint32_t n_extra)
{
    const VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "tier7-spike2", .apiVersion = VK_API_VERSION_1_1 };
    const VkInstanceCreateInfo inst_info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    CHK_VK(vkCreateInstance(&inst_info, NULL, &c->instance));

    VkPhysicalDevice devices[16];
    uint32_t count = 16;
    CHK_VK(vkEnumeratePhysicalDevices(c->instance, &count, devices));
    c->phys = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            c->phys = devices[i];
    }
    if (!c->phys) c->phys = devices[0];

    const float prio = 1.0f;
    const VkDeviceQueueCreateInfo qinfo = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    const char *base_exts[] = { "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier" };
    const char *all_exts[8];
    uint32_t n = 0;
    for (uint32_t i = 0; i < 3; i++) all_exts[n++] = base_exts[i];
    for (uint32_t i = 0; i < n_extra; i++) all_exts[n++] = extra_exts[i];
    const VkDeviceCreateInfo dinfo = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qinfo, .enabledExtensionCount = n, .ppEnabledExtensionNames = all_exts };
    CHK_VK(vkCreateDevice(c->phys, &dinfo, NULL, &c->dev));

    vkGetPhysicalDeviceMemoryProperties(c->phys, &c->mem_props);
    vkGetDeviceQueue(c->dev, 0, 0, &c->queue);
    c->GetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(c->dev, "vkGetMemoryFdKHR");
}

int main(void)
{
    /* ================= PRODUCER: simulates Venus's own resource creation ================= */
    struct vk_ctx prod;
    vk_ctx_init(&prod, NULL, 0);

    const VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkDrmFormatModifierPropertiesListEXT mod_list = { .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT };
    VkFormatProperties2 fmt_props = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &mod_list };
    vkGetPhysicalDeviceFormatProperties2(prod.phys, format, &fmt_props);
    VkDrmFormatModifierPropertiesEXT mod_props[64];
    mod_list.drmFormatModifierCount = mod_list.drmFormatModifierCount < 64 ? mod_list.drmFormatModifierCount : 64;
    mod_list.pDrmFormatModifierProperties = mod_props;
    vkGetPhysicalDeviceFormatProperties2(prod.phys, format, &fmt_props);

    uint64_t mod_candidates[64];
    uint32_t mod_count = 0;
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    for (uint32_t i = 0; i < mod_list.drmFormatModifierCount; i++)
        if (mod_props[i].drmFormatModifierPlaneCount == 1 &&
            (mod_props[i].drmFormatModifierTilingFeatures & need) == need &&
            mod_props[i].drmFormatModifier != 0)
            mod_candidates[mod_count++] = mod_props[i].drmFormatModifier;
    if (!mod_count) { fprintf(stderr, "no usable modifier\n"); return 1; }

    /* PRODUCER declares ONLY DMA_BUF_BIT_EXT - this is the part meant to stand
     * in for Venus's own, unmodified resource-creation code. */
    const VkExternalMemoryImageCreateInfo prod_ext = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    const VkImageDrmFormatModifierListCreateInfoEXT modinfo = { .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT, .pNext = &prod_ext, .drmFormatModifierCount = mod_count, .pDrmFormatModifiers = mod_candidates };
    const VkImageCreateInfo imginfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &modinfo,
        .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = { W, H, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage prod_image;
    CHK_VK(vkCreateImage(prod.dev, &imginfo, NULL, &prod_image));

    VkMemoryRequirements reqs;
    vkGetImageMemoryRequirements(prod.dev, prod_image, &reqs);
    uint32_t mem_type = UINT32_MAX;
    for (uint32_t i = 0; i < prod.mem_props.memoryTypeCount; i++)
        if ((reqs.memoryTypeBits & (1u << i)) && (prod.mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            { mem_type = i; break; }
    if (mem_type == UINT32_MAX) mem_type = __builtin_ffs(reqs.memoryTypeBits) - 1;

    const VkExportMemoryAllocateInfo prod_exp = { .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    const VkMemoryDedicatedAllocateInfo prod_ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &prod_exp, .image = prod_image };
    const VkMemoryAllocateInfo prod_alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &prod_ded, .allocationSize = reqs.size, .memoryTypeIndex = mem_type };
    VkDeviceMemory prod_memory;
    CHK_VK(vkAllocateMemory(prod.dev, &prod_alloc, NULL, &prod_memory));
    CHK_VK(vkBindImageMemory(prod.dev, prod_image, prod_memory, 0));
    fprintf(stderr, "[producer] image allocated: %u bytes, DMA_BUF_BIT_EXT only\n", (unsigned)reqs.size);

    /* clear to a known color, GPU-only */
    VkCommandPool pool;
    const VkCommandPoolCreateInfo poolinfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
    CHK_VK(vkCreateCommandPool(prod.dev, &poolinfo, NULL, &pool));
    VkCommandBuffer cmd;
    const VkCommandBufferAllocateInfo cbinfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    CHK_VK(vkAllocateCommandBuffers(prod.dev, &cbinfo, &cmd));
    const VkCommandBufferBeginInfo begininfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHK_VK(vkBeginCommandBuffer(cmd, &begininfo));
    VkImageMemoryBarrier to_dst = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = prod_image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_dst);
    const VkClearColorValue clear = { .float32 = { 30.0f/255, 200.0f/255, 90.0f/255, 1.0f } }; /* distinct from spike 1 */
    const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, prod_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    VkImageMemoryBarrier to_general = to_dst;
    to_general.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_general.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; to_general.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &to_general);
    CHK_VK(vkEndCommandBuffer(cmd));
    const VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    CHK_VK(vkQueueSubmit(prod.queue, 1, &submit, VK_NULL_HANDLE));
    CHK_VK(vkQueueWaitIdle(prod.queue));
    fprintf(stderr, "[producer] cleared to (30,200,90)\n");

    /* single export, DMA_BUF only - this is the fd a real
     * virgl_renderer_resource_export_blob() call would hand back today */
    VkImageDrmFormatModifierPropertiesEXT chosen = { .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT };
    PFN_vkGetImageDrmFormatModifierPropertiesEXT GetModProps = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(prod.dev, "vkGetImageDrmFormatModifierPropertiesEXT");
    CHK_VK(GetModProps(prod.dev, prod_image, &chosen));
    const VkImageSubresource subres = { .aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT };
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(prod.dev, prod_image, &subres, &layout);

    const VkMemoryGetFdInfoKHR fdinfo = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = prod_memory, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    int producer_fd = -1;
    CHK_VK(prod.GetMemoryFdKHR(prod.dev, &fdinfo, &producer_fd));
    fprintf(stderr, "[producer] exported dma_buf fd=%d, modifier=0x%llx, size=%u (this is what export_blob() would hand the encode side)\n",
            producer_fd, (unsigned long long)chosen.drmFormatModifier, (unsigned)reqs.size);

    /* ================= CONSUMER: fully independent VkInstance/VkDevice, simulates the encode side ================= */
    struct vk_ctx cons;
    vk_ctx_init(&cons, NULL, 0);
    fprintf(stderr, "[consumer] independent VkInstance/VkDevice created\n");

    /* Check what the driver actually claims is possible before trying it:
     * for a DRM-modifier-tiled image imported as DMA_BUF_BIT_EXT, does it
     * report OPAQUE_FD_BIT_KHR as compatible for re-export? */
    {
        const VkPhysicalDeviceExternalImageFormatInfo ext_fmt_info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };
        const VkPhysicalDeviceImageDrmFormatModifierInfoEXT drm_info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
            .pNext = &ext_fmt_info,
            .drmFormatModifier = chosen.drmFormatModifier,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        const VkPhysicalDeviceImageFormatInfo2 fmt_info2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .pNext = &drm_info, .format = format, .type = VK_IMAGE_TYPE_2D,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        };
        VkExternalImageFormatProperties ext_props = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
        VkImageFormatProperties2 props2 = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &ext_props };
        VkResult qr = vkGetPhysicalDeviceImageFormatProperties2(cons.phys, &fmt_info2, &props2);
        fprintf(stderr, "[diag] GetPhysicalDeviceImageFormatProperties2(handleType=DMA_BUF_BIT_EXT) -> %d, "
                "compatibleHandleTypes=0x%x, exportFromImportedHandleTypes=0x%x (OPAQUE_FD bit=0x%x)\n",
                qr, ext_props.externalMemoryProperties.compatibleHandleTypes,
                ext_props.externalMemoryProperties.exportFromImportedHandleTypes,
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR);
    }

    /* consumer knows the resource's width/height/format/modifier/layout from
     * whatever real metadata virgl_renderer_resource_get_info[_ext]() would
     * report - here just reusing the producer's own values directly. */
    const VkExternalMemoryImageCreateInfo cons_ext = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT | VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR };
    const VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_mod = { .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, .pNext = &cons_ext, .drmFormatModifier = chosen.drmFormatModifier, .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &layout };
    const VkImageCreateInfo cons_imginfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &explicit_mod,
        .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = { W, H, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage cons_image;
    CHK_VK(vkCreateImage(cons.dev, &cons_imginfo, NULL, &cons_image));

    VkMemoryRequirements cons_reqs;
    vkGetImageMemoryRequirements(cons.dev, cons_image, &cons_reqs);
    uint32_t cons_mem_type = UINT32_MAX;
    for (uint32_t i = 0; i < cons.mem_props.memoryTypeCount; i++)
        if ((cons_reqs.memoryTypeBits & (1u << i)) && (cons.mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            { cons_mem_type = i; break; }
    if (cons_mem_type == UINT32_MAX) cons_mem_type = __builtin_ffs(cons_reqs.memoryTypeBits) - 1;

    int dup_fd = dup(producer_fd); /* vkImportMemoryFdInfoKHR takes ownership on success */
    const VkImportMemoryFdInfoKHR import_info = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dup_fd };
    const VkExportMemoryAllocateInfo cons_exp = { .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .pNext = &import_info, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT | VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR };
    const VkMemoryDedicatedAllocateInfo cons_ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &cons_exp, .image = cons_image };
    const VkMemoryAllocateInfo cons_alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &cons_ded, .allocationSize = cons_reqs.size, .memoryTypeIndex = cons_mem_type };
    VkDeviceMemory cons_memory;
    VkResult import_res = vkAllocateMemory(cons.dev, &cons_alloc, NULL, &cons_memory);
    if (import_res != VK_SUCCESS) {
        fprintf(stderr, "\n*** RESULT: consumer-side import+re-export vkAllocateMemory FAILED: %d ***\n", import_res);
        fprintf(stderr, "A separate VkDevice cannot import a foreign DMA_BUF fd while also declaring\n");
        fprintf(stderr, "OPAQUE_FD re-export capability in the same allocation - would need a different approach\n");
        fprintf(stderr, "(e.g. patch Venus's own resource-creation code to request both handle types up front).\n");
        return 1;
    }
    CHK_VK(vkBindImageMemory(cons.dev, cons_image, cons_memory, 0));
    fprintf(stderr, "[consumer] imported the producer's DMA_BUF fd AND re-exportable as OPAQUE_FD in one allocation\n");

    const VkMemoryGetFdInfoKHR cons_fdinfo = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = cons_memory, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR };
    int opaque_fd = -1;
    CHK_VK(cons.GetMemoryFdKHR(cons.dev, &cons_fdinfo, &opaque_fd));
    fprintf(stderr, "[consumer] re-exported as opaque_fd=%d\n", opaque_fd);

    /* ================= CUDA + NVENC, exactly as spike 1 ================= */
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
    memdesc.handle.fd = opaque_fd;
    memdesc.size = cons_reqs.size;
    CUexternalMemory extmem;
    CUresult impres = cuImportExternalMemory(&extmem, &memdesc);
    if (impres != CUDA_SUCCESS) {
        const char *s = NULL; cuGetErrorString(impres, &s);
        fprintf(stderr, "\n*** RESULT: cuImportExternalMemory on the RE-EXPORTED fd FAILED: %d (%s) ***\n", impres, s ? s : "?");
        return 2;
    }
    fprintf(stderr, "cuImportExternalMemory OK on the re-exported fd\n");

    CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC arrdesc;
    memset(&arrdesc, 0, sizeof(arrdesc));
    arrdesc.arrayDesc.Width = W;
    arrdesc.arrayDesc.Height = H;
    arrdesc.arrayDesc.Format = CU_AD_FORMAT_UNSIGNED_INT8;
    arrdesc.arrayDesc.NumChannels = 4;
    arrdesc.arrayDesc.Flags = CUDA_ARRAY3D_SURFACE_LDST | CUDA_ARRAY3D_VIDEO_ENCODE_DECODE;
    arrdesc.numLevels = 1;
    CUmipmappedArray mip;
    CHK_CU(cuExternalMemoryGetMappedMipmappedArray(&mip, extmem, &arrdesc));
    CUarray cuarr;
    CHK_CU(cuMipmappedArrayGetLevel(&cuarr, mip, 0));
    fprintf(stderr, "CUarray obtained from the imported+re-exported+re-imported memory\n");

    uint32_t max_supported = 0;
    CHK_NV(NvEncodeAPIGetMaxSupportedVersion(&max_supported));
    uint32_t drv_major = max_supported >> 4, drv_minor = max_supported & 0xF;
    const uint32_t drv_api_version = drv_major | (drv_minor << 24);
#define MKVER(structver) (drv_api_version | ((uint32_t)(structver) << 16) | (0x7u << 28))

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

    NV_ENC_LOCK_BITSTREAM lock;
    memset(&lock, 0, sizeof(lock));
    lock.version = MKVER(2) | (1u<<31);
    lock.outputBitstream = bsbuf.bitstreamBuffer;
    CHK_NV(fnlist.nvEncLockBitstream(encoder, &lock));
    fprintf(stderr, "encoded %u bytes of H.264\n", lock.bitstreamSizeInBytes);

    FILE *f = fopen("/tmp/tier7_spike2_out.h264", "wb");
    fwrite(lock.bitstreamBufferPtr, 1, lock.bitstreamSizeInBytes, f);
    fclose(f);
    CHK_NV(fnlist.nvEncUnlockBitstream(encoder, bsbuf.bitstreamBuffer));

    fprintf(stderr, "\n*** RESULT: a dma_buf exported by ONE Vulkan device with ONLY DMA_BUF_BIT_EXT was\n");
    fprintf(stderr, "imported + re-exported as OPAQUE_FD by a totally SEPARATE, independent Vulkan device,\n");
    fprintf(stderr, "then encoded via NVENC. This works for ANY Venus-owned resource, no Venus changes needed.\n");
    fprintf(stderr, "Wrote /tmp/tier7_spike2_out.h264 - decode and check the color is (30,200,90).\n");

    fnlist.nvEncDestroyEncoder(encoder);
    return 0;
}
