// DeferredCommandList (deferred_list.h). Own code.
#include "deferred_list.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fcp::nr {

unsigned AutoPipelineThreads() {
  unsigned cpus = std::max(1u, std::thread::hardware_concurrency());
#ifdef _WIN32
  DWORD_PTR proc = 0, sys = 0;
  if (GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) && proc) cpus = unsigned(std::popcount(uint64_t(proc)));
#endif
  return std::clamp(cpus > 2 ? cpus - 2 : 1u, 1u, 8u);
}

void SetPoolThreadBackground(bool background) {
#ifdef _WIN32
  SetThreadPriority(GetCurrentThread(), background ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL);
#else
  (void)background;
#endif
}

namespace {

// Keeps the objects of a GraphicsState alive until the replay (the real list would reference them while recording).
struct StateRefs {
  nvrhi::GraphicsState state;
  nvrhi::GraphicsPipelineHandle pipeline;
  nvrhi::FramebufferHandle framebuffer;
  std::vector<nvrhi::BindingSetHandle> bindings;
  std::vector<nvrhi::BufferHandle> buffers;
};

}  // namespace

void DeferredCommandList::Replay() {
  if (!recording_) return;
  recording_ = false;  // calls from the ops go straight to the real list
  skip_draw_ = false;
  std::vector<Op> ops;
  ops.swap(ops_);
  for (Op& op : ops) op(real_);
  bytes_ = 0;
  skip_draw_ = false;
}

void DeferredCommandList::clearState() {
  Defer([](nvrhi::ICommandList* r) { r->clearState(); });
}

void DeferredCommandList::clearTextureFloat(nvrhi::ITexture* t, nvrhi::TextureSubresourceSet s, const nvrhi::Color& c) {
  Defer([t = nvrhi::TextureHandle(t), s, c](nvrhi::ICommandList* r) { r->clearTextureFloat(t, s, c); });
}

void DeferredCommandList::clearDepthStencilTexture(nvrhi::ITexture* t, nvrhi::TextureSubresourceSet s, bool clear_depth,
                                                   float depth, bool clear_stencil, uint8_t stencil) {
  Defer([t = nvrhi::TextureHandle(t), s, clear_depth, depth, clear_stencil, stencil](nvrhi::ICommandList* r) {
    r->clearDepthStencilTexture(t, s, clear_depth, depth, clear_stencil, stencil);
  });
}

void DeferredCommandList::clearTextureUInt(nvrhi::ITexture* t, nvrhi::TextureSubresourceSet s, uint32_t c) {
  Defer([t = nvrhi::TextureHandle(t), s, c](nvrhi::ICommandList* r) { r->clearTextureUInt(t, s, c); });
}

void DeferredCommandList::copyTexture(nvrhi::ITexture* dest, const nvrhi::TextureSlice& ds, nvrhi::ITexture* src,
                                      const nvrhi::TextureSlice& ss) {
  Defer([d = nvrhi::TextureHandle(dest), ds, s = nvrhi::TextureHandle(src), ss](nvrhi::ICommandList* r) { r->copyTexture(d, ds, s, ss); });
}

void DeferredCommandList::copyTexture(nvrhi::IStagingTexture* dest, const nvrhi::TextureSlice& ds, nvrhi::ITexture* src,
                                      const nvrhi::TextureSlice& ss) {
  Defer([d = nvrhi::StagingTextureHandle(dest), ds, s = nvrhi::TextureHandle(src), ss](nvrhi::ICommandList* r) {
    r->copyTexture(d.Get(), ds, s.Get(), ss);
  });
}

void DeferredCommandList::copyTexture(nvrhi::ITexture* dest, const nvrhi::TextureSlice& ds, nvrhi::IStagingTexture* src,
                                      const nvrhi::TextureSlice& ss) {
  Defer([d = nvrhi::TextureHandle(dest), ds, s = nvrhi::StagingTextureHandle(src), ss](nvrhi::ICommandList* r) {
    r->copyTexture(d.Get(), ds, s.Get(), ss);
  });
}

void DeferredCommandList::writeTexture(nvrhi::ITexture* dest, uint32_t array_slice, uint32_t mip, const void* data,
                                       size_t row_pitch, size_t depth_pitch) {
  if (!recording_) return real_->writeTexture(dest, array_slice, mip, data, row_pitch, depth_pitch);
  // Copy exactly what the backend reads (rows in blocks, the smaller of source and device pitch per row), packed
  // tightly; the replay passes the packed pitches. The uploaded bytes are the same.
  const nvrhi::TextureDesc& desc = dest->getDesc();
  const nvrhi::FormatInfo& fi = nvrhi::getFormatInfo(desc.format);
  const uint32_t w = std::max(desc.width >> mip, 1u), h = std::max(desc.height >> mip, 1u);
  const uint32_t d = desc.dimension == nvrhi::TextureDimension::Texture3D ? std::max(desc.depth >> mip, 1u) : 1u;
  const uint32_t block = std::max<uint32_t>(fi.blockSize, 1);
  const size_t cols = (w + block - 1) / block, rows = (h + block - 1) / block;
  const size_t device_pitch = cols * fi.bytesPerBlock;
  if (row_pitch == 0) row_pitch = device_pitch;
  const size_t pitch = std::min(device_pitch, row_pitch);
  std::vector<uint8_t> copy(pitch * rows * d);
  for (uint32_t z = 0; z < d; ++z)
    for (size_t y = 0; y < rows; ++y)
      std::memcpy(copy.data() + (z * rows + y) * pitch, static_cast<const uint8_t*>(data) + depth_pitch * z + row_pitch * y, pitch);
  bytes_ += copy.size();
  ops_.push_back([t = nvrhi::TextureHandle(dest), array_slice, mip, copy = std::move(copy), pitch, rows](nvrhi::ICommandList* r) {
    r->writeTexture(t, array_slice, mip, copy.data(), pitch, pitch * rows);
  });
}

