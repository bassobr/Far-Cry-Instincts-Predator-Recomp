// Standalone D3D12 device with a DXGI swapchain for NVRHI (renderer/host_device.h). This project's own
// code. The requirements check (binding tier 2, SM 6.0, formats) is King Kong's d3d12_requirements.h.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// DirectX headers first (preview names NVRHI needs), then DXGI.
#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <nvrhi/d3d12.h>
#include <nvrhi/validation.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "kk/backend/d3d12_requirements.h"
#include "renderer/diagnostics.h"
#include "renderer/host_device.h"

using Microsoft::WRL::ComPtr;

namespace fcp::nr {

namespace {

std::string Narrow(const wchar_t* w) {
  std::string s;
  for (; *w; ++w) s += *w < 128 ? char(*w) : '?';
  return s;
}

// Occlusion queries: query heap (OCCLUSION, exact count) and a persistently mapped readback buffer.
class D3D12Occlusion final : public OcclusionQueries {
 public:
  bool Init(ID3D12Device* device, uint32_t count) {
    count_ = count;
    D3D12_QUERY_HEAP_DESC hd{};
    hd.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    hd.Count = count;
    if (FAILED(device->CreateQueryHeap(&hd, IID_PPV_ARGS(&heap_)))) return false;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = uint64_t(count) * 8;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&readback_))))
      return false;
    void* p = nullptr;
    const D3D12_RANGE all{0, size_t(count) * 8};
    if (FAILED(readback_->Map(0, &all, &p))) return false;
    mapped_ = static_cast<const uint64_t*>(p);
    return true;
  }
  ~D3D12Occlusion() override {
    if (readback_ && mapped_) readback_->Unmap(0, nullptr);
  }
  uint32_t capacity() const override { return count_; }
  static ID3D12GraphicsCommandList* Native(nvrhi::ICommandList* cl) {
    return static_cast<ID3D12GraphicsCommandList*>(cl->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList).pointer);
  }
  void Begin(nvrhi::ICommandList* cl, uint32_t index) override {
    if (ID3D12GraphicsCommandList* c = Native(cl)) c->BeginQuery(heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, index);
  }
  void End(nvrhi::ICommandList* cl, uint32_t index) override {
    ID3D12GraphicsCommandList* c = Native(cl);
    if (!c) return;
    c->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, index);
    c->ResolveQueryData(heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, index, 1, readback_.Get(), uint64_t(index) * 8);
  }
  // The caller reads only after the list has completed (event query); the buffer is valid then.
  bool Read(uint32_t index, uint64_t* samples) override {
    if (!mapped_ || index >= count_) return false;
    *samples = mapped_[index];
    return true;
  }

 private:
  ComPtr<ID3D12QueryHeap> heap_;
  ComPtr<ID3D12Resource> readback_;
  const uint64_t* mapped_ = nullptr;
  uint32_t count_ = 0;
};

class D3D12Host final : public HostDevice {
 public:
  ~D3D12Host() override {
    if (nvrhi_) nvrhi_->waitForIdle();
    occlusion_.reset();
    back_buffers_.clear();
    frame_queries_.clear();
    barrier_list_ = nullptr;
    validation_ = nullptr;
    nvrhi_ = nullptr;
  }

