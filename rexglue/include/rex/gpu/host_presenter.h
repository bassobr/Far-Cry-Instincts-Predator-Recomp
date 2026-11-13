/**
 * @file        gpu/host_presenter.h
 * @brief       A host renderer that presents for the null Xenos driver
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * FCP (local change 10, see UPSTREAM.md): new file.
 */

#pragma once

#include <rex/guest.h>
#include <rex/module.h>
#include <rex/platform/native_window.h>
#include <rex/result.h>
#include <rex/types.h>

struct ImDrawData;

namespace rex::gpu {

/**
 * A renderer outside the SDK that draws the title's frames itself (high level emulation of the
 * title's graphics library) and presents them into the app's window.
 *
 * The null Xenos driver keeps running the guest ring buffer (fences, interrupts, vblank, swaps)
 * and builds no presenter of its own. When a connected module serves this interface, the null
 * driver hands it the window and the present calls of GraphicsSystem: SetSurface once, then per
 * present ResizeSurface (when the window changed), BeginPresent, RenderOverlays and EndPresent on
 * the present thread, paced by the driver's WaitForFrame (one present per guest swap). When no
 * module serves it, the null driver presents nothing, as upstream.
 */
REX_DECLARE_INTERFACE(IHostPresenter, 1)
  /**
   * Gives the presenter the window. Called once, before the first present.
   *
   * @return Ok, or the failure, which stops the graphics start.
   */
  [[nodiscard]] virtual Result<void> SetSurface(const NativeWindow& window) = 0;

  /** The window changed size. Present thread, before BeginPresent. */
  virtual void ResizeSurface(u32 width, u32 height) = 0;

  /**
   * Starts a present: draws the latest finished title frame into the next swap chain image.
   *
   * @return False when there is nothing to present into, and the present is skipped.
   */
  [[nodiscard]] virtual bool BeginPresent() = 0;

  /** Draws the overlays of this present. Present thread, between BeginPresent and EndPresent. */
  virtual void RenderOverlays(ImDrawData& draw_data) = 0;

  /** Finishes the present started by a BeginPresent that returned true. */
  virtual void EndPresent() = 0;

  /** The title stops; the presenter drops what refers to guest memory. Optional. */
  virtual void DetachTitle() {}

  /**
   * FCP (local change 12, see UPSTREAM.md): the title's physical memory and its write watch, given
   * when the title attaches (before the first guest swap). The presenter may watch guest ranges it
   * caches (textures, buffers) instead of hashing them. Both outlive the title until DetachTitle.
   * Optional.
   */
  virtual void AttachTitleMemory(IPhysicalMemory& memory, IPhysicalWriteWatch& watch) {
    (void)memory, (void)watch;
  }
};

}  // namespace rex::gpu
