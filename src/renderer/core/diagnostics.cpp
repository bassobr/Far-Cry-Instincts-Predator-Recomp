// Switches, counters, image dumps (renderer/diagnostics.h) and host device odds and ends. Own code.
#include "renderer/diagnostics.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <nvrhi/nvrhi.h>

#include "kk/resources/image_io.h"
#include "renderer/host_device.h"

namespace fcp::nr {

namespace {

uint64_t ParseHex(const char* s) { return s && *s ? std::strtoull(s, nullptr, 16) : 0; }
bool On(const char* s) { return s && (std::strcmp(s, "1") == 0 || std::strcmp(s, "true") == 0 || std::strcmp(s, "on") == 0); }
const char* EnvGet(const char* name) { return std::getenv(name); }

class MessageCallback final : public nvrhi::IMessageCallback {
 public:
  void message(nvrhi::MessageSeverity severity, const char* text) override {
    const char* level = "info";
    switch (severity) {
      case nvrhi::MessageSeverity::Info: return;  // too much; warnings and errors are enough
      case nvrhi::MessageSeverity::Warning: level = "warn"; break;
      case nvrhi::MessageSeverity::Error: level = "error"; Counters::Get().nvrhi_errors++; break;
      case nvrhi::MessageSeverity::Fatal: level = "fatal"; Counters::Get().nvrhi_errors++; break;
    }
    static std::atomic<uint32_t> shown{0};
    const uint32_t n = shown.fetch_add(1);
    if (n < 200) LogLine("[nvrhi %s] %s", level, text);
    else if (n == 200) LogLine("[nvrhi] further messages are only counted");
  }
};

std::atomic<void (*)(const char*)> g_log_sink{nullptr};

void Trace(bool on, const char* what, uint32_t frame, uint32_t a, uint32_t b, uint32_t c, uint32_t lr) {
  if (on) std::fprintf(stderr, "[draw] f%u %s %08X %08X %08X lr=%08X\n", frame, what, a, b, c, lr);
}

}  // namespace

void SetLogSink(void (*sink)(const char* line)) { g_log_sink.store(sink); }

void LogLine(const char* format, ...) {
  char buffer[2048];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  if (auto sink = g_log_sink.load()) sink(buffer);
  else std::fprintf(stderr, "%s\n", buffer);
}

Switches Switches::Read(const char* (*env)(const char*)) {
  Switches s;
  if (const char* v = env("FCP_RENDER_BACKEND"); v && *v) s.backend = v;
  s.gpu_debug = On(env("FCP_GPU_DEBUG"));
  s.gpu_warp = On(env("FCP_GPU_WARP"));
  s.solid_ps = ParseHex(env("FCP_RENDER_SOLID_PS"));
  s.solid_vs = ParseHex(env("FCP_RENDER_SOLID_VS"));
  if (const char* v = env("FCP_RENDER_SOLID_COLOR"); v && *v) s.solid_color = uint32_t(std::strtoul(v, nullptr, 16));
  if (const char* v = env("FCP_DUMP_FRAMES")) s.dump_frames = v;
  if (const char* v = env("FCP_DUMP_FRAME_AT"); v && *v) s.dump_frame_at = std::atoll(v);
  if (const char* v = env("FCP_SHADER_PACK")) s.shader_pack = v;
  if (const char* v = env("FCP_SHADER_CACHE")) s.shader_cache = v;
  if (const char* v = env("FCP_PIPELINE_CACHE")) s.pipeline_cache = v;
  if (const char* v = env("FCP_DRIVER_PIPELINE_CACHE")) s.driver_pipeline_cache = v;
  s.shader_pack_stale_ok = On(env("FCP_SHADER_PACK_STALE_OK"));
  s.trace_draws = On(env("FCP_TRACE_DRAWS"));
  s.stats = On(env("FCP_STATS"));
  return s;
}

const Switches& Switches::Get() {
  static const Switches s = Read(&EnvGet);
  return s;
}

Counters& Counters::Get() {
  static Counters c;
  return c;
}

void Counters::AddWait(std::chrono::steady_clock::time_point since) {
  wait_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - since).count());
}

std::string Counters::Summary() const {
  std::ostringstream o;
  o << "draws " << draws << " (marked " << draws_marked << ", skipped " << draws_skipped << "), clears " << clears
    << ", resolves " << resolves << ", tiling " << tilings << ", queries " << queries << ", swaps " << swaps
    << ", shader " << shaders_created << "; pipelines new " << pipelines_created << " ("
    << pipeline_create_us / 1000.0 << " ms), from cache " << pipelines_cached << "; GPU wait " << wait_us / 1000.0
    << " ms; errors nvrhi " << nvrhi_errors << ", validation " << validation_errors;
  return o.str();
}

