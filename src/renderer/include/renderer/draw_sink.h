// DrawSink: the C++ interface between the hook layer and the renderer.
//
// One call per replaced D3D function; the records are defined in nr_types.h. Without a render thread the
// implementation (the renderer) executes the call immediately on the game thread. With a render thread a
// ThreadedSink sits in between (src/renderer/kk/backend/render_thread.h) and exposes the same interface.
//
// Rules for implementations:
// - Records and everything they point to via host pointers are valid only during the call.
// - OnQuery with kQueryGetData must never block; the return value is a QueryStatus.
// - OnShaderCreated may come from any game thread (the renderer synchronizes itself).
// - Decorators (trace, counters, shader marking) wrap a DrawSink (diagnostics.h).
#pragma once

#include <cstdint>

#include "renderer/nr_types.h"

namespace fcp::nr {

class DrawSink {
 public:
  virtual ~DrawSink() = default;
  // The hook layer has found the device structure (after Direct3D_CreateDevice).
  virtual void OnDeviceCreated(uint32_t device) = 0;
  virtual void OnDraw(const DrawRecord& r) = 0;
  virtual void OnClear(const ClearRecord& r) = 0;
  virtual void OnResolve(const ResolveRecord& r) = 0;
  virtual void OnTiling(const TilingRecord& r) = 0;
  virtual void OnPredication(const PredicationRecord& r) = 0;
  // QueryStatus; samples only for kQueryReady and op == kQueryGetData.
  virtual uint32_t OnQuery(const QueryRecord& r, uint32_t* samples) = 0;
  virtual void OnShaderCreated(const ShaderCreatedRecord& r) = 0;
  virtual void OnShaderReleased(uint32_t kind, uint32_t object) = 0;
  // Frame finished (the native present path; while Swap still runs the original, the ring skimmer calls it on XE_SWAP).
  virtual void OnSwap(const SwapRecord& r) = 0;
  // CPU access to a resource (D3D::LockResource), before the original. Default: nothing.
  virtual void OnLock(const LockRecord& r) { (void)r; }
};

// Forwards everything to `inner` (base for decorators).
class ForwardingSink : public DrawSink {
 public:
  explicit ForwardingSink(DrawSink& inner) : inner_(inner) {}
  void OnDeviceCreated(uint32_t device) override { inner_.OnDeviceCreated(device); }
  void OnDraw(const DrawRecord& r) override { inner_.OnDraw(r); }
  void OnClear(const ClearRecord& r) override { inner_.OnClear(r); }
  void OnResolve(const ResolveRecord& r) override { inner_.OnResolve(r); }
  void OnTiling(const TilingRecord& r) override { inner_.OnTiling(r); }
  void OnPredication(const PredicationRecord& r) override { inner_.OnPredication(r); }
  uint32_t OnQuery(const QueryRecord& r, uint32_t* samples) override { return inner_.OnQuery(r, samples); }
  void OnShaderCreated(const ShaderCreatedRecord& r) override { inner_.OnShaderCreated(r); }
  void OnShaderReleased(uint32_t kind, uint32_t object) override { inner_.OnShaderReleased(kind, object); }
  void OnSwap(const SwapRecord& r) override { inner_.OnSwap(r); }
  void OnLock(const LockRecord& r) override { inner_.OnLock(r); }

 protected:
  DrawSink& inner_;
};

// Does nothing (queries: kQueryUnsupported). For tests and as the default while no renderer is running.
class NullSink : public DrawSink {
 public:
  void OnDeviceCreated(uint32_t) override {}
  void OnDraw(const DrawRecord&) override {}
  void OnClear(const ClearRecord&) override {}
  void OnResolve(const ResolveRecord&) override {}
  void OnTiling(const TilingRecord&) override {}
  void OnPredication(const PredicationRecord&) override {}
  uint32_t OnQuery(const QueryRecord&, uint32_t*) override { return kQueryUnsupported; }
  void OnShaderCreated(const ShaderCreatedRecord&) override {}
  void OnShaderReleased(uint32_t, uint32_t) override {}
  void OnSwap(const SwapRecord&) override {}
};

}  // namespace fcp::nr
