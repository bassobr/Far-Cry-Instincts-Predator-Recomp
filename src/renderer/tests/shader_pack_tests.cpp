// Pack format, disk cache, reflection and ShaderLibrary without game data (artificial entries).
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "fcshaders/pack.h"
#include "kk/backend/tests/check.h"
#include "renderer/shader_library.h"

using namespace fcp::nr;
using namespace fcshaders;

namespace {

std::string Temp(const char* name) {
  const auto p = std::filesystem::temp_directory_path() / (std::string("fcp_renderer_test_") + name);
  std::error_code ec;
  std::filesystem::remove_all(p, ec);
  return p.string();
}

Reflection SampleReflection(uint64_t hash, ShaderKind kind) {
  Reflection r;
  r.kind = kind;
  r.containerHash = hash;
  r.ucodeHash = hash * 3;
  r.inputHash = hash * 5;
  r.instructions = 17;
  VertexBinding v;
  v.usage = DeclUsage::TexCoord;
  v.usageIndex = 1;
  r.vertexBindings = {VertexBinding{}, v};
  SamplerBinding s;
  s.slot = 3;
  s.dimension = TextureDimension::Cube;
  r.samplers = {s};
  r.textures = {TextureUse{3, TextureDimension::Cube}};
  ConstantInfo c;
  c.name = "HybridLightFlags";
  c.registerSet = RegisterSet::Bool;
  c.registerIndex = 2;
  c.registerCount = 3;
  r.constants = {c};
  r.boolMask[0] = 0x1C;
  r.floatLiterals = {4, 200};
  r.registerWrites = {RegisterWrite{768, {1, 2, 3, 4}}};
  r.pixelOutputs = 1;
  r.readsVPos = true;
  r.warnings = {"w"};
  return r;
}

bool SameReflection(const Reflection& a, const Reflection& b) {
  return serializeReflection(a) == serializeReflection(b);
}

// A minimal container (header only): magic, virtualSize 24, physicalSize 0; enough for containerSize/hash.
std::vector<uint8_t> FakeContainer(uint32_t salt) {
  std::vector<uint8_t> c(24, 0);
  const uint32_t words[6] = {0x102A0E01u, 24, 0, 0, 0, salt};
  for (int i = 0; i < 6; ++i)
    for (int b = 0; b < 4; ++b) c[i * 4 + b] = uint8_t(words[i] >> (24 - 8 * b));
  return c;
}

}  // namespace

TEST(Reflection_SerializeRoundTrip) {
  const Reflection r = SampleReflection(0x1111, ShaderKind::Pixel);
  const auto bytes = serializeReflection(r);
  Reflection back;
  CHECK(deserializeReflection(bytes.data(), bytes.size(), back));
  CHECK(SameReflection(r, back));
  CHECK_EQ(back.samplers[0].slot, uint8_t(3));
  CHECK(back.constants[0].name == "HybridLightFlags");
  // Truncated or altered: rejected.
  CHECK(!deserializeReflection(bytes.data(), bytes.size() - 1, back));
  auto bad = bytes;
  bad[0] ^= 1;
  CHECK(!deserializeReflection(bad.data(), bad.size(), back));
}

TEST(ContainerSize_ReadsHeaderOnly) {
  const auto c = FakeContainer(1);
  CHECK_EQ(containerSize(c.data(), c.size()), size_t(24));
  CHECK_EQ(containerSize(c.data(), 23), size_t(0));  // larger than allowed
  auto bad = c;
  bad[3] = 0x05;  // falsches Magic
  CHECK_EQ(containerSize(bad.data(), bad.size()), size_t(0));
}

