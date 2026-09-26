/*
 * A real C2Component for c2.hardware.encoder.h264, backed by
 * redroid-nvidia's VCMD_ENCODE_RESOURCE (host-side NVENC, confirmed end to
 * end on real hardware - see redroid-nvidia's README.md/DEVLOG.md, Tier 7).
 *
 * Structural reference: redroid-hwenc's own VaapiEncComponent (same
 * SimpleC2Component shape, same C2GraphicBlock dma-buf extraction pattern) -
 * see that project's tier5-vaapi-daemon/codec2-component/VaapiEncComponent.h
 * for the reasoning behind this base class over external/v4l2_codec2's
 * EncodeComponent. The actual encode transport differs: no daemon socket
 * forwarding a dma-buf fd for VA-API to import - this project's buffer
 * already lives on the host (allocated via minigbm's nvidia_venus.c ->
 * vtest_gpu_alloc_gpu()), so the component only needs to resolve its Venus
 * resource id (vtest_encode_client.h) and ask the host to encode that
 * resource directly.
 */

#ifndef NVENC_CODEC2_COMPONENT_H
#define NVENC_CODEC2_COMPONENT_H

#include <SimpleC2Component.h>
#include <SimpleC2Interface.h>
#include <C2Config.h>
#include <android/hardware/graphics/common/1.0/types.h>

namespace android {

struct NvencEncInterface : public SimpleInterface<void>::BaseParams {
    explicit NvencEncInterface(const std::shared_ptr<C2ReflectorHelper> &helper);

    uint32_t width() const { return mSize->width; }
    uint32_t height() const { return mSize->height; }

private:
    static C2R SizeSetter(bool mayBlock, const C2P<C2StreamPictureSizeInfo::input> &oldMe,
                           C2P<C2StreamPictureSizeInfo::input> &me);
    static C2R ProfileLevelSetter(bool mayBlock, C2P<C2StreamProfileLevelInfo::output> &me);

    std::shared_ptr<C2StreamPictureSizeInfo::input> mSize;
    // Same reasoning as VaapiEncInterface: without this, GraphicBufferSource/
    // gralloc has no hint the input Surface feeds a hardware video encoder.
    std::shared_ptr<C2StreamUsageTuning::input> mInputUsage;
    // Same reasoning as VaapiEncInterface: without a reported profile/level,
    // Codec2InfoBuilder's MediaCodecList aggregation silently drops the
    // component even though it's otherwise listable/instantiable.
    std::shared_ptr<C2StreamProfileLevelInfo::output> mProfileLevel;
};

class NvencEncComponent : public SimpleC2Component {
public:
    NvencEncComponent(const char *name, c2_node_id_t id,
                       const std::shared_ptr<NvencEncInterface> &intf);
    ~NvencEncComponent() override;

    // SimpleC2Component
    c2_status_t onInit() override;
    c2_status_t onStop() override;
    void onReset() override;
    void onRelease() override;
    c2_status_t onFlush_sm() override;
    void process(const std::unique_ptr<C2Work> &work,
                 const std::shared_ptr<C2BlockPool> &pool) override;
    c2_status_t drain(uint32_t drainMode, const std::shared_ptr<C2BlockPool> &pool) override;

private:
    // Lazily opened, kept for the component's lifetime - a *separate* open()
    // of the render node from minigbm's own is fine, see
    // vtest_encode_client.h's own comment on why.
    int renderNodeFd();

    std::shared_ptr<NvencEncInterface> mIntf;
    int mRenderNodeFd = -1;
    // Same reasoning/shape as the reference C2SoftAvcEnc: the framework
    // (scrcpy included) expects the very first output work to carry a
    // separate C2StreamInitDataInfo (CSD) with the SPS/PPS, not just an
    // Annex-B buffer that happens to have them inline - confirmed the hard
    // way (a real, correctly decodable NVENC bitstream, SPS+PPS+IDR all
    // present, still rejected by scrcpy with "the first video packet is not
    // a config packet" until this was added). NVENC's own repeatSPSPPS
    // setting (vtest_gpu_encode.c) still stays on: it's what makes the
    // SPS/PPS bytes present in this component's very first response to
    // split out at all, this flag only tracks that the split has happened
    // once.
    bool mCsdSent = false;
};

}  // namespace android

#endif