void CountingSink::OnDraw(const DrawRecord& r) {
  Counters::Get().draws++;
  Trace(trace_, "draw", r.frame, r.kind, r.primitive, r.count, r.call.lr);
  inner_.OnDraw(r);
}
void CountingSink::OnClear(const ClearRecord& r) {
  Counters::Get().clears++;
  Trace(trace_, "clear", r.frame, r.flags, r.stencil, 0, r.call.lr);
  inner_.OnClear(r);
}
void CountingSink::OnResolve(const ResolveRecord& r) {
  Counters::Get().resolves++;
  Trace(trace_, "resolve", r.frame, r.flags, r.dest_texture, 0, r.call.lr);
  inner_.OnResolve(r);
}
void CountingSink::OnTiling(const TilingRecord& r) {
  Counters::Get().tilings++;
  Trace(trace_, r.begin ? "begin_tiling" : "end_tiling", r.frame, r.flags, r.rect_count, r.dest_texture, r.call.lr);
  inner_.OnTiling(r);
}
uint32_t CountingSink::OnQuery(const QueryRecord& r, uint32_t* samples) {
  Counters::Get().queries++;
  Trace(trace_, "query", r.frame, r.op, r.query, r.flags, r.call.lr);
  return inner_.OnQuery(r, samples);
}
void CountingSink::OnShaderCreated(const ShaderCreatedRecord& r) {
  Counters::Get().shaders_created++;
  Trace(trace_, "shader", 0, r.kind, r.object, r.container, r.call.lr);
  inner_.OnShaderCreated(r);
}
void CountingSink::OnSwap(const SwapRecord& r) {
  Counters::Get().swaps++;
  Trace(trace_, "swap", r.frame, r.front_buffer, r.flags, 0, r.call.lr);
  inner_.OnSwap(r);
}

std::string DumpPath(const std::string& pattern, uint64_t frame) {
  const size_t at = pattern.find("%d");
  if (at == std::string::npos) return pattern;
  return pattern.substr(0, at) + std::to_string(frame) + pattern.substr(at + 2);
}

bool WriteImage(const std::string& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba,
                std::string* error) {
  if (rgba.size() < size_t(width) * height * 4) return error && (*error = "not enough image data", false);
  auto ends = [&](const char* ext) {
    return path.size() >= std::strlen(ext) && _stricmp(path.c_str() + path.size() - std::strlen(ext), ext) == 0;
  };
  if (ends(".png")) {
    if (!fcp_res_tools::WritePng(path, width, height, rgba)) return error && (*error = "file " + path + " cannot be written", false);
    return true;
  }
  // BMP, 24 bit, bottom first.
  const uint32_t row = (width * 3 + 3) & ~3u;
  const uint32_t size = 54 + row * height;
  std::vector<uint8_t> f(size, 0);
  auto u16 = [&](size_t o, uint32_t v) { f[o] = uint8_t(v), f[o + 1] = uint8_t(v >> 8); };
  auto u32 = [&](size_t o, uint32_t v) { u16(o, v), u16(o + 2, v >> 16); };
  f[0] = 'B', f[1] = 'M';
  u32(2, size), u32(10, 54), u32(14, 40), u32(18, width), u32(22, height), u16(26, 1), u16(28, 24), u32(34, row * height);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* src = rgba.data() + size_t(height - 1 - y) * width * 4;
    uint8_t* dst = f.data() + 54 + size_t(y) * row;
    for (uint32_t x = 0; x < width; ++x) dst[x * 3] = src[x * 4 + 2], dst[x * 3 + 1] = src[x * 4 + 1], dst[x * 3 + 2] = src[x * 4];
  }
  std::FILE* out = std::fopen(path.c_str(), "wb");
  if (!out) return error && (*error = "file " + path + " cannot be written", false);
  const bool ok = std::fwrite(f.data(), 1, f.size(), out) == f.size();
  return (std::fclose(out) == 0 && ok) || (error && (*error = "write error " + path, false));
}

bool ReadbackRgba8(nvrhi::IDevice* device, nvrhi::ITexture* texture, std::vector<uint8_t>& rgba, std::string* error) {
  const nvrhi::TextureDesc& d = texture->getDesc();
  const bool bgra = d.format == nvrhi::Format::BGRA8_UNORM || d.format == nvrhi::Format::SBGRA8_UNORM;
  if (!bgra && d.format != nvrhi::Format::RGBA8_UNORM && d.format != nvrhi::Format::SRGBA8_UNORM)
    return error && (*error = "readback only for RGBA8/BGRA8", false);
  nvrhi::TextureDesc sd = d;
  sd.isRenderTarget = false;
  sd.isUAV = false;
  sd.initialState = nvrhi::ResourceStates::CopyDest;
  sd.keepInitialState = true;
  sd.debugName = "Readback";
  nvrhi::StagingTextureHandle staging = device->createStagingTexture(sd, nvrhi::CpuAccessMode::Read);
  nvrhi::CommandListHandle cl = device->createCommandList();
  cl->open();
  cl->copyTexture(staging, nvrhi::TextureSlice(), texture, nvrhi::TextureSlice());
  cl->close();
  device->executeCommandList(cl);
  const auto t0 = std::chrono::steady_clock::now();
  device->waitForIdle();
  Counters::Get().AddWait(t0);
  size_t pitch = 0;
  const auto* p = static_cast<const uint8_t*>(device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
  if (!p) return error && (*error = "mapStagingTexture failed", false);
  rgba.resize(size_t(d.width) * d.height * 4);
  for (uint32_t y = 0; y < d.height; ++y) {
    std::memcpy(rgba.data() + size_t(y) * d.width * 4, p + y * pitch, size_t(d.width) * 4);
    if (bgra)
      for (uint32_t x = 0; x < d.width; ++x) std::swap(rgba[(size_t(y) * d.width + x) * 4], rgba[(size_t(y) * d.width + x) * 4 + 2]);
  }
  device->unmapStagingTexture(staging);
  return true;
}

const char* HostApiName(HostApi api) { return api == HostApi::kVulkan ? "vulkan" : "d3d12"; }

nvrhi::IMessageCallback* HostMessageCallback() {
  static MessageCallback callback;
  return &callback;
}

std::unique_ptr<HostDevice> CreateHostDevice(const HostOptions& options, std::string* error) {
  return options.api == HostApi::kVulkan ? CreateVulkanHost(options, error) : CreateD3D12Host(options, error);
}

}  // namespace fcp::nr