  bool Init(const HostOptions& o, std::string* error) {
    options_ = o;
    UINT factory_flags = 0;
    if (o.debug) {
      ComPtr<ID3D12Debug> debug;
      if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        factory_flags = DXGI_CREATE_FACTORY_DEBUG;
      }
    }
    if (FAILED(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory_))))
      return error && (*error = "CreateDXGIFactory2 failed", false);
    ComPtr<IDXGIAdapter1> adapter;
    if (o.warp) {
      ComPtr<IDXGIFactory4> f4;
      if (SUCCEEDED(factory_.As(&f4))) f4->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    } else {
      ComPtr<IDXGIFactory6> f6;
      if (SUCCEEDED(factory_.As(&f6)))
        f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
      else
        factory_->EnumAdapters1(0, &adapter);
    }
    if (!adapter) return error && (*error = "no DXGI adapter", false);
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    description_ = "D3D12 " + Narrow(ad.Description);
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_))))
      return error && (*error = "D3D12CreateDevice failed (feature level 12_0)", false);
    std::string missing;
    const auto req = CheckD3D12Requirements(
        [&](D3D12_FEATURE f, void* data, UINT size) { return device_->CheckFeatureSupport(f, data, size); },
        [&](const char* name) { missing += std::string(missing.empty() ? "" : ", ") + name; });
    if (!req.supported) return error && (*error = "D3D12 requirements missing: " + missing, false);
    {
      // MSAA: highest level all screen-target formats support as render targets.
      max_samples_ = 1;
      for (UINT s : {2u, 4u, 8u}) {
        bool all = true;
        for (DXGI_FORMAT f : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM,
                              DXGI_FORMAT_D32_FLOAT_S8X24_UINT}) {
          D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{f, s, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
          if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q))) || !q.NumQualityLevels)
            all = false;
        }
        if (all) max_samples_ = s;
      }
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_))))
      return error && (*error = "CreateCommandQueue failed", false);

    nvrhi::d3d12::DeviceDesc desc;
    desc.errorCB = HostMessageCallback();
    desc.pDevice = device_.Get();
    desc.pGraphicsCommandQueue = queue_.Get();
    nvrhi_ = nvrhi::d3d12::createDevice(desc);
    if (!nvrhi_) return error && (*error = "nvrhi::d3d12::createDevice failed", false);
    if (o.debug) validation_ = nvrhi::validation::createValidationLayer(nvrhi_);
    barrier_list_ = device()->createCommandList();
    for (int i = 0; i < 2; ++i) frame_queries_.push_back(device()->createEventQuery());
    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) fence_.Reset();
    occlusion_ =std::make_unique<D3D12Occlusion>();
    if (!occlusion_->Init(device_.Get(), 4096)) {
      LogLine("[d3d12] query heap failed (no occlusion queries)");
      occlusion_.reset();
    }

    if (o.window) {
      DXGI_SWAP_CHAIN_DESC1 sd{};
      sd.Width = o.width;
      sd.Height = o.height;
      sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      sd.SampleDesc.Count = 1;
      sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      sd.BufferCount = 2;
      sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
      ComPtr<IDXGISwapChain1> sc1;
      if (FAILED(factory_->CreateSwapChainForHwnd(queue_.Get(), static_cast<HWND>(o.window), &sd, nullptr, nullptr, &sc1)) ||
          FAILED(sc1.As(&swapchain_)))
        return error && (*error = "CreateSwapChainForHwnd failed", false);
      factory_->MakeWindowAssociation(static_cast<HWND>(o.window), DXGI_MWA_NO_ALT_ENTER);
      return WrapBuffers();
    }
    return true;
  }

  bool WrapBuffers() {
    DXGI_SWAP_CHAIN_DESC1 sd{};
    swapchain_->GetDesc1(&sd);
    for (UINT i = 0; i < sd.BufferCount; ++i) {
      ComPtr<ID3D12Resource> buffer;
      if (FAILED(swapchain_->GetBuffer(i, IID_PPV_ARGS(&buffer)))) return false;
      nvrhi::TextureDesc td;
      td.width = sd.Width;
      td.height = sd.Height;
      td.format = nvrhi::Format::RGBA8_UNORM;
      td.debugName = "Swapchain";
      td.isRenderTarget = true;
      td.initialState = nvrhi::ResourceStates::Present;
      td.keepInitialState = true;
      back_buffers_.push_back(
          device()->createHandleForNativeTexture(nvrhi::ObjectTypes::D3D12_Resource, nvrhi::Object(buffer.Get()), td));
      buffers_.push_back(buffer);
    }
    options_.width = sd.Width;
    options_.height = sd.Height;
    return !back_buffers_.empty();
  }

  nvrhi::IDevice* device() override { return validation_ ? validation_.Get() : nvrhi_.Get(); }
  OcclusionQueries* occlusion_queries() override { return occlusion_.get(); }
  HostApi api() const override { return HostApi::kD3D12; }
  uint32_t max_samples() const override { return max_samples_; }
  std::string description() const override { return description_; }
  bool has_swapchain() const override { return bool(swapchain_); }

  // Split present: own fence per present, waiting without the lock (ID3D12Fence is free-threaded).
  void WaitFrameSlot() override {
    if (!swapchain_ || frame_ < frame_fences_.size()) return;
    const uint64_t v = frame_fences_[frame_ % frame_fences_.size()];
    if (!v || !fence_ || fence_->GetCompletedValue() >= v) return;
    const auto t0 = std::chrono::steady_clock::now();
    WaitFence(v);
    Counters::Get().AddWait(t0);
  }
  bool AcquireImage() override { return bool(swapchain_); }
  uint64_t SignalFence() override {
    if (!fence_) return 0;
    const uint64_t v = ++fence_next_;
    if (FAILED(queue_->Signal(fence_.Get(), v))) return 0;
    return v;
  }
  void WaitFence(uint64_t value) override {
    if (!fence_ || !value || fence_->GetCompletedValue() >= value) return;
    fence_->SetEventOnCompletion(value, nullptr);  // no event: blocks until the value is reached
  }

  nvrhi::ITexture* BeginFrame() override {
    if (!swapchain_) return nullptr;
    if (options_.split_present) return back_buffers_[swapchain_->GetCurrentBackBufferIndex()];
    nvrhi::IEventQuery* q = frame_queries_[frame_ % frame_queries_.size()];
    if (frame_ >= frame_queries_.size()) {
      const auto t0 = std::chrono::steady_clock::now();
      device()->waitEventQuery(q);
      Counters::Get().AddWait(t0);
    }
    return back_buffers_[swapchain_->GetCurrentBackBufferIndex()];
  }

  void Present() override {
    if (!swapchain_) return;
    nvrhi::ITexture* back = back_buffers_[swapchain_->GetCurrentBackBufferIndex()];
    barrier_list_->open();
    barrier_list_->setTextureState(back, nvrhi::AllSubresources, nvrhi::ResourceStates::Present);
    barrier_list_->commitBarriers();
    barrier_list_->close();
    device()->executeCommandList(barrier_list_);
    if (options_.split_present) {
      frame_fences_[frame_ % frame_fences_.size()] = SignalFence();
    } else {
      nvrhi::IEventQuery* q = frame_queries_[frame_ % frame_queries_.size()];
      device()->resetEventQuery(q);
      device()->setEventQuery(q, nvrhi::CommandQueue::Graphics);
    }
    swapchain_->Present(options_.vsync ? 1 : 0, 0);
    device()->runGarbageCollection();
    ++frame_;
  }

  void WaitIdle() override {
    if (device()) device()->waitForIdle();
  }

  uint32_t swapchain_width() const override { return swapchain_ ? options_.width : 0; }
  uint32_t swapchain_height() const override { return swapchain_ ? options_.height : 0; }

  bool Resize(uint32_t width, uint32_t height) override {
    if (!swapchain_ || !width || !height) return false;
    device()->waitForIdle();
    back_buffers_.clear();
    buffers_.clear();
    device()->runGarbageCollection();
    if (FAILED(swapchain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) {
      LogLine("[d3d12] ResizeBuffers %ux%u failed", width, height);
      return false;
    }
    options_.width = width;
    options_.height = height;
    return WrapBuffers();
  }

 private:
  HostOptions options_;
  ComPtr<IDXGIFactory2> factory_;
  ComPtr<ID3D12Device> device_;
  uint32_t max_samples_ = 1;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<IDXGISwapChain3> swapchain_;
  std::vector<ComPtr<ID3D12Resource>> buffers_;
  std::vector<nvrhi::TextureHandle> back_buffers_;
  std::vector<nvrhi::EventQueryHandle> frame_queries_;
  ComPtr<ID3D12Fence> fence_;         // own CPU fence (SignalFence/WaitFence)
  uint64_t fence_next_ = 0;           // under the caller's lock
  std::array<uint64_t, 2> frame_fences_{};  // fence per present (split present)
  nvrhi::CommandListHandle barrier_list_;
  nvrhi::DeviceHandle nvrhi_, validation_;
  std::unique_ptr<D3D12Occlusion> occlusion_;
  std::string description_;
  uint64_t frame_ = 0;
};

}  // namespace

std::unique_ptr<HostDevice> CreateD3D12Host(const HostOptions& options, std::string* error) {
  auto host = std::make_unique<D3D12Host>();
  if (!host->Init(options, error)) return nullptr;
  return host;
}

}  // namespace fcp::nr
