// GuestDevice: the renderer's view of the D3D device structure and the guest objects.
//
// All offsets come from config/predator/renderer.toml (GuestLayout::Load); none is in the code. A field the
// configuration does not name is "missing" (has() == false) and reads 0. Every field carries the status from
// the configuration ("gemessen" = measured / "kk" / "kk+16") so diagnostics and trace can flag unverified fields.
//
// Guest memory is big-endian; the view reads through a host pointer to the start of the structure (the
// hook layer: guest address via GuestMemory::Virtual; the render thread: its copy).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "kk/backend/guest_memory.h"

namespace fcp::nr {

// Fields of the device structure, keys in [device.fields].
#define FCP_DEVICE_FIELDS(X)                                                                                 \
  X(dirty_masks) X(fetch_constants) X(vs_constants) X(ps_constants) X(bool_constants) X(loop_constants)     \
  X(fence_writeback) X(current_fence) X(vertex_declaration) X(rb_surface_info) X(rb_color_info)             \
  X(rb_depth_info) X(rb_color1_info) X(pa_sc_window_offset) X(pa_sc_window_scissor_tl)                      \
  X(pa_sc_window_scissor_br) X(vgt_indx_offset) X(rb_color_mask) X(rb_blend_rgba) X(rb_stencil_ref_mask_bf) \
  X(rb_stencil_ref_mask) X(rb_alpha_ref) X(pa_cl_vport) X(rb_depth_control) X(rb_blend_control0)            \
  X(rb_color_control) X(pa_cl_clip_cntl) X(pa_su_sc_mode_cntl) X(pa_cl_vte_cntl) X(rb_blend_control1)       \
  X(pa_su_vtx_cntl) X(pa_su_poly_offset) X(index_buffer) X(render_targets) X(depth_stencil)                 \
  X(stream_offset) X(stream_buffer) X(stream_stride) X(textures) X(viewport) X(scissor_rect)                \
  X(clip_planes) X(pixel_shader) X(predication_mask) X(present_interval) X(front_buffer) X(back_buffer)     \
  X(gamma_ramp) X(vertex_shader)

enum class DeviceField : uint32_t {
#define FCP_X(name) name,
  FCP_DEVICE_FIELDS(FCP_X)
#undef FCP_X
  kCount
};
const char* DeviceFieldName(DeviceField field);

enum class FieldStatus : uint8_t { kMissing, kMeasured, kKingKong, kKingKongShifted };
const char* FieldStatusName(FieldStatus status);

struct FieldLayout {
  uint32_t offset = 0;
  uint32_t stride = 0;
  FieldStatus status = FieldStatus::kMissing;
};

// A guest object: named offsets (keys as in [objects.<name>]) and its size.
struct ObjectLayout {
  uint32_t size = 0;
  FieldStatus status = FieldStatus::kMissing;
  std::vector<std::pair<std::string, uint32_t>> values;
  // Value of a key; `fallback` if it is missing (and `found` = false).
  uint32_t get(const char* key, uint32_t fallback = 0, bool* found = nullptr) const;
};

struct GuestLayout {
  uint32_t device_size = 0;
  std::array<FieldLayout, size_t(DeviceField::kCount)> fields{};
  std::vector<std::pair<uint32_t, uint32_t>> snapshot_ranges;  // [begin, end) of the ranges
  ObjectLayout surface, vertex_buffer, index_buffer, declaration, texture, shader, query;

  const FieldLayout& field(DeviceField f) const { return fields[size_t(f)]; }
  // Reads the file (or a TOML text). False with a message on a syntax error, a missing size or a
  // field outside the structure.
  bool Load(const std::string& path, std::string* error);
  bool LoadText(const std::string& toml, std::string* error);
  // Checks whether every range of a field the renderer reads lies in snapshot_ranges (render thread).
  // Returns the names of the fields not covered.
  std::vector<std::string> FieldsOutsideSnapshot() const;
};

// Read-only view of a device structure (host pointer to its first byte).
class GuestDeviceView {
 public:
  GuestDeviceView(const GuestLayout& layout, const uint8_t* base) : layout_(layout), base_(base) {}
  bool valid() const { return base_ != nullptr && layout_.device_size != 0; }
  bool has(DeviceField f) const { return layout_.field(f).status != FieldStatus::kMissing; }
  const GuestLayout& layout() const { return layout_; }
  // Host pointer to element `index` of a field (null if the field is missing).
  const uint8_t* at(DeviceField f, uint32_t index = 0) const;
  uint32_t u32(DeviceField f, uint32_t index = 0) const;
  float f32(DeviceField f, uint32_t index = 0) const;
  uint8_t u8(DeviceField f, uint32_t index = 0) const;
  // Fetch constant n (6 words, host order).
  void fetch_constant(uint32_t n, uint32_t out[6]) const;

 private:
  const GuestLayout& layout_;
  const uint8_t* base_;
};

}  // namespace fcp::nr
