//#define LOG_NDEBUG 0
#define LOG_TAG "NvencEncComponent"

#include "NvencEncComponent.h"

#include <cros_gralloc_handle.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include <log/log.h>
#include <media/stagefright/MediaDefs.h>
#include <util/C2InterfaceHelper.h>

#include "vtest_encode_client.h"

namespace android {

// cros_gralloc/cros_gralloc_helpers.h's own constant - not pulled in via that
// header directly since the rest of it is cros_gralloc's internal driver
// API, not meant for an external consumer; this one value is the stable,
// public part of the wire format cros_gralloc_handle_t itself already is.
static constexpr uint32_t kCrosGrallocMagic = 0xABCDDCBA;

NvencEncInterface::NvencEncInterface(const std::shared_ptr<C2ReflectorHelper> &helper)
    : SimpleInterface<void>::BaseParams(helper, "c2.hardware.encoder.h264",
                                         C2Component::KIND_ENCODER, C2Component::DOMAIN_VIDEO,
                                         MEDIA_MIMETYPE_VIDEO_AVC) {
    noPrivateBuffers();
    noInputReferences();
    noOutputReferences();
    noTimeStretch();
    setDerivedInstance(this);

    addParameter(DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
                         .withDefault(new C2StreamPictureSizeInfo::input(0u, 1280, 720))
                         .withFields({
                                 C2F(mSize, width).inRange(2, 3840, 2),
                                 C2F(mSize, height).inRange(2, 2160, 2),
                         })
                         .withSetter(SizeSetter)
                         .build());

    addParameter(DefineParam(mInputUsage, C2_PARAMKEY_INPUT_STREAM_USAGE)
                         .withConstValue(new C2StreamUsageTuning::input(
                                 0u,
                                 static_cast<uint64_t>(
                                         android::hardware::graphics::common::V1_0::BufferUsage::
                                                 VIDEO_ENCODER)))
                         .build());

    addParameter(DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                         .withDefault(new C2StreamProfileLevelInfo::output(
                                 0u, PROFILE_AVC_CONSTRAINED_BASELINE, LEVEL_AVC_3))
                         .withFields({
                                 C2F(mProfileLevel, profile).oneOf({
                                         PROFILE_AVC_CONSTRAINED_BASELINE,
                                 }),
                                 C2F(mProfileLevel, level).oneOf({
                                         LEVEL_AVC_3,
                                 }),
                         })
                         .withSetter(ProfileLevelSetter)
                         .build());
}

C2R NvencEncInterface::ProfileLevelSetter(bool mayBlock, C2P<C2StreamProfileLevelInfo::output> &me) {
    (void)mayBlock;
    (void)me;
    return C2R::Ok();
}

C2R NvencEncInterface::SizeSetter(bool mayBlock, const C2P<C2StreamPictureSizeInfo::input> &oldMe,
                                   C2P<C2StreamPictureSizeInfo::input> &me) {
    (void)mayBlock;
    C2R res = C2R::Ok();
    if (!me.F(me.v.width).supportsAtAll(me.v.width)) {
        res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.width)));
        me.set().width = oldMe.v.width;
    }
    if (!me.F(me.v.height).supportsAtAll(me.v.height)) {
        res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.height)));
        me.set().height = oldMe.v.height;
    }
    return res;
}

NvencEncComponent::NvencEncComponent(const char *name, c2_node_id_t id,
                                      const std::shared_ptr<NvencEncInterface> &intf)
    : SimpleC2Component(std::make_shared<SimpleInterface<NvencEncInterface>>(name, id, intf)),
      mIntf(intf) {}

NvencEncComponent::~NvencEncComponent() {
    if (mRenderNodeFd >= 0) close(mRenderNodeFd);
}

c2_status_t NvencEncComponent::onInit() {
    return C2_OK;
}

c2_status_t NvencEncComponent::onStop() {
    return C2_OK;
}

void NvencEncComponent::onReset() {}

void NvencEncComponent::onRelease() {}

c2_status_t NvencEncComponent::onFlush_sm() {
    return C2_OK;
}

c2_status_t NvencEncComponent::drain(uint32_t drainMode, const std::shared_ptr<C2BlockPool> &pool) {
    (void)drainMode;
    (void)pool;
    return C2_OK;
}

int NvencEncComponent::renderNodeFd() {
    if (mRenderNodeFd >= 0) return mRenderNodeFd;

    // Same convention as minigbm's own nvidia_venus.c / this project's
    // gpu_config.sh: androidboot.redroid_gpu_node (surfaced here as
    // ro.boot.redroid_gpu_node), falling back to the fixed path this
    // project's own deployment already uses (see README.md's Tier 1 entry).
    char path[PROP_VALUE_MAX];
    if (__system_property_get("ro.boot.redroid_gpu_node", path) <= 0)
        strncpy(path, "/dev/dri/renderD128", sizeof(path) - 1);

    mRenderNodeFd = open(path, O_RDWR | O_CLOEXEC);
    if (mRenderNodeFd < 0)
        ALOGE("open(%s) failed: %s", path, strerror(errno));
    return mRenderNodeFd;
}

