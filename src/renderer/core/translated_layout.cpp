// TranslatedLayout (renderer/translated_layout.h). Arrangement of the layouts after King Kong Recompiled
// (native-renderer/backend/game_renderer.cpp, e3e5d3f, MIT); code rewritten.
#include "renderer/translated_layout.h"

#include <chrono>

#include "renderer/diagnostics.h"
#include "renderer/shader_library.h"

namespace fcp::nr {

bool TranslatedLayout::Init(nvrhi::IDevice* device, const TableCapacities& capacities, std::string* error) {
  device_ = device;
  capacities_ = capacities;
  nvrhi::BindingLayoutDesc constants;
  constants.visibility = nvrhi::ShaderType::All;
  constants.registerSpace = 0;
  constants.bindingOffsets =
      nvrhi::VulkanBindingOffsets().setShaderResourceOffset(0).setSamplerOffset(0).setConstantBufferOffset(0).setUnorderedAccessViewOffset(0);
  constants.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0), nvrhi::BindingLayoutItem::VolatileConstantBuffer(1),
                        nvrhi::BindingLayoutItem::VolatileConstantBuffer(2), nvrhi::BindingLayoutItem::VolatileConstantBuffer(3)};
  nvrhi::BindingLayoutHandle constants_layout = device->createBindingLayout(constants);
  if (!constants_layout) return error && (*error = "constants layout", false);
  layouts_ = {constants_layout};

  auto cb = [&](uint32_t size, const char* name) {
    nvrhi::BufferDesc d;
    d.byteSize = size;
    d.isConstantBuffer = true;
    d.isVolatile = true;
    d.maxVersions = capacities.constant_versions;
    d.debugName = name;
    return device->createBuffer(d);
  };
  vs_constants_ = cb(4096, "VS constants");
  ps_constants_ = cb(4096, "PS constants");
  draw_constants_ = cb(sizeof(fcshaders::DrawConstants), "Draw constants");
  // b3: render scale (abi.h ScaleConstants); always bound, all 1 at factor 1.
  scale_constants_ = cb(sizeof(fcshaders::ScaleConstants), "Scale constants");
  nvrhi::BindingSetDesc cs;
  cs.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, vs_constants_), nvrhi::BindingSetItem::ConstantBuffer(1, ps_constants_),
                 nvrhi::BindingSetItem::ConstantBuffer(2, draw_constants_), nvrhi::BindingSetItem::ConstantBuffer(3, scale_constants_)};
  constants_set_ = device->createBindingSet(cs, constants_layout);
  if (!constants_set_) return error && (*error = "constants set", false);

  const nvrhi::BindingLayoutItem items[] = {
      nvrhi::BindingLayoutItem::Texture_SRV(1), nvrhi::BindingLayoutItem::Texture_SRV(2),
      nvrhi::BindingLayoutItem::Texture_SRV(3), nvrhi::BindingLayoutItem::Texture_SRV(4),
      nvrhi::BindingLayoutItem::Sampler(5),     nvrhi::BindingLayoutItem::RawBuffer_SRV(6)};
  for (size_t i = 0; i < size_t(Table::kCount); ++i) {
    nvrhi::BindlessLayoutDesc d;
    d.visibility = nvrhi::ShaderType::All;
    d.firstSlot = 0;
    d.maxCapacity = capacities.slots[i];
    d.registerSpaces.push_back(items[i]);
    nvrhi::BindingLayoutHandle layout = device->createBindlessLayout(d);
    if (!layout) return error && (*error = "bindless layout " + std::to_string(i), false);
    tables_[i] = device->createDescriptorTable(layout);
    if (!tables_[i]) return error && (*error = "descriptor table " + std::to_string(i), false);
    device->resizeDescriptorTable(tables_[i], capacities.slots[i], false);
    layouts_.push_back(layout);
  }

  // Slot 0: fallback resources.
  auto dummy = [&](nvrhi::TextureDimension dim, uint32_t layers, const char* name) {
    nvrhi::TextureDesc d;
    d.width = d.height = 1;
    d.arraySize = layers;
    d.dimension = dim;
    d.format = nvrhi::Format::RGBA8_UNORM;
    d.initialState = nvrhi::ResourceStates::ShaderResource;
    d.keepInitialState = true;
    d.debugName = name;
    return device->createTexture(d);
  };
  dummy_2d_ = dummy(nvrhi::TextureDimension::Texture2D, 1, "dummy 2D");
  dummy_3d_ = dummy(nvrhi::TextureDimension::Texture3D, 1, "dummy 3D");
  dummy_cube_ = dummy(nvrhi::TextureDimension::TextureCube, 6, "dummy cube");
  dummy_array_ = dummy(nvrhi::TextureDimension::Texture2DArray, 1, "dummy array");
  dummy_sampler_ = device->createSampler(nvrhi::SamplerDesc().setAllFilters(false));
  nvrhi::BufferDesc bd;
  bd.byteSize = 256;
  bd.canHaveRawViews = true;
  bd.initialState = nvrhi::ResourceStates::ShaderResource;
  bd.keepInitialState = true;
  bd.debugName = "dummy buffer";
  dummy_buffer_ = device->createBuffer(bd);
  nvrhi::CommandListHandle cl = device->createCommandList();
  cl->open();
  const uint32_t zero[64] = {};
  for (nvrhi::ITexture* t : {dummy_2d_.Get(), dummy_3d_.Get(), dummy_array_.Get()}) cl->writeTexture(t, 0, 0, zero, 4);
  for (uint32_t face = 0; face < 6; ++face) cl->writeTexture(dummy_cube_, face, 0, zero, 4);
  cl->writeBuffer(dummy_buffer_, zero, sizeof(zero));
  cl->close();
  device->executeCommandList(cl);
  AddTexture(Table::kTex2D, dummy_2d_);
  AddTexture(Table::kTex3D, dummy_3d_);
  AddTexture(Table::kTexCube, dummy_cube_);
  AddTexture(Table::kTex2DArray, dummy_array_);
  AddSampler(dummy_sampler_);
  AddBuffer(dummy_buffer_);
  return true;
}

