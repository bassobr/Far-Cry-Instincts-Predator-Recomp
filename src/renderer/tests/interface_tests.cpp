// Interfaces without GPU and without the game: device view from the configuration, NrApi, decorators, switches,
// render thread with artificial guest memory.
#include <cstring>
#include <map>
#include <vector>

#include "kk/backend/render_thread.h"
#include "kk/backend/tests/check.h"
#include "renderer/diagnostics.h"
#include "renderer/guest_device.h"
#include "renderer/nr_api.h"

using namespace fcp::nr;

namespace {

GuestLayout RepoLayout() {
  GuestLayout l;
  std::string error;
  const bool ok = l.Load(std::string(FCP_REPO_DIR) + "/config/predator/renderer.toml", &error);
  if (!ok) std::fprintf(stderr, "  %s\n", error.c_str());
  CHECK(ok);
  return l;
}

// Guest memory: a virtual -> bytes mapping for a few ranges.
class FakeGuest final : public GuestMemory {
 public:
  uint8_t* Map(uint32_t address, uint32_t size) {
    auto& v = regions_[address];
    v.assign(size, 0);
    return v.data();
  }
  const uint8_t* Virtual(uint32_t address) const override {
    for (const auto& [a, v] : regions_)
      if (address >= a && address - a < v.size()) return v.data() + (address - a);
    return nullptr;
  }
  const uint8_t* Physical(uint32_t) const override { return nullptr; }

 private:
  std::map<uint32_t, std::vector<uint8_t>> regions_;
};

// Records what arrives and reads the device state through the snapshot view on each draw.
class RecordingSink final : public NullSink {
 public:
  RecordingSink(const GuestMemory& memory, const GuestLayout& layout) : memory_(memory), layout_(layout) {}
  void OnDraw(const DrawRecord& r) override {
    const uint8_t* dev = memory_.Virtual(r.device);
    GuestDeviceView view(layout_, dev);
    depth_controls.push_back(view.u32(DeviceField::rb_depth_control));
    up_first.push_back(r.up_data ? r.up_data[0] : 0);
    lrs.push_back(r.call.lr);
  }
  void OnSwap(const SwapRecord& r) override { swaps.push_back(r.frame); }
  uint32_t OnQuery(const QueryRecord& r, uint32_t* samples) override {
    if (r.op == kQueryGetData) {
      if (samples) *samples = 1234;
      return kQueryReady;
    }
    ++issued;
    return kQueryNotReady;
  }
  std::vector<uint32_t> depth_controls, swaps, lrs;
  std::vector<uint8_t> up_first;
  int issued = 0;

 private:
  const GuestMemory& memory_;
  const GuestLayout& layout_;
};

}  // namespace

TEST(GuestLayout_LoadsRepoConfig) {
  const GuestLayout l = RepoLayout();
  CHECK_EQ(l.device_size, 20608u);
  // Spot checks against the measured D3D device layout and the status marker.
  CHECK_EQ(l.field(DeviceField::rb_depth_control).offset, 11636u);
  CHECK(l.field(DeviceField::rb_depth_control).status == FieldStatus::kMeasured);
  CHECK(l.field(DeviceField::rb_blend_control0).status == FieldStatus::kKingKong);
  CHECK_EQ(l.field(DeviceField::gamma_ramp).offset, 14076u);
  CHECK_EQ(l.field(DeviceField::vertex_shader).offset, 20476u);
  CHECK_EQ(l.field(DeviceField::fetch_constants).stride, 24u);
  CHECK_EQ(l.surface.size, 64u);
  CHECK_EQ(l.declaration.get("elements"), 36u);
  // Every configured field that the renderer reads during execution lies in the render thread's snapshot.
  const auto outside = l.FieldsOutsideSnapshot();
  for (const auto& n : outside) std::fprintf(stderr, "  not in the snapshot: %s\n", n.c_str());
  CHECK(outside.empty());
  // All fields are configured.
  for (size_t i = 0; i < size_t(DeviceField::kCount); ++i) CHECK(l.fields[i].status != FieldStatus::kMissing);
}

TEST(GuestLayout_RejectsBadConfig) {
  GuestLayout l;
  std::string error;
  CHECK(!l.LoadText("[device]\nsize = 100\n[device.fields]\nvs_constants = { offset = 200, status = \"kk\" }\n", &error));
  CHECK(!error.empty());
  CHECK(!l.LoadText("[device]\nsize = 100\n[device.fields]\nnot_a_field = { offset = 4, status = \"kk\" }\n", &error));
  CHECK(!l.LoadText("[device]\nsize = 100\n[device.fields]\nvs_constants = { offset = 4 }\n", &error));
  CHECK(l.LoadText("[device]\nsize = 100\n[device.fields]\nvs_constants = { offset = 4, status = \"gemessen\" }\n", &error));
  // Fields that are not configured read 0 and report has() == false.
  uint8_t dev[100] = {};
  dev[4] = 0x12, dev[5] = 0x34, dev[6] = 0x56, dev[7] = 0x78;
  GuestDeviceView v(l, dev);
  CHECK_EQ(v.u32(DeviceField::vs_constants), 0x12345678u);
  CHECK(!v.has(DeviceField::ps_constants));
  CHECK_EQ(v.u32(DeviceField::ps_constants), 0u);
}

