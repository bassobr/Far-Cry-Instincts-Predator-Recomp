// NrApi -> DrawSink (renderer/nr_api.h). This project's own code.
#include "renderer/nr_api.h"

#include <atomic>

#include "renderer/draw_sink.h"

namespace fcp::nr {

namespace {

DrawSink& S(void* c) { return *static_cast<DrawSink*>(c); }
std::atomic<const NrApi*> g_api{nullptr};

}  // namespace

NrApi MakeNrApi(DrawSink& sink) {
  NrApi a{};
  a.size = sizeof(NrApi);
  a.version = FCP_NR_API_VERSION;
  a.context = &sink;
  a.device_created = [](void* c, uint32_t device) { S(c).OnDeviceCreated(device); };
  a.draw = [](void* c, const DrawRecord* r) { S(c).OnDraw(*r); };
  a.clear = [](void* c, const ClearRecord* r) { S(c).OnClear(*r); };
  a.resolve = [](void* c, const ResolveRecord* r) { S(c).OnResolve(*r); };
  a.tiling = [](void* c, const TilingRecord* r) { S(c).OnTiling(*r); };
  a.predication = [](void* c, const PredicationRecord* r) { S(c).OnPredication(*r); };
  a.query = [](void* c, const QueryRecord* r, uint32_t* samples) { return S(c).OnQuery(*r, samples); };
  a.shader_created = [](void* c, const ShaderCreatedRecord* r) { S(c).OnShaderCreated(*r); };
  a.shader_released = [](void* c, uint32_t kind, uint32_t object) { S(c).OnShaderReleased(kind, object); };
  a.swap = [](void* c, const SwapRecord* r) { S(c).OnSwap(*r); };
  a.lock = [](void* c, const LockRecord* r) { S(c).OnLock(*r); };
  return a;
}

const NrApi* GetNrApi() { return g_api.load(std::memory_order_acquire); }
void SetNrApi(const NrApi* api) { g_api.store(api, std::memory_order_release); }

}  // namespace fcp::nr