uint32_t TranslatedLayout::AddTexture(Table table, nvrhi::ITexture* texture) {
  const size_t t = size_t(table);
  if (next_[t] >= capacities_.slots[t]) return 0;
  const uint32_t slot = next_[t]++;
  device_->writeDescriptorTable(tables_[t], nvrhi::BindingSetItem::Texture_SRV(slot, texture));
  return slot;
}

uint32_t TranslatedLayout::AddTextureView(Table table, nvrhi::ITexture* texture, nvrhi::ComponentMapping mapping,
                                          nvrhi::TextureDimension dimension) {
  const size_t t = size_t(table);
  if (next_[t] >= capacities_.slots[t]) return 0;
  const uint32_t slot = next_[t]++;
  WriteTextureView(table, slot, texture, mapping, dimension);
  return slot;
}

void TranslatedLayout::WriteTextureView(Table table, uint32_t slot, nvrhi::ITexture* texture,
                                        nvrhi::ComponentMapping mapping, nvrhi::TextureDimension dimension) {
  std::optional<nvrhi::ComponentMapping> m;
  if (!mapping.isIdentity()) m = mapping;
  device_->writeDescriptorTable(tables_[size_t(table)],
                                nvrhi::BindingSetItem::Texture_SRV(slot, texture, nvrhi::Format::UNKNOWN,
                                                                   nvrhi::AllSubresources, dimension, m));
}

uint32_t TranslatedLayout::AddSampler(nvrhi::ISampler* sampler) {
  const size_t t = size_t(Table::kSamplers);
  if (next_[t] >= capacities_.slots[t]) return 0;
  const uint32_t slot = next_[t]++;
  device_->writeDescriptorTable(tables_[t], nvrhi::BindingSetItem::Sampler(slot, sampler));
  return slot;
}

uint32_t TranslatedLayout::AddBuffer(nvrhi::IBuffer* buffer) {
  const size_t t = size_t(Table::kBuffers);
  if (next_[t] >= capacities_.slots[t]) return 0;
  const uint32_t slot = next_[t]++;
  device_->writeDescriptorTable(tables_[t], nvrhi::BindingSetItem::RawBuffer_SRV(slot, buffer));
  return slot;
}

nvrhi::ShaderHandle TranslatedLayout::CreateShader(const ShaderEntry& e) const {
  nvrhi::ShaderDesc d;
  d.shaderType = e.kind == fcshaders::ShaderKind::Vertex ? nvrhi::ShaderType::Vertex : nvrhi::ShaderType::Pixel;
  d.entryName = "main";
  char name[32];
  std::snprintf(name, sizeof(name), "%016llX", (unsigned long long)e.container_hash);
  d.debugName = name;
  return device_->createShader(d, e.code.data(), e.code.size());
}

nvrhi::GraphicsPipelineHandle TranslatedLayout::CreatePipeline(nvrhi::IShader* vs, nvrhi::IShader* ps,
                                                               const nvrhi::FramebufferInfo& fb, const FixedState& s) const {
  nvrhi::GraphicsPipelineDesc d;
  d.VS = vs;
  d.PS = ps;
  d.primType = s.primitive;
  d.bindingLayouts = layouts_;
  d.renderState.blendState.targets[0] = s.blend;
  d.renderState.depthStencilState = s.depth;
  d.renderState.rasterState = s.raster;
  const auto t0 = std::chrono::steady_clock::now();
  nvrhi::GraphicsPipelineHandle p = device_->createGraphicsPipeline(d, fb);
  Counters& c = Counters::Get();
  c.pipeline_create_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
  if (p) c.pipelines_created++;
  return p;
}

void TranslatedLayout::WriteConstants(nvrhi::ICommandList* cl, const void* vs, const void* ps,
                                      const fcshaders::DrawConstants& draw) const {
  cl->writeBuffer(vs_constants_, vs, 4096);
  cl->writeBuffer(ps_constants_, ps, 4096);
  cl->writeBuffer(draw_constants_, &draw, sizeof(draw));
  // b3 is part of the same set: even a list with only blits must write it (volatile buffer).
  static const fcshaders::ScaleConstants one = [] {
    fcshaders::ScaleConstants s{};
    for (float& v : s.renderScale) v = 1.0f;
    for (auto& t : s.texInvScale) t[0] = t[1] = t[2] = t[3] = 1.0f;
    return s;
  }();
  cl->writeBuffer(scale_constants_, &one, sizeof(one));
}

void TranslatedLayout::Bind(nvrhi::GraphicsState& state) const {
  state.bindings = {constants_set_};
  for (const auto& t : tables_) state.bindings.push_back(t);
}

}  // namespace fcp::nr
