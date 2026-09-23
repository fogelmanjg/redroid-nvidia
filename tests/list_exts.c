/*
 * Standalone Venus device-extension probe. Reuses the exact same connection
 * method as Tier 3's vnprobe.c (no Android boot needed) to check what the
 * host's virgl_test_server/virglrenderer actually forwards to the guest
 * over the vtest protocol, as opposed to what the real NVIDIA driver
 * supports locally (`vulkaninfo` on the host) - the two are negotiated
 * separately and can differ.
 *
 * Build:
 *   gcc -O1 -o list_exts list_exts.c $(pkg-config --cflags --libs vulkan)
 *
 * Run (against a virgl_test_server already listening on the given socket):
 *   VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/virtio_icd.json \
 *   VN_DEBUG=vtest VTEST_SOCKET_NAME=/path/to/venus.sock ./list_exts
 *
 * Used in Tier 4 to confirm EXT_image_drm_format_modifier and
 * EXT_queue_family_foreign - the two prerequisites
 * vn_physical_device_get_native_extensions() checks before advertising
 * ANDROID_external_memory_android_hardware_buffer - both reach the guest
 * correctly. See DEVLOG.md's 2026-09-23 entries.
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { fprintf(stderr, "no instance\n"); return 1; }

    uint32_t n = 1; VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) { fprintf(stderr, "no device\n"); return 1; }

    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    printf("device: %s\n", props.deviceName);

    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ext_count, NULL);
    VkExtensionProperties *exts = malloc(sizeof(*exts) * ext_count);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ext_count, exts);
    printf("device extensions: %u\n", ext_count);
    int found_modifier = 0, found_foreign = 0, found_ahb = 0;
    for (uint32_t i = 0; i < ext_count; i++) {
        printf("  %s\n", exts[i].extensionName);
        if (!strcmp(exts[i].extensionName, "VK_EXT_image_drm_format_modifier")) found_modifier = 1;
        if (!strcmp(exts[i].extensionName, "VK_EXT_queue_family_foreign")) found_foreign = 1;
        if (!strcmp(exts[i].extensionName, "VK_ANDROID_external_memory_android_hardware_buffer")) found_ahb = 1;
    }
    printf("\nEXT_image_drm_format_modifier: %s\n", found_modifier ? "YES" : "NO");
    printf("EXT_queue_family_foreign: %s\n", found_foreign ? "YES" : "NO");
    printf("ANDROID_external_memory_android_hardware_buffer: %s\n", found_ahb ? "YES" : "NO");
    return 0;
}