void NvencEncComponent::process(const std::unique_ptr<C2Work> &work,
                                 const std::shared_ptr<C2BlockPool> &pool) {
    work->result = C2_OK;
    work->workletsProcessed = 0u;
    work->worklets.front()->output.flags = work->input.flags;

    if (work->input.buffers.empty()) {
        work->workletsProcessed = 1u;
        return;
    }

    std::shared_ptr<C2Buffer> inputBuffer = work->input.buffers[0];
    if (inputBuffer->data().graphicBlocks().empty()) {
        ALOGE("input C2Buffer has no graphic block");
        work->result = C2_CORRUPTED;
        work->workletsProcessed = 1u;
        return;
    }

    // Same fd-extraction spirit as VaapiEncComponent's own process() (and
    // v4l2_codec2's createInputFrame() before it): read the native_handle_t
    // directly, no block.map()/layout() call. Unlike VaapiEncComponent,
    // this project's actual gralloc HAL (minigbm's cros_gralloc, confirmed
    // active in Tier 4) produces a real, known cros_gralloc_handle_t for
    // every buffer - no empirical reverse-engineering needed here, just the
    // same validation cros_gralloc_convert_handle() itself does.
    C2ConstGraphicBlock block = inputBuffer->data().graphicBlocks().front();
    const C2Handle *const handle = block.handle();
    if (!handle) {
        ALOGE("input graphic block has no handle");
        work->result = C2_CORRUPTED;
        work->workletsProcessed = 1u;
        return;
    }
    if (sizeof(native_handle_t) + sizeof(int) * (size_t)(handle->numFds + handle->numInts) !=
        sizeof(cros_gralloc_handle)) {
        ALOGE("input handle isn't a cros_gralloc_handle (numFds=%d numInts=%d, expected total "
              "%zu ints for a real cros_gralloc_handle)",
              handle->numFds, handle->numInts,
              (sizeof(cros_gralloc_handle) - sizeof(native_handle_t)) / sizeof(int));
        {
            const int32_t *raw = handle->data;
            std::string fdsDump, intsDump;
            for (int i = 0; i < handle->numFds; i++) {
                char buf[16];
                snprintf(buf, sizeof(buf), "%d ", raw[i]);
                fdsDump += buf;
            }
            for (int i = 0; i < handle->numInts; i++) {
                char buf[16];
                snprintf(buf, sizeof(buf), "0x%x ", raw[handle->numFds + i]);
                intsDump += buf;
            }
            ALOGE("raw handle dump: fds=[%s] ints=[%s]", fdsDump.c_str(), intsDump.c_str());
        }
        work->result = C2_CORRUPTED;
        work->workletsProcessed = 1u;
        return;
    }
    auto hnd = reinterpret_cast<cros_gralloc_handle_t>(handle);
    if (hnd->magic != kCrosGrallocMagic) {
        ALOGE("input handle has the wrong magic (0x%08x)", hnd->magic);
        work->result = C2_CORRUPTED;
        work->workletsProcessed = 1u;
        return;
    }

    int fd = renderNodeFd();
    if (fd < 0) {
        work->result = C2_CORRUPTED;
        work->workletsProcessed = 1u;
        return;
    }
    uint32_t resId = vtest_encode_resolve_res_id(fd, hnd->fds[0]);
    if (!resId) {
        ALOGE("failed to resolve a Venus resource id for this buffer");
        work->result = C2_CORRUPTED;
        work->workletsProcessed = 1u;
        return;
    }

    uint8_t *coded = nullptr;
    uint32_t codedSize = 0;
    int ret = vtest_encode_resource(resId, hnd->width, hnd->height, hnd->format, hnd->strides[0],
                                     hnd->format_modifier, &coded, &codedSize);
    if (ret) {
        ALOGE("vtest_encode_resource failed: %d", ret);
        work->result = C2_CORRUPTED;
        work->workletsProcessed = 1u;
        return;
    }

    std::shared_ptr<C2LinearBlock> outBlock;
    C2MemoryUsage usage = {C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE};
    c2_status_t err = pool->fetchLinearBlock((size_t)codedSize, usage, &outBlock);
    if (err != C2_OK) {
        ALOGE("fetchLinearBlock failed: %d", err);
        free(coded);
        work->result = err;
        work->workletsProcessed = 1u;
        return;
    }
    {
        C2WriteView wView = outBlock->map().get();
        if (wView.error() != C2_OK) {
            ALOGE("output block map failed: %d", wView.error());
            free(coded);
            work->result = wView.error();
            work->workletsProcessed = 1u;
            return;
        }
        memcpy(wView.base(), coded, codedSize);
    }
    free(coded);

    std::shared_ptr<C2Buffer> outBuffer = createLinearBuffer(outBlock, 0, codedSize);
    outBuffer->setInfo(
            std::make_shared<C2StreamPictureTypeMaskInfo::output>(0u, C2Config::SYNC_FRAME));

    work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.buffers.push_back(outBuffer);
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;

    ALOGD("encoded %u bytes via VCMD_ENCODE_RESOURCE (res_id=%u)", codedSize, resId);
}

}  // namespace android
