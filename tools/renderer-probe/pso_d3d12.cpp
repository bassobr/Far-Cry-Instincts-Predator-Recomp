// Measurement program: can the DXIL shaders produced by fcshaders be built into pipelines in D3D12, and how long
// does it take? Builds a root signature per tools/shaders/include/fcshaders/abi.h (b0-b2 space0, bindless tables
// space1-6) and, per pair from pairs.tsv, a graphics PSO without input layout (vertex pulling).
//
//   pso_d3d12 <run>/dxil <pairs.tsv> [--threads N] [--group g] [--variants K] [--save lib.bin] [--load lib.bin]
//             [--warp] [--debug] [--out times.tsv]
//
// --variants K: K PSOs per pair with different blend/depth/cull state (cost of a state variant).
// --save/--load: serialize an ID3D12PipelineLibrary or load PSOs from it (cost of an own disk cache).
#include "probe_common.h"

#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using namespace probe;

static void applyVariant(D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, int v) {
    // v = 0: opaque, depth write, back-face cull. Further variants only change fixed-function state.
    auto& rt = d.BlendState.RenderTarget[0];
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = (v & 4) ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.DepthStencilState.DepthEnable = TRUE;
    d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    d.DepthStencilState.DepthWriteMask = (v & 2) ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
    if (v & 1) {
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = (v & 8) ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
}

int main(int argc, char** argv) {
    Options o = parseArgs(argc, argv);
    std::vector<Pair> pairs = readPairs(o);
    std::printf("Pairs: %zu, variants per pair: %d, threads: %d\n", pairs.size(), o.variants, o.threads);

    if (o.debug) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
        else std::printf("debug layer not available\n");
    }
    ComPtr<IDXGIFactory6> factory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter;
    if (o.warp) factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    else factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    std::printf("Adapter: %ls\n", ad.Description);
    ComPtr<ID3D12Device1> device;
    HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) { std::printf("D3D12CreateDevice: 0x%08lX\n", (unsigned long)hr); return 1; }
    D3D12_FEATURE_DATA_D3D12_OPTIONS opt{};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opt, sizeof(opt));
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_0};
    device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
    std::printf("ResourceBindingTier: %d, Shader Model >= 6.0: %s\n", (int)opt.ResourceBindingTier,
                sm.HighestShaderModel >= D3D_SHADER_MODEL_6_0 ? "yes" : "no");

    // Root signature: 3 root CBVs + 5 unbounded SRV tables + 1 unbounded sampler table.
    D3D12_DESCRIPTOR_RANGE ranges[6]{};
    const UINT srvSpaces[5] = {1, 2, 3, 4, 6};
    for (int i = 0; i < 5; ++i) {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = UINT_MAX;
        ranges[i].RegisterSpace = srvSpaces[i];
    }
    ranges[5].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    ranges[5].NumDescriptors = UINT_MAX;
    ranges[5].RegisterSpace = 5;
    D3D12_ROOT_PARAMETER params[9]{};
    for (int i = 0; i < 3; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[i].Descriptor.ShaderRegister = i;
    }
    for (int i = 0; i < 6; ++i) {
        params[3 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[3 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
    }
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 9;
    rsd.pParameters = params;
    ComPtr<ID3DBlob> rsBlob, rsErr;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &rsBlob, &rsErr);
    if (FAILED(hr)) { std::printf("root signature: %s\n", rsErr ? (char*)rsErr->GetBufferPointer() : "?"); return 1; }
    ComPtr<ID3D12RootSignature> rootSig;
    hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig));
    if (FAILED(hr)) { std::printf("CreateRootSignature: 0x%08lX\n", (unsigned long)hr); return 1; }

    // Load the shaders (each file once).
    std::map<std::string, std::vector<uint8_t>> blobs;
    size_t bytes = 0;
    for (auto& p : pairs)
        for (const std::string* n : {&p.vs, &p.ps})
            if (!blobs.count(*n)) {
                blobs[*n] = readFile(o.shaderDir + "/" + *n + ".dxil");
                if (blobs[*n].empty()) { std::printf("missing: %s\n", n->c_str()); return 1; }
                bytes += blobs[*n].size();
            }
    std::printf("Shaders loaded: %zu files, %.1f MB\n", blobs.size(), bytes / 1048576.0);

    ComPtr<ID3D12PipelineLibrary> library;
    std::vector<uint8_t> libData;
    if (!o.load.empty()) {
        libData = readFile(o.load);
        auto t0 = Clock::now();
        hr = device->CreatePipelineLibrary(libData.data(), libData.size(), IID_PPV_ARGS(&library));
        std::printf("Load library (%.1f MB): 0x%08lX in %.1f ms\n", libData.size() / 1048576.0,
                    (unsigned long)hr, msSince(t0));
        if (FAILED(hr)) return 1;
    } else if (!o.save.empty()) {
        hr = device->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library));
        if (FAILED(hr)) { std::printf("CreatePipelineLibrary: 0x%08lX\n", (unsigned long)hr); return 1; }
    }

    const int total = (int)pairs.size() * o.variants;
    std::vector<double> ms(total, 0.0);
    std::vector<HRESULT> results(total, S_OK);
    std::vector<ComPtr<ID3D12PipelineState>> psos(total);
    std::atomic<int> failed{0};
    auto t0 = Clock::now();
    parallelFor(total, o.threads, [&](int i) {
        const Pair& p = pairs[i / o.variants];
        const int v = i % o.variants;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = rootSig.Get();
        d.VS = {blobs[p.vs].data(), blobs[p.vs].size()};
        d.PS = {blobs[p.ps].data(), blobs[p.ps].size()};
        d.SampleMask = UINT_MAX;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        d.SampleDesc.Count = 1;
        applyVariant(d, v);
        wchar_t name[32];
        swprintf(name, 32, L"p%d", i);
        auto t = Clock::now();
        HRESULT r;
        if (!o.load.empty()) r = library->LoadGraphicsPipeline(name, &d, IID_PPV_ARGS(&psos[i]));
        else r = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&psos[i]));
        ms[i] = msSince(t);
        results[i] = r;
        if (FAILED(r)) ++failed;
    });
    double wall = msSince(t0);
    printStats(o.load.empty() ? "D3D12 CreateGraphicsPipelineState" : "D3D12 LoadGraphicsPipeline", ms, wall,
               o.threads, failed.load());
    for (int i = 0, shown = 0; i < total && shown < 10; ++i)
        if (FAILED(results[i])) {
            const Pair& p = pairs[i / o.variants];
            std::printf("  error 0x%08lX: %s + %s (variant %d)\n", (unsigned long)results[i], p.vs.c_str(),
                        p.ps.c_str(), i % o.variants);
            ++shown;
        }

    if (!o.save.empty() && library) {
        auto t = Clock::now();
        for (int i = 0; i < total; ++i)
            if (psos[i]) {
                wchar_t name[32];
                swprintf(name, 32, L"p%d", i);
                library->StorePipeline(name, psos[i].Get());
            }
        std::vector<uint8_t> out(library->GetSerializedSize());
        library->Serialize(out.data(), out.size());
        std::ofstream f(o.save, std::ios::binary);
        f.write((const char*)out.data(), out.size());
        std::printf("Library saved: %.1f MB in %.1f ms\n", out.size() / 1048576.0, msSince(t));
    }
    if (!o.out.empty()) {
        std::ofstream f(o.out);
        for (int i = 0; i < total; ++i) {
            const Pair& p = pairs[i / o.variants];
            f << p.vs << '\t' << p.ps << '\t' << (i % o.variants) << '\t' << ms[i] << '\t' << std::hex
              << (unsigned long)results[i] << std::dec << '\n';
        }
    }
    if (o.debug) {
        ComPtr<ID3D12InfoQueue> q;
        if (SUCCEEDED(device.As(&q))) {
            UINT64 n = q->GetNumStoredMessages();
            std::printf("Debug layer: %llu messages\n", (unsigned long long)n);
            for (UINT64 i = 0; i < n && i < 8; ++i) {
                SIZE_T len = 0;
                q->GetMessage(i, nullptr, &len);
                std::vector<uint8_t> buf(len);
                auto* m = (D3D12_MESSAGE*)buf.data();
                q->GetMessage(i, m, &len);
                std::printf("  %s\n", m->pDescription);
            }
        }
    }
    return failed.load() ? 3 : 0;
}
