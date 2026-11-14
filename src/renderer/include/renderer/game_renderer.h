// GameRenderer: the native in-game renderer.
//
// A DrawSink that executes the records of the D3D hooks: state from the device structure (GuestDeviceView,
// offsets from config/predator/renderer.toml), shaders from the ShaderLibrary (disc pack, disk cache,
// translation in the create hook), textures and vertex data from guest memory (King Kong's conversion), targets as
// host textures without EDRAM. Predicated tiling is not emulated: BeginTiling..EndTiling is a normal full-size
// pass, EndTiling resolves the whole image into the destination texture. Resolve targets live on as host
// textures and replace the guest data at their address when sampled.
//
// Presentation: window and swapchain belong to this renderer (HostDevice). The SDK calls BeginPresent/EndPresent
// through IHostPresenter (rexglue, local change 9) on its present thread; the image is the copy that OnSwap made
// from the game's front buffer.
//
// Threads: the game threads call the DrawSink methods (the game serializes them through device ownership),
// the present thread calls Begin/EndPresent. A mutex protects device and queue.
#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <string>

#include "renderer/draw_sink.h"
#include "renderer/host_device.h"
#include "renderer/overlay.h"
#include "renderer/texture_replace.h"

namespace fcp::nr {

struct GameRendererOptions {
  HostApi api = HostApi::kVulkan;
  bool debug = false;               // FCP_GPU_DEBUG: API validation layer + NVRHI validation
  bool vsync = true;
  std::string layout_path;          // config/predator/renderer.toml (copy next to the EXE)
  std::string shader_pack;          // empty: no pack
  std::string shader_cache;         // disk cache directory; empty: none
  std::string dxc_path;             // empty: next to the EXE
  std::string driver_pipeline_cache;  // Vulkan driver cache (file), empty: none
  std::string dump_pattern;         // FCP_DUMP_FRAMES: image file, "%d" = frame number
  std::string dump_frames;          // FCP_DUMP_FRAME_AT: list "n,n,..." (empty: none)
  uint32_t dump_every = 0;          // FCP_DUMP_FRAME_EVERY: every n frames
  bool dump_present = false;        // FCP_DUMP_PRESENT=1: dumps from the shown image (swapchain, with SMAA/bars)
  uint64_t dump_resolves_frame = ~0ull;  // FCP_DUMP_RESOLVES=<n>: every resolve target (RGBA8) of frame n
  uint32_t stats_every = 150;       // frame statistics to the log every n frames (0: off)
  bool trace_draws = false;         // FCP_TRACE_DRAWS
  uint64_t trace_first = ~0ull;     // FCP_RENDER_TRACE=<frame>[,<count>]: every draw/resolve of these frames to the log
  uint64_t trace_count = 1;
  // FCP_RENDER_PROBE=<x>,<y>: in the trace frames, read pixel (x,y) of color target 0 after every
  // draw/clear/resolve and log every change (draw probe). -1: off.
  int32_t probe_x = -1, probe_y = -1;
  uint64_t trace_consts = 0;
  // FCP_RENDER_CONDITIONAL: how draws inside BeginConditionalRendering..End are handled.
  //   0 = draw all (as before), 1 = none (diagnostics), 2 = by survey result (GPU count)
  uint32_t conditional_mode = 0;
  // FCP_RENDER_QUERY: "original" = answer of the original (null driver), 1 = "visible" (the hooks set a
  // fixed value), 2 = "host": real host occlusion queries, count in guest samples (host count × guest MSAA /
  // host MSAA / scale²), GetData never blocks (NotReady until the result is there). Default in game: host (render_bridge).
  uint32_t query_mode = 0;
  // Pipeline list on disk (records without game code: shader hashes and register values); new pipelines are
  // appended and pre-created in the background at startup (FCP_PIPELINE_PREWARM=0 off). Empty: no list.
  std::string pipeline_list;
  bool pipeline_prewarm = true;
  // Shipped pipeline list (pipelines.fcpl next to the EXE, tools/pipelines_build.ps1): shader container hashes and
  // render state only, the same for Vulkan and D3D12. Prewarmed together with pipeline_list, menu first, the level
  // being loaded moved ahead (OnLevelLoading). Empty: none (FCP_RENDER_PIPELINE_SHIPPED=0).
  std::string pipeline_shipped;
  // FCP_PIPELINE_COLLECT=<folder>: every record a draw uses goes to <folder>/<group>.bin (group "menu" before the
  // first level, else the level name) - input of tools/pipelines_build.ps1.
  std::string pipeline_collect;
  // FCP_RENDER_PREWARM_LOW_PRIORITY=0: prewarm jobs at normal thread priority (default: below normal).
  bool prewarm_low_priority = true;
  // Missing pipelines on worker threads: the rest of the command list is recorded and replayed in order once its
  // pipelines are made (same commands, same image; the pipelines of a list are made in parallel), the pipeline list
  // is prewarmed by the same threads. FCP_RENDER_PIPELINE_ASYNC=0: on the render thread as before (one prewarm thread).
  bool pipeline_async = false;
  uint32_t pipeline_threads = 0;  // FCP_RENDER_PIPELINE_THREADS, 0 = logical CPUs of the process - 2 (1..8)
  // FCP_RENDER_PIPELINE_MAX_WAIT_MS=<ms> (NOT the default, changes the image): wait at most this long at the end of a
  // command list; draws whose pipeline is not ready are left out of that frame (pop-in instead of a hitch). -1: wait.
  double pipeline_max_wait_ms = -1.0;
  // Shaders missing from pack and disk cache (the game's run-time shaders, first start) are translated on worker
  // threads (as many as pipeline_threads); the first draw with the shader waits for it. Same code as before.
  // FCP_RENDER_SHADER_ASYNC=0: in the create hook on the game's thread.
  bool shader_async = false;
  bool depth_resolve = true;        // FCP_RENDER_DEPTH_RESOLVE=0: skip depth resolves (old behavior)        // FCP_TRACE_CONSTS=<hash>: in trace frames c0-c31 of draws with this VS/PS
  std::set<uint64_t> skip_ps;       // FCP_RENDER_SKIP_PS=<hash>[,<hash>..]: omit draws with these pixel shaders
  bool texture_check = true;
  // FCP_RENDER_TEXTURE_WATCH=0: hash guest textures every frame instead of using the SDK write watch.
  bool texture_watch = true;
  // FCP_RENDER_LOCK_READBACK=0: do not write resolve targets back to guest memory on CPU lock (the CPU
  // then reads zeros; old behavior: black loading screen).
  bool lock_readback = true;
  // FCP_RENDER_GAMMA=0: do not apply the game's gamma ramp (SetGammaRamp) at present.
  bool gamma_ramp = true;
  // FCP_RENDER_MSAA (on by default in game, =0 off): targets with the guest's MSAA level (2x main target, 4x 512x512), hardware resolve.
  bool msaa = false;
  // [graphics] msaa = 2|4|8: sample count of the screen targets the game renders with MSAA
  // (main target 1280x720 and its depth), clamped to HostDevice::max_samples(); other targets as on the console.
  // 0 = as on the console (2x). Only with msaa = true.
  uint32_t msaa_samples = 0;
  // [graphics] smaa (FCP_RENDER_SMAA): SMAA 1x on the finished image before the overlay. Switchable live (SetSmaa).
  bool smaa = false;
  // FCP_RENDER_CB_VERSIONS: versions per volatile constant buffer (Vulkan; 0 = layout default).
  uint32_t constant_versions = 0;
  // FCP_RENDER_TABLE_SLOTS=<2D>,<3D>,<cube>,<array>,<samplers>,<buffers>: capacities of the bindless tables
  // (diagnostics; 0 = layout default).
  uint32_t table_slots[6] = {};
  // Evict textures and vertex buffers not bound for a while when their bindless table fills up and reuse their
  // slots (GitHub issue #1). FCP_RENDER_SLOT_RECYCLE=0: slots are never reused (old behaviour).
  bool slot_recycle = true;
  // Small, continuously read targets asynchronously (1-2 frames old) instead of synchronously; FCP_RENDER_LOCK_READBACK=sync: always synchronous.
  bool lock_readback_async = true;
  // FCP_RENDER_BUFFER_WATCH=0: hash vertex buffers every frame instead of using the write watch.
  bool buffer_watch = true;
  // Log frames at or above this duration with breakdown (FCP_TRACE_HITCHES=<ms>, 0: off).
  double slow_frame_ms = 0;
  // Endianness per vertex element from the D3DDECLTYPE instead of from the buffer's fetch constant (King Kong's
  // element_endian). FCP_RENDER_ELEMENT_ENDIAN=0/1.
  bool element_endian = true;        // FCP_RENDER_TEXTURE_CHECK=0: read guest textures only the first time
  // The present thread waits for the GPU (frame lead) and acquires the swapchain image without the renderer lock;
  // previously it held the lock meanwhile and the game's render thread waited in OnQuery/OnLock/OnDraw.
  // FCP_RENDER_PRESENT_UNLOCKED=0: old path.
  bool present_unlocked = false;
  // At most this many game swaps outstanding on the GPU; OnSwap waits without the lock for the previous swap
  // (back pressure previously provided by the present thread waiting under the lock). 0: no waiting.
  // FCP_RENDER_FRAMES_AHEAD=<n>, only with present_unlocked.
  uint32_t frames_ahead = 2;
  // Draw path: convert/compare constants only when the guest data changed (compare the big-endian raw data
  // against the last copy instead of converting + comparing the floats). FCP_RENDER_FAST_CONSTANTS=0: old path.
  bool fast_constants = false;
  // Index cache: guest index buffers converted once into host buffers (write watch) instead of a copy per draw into
  // the ring (the copy ends the render pass and sets a barrier). FCP_RENDER_INDEX_CACHE=0: old path.
  bool index_cache = false;
  // UP data and ring indices collected per command list into one copy before the list instead of one copy per draw.
  // FCP_RENDER_RING_BATCH=0: old path.
  bool ring_batch = false;
  // Graphics extras. Default = console.
  // Resolution scaling of the screen targets (guest width >= scale_min_width, landscape): host = guest x scale.
  // scale_auto: factor = swapchain height / 720 (recomputed on resize). FCP_RENDER_SCALE.
  float scale = 1.0f;
  bool scale_auto = false;
  uint32_t scale_min_width = 640;  // FCP_RENDER_SCALE_MIN_WIDTH
  // Shadow maps (square targets from 1024): separate factor. FCP_RENDER_SHADOW_SCALE.
  float shadow_scale = 1.0f;
  // Anisotropic filtering: at least this level for samplers with linear min/mag filter (0/1: as the game).
  uint32_t aniso = 0;
  // Hor+ ([graphics] aspect / FCP_RENDER_ASPECT): shape of the shown 3D picture. 0 = auto (the output's when wider
  // than 16:9), else W/H; 16/9 or narrower = off (pillarbox as before). Screen-sized targets get wide = aspect / (16/9)
  // times wider (anamorphic: guest coordinates stay 1280 wide), the camera (fcp/camera_fov.cpp) by the same factor;
  // 2D after the post processing of a 3D picture goes into the 16:9 middle.
  float aspect = 0.0f;
  // Vert+ (FCP_RENDER_VERTPLUS, default on): a picture shape narrower than 16:9 (16:10, 4:3, 3:2) fills the output;
  // screen-sized targets get tall = (16/9) / aspect times taller, the camera keeps the horizontal view and sees
  // more above and below, 2D after the post processing goes into the 16:9 band in the middle. false: such outputs
  // show 16:9 with bars as before.
  bool vert_plus = true;
  // Render resolution ([graphics] resolution / FCP_RENDER_RESOLUTION): 0 = from `scale` (as before); 1 = native (the
  // picture's size on the output); 2 = fixed res_w x res_h host pixels for the 1280x720 guest screen targets (other
  // shapes than the picture's are stretched to it). Replaces `scale` when not 0.
  uint32_t resolution_mode = 0;
  uint32_t res_w = 0, res_h = 0;
  // FCP_TRACE_WIDE=1: each new kind of 2D draw over a wide 3D picture (shaders, box, decision) once into the log.
  bool trace_wide = false;
  // Present timing and display pacing (renderer/present_pacer.h; FCP_PRESENT_STATS, FCP_FPS_PACING=display).
  double present_stats_seconds = 10;  // presentstat line every n seconds, 0: none
  double pace_fps = 0;                // display pacing target (0: off, < 0: every refresh)
  uint32_t pace_queued = 1;           // FCP_PACE_QUEUE
  double pace_hold = 0.0;             // FCP_PACE_HOLD, fraction of a refresh
  double pace_early_ms = 0;           // FCP_PACE_EARLY
  double pace_spin_ms = 0;            // FCP_PACE_SPIN
  bool pace_trace = false;            // FCP_PACE_TRACE
  bool pace_single = false;           // FCP_PACE_SINGLE
  // Log (the game's log thread); null: stderr.
  void (*log)(const char* line) = nullptr;
};

class GameRenderer : public DrawSink {
 public:
  // Guest memory: base = host pointer to guest address 0 (virtual; physical = base + 0xA0000000).
  static std::unique_ptr<GameRenderer> Create(const GameRendererOptions& options, std::string* error);
  ~GameRenderer() override;

