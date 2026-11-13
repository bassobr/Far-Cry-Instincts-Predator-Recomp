// Tests with real data from the user's disc. Skipped if the data is missing.
//
// Inputs (tools/renderer_tests.cmd creates them; derived game data under work/, never commit it):
//   FCP_RENDERER_DDS_DIR   folder with DDS/XBT from common.fat (tools/xfat.py extract), default work/renderer/common
//   FCP_SHADER_PACK_DIR    folder with predator.spv.fcpack / predator.dxil.fcpack (fcshaders pack), default work/shaders
//   FCP_PIPELINE_THREADS   threads for building pipelines (default 16)
#include <algorithm>
#include <atomic>
#include <map>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>

#include "disc_util.h"
#include "fcshaders/compiler.h"
#include "fcshaders/pack.h"
#include "kk/backend/tests/check.h"
#include "renderer/diagnostics.h"
#include "renderer/host_device.h"
#include "renderer/shader_library.h"
#include "renderer/translated_layout.h"

using namespace fcp::nr;
using namespace fcp::test;
namespace fs = std::filesystem;

namespace {

std::string EnvOr(const char* name, const std::string& fallback) {
  const char* v = std::getenv(name);
  return v && *v ? v : fallback;
}

double Ms(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

bool HaveDisc() {
  if (Exists(DiscDir())) return true;
  std::printf("  skipped: %s missing\n", DiscDir().c_str());
  return false;
}

void BuildAllPipelines(HostApi api) {
  if (!HaveDisc()) return;
  const fcshaders::PackTarget target = api == HostApi::kVulkan ? fcshaders::PackTarget::Spirv : fcshaders::PackTarget::Dxil;
  const std::string pack = EnvOr("FCP_SHADER_PACK_DIR", RepoPath("work/shaders")) + "/predator." +
                           fcshaders::packTargetName(target) + ".fcpack";
  if (!Exists(pack)) {
    std::printf("  skipped: %s missing (tools\\renderer_tests.cmd builds it)\n", pack.c_str());
    return;
  }
  std::string error;
  CHECK(fcshaders::loadDxc("", &error));
  fcshaders::PackReader reader;
  CHECK(reader.open(pack, fcshaders::buildStamp(), &error));
  if (!reader.isOpen()) return;
  CHECK(!reader.stale());
  HostOptions o;
  o.api = api;
  auto host = CreateHostDevice(o, &error);
  if (!host) {
    std::printf("  skipped: no %s device (%s)\n", HostApiName(api), error.c_str());
    return;
  }
  std::printf("  %s, pack %zu entries\n", host->description().c_str(), reader.count());
  nvrhi::IDevice* dev = host->device();
  TranslatedLayout layout;
  CHECK(layout.Init(dev, TableCapacities{}, &error));

  // All shader objects (time: reading the pack + createShader).
  auto t0 = std::chrono::steady_clock::now();
  std::vector<nvrhi::ShaderHandle> vs, ps;
  size_t shader_failures = 0;
  for (uint64_t h : reader.hashes()) {
    fcshaders::PackEntry e;
    CHECK(reader.find(h, e));
    ShaderEntry entry;
    entry.container_hash = h;
    entry.kind = e.kind;
    entry.code = std::move(e.code);
    nvrhi::ShaderHandle s = layout.CreateShader(entry);
    if (!s) {
      ++shader_failures;
      continue;
    }
    (e.kind == fcshaders::ShaderKind::Vertex ? vs : ps).push_back(s);
  }
  const double shader_ms = Ms(t0);
  CHECK_EQ(shader_failures, size_t(0));

  // Targets as in the design: one RGBA8 colour target, D32S8 depth.
  nvrhi::TextureDesc cd;
  cd.width = cd.height = 64;
  cd.format = nvrhi::Format::RGBA8_UNORM;
  cd.isRenderTarget = true;
  cd.initialState = nvrhi::ResourceStates::RenderTarget;
  cd.keepInitialState = true;
  nvrhi::TextureHandle color = dev->createTexture(cd);
  nvrhi::TextureDesc dd = cd;
  dd.format = nvrhi::Format::D32S8;
  dd.initialState = nvrhi::ResourceStates::DepthWrite;
  nvrhi::TextureHandle depth = dev->createTexture(dd);
  nvrhi::FramebufferHandle fb = dev->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(color).setDepthAttachment(depth));
  const nvrhi::FramebufferInfo info = fb->getFramebufferInfo();

  // Artificial pairing: the interpolator signature is the same for all translated shaders, so every VS fits every
  // PS. Every shader occurs at least once: max(VS, PS) pipelines.
  const size_t count = std::max(vs.size(), ps.size());
  const unsigned threads = unsigned(std::max(1, std::atoi(EnvOr("FCP_PIPELINE_THREADS", "16").c_str())));
  std::atomic<size_t> next{0}, failed{0};
  std::vector<double> times(count, 0.0);
  FixedState state;
  t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> pool;
  for (unsigned k = 0; k < threads; ++k)
    pool.emplace_back([&] {
      for (;;) {
        const size_t i = next++;
        if (i >= count) break;
        const auto a = std::chrono::steady_clock::now();
        nvrhi::GraphicsPipelineHandle p = layout.CreatePipeline(vs[i % vs.size()], ps[i % ps.size()], info, state);
        times[i] = Ms(a);
        if (!p) ++failed;
      }
    });
  for (auto& t : pool) t.join();
  const double wall = Ms(t0);
  std::sort(times.begin(), times.end());
  const double p99 = times.empty() ? 0 : times[size_t((times.size() - 1) * 0.99)];
  double sum = 0;
  for (double t : times) sum += t;
  std::printf("  %zu VS, %zu PS, shader objects %.0f ms; %zu pipelines with %u threads: %.2f s wall, mean %.2f ms, p99 %.1f ms, "
              "max %.1f ms, errors %zu\n",
              vs.size(), ps.size(), shader_ms, count, threads, wall / 1000, count ? sum / count : 0, p99,
              times.empty() ? 0 : times.back(), size_t(failed));
  CHECK_EQ(size_t(failed), size_t(0));
  if (api == HostApi::kVulkan) host->SavePipelineCache();
}

}  // namespace