TEST(Pack_WriteReadRoundTrip) {
  const std::string path = Temp("pack.fcpack");
  PackWriter w(PackTarget::Spirv, 0xABCDEF);
  // Added unsorted, so that the index has to be sorted.
  const uint64_t hashes[] = {900, 5, 77, 123456789012345ull, 6};
  for (uint64_t h : hashes) {
    std::vector<uint8_t> code(size_t(h % 97 + 4), uint8_t(h));
    CHECK(w.add(h, h & 1 ? ShaderKind::Vertex : ShaderKind::Pixel, code, SampleReflection(h, ShaderKind::Pixel)));
  }
  CHECK(!w.add(77, ShaderKind::Pixel, {1}, SampleReflection(77, ShaderKind::Pixel)));  // doppelt
  std::string error;
  CHECK(w.write(path, &error));
  PackReader r;
  CHECK(r.open(path, 0xABCDEF, &error));
  CHECK(!r.stale());
  CHECK_EQ(r.count(), size_t(5));
  const auto sorted = r.hashes();
  CHECK(std::is_sorted(sorted.begin(), sorted.end()));
  for (uint64_t h : hashes) {
    PackEntry e;
    CHECK(r.find(h, e));
    CHECK_EQ(e.code.size(), size_t(h % 97 + 4));
    CHECK_EQ(e.code[0], uint8_t(h));
    CHECK(e.kind == (h & 1 ? ShaderKind::Vertex : ShaderKind::Pixel));
    CHECK(SameReflection(e.reflection, SampleReflection(h, ShaderKind::Pixel)));
  }
  PackEntry missing;
  CHECK(!r.find(4, missing));
  CHECK(!r.contains(901));
  r.close();
  // Different stamp: opened, but reported as stale.
  CHECK(r.open(path, 0x1234, &error));
  CHECK(r.stale());
  r.close();
  // Corrupt index: rejected.
  {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(-3, std::ios::end);
    f.put('\x55');
  }
  CHECK(!r.open(path, 0, &error));
  CHECK(error.find("corrupt") != std::string::npos);
  std::error_code ec;
  std::filesystem::remove(path, ec);  // the library still holds the pack open
}

TEST(DiskCache_PutGetAndStamp) {
  const std::string dir = Temp("cache");
  DiskCache c;
  std::string error;
  CHECK(c.open(dir, PackTarget::Dxil, 42, &error));
  PackEntry e;
  e.containerHash = 0xFEEDu;
  e.kind = ShaderKind::Vertex;
  e.code = {1, 2, 3, 4, 5};
  e.reflection = SampleReflection(0xFEED, ShaderKind::Vertex);
  CHECK(c.put(e, &error));
  PackEntry back;
  CHECK(c.get(0xFEED, back));
  CHECK(back.code == e.code);
  CHECK(back.kind == ShaderKind::Vertex);
  CHECK(SameReflection(back.reflection, e.reflection));
  CHECK(!c.get(0xFEEE, back));
  // A different translator/DXC (stamp) does not read the entry.
  DiskCache other;
  CHECK(other.open(dir, PackTarget::Dxil, 43, &error));
  CHECK(!other.get(0xFEED, back));
  // Nor does a different target.
  DiskCache spirv;
  CHECK(spirv.open(dir, PackTarget::Spirv, 42, &error));
  CHECK(!spirv.get(0xFEED, back));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(ShaderLibrary_ResolvesFromPackByContainer) {
  const std::string path = Temp("lib.fcpack");
  const auto container = FakeContainer(99);
  const uint64_t hash = containerHash(container.data(), container.size());
  PackWriter w(PackTarget::Spirv, 0);
  CHECK(w.add(hash, ShaderKind::Vertex, {7, 7, 7, 7}, SampleReflection(hash, ShaderKind::Vertex)));
  std::string error, note;
  CHECK(w.write(path, &error));
  ShaderLibrary lib;
  ShaderLibrary::Options o;
  o.pack_path = path;
  o.translate_missing = false;
  CHECK(lib.Open(o, &error, &note));
  const ShaderEntry* e = lib.OnShaderCreated(0x80001000, container.data(), 4096);
  CHECK(e != nullptr);
  if (e) {
    CHECK(e->source == ShaderSource::kPack);
    CHECK_EQ(e->code.size(), size_t(4));
    CHECK_EQ(e->container_hash, hash);
  }
  CHECK(lib.Find(0x80001000) == e);
  // The same container for a second object: from memory.
  CHECK(lib.OnShaderCreated(0x80002000, container.data(), 4096) == e);
  // Unknown container, translation off: failed entry (the draw gets the fallback shader).
  const auto other = FakeContainer(100);
  const ShaderEntry* f = lib.OnShaderCreated(0x80003000, other.data(), other.size());
  CHECK(f != nullptr && f->source == ShaderSource::kFailed);
  lib.OnShaderReleased(0x80001000);
  CHECK(lib.Find(0x80001000) == nullptr);
  const auto st = lib.stats();
  CHECK_EQ(st.pack_hits, uint64_t(1));
  CHECK_EQ(st.memory_hits, uint64_t(1));
  CHECK_EQ(st.failed, uint64_t(1));
  std::error_code ec;
  std::filesystem::remove(path, ec);  // the library still holds the pack open
}
