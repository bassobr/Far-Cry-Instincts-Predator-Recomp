#include "gpu/xenos/null/null_xenos_graphics_driver.h"

#include "gpu/xenos/null/null_command_processor.h"

namespace rex::gpu::null {

NullXenosGraphicsDriver::NullXenosGraphicsDriver() = default;

NullXenosGraphicsDriver::~NullXenosGraphicsDriver() = default;

// FCP (local change 10, see UPSTREAM.md): the host presenter of the connected modules, or null.
namespace {
IHostPresenter* g_host_presenter = nullptr;
}  // namespace

void NullXenosGraphicsDriver::SetHostPresenter(IHostPresenter* presenter) {
  g_host_presenter = presenter;
}

Result<void> NullXenosGraphicsDriver::SetSurface(const NativeWindow& window) {
  if (g_host_presenter) {
    return g_host_presenter->SetSurface(window);
  }
  return Ok();
}

void NullXenosGraphicsDriver::ResizeSurface(u32 width, u32 height) {
  if (g_host_presenter) {
    g_host_presenter->ResizeSurface(width, height);
  }
}

bool NullXenosGraphicsDriver::BeginPresent() {
  return g_host_presenter && g_host_presenter->BeginPresent();
}

void NullXenosGraphicsDriver::RenderOverlays(ImDrawData& draw_data) {
  if (g_host_presenter) {
    g_host_presenter->RenderOverlays(draw_data);
  }
}

void NullXenosGraphicsDriver::EndPresent() {
  if (g_host_presenter) {
    g_host_presenter->EndPresent();
  }
}

void NullXenosGraphicsDriver::DetachTitle() {
  if (g_host_presenter) {
    g_host_presenter->DetachTitle();
  }
  XenosGraphicsDriverBase::DetachTitle();
}

void NullXenosGraphicsDriver::CreateProvider(bool) {}

std::unique_ptr<CommandProcessor> NullXenosGraphicsDriver::CreateCommandProcessor() {
  // FCP (local change 12, see UPSTREAM.md): the presenter gets the title memory and write watch.
  if (g_host_presenter && physical_memory_ && write_watch_) {
    g_host_presenter->AttachTitleMemory(*physical_memory_, *write_watch_);
  }
  return std::make_unique<NullCommandProcessor>(this, title_info_, *physical_memory_, *write_watch_,
                                                *guest_interrupts_);
}

}  // namespace rex::gpu::null
