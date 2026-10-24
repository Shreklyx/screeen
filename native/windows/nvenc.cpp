// NVENC: the captured texture is encoded where it is – P1 preset, ultra-low-latency tuning, no
// B-frames or lookahead, CBR with a two-frame buffer, keyframes only when asked for.
#include <cstring>
#include <vector>

#include "encoder.h"
#include "nvEncodeAPI.h" // NVIDIA Video Codec SDK 11.1 API header (MIT licence)

namespace ss {

namespace {

typedef NVENCSTATUS(NVENCAPI* PCreateInstance)(NV_ENCODE_API_FUNCTION_LIST*);
typedef NVENCSTATUS(NVENCAPI* PMaxVersion)(uint32_t*);

template <class F>
F proc(HMODULE lib, const char* name) {
  FARPROC f = lib ? GetProcAddress(lib, name) : nullptr;
  F out;
  std::memcpy(&out, &f, sizeof out);
  return out;
}

NV_ENCODE_API_FUNCTION_LIST* api(std::string& error) {
  static NV_ENCODE_API_FUNCTION_LIST list = {};
  static std::string why = [] {
    HMODULE lib = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!lib) return std::string("no-nvenc");
    auto maxVersion = proc<PMaxVersion>(lib, "NvEncodeAPIGetMaxSupportedVersion");
    auto create = proc<PCreateInstance>(lib, "NvEncodeAPICreateInstance");
    uint32_t version = 0;
    if (!maxVersion || !create || maxVersion(&version) != NV_ENC_SUCCESS) return std::string("no-nvenc");
    if (version < ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION)) return std::string("nvidia-driver-too-old");
    list.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    return create(&list) == NV_ENC_SUCCESS ? std::string() : std::string("no-nvenc");
  }();
  error = why;
  return why.empty() ? &list : nullptr;
}

class Nvenc : public VideoEncoder {
 public:
  ~Nvenc() override { close(); }

  std::string open(ID3D11Device* dev, ID3D11DeviceContext*, ID3D11Texture2D* input, int w, int h, int fps, int bps) override {
    std::string error;
    api_ = api(error);
    if (!api_) return error;
    width_ = uint32_t(w);
    height_ = uint32_t(h);
    fps_ = uint32_t(fps);
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sp = {};
    sp.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sp.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    sp.device = dev;
    sp.apiVersion = NVENCAPI_VERSION;
    if (api_->nvEncOpenEncodeSessionEx(&sp, &session_) != NV_ENC_SUCCESS) return "nvenc-session";
    NV_ENC_PRESET_CONFIG preset = {};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    if (api_->nvEncGetEncodePresetConfigEx(session_, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P1_GUID, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset) != NV_ENC_SUCCESS)
      return "nvenc-preset";
    config_ = preset.presetCfg;
    config_.version = NV_ENC_CONFIG_VER;
    config_.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
    config_.gopLength = NVENC_INFINITE_GOPLENGTH; // keyframes only when a viewer needs one
    config_.frameIntervalP = 1;                    // no B-frames
    setRate(uint32_t(bps));
    NV_ENC_CONFIG_H264& h264 = config_.encodeCodecConfig.h264Config;
    h264.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    h264.repeatSPSPPS = 1;
    h264.chromaFormatIDC = 1;
    NV_ENC_CONFIG_H264_VUI_PARAMETERS& vui = h264.h264VUIParameters; // BT.709, as the decoders assume for HD
    vui.videoSignalTypePresentFlag = 1;
    vui.videoFormat = 5;
    vui.colourDescriptionPresentFlag = 1;
    vui.colourPrimaries = 1;
    vui.transferCharacteristics = 1;
    vui.colourMatrix = 1;
    init_ = {};
    init_.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init_.encodeGUID = NV_ENC_CODEC_H264_GUID;
    init_.presetGUID = NV_ENC_PRESET_P1_GUID;
    init_.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    init_.encodeWidth = init_.darWidth = init_.maxEncodeWidth = width_;
    init_.encodeHeight = init_.darHeight = init_.maxEncodeHeight = height_;
    init_.frameRateNum = fps_;
    init_.frameRateDen = 1;
    init_.enablePTD = 1;
    init_.encodeConfig = &config_;
    if (api_->nvEncInitializeEncoder(session_, &init_) != NV_ENC_SUCCESS) return "nvenc-init";
    NV_ENC_CREATE_BITSTREAM_BUFFER bb = {};
    bb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    if (api_->nvEncCreateBitstreamBuffer(session_, &bb) != NV_ENC_SUCCESS) return "nvenc-buffer";
    bitstream_ = bb.bitstreamBuffer;
    NV_ENC_REGISTER_RESOURCE reg = {};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.width = width_;
    reg.height = height_;
    reg.resourceToRegister = input;
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB; // DXGI B8G8R8A8: the encoder converts to YUV itself
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;
    if (api_->nvEncRegisterResource(session_, &reg) != NV_ENC_SUCCESS) return "nvenc-register";
    input_ = reg.registeredResource;
    return "";
  }