TEST(NrApi_ForwardsEveryEntry) {
  FakeGuest guest;
  GuestLayout layout;
  std::string error;
  CHECK(layout.LoadText("[device]\nsize = 64\n", &error));
  RecordingSink sink(guest, layout);
  CountingSink counting(sink, false);
  const NrApi api = MakeNrApi(counting);
  CHECK_EQ(api.size, uint32_t(sizeof(NrApi)));
  CHECK_EQ(api.version, FCP_NR_API_VERSION);
  SetNrApi(&api);
  const NrApi* a = GetNrApi();
  CHECK(a == &api);
  const uint64_t draws = Counters::Get().draws;
  DrawRecord d{};
  d.call.lr = 0x1234;
  a->draw(a->context, &d);
  QueryRecord q{};
  q.op = kQueryGetData;
  uint32_t samples = 0;
  CHECK_EQ(a->query(a->context, &q, &samples), uint32_t(kQueryReady));
  CHECK_EQ(samples, 1234u);
  SwapRecord s{};
  s.frame = 7;
  a->swap(a->context, &s);
  CHECK_EQ(Counters::Get().draws - draws, uint64_t(1));
  CHECK_EQ(sink.lrs.size(), size_t(1));
  CHECK_EQ(sink.swaps.size(), size_t(1));
  SetNrApi(nullptr);
  CHECK(GetNrApi() == nullptr);
}

TEST(Switches_ReadOnceFromEnvironment) {
  static std::map<std::string, std::string> env;
  env = {{"FCP_RENDER_BACKEND", "d3d12"}, {"FCP_RENDER_SOLID_PS", "00000000DEADBEEF"}, {"FCP_GPU_DEBUG", "1"},
         {"FCP_DUMP_FRAMES", "out/f%d.png"}};
  const Switches s = Switches::Read([](const char* name) -> const char* {
    auto it = env.find(name);
    return it == env.end() ? nullptr : it->second.c_str();
  });
  CHECK(s.backend == "d3d12");
  CHECK(s.gpu_debug);
  CHECK(!s.gpu_warp);
  CHECK_EQ(s.solid_ps, 0xDEADBEEFull);
  CHECK(s.IsMarked(1, 0xDEADBEEF));
  CHECK(!s.IsMarked(0xDEADBEEF, 2));
  CHECK(DumpPath(s.dump_frames, 12) == "out/f12.png");
}

TEST(ThreadedSink_CopiesDeviceBlocksAndUpData) {
  const GuestLayout layout = RepoLayout();
  FakeGuest guest;
  constexpr uint32_t kDevice = 0x40000000;
  uint8_t* dev = guest.Map(kDevice, layout.device_size);
  SnapshotGuestMemory snapshot(guest);
  RecordingSink inner(snapshot, layout);  // reads through the snapshot view like the renderer
  const uint32_t dc = layout.field(DeviceField::rb_depth_control).offset;
  {
    ThreadedSink threaded(inner, guest, layout);
    uint8_t up[16] = {};
    for (uint32_t i = 0; i < 100; ++i) {
      StoreBE32(dev + dc, i);
      up[0] = uint8_t(i);  // the game changes the device right after the call
      DrawRecord d{};
      d.device = kDevice;
      d.kind = kDrawVerticesUP;
      d.up_data = up;
      d.up_bytes = sizeof(up);
      threaded.OnDraw(d);
      up[0] = 0xFF;  // the pointer is only valid during the call
    }
    QueryRecord q{};
    q.op = kQueryGetData;
    uint32_t samples = 0;
    CHECK_EQ(threaded.OnQuery(q, &samples), uint32_t(kQueryReady));  // never through the queue
    SwapRecord s{};
    s.device = kDevice;
    s.frame = 1;
    threaded.OnSwap(s);
    threaded.WaitDone(threaded.Ticket());
    const auto st = threaded.stats();
    CHECK_EQ(st.commands, uint64_t(101));
    // Per draw only the changed block (after the first one, which copies everything).
    CHECK(st.device_bytes < uint64_t(layout.device_size) + 101 * 64);
  }
  CHECK_EQ(inner.depth_controls.size(), size_t(100));
  bool in_order = true;
  for (uint32_t i = 0; i < inner.depth_controls.size(); ++i)
    in_order = in_order && inner.depth_controls[i] == i && inner.up_first[i] == uint8_t(i);
  CHECK(in_order);
  CHECK_EQ(inner.swaps.size(), size_t(1));
}
