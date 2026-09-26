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
    // directly, no block.map()/layout() call. This project's own gralloc HAL
    // (minigbm's cros_gralloc) produces a real cros_gralloc_handle_t for a
    // hand-built C2Work test buffer - handled below as the primary path -
    // but a genuine Surface-sourced frame (scrcpy's virtual-display capture
    // via GraphicBufferSource) does NOT arrive this way, confirmed on real
    // hardware: numFds=1, numInts=46 (vs. 36 for a real cros_gralloc_handle).
    // Exactly the same discovery VaapiEncComponent's own header already
    // documents for AMD/Intel (its own equivalent buffer measured numFds=1,
    // numInts=23) - some other, more generic native_handle_t this pipeline
    // uses when GraphicBufferSource hands over a captured frame, not this
    // vendor's own gralloc wire format. Offsets below are empirical (dumped
    // the raw ints across two different scrcpy-driven resolutions - a native
    // 720x1280 capture and a -m800-constrained 450x800 one - and confirmed
    // the same four offsets track the real, independently-known values in
    // both: width/height exactly match each resolution's real dimensions,
    // the stride-shaped value at offset 4 combined with height reproduces
    // the total-size value at offset 12 exactly (stride*height) for both
    // resolutions, and the format offset holds the DRM fourcc 'AB24' -
    // ABGR8888 - in both). No identifiable modifier field in this wrapper;
    // passed as 0 and left for the host side to determine independently if
    // that turns out not to be good enough (this project's own
    // vtest_gpu_encode.c, unlike VA-API's daemon, currently trusts whatever
    // modifier the wire protocol sends for the plain-import step).
    C2ConstGraphicBlock block = inputBuffer->data().graphicBlocks().front();
    const C2Handle *const handle = block.handle();
    if (!handle || handle->numFds < 1) {
        ALOGE("input graphic block has no dma-buf fd");
        work->result = C2_CORRUPTED;
        work->workletsProcessed = 1u;
        return;
    }

    int dmabufFd;
    uint32_t width, height, stride, drmFormat;
    uint64_t formatModifier;

    const bool looksLikeCrosGralloc =
            sizeof(native_handle_t) + sizeof(int) * (size_t)(handle->numFds + handle->numInts) ==
            sizeof(cros_gralloc_handle);
    if (looksLikeCrosGralloc &&
        reinterpret_cast<cros_gralloc_handle_t>(handle)->magic == kCrosGrallocMagic) {
        auto hnd = reinterpret_cast<cros_gralloc_handle_t>(handle);
        dmabufFd = hnd->fds[0];
        width = hnd->width;
        height = hnd->height;
        stride = hnd->strides[0];
        drmFormat = hnd->format;
        formatModifier = hnd->format_modifier;
    } else if (handle->numFds == 1 && handle->numInts >= 20) {
        const int32_t *ints = &handle->data[handle->numFds];
        dmabufFd = handle->data[0];
        stride = (uint32_t)ints[4];
        width = (uint32_t)ints[17];
        height = (uint32_t)ints[18];
        drmFormat = (uint32_t)ints[19];
        formatModifier = 0; /* not identifiable in this wrapper, see above */
    } else {
        ALOGE("input handle matches neither known layout (numFds=%d numInts=%d)", handle->numFds,
              handle->numInts);
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
    uint32_t resId = vtest_encode_resolve_res_id(fd, dmabufFd);

    uint8_t *coded = nullptr;
    uint32_t codedSize = 0;
    int ret;
    if (resId) {
        /* A genuine Venus-owned resource (e.g. this project's own allocation
         * path, or a hand-built C2Work test) - the proven, three-spikes-deep
         * VCMD_ENCODE_RESOURCE path. */
        ret = vtest_encode_resource(resId, width, height, drmFormat, stride, formatModifier,
                                     &coded, &codedSize);
        if (ret) ALOGE("vtest_encode_resource failed: %d", ret);
    } else {
        /* A real, importable dma-buf that simply was never registered as a
         * Venus resource (confirmed 2026-09-26: this is exactly what a real
         * Surface-sourced GraphicBufferSource capture buffer is, on this
         * project's own NVIDIA/cros_gralloc stack) - VCMD_ENCODE_RESOURCE
         * fundamentally cannot name it, no matter how correctly it's parsed.
         * Fall back to the SCM_RIGHTS transport instead. */
        ret = vtest_encode_via_scm(dmabufFd, width, height, drmFormat, stride, formatModifier,
                                    &coded, &codedSize);
        if (ret) ALOGE("vtest_encode_via_scm failed: %d", ret);
    }
    if (ret) {
        work->result = C2_CORRUPTED;
        work->workletsProcessed = 1u;
        return;
    }

    // Same reasoning as the reference C2SoftAvcEnc: the framework (and a real
    // client, scrcpy, confirmed the hard way) expects the very first output
    // work to carry the SPS/PPS as a separate C2StreamInitDataInfo, not just
    // an Annex-B buffer that happens to have them inline first - even though
    // NVENC's repeatSPSPPS setting (vtest_gpu_encode.c) does put genuinely
    // valid, ffprobe-decodable SPS+PPS+IDR bytes in that first buffer.
    // Splits off everything before the first VCL NAL (slice types 1-5) as
    // the CSD; only fires once, since repeatSPSPPS only repeats on IDRs and
    // this component's own first call is always this stream's first IDR.
    uint32_t frameOffset = 0;
    if (!mCsdSent) {
        for (uint32_t i = 0; i + 4 < codedSize; i++) {
            bool startCode3 = coded[i] == 0 && coded[i + 1] == 0 && coded[i + 2] == 1;
            bool startCode4 =
                    startCode3 == false && coded[i] == 0 && coded[i + 1] == 0 &&
                    coded[i + 2] == 0 && coded[i + 3] == 1;
            if (!startCode3 && !startCode4) continue;
            uint32_t nalOffset = i + (startCode4 ? 4 : 3);
            uint32_t nalType = coded[nalOffset] & 0x1f;
            if (nalType >= 1 && nalType <= 5) {
                frameOffset = nalOffset - (startCode4 ? 4 : 3);
                break;
            }
        }
        if (frameOffset > 0) {
            std::unique_ptr<C2StreamInitDataInfo::output> csd =
                    C2StreamInitDataInfo::output::AllocUnique(frameOffset, 0u);
            if (csd) {
                memcpy(csd->m.value, coded, frameOffset);
                work->worklets.front()->output.configUpdate.push_back(std::move(csd));
                mCsdSent = true;
            } else {
                ALOGE("CSD allocation failed, sending SPS/PPS inline instead");
                frameOffset = 0;
            }
        }
    }
    const uint32_t frameSize = codedSize - frameOffset;

    std::shared_ptr<C2LinearBlock> outBlock;
    C2MemoryUsage usage = {C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE};
    c2_status_t err = pool->fetchLinearBlock((size_t)frameSize, usage, &outBlock);
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
        memcpy(wView.base(), coded + frameOffset, frameSize);
    }
    free(coded);

    std::shared_ptr<C2Buffer> outBuffer = createLinearBuffer(outBlock, 0, frameSize);
    outBuffer->setInfo(
            std::make_shared<C2StreamPictureTypeMaskInfo::output>(0u, C2Config::SYNC_FRAME));

    work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.buffers.push_back(outBuffer);
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;

    ALOGD("encoded %u bytes via %s", codedSize,
          resId ? "VCMD_ENCODE_RESOURCE" : "SCM_RIGHTS fallback");
}

}  // namespace android
