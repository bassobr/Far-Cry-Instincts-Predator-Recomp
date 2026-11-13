/**
 * @file        gpu/xenos/null/null_xenos_graphics_driver.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <string>

#include <rex/gpu/host_presenter.h>

#include "gpu/xenos/xenos_graphics_driver_base.h"

namespace rex::gpu::null {

/**
 * Runs the title's PM4 on the null command processor with no provider and no presenter.
 *
 * FCP (local change 10, see UPSTREAM.md): when a connected module serves IHostPresenter, the window
 * and the present calls go to it; without one the driver presents nothing, as upstream.
 */
class NullXenosGraphicsDriver final : public XenosGraphicsDriverBase {
 public:
  NullXenosGraphicsDriver();
  ~NullXenosGraphicsDriver() override;

  std::string_view GetName() const override { return "null"; }

  /** Accepts the window and builds no presenter; a host presenter, if any, gets the window. */
  [[nodiscard]] Result<void> SetSurface(const NativeWindow& window) override;

  // FCP: the present calls go to the host presenter (IHostPresenter), if a module serves one.
  void ResizeSurface(u32 width, u32 height) override;
  [[nodiscard]] bool BeginPresent() override;
  void RenderOverlays(ImDrawData& draw_data) override;
  void EndPresent() override;
  void DetachTitle() override;

  /** Sets the host presenter for the drivers of this module. Provider Connect / Disconnect. */
  static void SetHostPresenter(IHostPresenter* presenter);

 protected:
  void CreateProvider(bool with_presentation) override;

 private:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::gpu::null
