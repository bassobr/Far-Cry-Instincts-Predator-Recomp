// GuestLayout from config/predator/renderer.toml (renderer/guest_device.h). This project's own code.
#include "renderer/guest_device.h"

#include <cstring>
#include <fstream>
#include <sstream>

#define TOML_EXCEPTIONS 0
#include <toml++/toml.hpp>

namespace fcp::nr {

namespace {

const char* const kFieldNames[] = {
#define FCP_X(name) #name,
    FCP_DEVICE_FIELDS(FCP_X)
#undef FCP_X
};

FieldStatus ParseStatus(std::string_view s) {
  if (s == "gemessen") return FieldStatus::kMeasured;
  if (s == "kk") return FieldStatus::kKingKong;
  if (s == "kk+16") return FieldStatus::kKingKongShifted;
  return FieldStatus::kMissing;
}

bool ReadObject(const toml::table& root, const char* name, ObjectLayout& out, std::string* error) {
  const toml::table* t = root["objects"][name].as_table();
  if (!t) return true;  // optional
  for (const auto& [key, node] : *t) {
    if (key.str() == "status") {
      out.status = ParseStatus(node.value_or(std::string_view{}));
      continue;
    }
    const auto v = node.value<int64_t>();
    if (!v || *v < 0) {
      if (error) *error = std::string("objects.") + name + "." + std::string(key.str()) + ": not a number";
      return false;
    }
    if (key.str() == "size") out.size = uint32_t(*v);
    out.values.emplace_back(std::string(key.str()), uint32_t(*v));
  }
  if (out.status == FieldStatus::kMissing) out.status = FieldStatus::kKingKong;
  return true;
}

}  // namespace

const char* DeviceFieldName(DeviceField field) {
  return field < DeviceField::kCount ? kFieldNames[size_t(field)] : "?";
}

const char* FieldStatusName(FieldStatus status) {
  switch (status) {
    case FieldStatus::kMeasured: return "gemessen";
    case FieldStatus::kKingKong: return "kk";
    case FieldStatus::kKingKongShifted: return "kk+16";
    case FieldStatus::kMissing: break;
  }
  return "missing";
}

uint32_t ObjectLayout::get(const char* key, uint32_t fallback, bool* found) const {
  for (const auto& [k, v] : values)
    if (k == key) {
      if (found) *found = true;
      return v;
    }
  if (found) *found = false;
  return fallback;
}

bool GuestLayout::Load(const std::string& path, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = "missing: " + path;
    return false;
  }
  std::ostringstream s;
  s << in.rdbuf();
  if (!LoadText(s.str(), error)) {
    if (error) *error = path + ": " + *error;
    return false;
  }
  return true;
}

bool GuestLayout::LoadText(const std::string& text, std::string* error) {
  *this = GuestLayout{};
  toml::parse_result parsed = toml::parse(text);
  if (!parsed) {
    if (error) *error = std::string(parsed.error().description());
    return false;
  }
  const toml::table& root = parsed.table();
  device_size = uint32_t(root["device"]["size"].value_or(int64_t(0)));
  if (!device_size) {
    if (error) *error = "device.size missing";
    return false;
  }
  if (const toml::table* f = root["device"]["fields"].as_table()) {
    for (const auto& [key, node] : *f) {
      size_t index = 0;
      while (index < size_t(DeviceField::kCount) && key.str() != kFieldNames[index]) ++index;
      if (index == size_t(DeviceField::kCount)) {
        if (error) *error = "unknown field device.fields." + std::string(key.str());
        return false;
      }
      const toml::table* t = node.as_table();
      const auto offset = t ? (*t)["offset"].value<int64_t>() : std::optional<int64_t>{};
      if (!offset || *offset < 0 || uint64_t(*offset) + 4 > device_size) {
        if (error) *error = "device.fields." + std::string(key.str()) + ": offset missing or out of range";
        return false;
      }
      FieldLayout& l = fields[index];
      l.offset = uint32_t(*offset);
      l.stride = uint32_t((*t)["stride"].value_or(int64_t(0)));
      l.status = ParseStatus((*t)["status"].value_or(std::string_view{}));
      if (l.status == FieldStatus::kMissing) {
        if (error) *error = "device.fields." + std::string(key.str()) + ": status missing or unknown";
        return false;
      }
    }
  }
  if (const toml::array* ranges = root["device"]["snapshot"]["ranges"].as_array()) {
    for (const auto& r : *ranges) {
      const toml::array* pair = r.as_array();
      if (!pair || pair->size() != 2) continue;
      const int64_t a = (*pair)[0].value_or(int64_t(-1)), b = (*pair)[1].value_or(int64_t(-1));
      if (a < 0 || b <= a || uint64_t(b) > device_size) {
        if (error) *error = "device.snapshot.ranges: invalid range";
        return false;
      }
      snapshot_ranges.emplace_back(uint32_t(a), uint32_t(b));
    }
  }
  return ReadObject(root, "surface", surface, error) && ReadObject(root, "vertex_buffer", vertex_buffer, error) &&
         ReadObject(root, "index_buffer", index_buffer, error) &&
         ReadObject(root, "declaration", declaration, error) && ReadObject(root, "texture", texture, error) &&
         ReadObject(root, "shader", shader, error) && ReadObject(root, "query", query, error);
}

std::vector<std::string> GuestLayout::FieldsOutsideSnapshot() const {
  std::vector<std::string> out;
  for (size_t i = 0; i < fields.size(); ++i) {
    const FieldLayout& f = fields[i];
    if (f.status == FieldStatus::kMissing) continue;
    if (DeviceField(i) == DeviceField::dirty_masks || DeviceField(i) == DeviceField::fence_writeback ||
        DeviceField(i) == DeviceField::current_fence)
      continue;  // not read by the renderer while executing a call
    const uint32_t end = f.offset + (f.stride ? f.stride : 4);
    bool covered = false;
    for (const auto& [a, b] : snapshot_ranges) covered = covered || (f.offset >= a && end <= b);
    if (!covered) out.push_back(kFieldNames[i]);
  }
  return out;
}

const uint8_t* GuestDeviceView::at(DeviceField f, uint32_t index) const {
  const FieldLayout& l = layout_.field(f);
  if (!base_ || l.status == FieldStatus::kMissing) return nullptr;
  const uint64_t offset = uint64_t(l.offset) + uint64_t(l.stride) * index;
  return offset + 4 <= layout_.device_size ? base_ + offset : nullptr;
}

uint32_t GuestDeviceView::u32(DeviceField f, uint32_t index) const {
  const uint8_t* p = at(f, index);
  return p ? LoadBE32(p) : 0;
}

float GuestDeviceView::f32(DeviceField f, uint32_t index) const {
  const uint8_t* p = at(f, index);
  return p ? LoadBEFloat(p) : 0.0f;
}

uint8_t GuestDeviceView::u8(DeviceField f, uint32_t index) const {
  const uint8_t* p = at(f, index);
  return p ? *p : 0;
}

void GuestDeviceView::fetch_constant(uint32_t n, uint32_t out[6]) const {
  const uint8_t* p = at(DeviceField::fetch_constants, n);
  for (uint32_t i = 0; i < 6; ++i) out[i] = p ? LoadBE32(p + 4 * i) : 0;
}

}  // namespace fcp::nr
