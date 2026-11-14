// DeferredCommandList: an NVRHI command list that records calls and replays them later onto the real list
// (GameRenderer, FCP_RENDER_PIPELINE_ASYNC).
//
// Why: a draw whose pipeline does not exist yet would compile it on the game's render thread (10-50 ms per pipeline
// with a cold driver cache, several per frame when a new area or level appears). From the first such draw of a
// command list on, the renderer records into this list instead and hands the pipelines to worker threads; at the end
// of the list (GameRenderer::Flush) it waits for them and replays everything in the original order. The commands, their
// order and the submission point stay the same: the image is the same as with synchronous creation; only the
// pipelines of one list are made in parallel.
//
// Recorded: the calls the draw path and its helpers use (clears, copies, uploads, resolves, states, draws, markers,
// GraphicsState with a PendingPipeline). Data passed by pointer is copied. Every other call first replays what was
// recorded (the owner's materialize callback) and then goes straight to the real list, so the order is always kept.
// While not recording, every call goes straight to the real list.
// Own code.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "pipeline_pool.h"

namespace fcp::nr {

// Stand-in for a pipeline a worker is still making; only DeferredCommandList ever sees it in a GraphicsState.
class PendingPipeline final : public nvrhi::RefCounter<nvrhi::IGraphicsPipeline> {
 public:
  PendingPipeline(PipelineJobPtr job, nvrhi::GraphicsPipelineDesc desc, nvrhi::FramebufferInfo fb)
      : job_(std::move(job)), desc_(std::move(desc)), fb_(std::move(fb)) {}
  const nvrhi::GraphicsPipelineDesc& getDesc() const override { return desc_; }
  const nvrhi::FramebufferInfo& getFramebufferInfo() const override { return fb_; }
  const PipelineJobPtr& job() const { return job_; }
  // What the replay binds: set by the owner before replaying (the job's result, or null = leave the draw out).
  void Resolve(nvrhi::IGraphicsPipeline* p) { resolved_ = p; }
  nvrhi::IGraphicsPipeline* resolved() const { return resolved_; }

 private:
  PipelineJobPtr job_;
  nvrhi::GraphicsPipelineHandle resolved_;
  nvrhi::GraphicsPipelineDesc desc_;
  nvrhi::FramebufferInfo fb_;
};

class DeferredCommandList final : public nvrhi::RefCounter<nvrhi::ICommandList> {
 public:
  using Op = std::function<void(nvrhi::ICommandList*)>;

  // materialize: called when a call cannot be recorded; it must call Replay (after waiting for the pipelines).
  DeferredCommandList(nvrhi::IDevice* device, std::function<void()> materialize)
      : device_(device), materialize_(std::move(materialize)) {}

  void Begin(nvrhi::ICommandList* real) { real_ = real, recording_ = true; }
  bool recording() const { return recording_; }
  size_t ops() const { return ops_.size(); }
  uint64_t bytes() const { return bytes_; }
  // Any call as a closure (e.g. native query begin/end on the real list).
  void Defer(Op op) {
    if (!recording_) return op(real_);
    ops_.push_back(std::move(op));
  }
  // Runs every recorded call on the real list (PendingPipelines already done) and ends recording.
  void Replay();
  // The real list for everything that is not recorded (after replaying).
  nvrhi::ICommandList* Real() {
    if (recording_) materialize_();
    return real_;
  }