TEST(Disc_TexturesThroughGuestPath) {
  if (!HaveDisc()) return;
  const std::string dir = EnvOr("FCP_RENDERER_DDS_DIR", RepoPath("work/renderer/common"));
  if (!Exists(dir)) {
    std::printf("  skipped: %s missing (tools\\renderer_tests.cmd unpacks common.fat)\n", dir.c_str());
    return;
  }
  size_t files = 0, loaded = 0, skipped = 0, equal = 0, compared = 0;
  double encode = 0, convert = 0;
  uint64_t bytes = 0;
  std::map<std::string, size_t> formats;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    const auto ext = e.path().extension().string();
    if (ext != ".dds" && ext != ".xbt") continue;
    ++files;
    DdsImage dds;
    std::string why;
    if (!LoadDds(e.path().string(), dds, &why)) {
      ++skipped;  // cube maps, rare formats
      continue;
    }
    ++loaded;
    formats[dds.fourcc]++;
    RoundTrip rt;
    const bool ok = GuestRoundTrip(dds, true, rt, &why);
    if (!ok) std::printf("  %s: %s\n", e.path().filename().string().c_str(), why.c_str());
    CHECK(ok);
    if (!ok) continue;
    encode += rt.encode_ms;
    convert += rt.convert_ms;
    bytes += rt.host.bytes.size();
    if (rt.host.plan.conversion == fcp::res::Conversion::kCopy) {
      ++compared;
      equal += rt.bytes_equal;
      if (!rt.bytes_equal) std::printf("  different: %s (%s %ux%u)\n", e.path().filename().string().c_str(), dds.fourcc.c_str(), dds.width, dds.height);
    }
  }
  std::printf("  %zu files, %zu loaded, %zu skipped; formats:", files, loaded, skipped);
  for (const auto& [k, v] : formats) std::printf(" %s %zu", k.c_str(), v);
  std::printf("\n  byte-identical after tiling+swap+untiling: %zu of %zu (kCopy formats); %.1f MB, encoding %.0f ms, "
              "conversion (kk ConvertTexture) %.0f ms\n",
              equal, compared, bytes / 1048576.0, encode, convert);
  CHECK(loaded > 0);
  CHECK_EQ(equal, compared);
}

TEST(Disc_AllPackShadersToPipelines_Vulkan) { BuildAllPipelines(HostApi::kVulkan); }
TEST(Disc_AllPackShadersToPipelines_D3D12) { BuildAllPipelines(HostApi::kD3D12); }
