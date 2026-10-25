// Media Foundation H.264 encoder: the GPU's own encoder (Intel Quick Sync, AMD) on the adapter that
// captured the screen, or Windows' software encoder. The BGRA frame is converted to NV12 on the GPU
// (D3D11 video processor, BT.709); low-latency mode, CBR, no B-frames, keyframes when asked for.
#include <windows.h>
#include <codecapi.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h> // ICodecAPI
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <vector>

#include "clock.h"
#include "encoder.h"

namespace ss {

using Microsoft::WRL::ComPtr;

namespace {

// IID_ICodecAPI (not linked in every toolchain)
const IID kCodecApi = {0x901db4c7, 0x31ce, 0x41a2, {0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda}};

// (not in every SDK's headers)
const GUID kAdapterLuid = {0x1d39518c, 0xe220, 0x4da8, {0xa0, 0x7f, 0xba, 0x17, 0x25, 0x52, 0xd6, 0xb1}}; // MFT_ENUM_ADAPTER_LUID

void setUInt(ICodecAPI* api, const GUID& key, ULONG value) {
  VARIANT v;
  VariantInit(&v);
  v.vt = VT_UI4;
  v.ulVal = value;
  api->SetValue(&key, &v);
}

void setBool(ICodecAPI* api, const GUID& key, bool value) {
  VARIANT v;
  VariantInit(&v);
  v.vt = VT_BOOL;
  v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
  api->SetValue(&key, &v);
}

class MfEncoder : public VideoEncoder {
 public:
  MfEncoder(LUID adapter, bool hardware) : adapter_(adapter), hardware_(hardware) {}
  ~MfEncoder() override {
    if (mft_) {
      mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
      mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
  }

  std::string open(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* input, int w, int h, int fps, int bps) override {
    dev_ = dev;
    ctx_ = ctx;
    width_ = w;
    height_ = h;
    fps_ = fps;
    std::string why = createTransform();
    if (!why.empty()) return why;
    if (!createConverter(input)) {
      // no video processor on this GPU (a basic display driver, a virtual machine): the software
      // encoder gets its NV12 made on the CPU instead
      if (hardware_) return "video-processor";
      cpuConvert_ = true;
      input_ = input;
    }
    ComPtr<ICodecAPI> codec;
    if (SUCCEEDED(mft_->QueryInterface(kCodecApi, reinterpret_cast<void**>(codec.GetAddressOf())))) {
      codec_ = codec;
      setUInt(codec.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
      setUInt(codec.Get(), CODECAPI_AVEncCommonMeanBitRate, ULONG(bps));
      setBool(codec.Get(), CODECAPI_AVLowLatencyMode, true); // one frame in, one frame out
      setUInt(codec.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
      setUInt(codec.Get(), CODECAPI_AVEncMPVGOPSize, ULONG(fps * 600)); // keyframes when a viewer needs one
      // speed over the last bit of quality, as real-time streaming does (the encode time is delay):
      // a GPU's encoder at its balanced setting (Intel: target usage 4, AMD: balanced), the
      // processor at a fast one (it is the fallback); 100 was the slowest, best-quality setting
      setUInt(codec.Get(), CODECAPI_AVEncCommonQualityVsSpeed, hardware_ ? 50 : 33);
    }
    ComPtr<IMFMediaType> out;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, UINT32(bps));
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, UINT32(w), UINT32(h));
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, UINT32(fps), 1);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    if (FAILED(mft_->SetOutputType(0, out.Get(), 0))) {
      out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
      if (FAILED(mft_->SetOutputType(0, out.Get(), 0))) return "mf-output-type";
    }
    ComPtr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, UINT32(w), UINT32(h));
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, UINT32(fps), 1);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(mft_->SetInputType(0, in.Get(), 0))) return "mf-input-type";
    MFT_OUTPUT_STREAM_INFO info = {};
    mft_->GetOutputStreamInfo(0, &info);
    providesSamples_ = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    outSize_ = std::max<DWORD>(info.cbSize, DWORD(w * h * 3 / 2));
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return "";
  }

  bool encode(bool key, double captureMs, const EncodedFrame& out) override {
    ComPtr<IMFMediaBuffer> buffer;
    if (cpuConvert_) {
      if (!convertOnCpu(buffer)) return false;
    } else {
      // BGRA → NV12 on the GPU
      ComPtr<ID3D11Texture2D>& nv12 = pool_[next_ % pool_.size()];
      ComPtr<ID3D11VideoProcessorOutputView>& view = poolViews_[next_ % pool_.size()];
      next_++;
      D3D11_VIDEO_PROCESSOR_STREAM stream = {};
      stream.Enable = TRUE;
      stream.pInputSurface = inView_.Get();
      if (FAILED(videoCtx_->VideoProcessorBlt(processor_.Get(), view.Get(), 0, 1, &stream))) return false;
      if (hardware_) {
        if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12.Get(), 0, FALSE, &buffer))) return false;
        DWORD len = 0;
        ComPtr<IMF2DBuffer> b2;
        if (SUCCEEDED(buffer.As(&b2))) b2->GetContiguousLength(&len);
        buffer->SetCurrentLength(len);
      } else if (!readBack(nv12.Get(), buffer)) {
        return false;
      }
    }
    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buffer.Get());
    LONGLONG time = LONGLONG(frame_++) * 10000000 / fps_;
    sample->SetSampleTime(time);
    sample->SetSampleDuration(10000000 / fps_);
    times_[time] = captureMs;
    if (times_.size() > 32) times_.erase(times_.begin());
    if (key && codec_) setUInt(codec_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
    if (events_) {
      // an asynchronous (hardware) encoder: input when it asks for it, output when it has it
      if (!waitFor(METransformNeedInput, 200)) return false;
      needInput_--;
      if (FAILED(mft_->ProcessInput(0, sample.Get(), 0))) return false;
      double until = nowMs() + 100;
      while (nowMs() < until) {
        pump(false);
        if (haveOutput_ > 0) {
          haveOutput_--;
          drain(out);
          break;
        }
        Sleep(0);
      }
      return true;
    }
    if (FAILED(mft_->ProcessInput(0, sample.Get(), 0))) return false;
    drain(out);
    return true;
  }

  void setBitrate(int bps) override {
    if (codec_) setUInt(codec_.Get(), CODECAPI_AVEncCommonMeanBitRate, ULONG(bps));
  }

  std::string name() const override { return name_; }
  bool hardware() const override { return hardware_; }

 private:
  std::string createTransform() {
    MFT_REGISTER_TYPE_INFO inType = {MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO outType = {MFMediaType_Video, MFVideoFormat_H264};
    UINT32 flags = hardware_ ? MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER : MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;
    IMFActivate** list = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &inType, &outType, &list, &count)) || !count) return hardware_ ? "no-hw-encoder" : "no-encoder";
    UINT32 pick = 0;
    for (UINT32 i = 0; i < count && hardware_; i++) {
      LUID luid;
      if (SUCCEEDED(list[i]->GetBlob(kAdapterLuid, reinterpret_cast<UINT8*>(&luid), sizeof luid, nullptr)) && luid.LowPart == adapter_.LowPart && luid.HighPart == adapter_.HighPart) {
        pick = i;
        break;
      }
    }
    wchar_t friendly[128] = L"";
    list[pick]->GetString(MFT_FRIENDLY_NAME_Attribute, friendly, 128, nullptr);
    HRESULT hr = list[pick]->ActivateObject(IID_PPV_ARGS(&mft_));
    for (UINT32 i = 0; i < count; i++) list[i]->Release();
    CoTaskMemFree(list);
    if (FAILED(hr)) return "mf-activate";
    char buf[128];
    WideCharToMultiByte(CP_UTF8, 0, friendly, -1, buf, sizeof buf, nullptr, nullptr);
    name_ = hardware_ ? buf : "szoftveres";
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(mft_->GetAttributes(&attrs))) {
      UINT32 async = 0;
      if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async) {
        attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        mft_.As(&events_);
      }
      attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }
    if (hardware_) {
      UINT token = 0;
      if (FAILED(MFCreateDXGIDeviceManager(&token, &manager_)) || FAILED(manager_->ResetDevice(dev_, token))) return "mf-device";
      if (FAILED(mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager_.Get())))) return "mf-device";
    }
    return "";
  }

  bool createConverter(ID3D11Texture2D* input) {
    ComPtr<ID3D11Device> dev(dev_);
    ComPtr<ID3D11DeviceContext> ctx(ctx_);
    if (FAILED(dev.As(&videoDev_)) || FAILED(ctx.As(&videoCtx_))) return false;
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = cd.OutputWidth = UINT(width_);
    cd.InputHeight = cd.OutputHeight = UINT(height_);
    cd.InputFrameRate = {UINT(fps_), 1};
    cd.OutputFrameRate = {UINT(fps_), 1};
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (FAILED(videoDev_->CreateVideoProcessorEnumerator(&cd, &enum_)) || FAILED(videoDev_->CreateVideoProcessor(enum_.Get(), 0, &processor_))) return false;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv = {};
    iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    if (FAILED(videoDev_->CreateVideoProcessorInputView(input, enum_.Get(), &iv, &inView_))) return false;
    // RGB in full range, YUV out in BT.709 video range (what the decoders assume)
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs = {};
    inCs.RGB_Range = 0;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs = {};
    outCs.YCbCr_Matrix = 1;
    outCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoCtx_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &inCs);
    videoCtx_->VideoProcessorSetOutputColorSpace(processor_.Get(), &outCs);
    videoCtx_->VideoProcessorSetStreamFrameFormat(processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    for (size_t i = 0; i < pool_.size(); i++) {
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = UINT(width_);
      td.Height = UINT(height_);
      td.MipLevels = 1;
      td.ArraySize = 1;
      td.Format = DXGI_FORMAT_NV12;
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_DEFAULT;
      td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      if (FAILED(dev_->CreateTexture2D(&td, nullptr, &pool_[i]))) return false;
      D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov = {};
      ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
      if (FAILED(videoDev_->CreateVideoProcessorOutputView(pool_[i].Get(), enum_.Get(), &ov, &poolViews_[i]))) return false;
    }
    return true;
  }

  /** The software encoder takes system memory: the NV12 frame comes back from the GPU. */
  bool readBack(ID3D11Texture2D* nv12, ComPtr<IMFMediaBuffer>& buffer) {
    if (!staging_) {
      D3D11_TEXTURE2D_DESC td;
      nv12->GetDesc(&td);
      td.Usage = D3D11_USAGE_STAGING;
      td.BindFlags = 0;
      td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (FAILED(dev_->CreateTexture2D(&td, nullptr, &staging_))) return false;
    }
    ctx_->CopyResource(staging_.Get(), nv12);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    DWORD size = DWORD(width_ * height_ * 3 / 2);
    MFCreateMemoryBuffer(size, &buffer);
    BYTE* dst = nullptr;
    buffer->Lock(&dst, nullptr, nullptr);
    const BYTE* src = static_cast<const BYTE*>(m.pData);
    for (int y = 0; y < height_ * 3 / 2; y++) std::memcpy(dst + size_t(y) * width_, src + size_t(y) * m.RowPitch, size_t(width_));
    buffer->Unlock();
    buffer->SetCurrentLength(size);
    ctx_->Unmap(staging_.Get(), 0);
    return true;
  }

  /** No video processor: the BGRA frame comes back from the GPU and becomes NV12 here (BT.709, video range). */
  bool convertOnCpu(ComPtr<IMFMediaBuffer>& buffer) {
    if (!stagingBgra_) {
      D3D11_TEXTURE2D_DESC td;
      input_->GetDesc(&td);
      if (int(td.Width) < width_ || int(td.Height) < height_) return false;
      td.Usage = D3D11_USAGE_STAGING;
      td.BindFlags = 0;
      td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      td.MiscFlags = 0;
      if (FAILED(dev_->CreateTexture2D(&td, nullptr, &stagingBgra_))) return false;
    }
    ctx_->CopyResource(stagingBgra_.Get(), input_);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(stagingBgra_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    DWORD size = DWORD(width_ * height_ * 3 / 2);
    MFCreateMemoryBuffer(size, &buffer);
    BYTE* dst = nullptr;
    buffer->Lock(&dst, nullptr, nullptr);
    BYTE* chroma = dst + size_t(width_) * height_;
    for (int y = 0; y + 1 < height_; y += 2) {
      const BYTE* rows[2] = {static_cast<const BYTE*>(m.pData) + size_t(y) * m.RowPitch, static_cast<const BYTE*>(m.pData) + size_t(y + 1) * m.RowPitch};
      BYTE* luma[2] = {dst + size_t(y) * width_, dst + size_t(y + 1) * width_};
      BYTE* c = chroma + size_t(y / 2) * width_;
      for (int x = 0; x + 1 < width_; x += 2) {
        int sr = 0, sg = 0, sb = 0;
        for (int k = 0; k < 4; k++) {
          const BYTE* p = rows[k >> 1] + size_t(x + (k & 1)) * 4; // B, G, R, A
          int b = p[0], g = p[1], r = p[2];
          luma[k >> 1][x + (k & 1)] = BYTE(((47 * r + 157 * g + 16 * b + 128) >> 8) + 16);
          sr += r;
          sg += g;
          sb += b;
        }
        sr = (sr + 2) >> 2;
        sg = (sg + 2) >> 2;
        sb = (sb + 2) >> 2;
        c[x] = BYTE(((-26 * sr - 86 * sg + 112 * sb + 128) >> 8) + 128);
        c[x + 1] = BYTE(((112 * sr - 102 * sg - 10 * sb + 128) >> 8) + 128);
      }
    }
    buffer->Unlock();
    buffer->SetCurrentLength(size);
    ctx_->Unmap(stagingBgra_.Get(), 0);
    return true;
  }

  void pump(bool block) {
    for (;;) {
      ComPtr<IMFMediaEvent> ev;
      if (FAILED(events_->GetEvent(block ? 0 : MF_EVENT_FLAG_NO_WAIT, &ev)) || !ev) return;
      MediaEventType type;
      ev->GetType(&type);
      if (type == METransformNeedInput) needInput_++;
      else if (type == METransformHaveOutput) haveOutput_++;
      block = false;
    }
  }

  bool waitFor(MediaEventType type, double ms) {
    double until = nowMs() + ms;
    for (;;) {
      pump(false);
      if (type == METransformNeedInput && needInput_ > 0) return true;
      if (nowMs() > until) return false;
      Sleep(0);
    }
  }

  void drain(const EncodedFrame& out) {
    for (;;) {
      MFT_OUTPUT_DATA_BUFFER ob = {};
      ComPtr<IMFSample> mine;
      if (!providesSamples_) {
        MFCreateSample(&mine);
        ComPtr<IMFMediaBuffer> b;
        MFCreateMemoryBuffer(outSize_, &b);
        mine->AddBuffer(b.Get());
        ob.pSample = mine.Get();
      }
      DWORD status = 0;
      HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
      if (ob.pEvents) ob.pEvents->Release();
      if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        // the encoder settled its output type: take it and ask again
        ComPtr<IMFMediaType> t;
        if (SUCCEEDED(mft_->GetOutputAvailableType(0, 0, &t))) mft_->SetOutputType(0, t.Get(), 0);
        continue;
      }
      if (FAILED(hr) || !ob.pSample) return;
      ComPtr<IMFSample> sample;
      sample.Attach(providesSamples_ ? ob.pSample : (mine.Get()->AddRef(), mine.Get()));
      ComPtr<IMFMediaBuffer> buf;
      if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
        BYTE* p = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
          UINT32 clean = 0;
          sample->GetUINT32(MFSampleExtension_CleanPoint, &clean);
          LONGLONG t = 0;
          sample->GetSampleTime(&t);
          auto it = times_.find(t);
          out(p, len, clean != 0, it != times_.end() ? it->second : nowMs());
          buf->Unlock();
        }
      }
      if (events_) return; // asynchronous: one output per HaveOutput event
    }
  }

  LUID adapter_;
  bool hardware_;
  std::string name_;
  ID3D11Device* dev_ = nullptr;
  ID3D11DeviceContext* ctx_ = nullptr;
  int width_ = 0, height_ = 0, fps_ = 60;
  ComPtr<IMFTransform> mft_;
  ComPtr<IMFMediaEventGenerator> events_;
  ComPtr<ICodecAPI> codec_;
  ComPtr<IMFDXGIDeviceManager> manager_;
  ComPtr<ID3D11VideoDevice> videoDev_;
  ComPtr<ID3D11VideoContext> videoCtx_;
  ComPtr<ID3D11VideoProcessorEnumerator> enum_;
  ComPtr<ID3D11VideoProcessor> processor_;
  ComPtr<ID3D11VideoProcessorInputView> inView_;
  std::array<ComPtr<ID3D11Texture2D>, 4> pool_;
  std::array<ComPtr<ID3D11VideoProcessorOutputView>, 4> poolViews_;
  ComPtr<ID3D11Texture2D> staging_;
  bool cpuConvert_ = false;
  ID3D11Texture2D* input_ = nullptr; // the capture's BGRA frame (it outlives the encoder)
  ComPtr<ID3D11Texture2D> stagingBgra_;
  size_t next_ = 0;
  uint64_t frame_ = 0;
  std::map<LONGLONG, double> times_;
  bool providesSamples_ = false;
  DWORD outSize_ = 0;
  int needInput_ = 0, haveOutput_ = 0;
};

}  // namespace

std::unique_ptr<VideoEncoder> createMfEncoder(LUID adapter, bool hardware) { return std::unique_ptr<VideoEncoder>(new MfEncoder(adapter, hardware)); }

}  // namespace ss