  // --- nvrhi::ICommandList ---------------------------------------------------------------------------------
  nvrhi::Object getNativeObject(nvrhi::ObjectType type) override { return Real()->getNativeObject(type); }
  void open() override { Real()->open(); }
  void close() override { Real()->close(); }
  void clearState() override;
  void clearTextureFloat(nvrhi::ITexture* t, nvrhi::TextureSubresourceSet s, const nvrhi::Color& c) override;
  void clearDepthStencilTexture(nvrhi::ITexture* t, nvrhi::TextureSubresourceSet s, bool clear_depth, float depth,
                                bool clear_stencil, uint8_t stencil) override;
  void clearTextureUInt(nvrhi::ITexture* t, nvrhi::TextureSubresourceSet s, uint32_t c) override;
  void copyTexture(nvrhi::ITexture* dest, const nvrhi::TextureSlice& ds, nvrhi::ITexture* src, const nvrhi::TextureSlice& ss) override;
  void copyTexture(nvrhi::IStagingTexture* dest, const nvrhi::TextureSlice& ds, nvrhi::ITexture* src,
                   const nvrhi::TextureSlice& ss) override;
  void copyTexture(nvrhi::ITexture* dest, const nvrhi::TextureSlice& ds, nvrhi::IStagingTexture* src,
                   const nvrhi::TextureSlice& ss) override;
  void writeTexture(nvrhi::ITexture* dest, uint32_t array_slice, uint32_t mip, const void* data, size_t row_pitch,
                    size_t depth_pitch) override;
  void resolveTexture(nvrhi::ITexture* dest, const nvrhi::TextureSubresourceSet& ds, nvrhi::ITexture* src,
                      const nvrhi::TextureSubresourceSet& ss) override;
  void writeBuffer(nvrhi::IBuffer* b, const void* data, size_t size, uint64_t offset) override;
  void clearBufferUInt(nvrhi::IBuffer* b, uint32_t value) override;
  void copyBuffer(nvrhi::IBuffer* dest, uint64_t doff, nvrhi::IBuffer* src, uint64_t soff, uint64_t bytes) override;
  void clearSamplerFeedbackTexture(nvrhi::ISamplerFeedbackTexture* t) override { Real()->clearSamplerFeedbackTexture(t); }
  void decodeSamplerFeedbackTexture(nvrhi::IBuffer* b, nvrhi::ISamplerFeedbackTexture* t, nvrhi::Format f) override {
    Real()->decodeSamplerFeedbackTexture(b, t, f);
  }
  void setSamplerFeedbackTextureState(nvrhi::ISamplerFeedbackTexture* t, nvrhi::ResourceStates s) override {
    Real()->setSamplerFeedbackTextureState(t, s);
  }
  void setPushConstants(const void* data, size_t size) override;
  void setGraphicsState(const nvrhi::GraphicsState& state) override;
  void draw(const nvrhi::DrawArguments& args) override;
  void drawIndexed(const nvrhi::DrawArguments& args) override;
  void drawIndirect(uint32_t o, uint32_t n) override { Real()->drawIndirect(o, n); }
  void drawIndexedIndirect(uint32_t o, uint32_t n) override { Real()->drawIndexedIndirect(o, n); }
  void drawIndexedIndirectCount(uint32_t p, uint32_t c, uint32_t m) override { Real()->drawIndexedIndirectCount(p, c, m); }
  void setComputeState(const nvrhi::ComputeState& s) override { Real()->setComputeState(s); }
  void dispatch(uint32_t x, uint32_t y, uint32_t z) override { Real()->dispatch(x, y, z); }
  void dispatchIndirect(uint32_t o) override { Real()->dispatchIndirect(o); }
  void setMeshletState(const nvrhi::MeshletState& s) override { Real()->setMeshletState(s); }
  void dispatchMesh(uint32_t x, uint32_t y, uint32_t z) override { Real()->dispatchMesh(x, y, z); }
  void dispatchMeshIndirect(uint32_t o, uint32_t m) override { Real()->dispatchMeshIndirect(o, m); }
  void dispatchMeshIndirectCount(uint32_t p, uint32_t c, uint32_t m) override { Real()->dispatchMeshIndirectCount(p, c, m); }
  void setRayTracingState(const nvrhi::rt::State& s) override { Real()->setRayTracingState(s); }
  void dispatchRays(const nvrhi::rt::DispatchRaysArguments& a) override { Real()->dispatchRays(a); }
  void buildOpacityMicromap(nvrhi::rt::IOpacityMicromap* o, const nvrhi::rt::OpacityMicromapDesc& d) override {
    Real()->buildOpacityMicromap(o, d);
  }
  void buildBottomLevelAccelStruct(nvrhi::rt::IAccelStruct* as, const nvrhi::rt::GeometryDesc* g, size_t n,
                                   nvrhi::rt::AccelStructBuildFlags f) override {
    Real()->buildBottomLevelAccelStruct(as, g, n, f);
  }
  void compactBottomLevelAccelStructs() override { Real()->compactBottomLevelAccelStructs(); }
  void copyRaytracingAccelerationStructure(nvrhi::rt::IAccelStruct* d, nvrhi::rt::IAccelStruct* s) override {
    Real()->copyRaytracingAccelerationStructure(d, s);
  }
  void buildTopLevelAccelStruct(nvrhi::rt::IAccelStruct* as, const nvrhi::rt::InstanceDesc* i, size_t n,
                                nvrhi::rt::AccelStructBuildFlags f) override {
    Real()->buildTopLevelAccelStruct(as, i, n, f);
  }
  void executeMultiIndirectClusterOperation(const nvrhi::rt::cluster::OperationDesc& d) override {
    Real()->executeMultiIndirectClusterOperation(d);
  }
  void buildTopLevelAccelStructFromBuffer(nvrhi::rt::IAccelStruct* as, nvrhi::IBuffer* b, uint64_t o, size_t n,
                                          nvrhi::rt::AccelStructBuildFlags f) override {
    Real()->buildTopLevelAccelStructFromBuffer(as, b, o, n, f);
  }
  void convertCoopVecMatrices(nvrhi::coopvec::ConvertMatrixLayoutDesc const* d, size_t n) override {
    Real()->convertCoopVecMatrices(d, n);
  }
  void beginTimerQuery(nvrhi::ITimerQuery* q) override { Real()->beginTimerQuery(q); }
  void endTimerQuery(nvrhi::ITimerQuery* q) override { Real()->endTimerQuery(q); }
  void beginMarker(const char* name) override;
  void endMarker() override;
  void setEnableAutomaticBarriers(bool e) override { Real()->setEnableAutomaticBarriers(e); }
  void setResourceStatesForBindingSet(nvrhi::IBindingSet* b) override { Real()->setResourceStatesForBindingSet(b); }
  void setEnableUavBarriersForTexture(nvrhi::ITexture* t, bool e) override { Real()->setEnableUavBarriersForTexture(t, e); }
  void setEnableUavBarriersForBuffer(nvrhi::IBuffer* b, bool e) override { Real()->setEnableUavBarriersForBuffer(b, e); }
  void beginTrackingTextureState(nvrhi::ITexture* t, nvrhi::TextureSubresourceSet s, nvrhi::ResourceStates st) override {
    Real()->beginTrackingTextureState(t, s, st);
  }
  void beginTrackingBufferState(nvrhi::IBuffer* b, nvrhi::ResourceStates s) override { Real()->beginTrackingBufferState(b, s); }
  void setTextureState(nvrhi::ITexture* t, nvrhi::TextureSubresourceSet s, nvrhi::ResourceStates st) override;
  void setBufferState(nvrhi::IBuffer* b, nvrhi::ResourceStates s) override;
  void setAccelStructState(nvrhi::rt::IAccelStruct* as, nvrhi::ResourceStates s) override { Real()->setAccelStructState(as, s); }
  void setPermanentTextureState(nvrhi::ITexture* t, nvrhi::ResourceStates s) override;
  void setPermanentBufferState(nvrhi::IBuffer* b, nvrhi::ResourceStates s) override;
  void commitBarriers() override;
  nvrhi::ResourceStates getTextureSubresourceState(nvrhi::ITexture* t, nvrhi::ArraySlice a, nvrhi::MipLevel m) override {
    return Real()->getTextureSubresourceState(t, a, m);
  }
  nvrhi::ResourceStates getBufferState(nvrhi::IBuffer* b) override { return Real()->getBufferState(b); }
  nvrhi::IDevice* getDevice() override { return device_; }
  const nvrhi::CommandListParameters& getDesc() override { return Real()->getDesc(); }

 private:
  nvrhi::IDevice* device_ = nullptr;
  nvrhi::ICommandList* real_ = nullptr;
  std::function<void()> materialize_;
  bool recording_ = false;
  bool skip_draw_ = false;  // replay: the last GraphicsState had a pipeline that failed (the draw is left out)
  std::vector<Op> ops_;
  uint64_t bytes_ = 0;
};

}  // namespace fcp::nr