  bool encode(bool key, double captureMs, const EncodedFrame& out) override {
    NV_ENC_MAP_INPUT_RESOURCE map = {};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = input_;
    if (api_->nvEncMapInputResource(session_, &map) != NV_ENC_SUCCESS) return false;
    NV_ENC_PIC_PARAMS pic = {};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputBuffer = map.mappedResource;
    pic.bufferFmt = map.mappedBufferFmt;
    pic.inputWidth = width_;
    pic.inputHeight = height_;
    pic.outputBitstream = bitstream_;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp = uint64_t(captureMs * 1000);
    if (key) pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    bool ok = api_->nvEncEncodePicture(session_, &pic) == NV_ENC_SUCCESS;
    if (ok) {
      NV_ENC_LOCK_BITSTREAM lock = {};
      lock.version = NV_ENC_LOCK_BITSTREAM_VER;
      lock.outputBitstream = bitstream_;
      ok = api_->nvEncLockBitstream(session_, &lock) == NV_ENC_SUCCESS;
      if (ok) {
        bool isKey = lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
        out(static_cast<const uint8_t*>(lock.bitstreamBufferPtr), lock.bitstreamSizeInBytes, isKey, captureMs);
        api_->nvEncUnlockBitstream(session_, bitstream_);
      }
    }
    api_->nvEncUnmapInputResource(session_, map.mappedResource);
    return ok;
  }

  void setBitrate(int bps) override {
    if (!session_) return;
    setRate(uint32_t(bps));
    NV_ENC_RECONFIGURE_PARAMS rp = {};
    rp.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    rp.reInitEncodeParams = init_;
    rp.reInitEncodeParams.encodeConfig = &config_;
    api_->nvEncReconfigureEncoder(session_, &rp);
  }

  std::string name() const override { return "NVENC"; }
  bool hardware() const override { return true; }

 private:
  void setRate(uint32_t bps) {
    NV_ENC_RC_PARAMS& rc = config_.rcParams;
    rc.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    rc.averageBitRate = bps;
    rc.maxBitRate = bps;
    rc.vbvBufferSize = uint32_t(2.0 * bps / fps_); // two frames: a keyframe does not wait long
    rc.vbvInitialDelay = rc.vbvBufferSize;
    rc.zeroReorderDelay = 1;
    rc.enableLookahead = 0;
  }

  void close() {
    if (!session_) return;
    if (input_) api_->nvEncUnregisterResource(session_, input_);
    if (bitstream_) api_->nvEncDestroyBitstreamBuffer(session_, bitstream_);
    api_->nvEncDestroyEncoder(session_);
    session_ = nullptr;
  }

  NV_ENCODE_API_FUNCTION_LIST* api_ = nullptr;
  void* session_ = nullptr;
  NV_ENC_INITIALIZE_PARAMS init_ = {};
  NV_ENC_CONFIG config_ = {};
  NV_ENC_OUTPUT_PTR bitstream_ = nullptr;
  NV_ENC_REGISTERED_PTR input_ = nullptr;
  uint32_t width_ = 0, height_ = 0, fps_ = 60;
};

}  // namespace

std::unique_ptr<VideoEncoder> createNvencEncoder() { return std::unique_ptr<VideoEncoder>(new Nvenc); }

}  // namespace ss