void DeferredCommandList::resolveTexture(nvrhi::ITexture* dest, const nvrhi::TextureSubresourceSet& ds, nvrhi::ITexture* src,
                                         const nvrhi::TextureSubresourceSet& ss) {
  Defer([d = nvrhi::TextureHandle(dest), ds, s = nvrhi::TextureHandle(src), ss](nvrhi::ICommandList* r) { r->resolveTexture(d, ds, s, ss); });
}

void DeferredCommandList::writeBuffer(nvrhi::IBuffer* b, const void* data, size_t size, uint64_t offset) {
  if (!recording_) return real_->writeBuffer(b, data, size, offset);
  std::vector<uint8_t> copy(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
  bytes_ += size;
  ops_.push_back([b = nvrhi::BufferHandle(b), copy = std::move(copy), offset](nvrhi::ICommandList* r) {
    r->writeBuffer(b, copy.data(), copy.size(), offset);
  });
}

void DeferredCommandList::clearBufferUInt(nvrhi::IBuffer* b, uint32_t value) {
  Defer([b = nvrhi::BufferHandle(b), value](nvrhi::ICommandList* r) { r->clearBufferUInt(b, value); });
}

void DeferredCommandList::copyBuffer(nvrhi::IBuffer* dest, uint64_t doff, nvrhi::IBuffer* src, uint64_t soff, uint64_t bytes) {
  Defer([d = nvrhi::BufferHandle(dest), doff, s = nvrhi::BufferHandle(src), soff, bytes](nvrhi::ICommandList* r) {
    r->copyBuffer(d, doff, s, soff, bytes);
  });
}

void DeferredCommandList::setPushConstants(const void* data, size_t size) {
  if (!recording_) return real_->setPushConstants(data, size);
  std::vector<uint8_t> copy(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
  ops_.push_back([copy = std::move(copy)](nvrhi::ICommandList* r) { r->setPushConstants(copy.data(), copy.size()); });
}

void DeferredCommandList::setGraphicsState(const nvrhi::GraphicsState& state) {
  if (!recording_) return real_->setGraphicsState(state);
  auto refs = std::make_shared<StateRefs>();
  refs->state = state;
  refs->pipeline = state.pipeline;
  refs->framebuffer = state.framebuffer;
  for (nvrhi::IBindingSet* b : state.bindings) refs->bindings.emplace_back(b);
  for (const nvrhi::VertexBufferBinding& v : state.vertexBuffers) refs->buffers.emplace_back(v.buffer);
  refs->buffers.emplace_back(state.indexBuffer.buffer);
  refs->buffers.emplace_back(state.indirectParams);
  refs->buffers.emplace_back(state.indirectCountBuffer);
  ops_.push_back([this, refs](nvrhi::ICommandList* r) {
    nvrhi::GraphicsState gs = refs->state;
    if (auto* pending = dynamic_cast<PendingPipeline*>(gs.pipeline)) {
      gs.pipeline = pending->resolved();  // set before the replay (null: failed, or not ready within the wait limit)
      if (!gs.pipeline) {
        skip_draw_ = true;  // as the synchronous path: no pipeline, no draw
        return;
      }
    }
    skip_draw_ = false;
    r->setGraphicsState(gs);
  });
}

void DeferredCommandList::draw(const nvrhi::DrawArguments& args) {
  if (!recording_) return real_->draw(args);
  ops_.push_back([this, args](nvrhi::ICommandList* r) {
    if (!skip_draw_) r->draw(args);
  });
}

void DeferredCommandList::drawIndexed(const nvrhi::DrawArguments& args) {
  if (!recording_) return real_->drawIndexed(args);
  ops_.push_back([this, args](nvrhi::ICommandList* r) {
    if (!skip_draw_) r->drawIndexed(args);
  });
}

void DeferredCommandList::beginMarker(const char* name) {
  Defer([n = std::string(name ? name : "")](nvrhi::ICommandList* r) { r->beginMarker(n.c_str()); });
}

void DeferredCommandList::endMarker() {
  Defer([](nvrhi::ICommandList* r) { r->endMarker(); });
}

void DeferredCommandList::setTextureState(nvrhi::ITexture* t, nvrhi::TextureSubresourceSet s, nvrhi::ResourceStates st) {
  Defer([t = nvrhi::TextureHandle(t), s, st](nvrhi::ICommandList* r) { r->setTextureState(t, s, st); });
}

void DeferredCommandList::setBufferState(nvrhi::IBuffer* b, nvrhi::ResourceStates s) {
  Defer([b = nvrhi::BufferHandle(b), s](nvrhi::ICommandList* r) { r->setBufferState(b, s); });
}

void DeferredCommandList::setPermanentTextureState(nvrhi::ITexture* t, nvrhi::ResourceStates s) {
  Defer([t = nvrhi::TextureHandle(t), s](nvrhi::ICommandList* r) { r->setPermanentTextureState(t, s); });
}

void DeferredCommandList::setPermanentBufferState(nvrhi::IBuffer* b, nvrhi::ResourceStates s) {
  Defer([b = nvrhi::BufferHandle(b), s](nvrhi::ICommandList* r) { r->setPermanentBufferState(b, s); });
}

void DeferredCommandList::commitBarriers() {
  Defer([](nvrhi::ICommandList* r) { r->commitBarriers(); });
}

}  // namespace fcp::nr
