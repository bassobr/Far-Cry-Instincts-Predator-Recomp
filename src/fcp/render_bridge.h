// Connects the native renderer (src/renderer, GameRenderer) to predator.exe:
//  - a code module that serves rex::gpu::IHostPresenter (rexglue local change 10): the null Xenos driver hands
//    it the window and the present calls of the SDK's present thread;
//  - the NrApi table the D3D hooks call (src/fcp/d3d_hooks.cpp).
//
// Switches (read once at start):
//   FCP_RENDERER=0                 no renderer: no module, no NrApi, the hooks only run the originals (boot state)
//   FCP_RENDER_BACKEND=vulkan|d3d12  host API; default from renderer.toml [render] backend, else vulkan
//   FCP_GPU_DEBUG=1                Vulkan validation layer if installed, else NVRHI validation; D3D12 debug layer
//   FCP_DUMP_FRAMES=<file.png>     frame dump pattern ("%d" = frame), relative to the log folder
//   FCP_DUMP_FRAME_AT=<n>[,<n>..]  frames to dump; FCP_DUMP_FRAME_EVERY=<n> every n-th frame
//   FCP_RENDER_SOLID_PS/_VS=<hash> draw that shader's draws in a signal colour (renderer/diagnostics.h)
//   FCP_SHADER_PACK=<file>, FCP_SHADER_CACHE=<dir>  override the defaults next to the EXE
//   FCP_RENDER_VSYNC=0             present without vsync
//   FCP_RENDER_TEXTURE_CHECK=0     read guest textures only once (no per-frame content check)
//   FCP_RENDER_STATS=<n>           renderer statistics line every n frames (default 150, 0 = off)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rex {
class IModule;
}

namespace fcp::render {

/** True unless FCP_RENDERER=0. */
bool Enabled();

/** Adds the presenter module (App::OnRegisterModules). Does nothing when the renderer is off. */
void RegisterModule(std::vector<rex::IModule*>& modules);

/** The guest memory base the hooks see (first call wins). */
void SetGuestBase(uint8_t* base);

/** Waits for the GPU, saves caches and writes the summary into the log. */
void Shutdown();

// ---- for the settings menu (fcp/settings_menu.h) ----

/** True when the renderer runs and presents into the window (the SDK's ImGui overlays are drawn then). */
bool Presenting();

/** The host API in use ("vulkan", "d3d12") or "off". */
const char* BackendName();

/** Test: the next presented picture (game plus overlay) as an image file; relative = the log folder. */
void RequestPresentDump(const std::string& path);

/** At once: the gamma ramp on or off; the render scale (1, 1.5, ..., or auto), from the next frame. */
void SetGammaRamp(bool on);
void SetScale(const std::string& value);
// [graphics] smaa: SMAA 1x on the final image, from the next present.
void SetSmaa(bool on);

// ---- Hor+ ([graphics] aspect, FCP_RENDER_ASPECT) ----
/** How much wider than 16:9 the screen-sized targets are now (1 = off); the camera (fcp/camera_fov.cpp) widens its
 *  projection by the same factor. Any thread. */
float HorPlusWide();

}  // namespace fcp::render