  void SetGuestBase(const uint8_t* base);
  // SDK write watch (rexglue change 12): `watch(physical, bytes)` watches a range until the next write;
  // the SDK reports it with OnGuestWrite (any thread, under the SDK lock, without the renderer lock).
  // Without this call: content check by hash as before.
  void SetWriteWatch(void (*watch)(void* context, uint32_t physical, uint32_t bytes), void* context);
  void OnGuestWrite(uint32_t physical, uint32_t bytes);
  // Window (HWND) from SetSurface; creates device and swapchain. Without this call the device is created on the
  // first record without a swapchain (dumps only).
  bool AttachWindow(void* hwnd, uint32_t width, uint32_t height, std::string* error);
  void Resize(uint32_t width, uint32_t height);
  // Present thread: the most recently finished image into the swapchain (false: nothing to show).
  bool BeginPresent();
  void EndPresent();
  // Overlay (renderer/overlay.h): present thread, between a BeginPresent that returned true and EndPresent.
  // First the texture requests (IDs of new textures land in ops[i].id), then the image over the game image.
  // No draws, no GPU work. Returns the number of draws.
  uint32_t RenderOverlay(const OverlayFrame& frame, OverlayTextureOp* ops, size_t op_count);
  // Test: the next shown image (swapchain image with overlay, before present) as an image file (extension .png/.bmp).
  void RequestPresentDump(const std::string& path);
  // Runtime switches for the settings menu: gamma ramp on/off; render scale (factor, or auto =
  // swapchain height / 720), applied on the next swap.
  void SetGammaRamp(bool on);
  void SetScale(float scale, bool scale_auto);
  // Render resolution (mode and size as in GameRendererOptions::resolution_mode), applied on the next swap.
  void SetResolution(uint32_t mode, uint32_t w, uint32_t h);
  // Hor+: how much wider than 16:9 the screen-sized targets are now (1 = off). Any thread (camera hook).
  float Wide() const;
  // Vert+: how much taller than 16:9 the screen-sized targets are now (1 = off). Any thread (camera hook).
  float Tall() const;
  // SMAA 1x on the finished image before the overlay, from the next present on.
  void SetSmaa(bool on);
  // Texture replacement (renderer/texture_replace.h, button glyphs); nullptr = off. The source must live until Shutdown.
  void SetTextureReplaceSource(TextureReplaceSource* source);
  // The game starts loading a level (StartLevel command, any thread): prewarm jobs of that level's group go first;
  // with FCP_PIPELINE_COLLECT the following records go into that group's file.
  void OnLevelLoading(const std::string& level);
  void Shutdown();
  std::string Summary() const;
  // Display pacing is on (present_pacer.h): the game then needs no frame rate limiter and no vblank wait of its own.
  bool DisplayPacingActive() const;
  // While display pacing is on: the display's frame period in ms (refreshes per frame x refresh), else 0.
  double DisplayFrameMs() const;

  // DrawSink
  void OnDeviceCreated(uint32_t device) override;
  void OnDraw(const DrawRecord& r) override;
  void OnClear(const ClearRecord& r) override;
  void OnResolve(const ResolveRecord& r) override;
  void OnTiling(const TilingRecord& r) override;
  void OnPredication(const PredicationRecord& r) override;
  uint32_t OnQuery(const QueryRecord& r, uint32_t* samples) override;
  void OnShaderCreated(const ShaderCreatedRecord& r) override;
  void OnShaderReleased(uint32_t kind, uint32_t object) override;
  void OnSwap(const SwapRecord& r) override;
  void OnLock(const LockRecord& r) override;

  struct Impl;

 private:
  GameRenderer() = default;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fcp::nr
