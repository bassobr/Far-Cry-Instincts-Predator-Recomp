// fcshaders: translate Predator shaders (container 0x102A0E00/01) ahead of time to HLSL, DXIL and SPIR-V.
//
//   fcshaders run <fxo-folder> --dxc <dxc-release> --out <folder> [--jobs N] [--no-dxil] [--no-spirv]
//                 [--hlsl] [--filter TEXT]
//       Translates every *.fxo file, compiles with DXC and writes <out>/results.tsv (status per shader),
//       <out>/interface.jsonl (what the renderer must bind per shader), <out>/dxil/*.dxil, <out>/spirv/*.spv
//       and with --hlsl <out>/hlsl/*.hlsl. Failed HLSL always ends up in <out>/failed/.
//   fcshaders pack <fxo-folder> --dxc <dxc-release> --out <file.fcpack> [--target dxil|spirv] [--jobs N]
//       Disc pack of one target with a hash index (fcshaders/pack.h); derived game data, never commit.
//   fcshaders translate <file.fxo>            HLSL to stdout
//   fcshaders compile <file.hlsl> vs|ps --dxc <dxc-release> [--spirv]   one HLSL file with the same options
//
// Original code of this project; the container adapter, translator and DXC binding come from King Kong
// Recompiled (MIT), the translation core from XenosRecomp (MIT), see tools/shaders/xenosrecomp/CHANGES.md.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "fcshaders/compiler.h"
#include "fcshaders/container.h"
#include "fcshaders/library.h"
#include "fcshaders/pack.h"
#include "fcshaders/translator.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace fcshaders;

namespace {

// Default thread count: the logical processors this process may run on (process affinity, e.g.
// via FCP_GAME_CODE_CPUS on first start or `start /affinity`), instead of hardware_concurrency(), which always
// counts all processors of the machine (measured: 32 threads on 8 allowed processors). FCSHADERS_JOBS=<n> or
// --jobs N take precedence; FCSHADERS_JOBS=all = old behaviour (hardware_concurrency).
unsigned defaultJobs() {
    const unsigned all = std::max(1u, std::thread::hardware_concurrency());
    if (const char* e = std::getenv("FCSHADERS_JOBS"); e && *e) {
        if (std::strcmp(e, "all") == 0) return all;
        if (const int n = std::atoi(e); n > 0) return unsigned(n);
    }
#ifdef _WIN32
    DWORD_PTR process = 0, system = 0;
    // With more than one processor group (> 64 logical processors) the mask covers only one group: then use all.
    if (GetActiveProcessorGroupCount() == 1 && GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
        unsigned n = 0;
        for (DWORD_PTR m = process; m; m &= m - 1) ++n;
        return std::max(1u, std::min(n, all));
    }
#endif
    return all;
}

std::vector<uint8_t> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void writeFile(const fs::path& p, const void* data, size_t size) {
    std::ofstream f(p, std::ios::binary);
    f.write(static_cast<const char*>(data), std::streamsize(size));
}

std::string firstLine(const std::string& s) {
    // prefer the first line containing "error", else the first non-empty one
    std::istringstream in(s);
    std::string line, first;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (first.empty()) first = line;
        if (line.find("error") != std::string::npos) return line;
    }
    return first;
}

std::string clean(std::string s) {
    for (char& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += c;
        else if (uint8_t(c) < 0x20) o += ' ';
        else o += c;
    }
    return o;
}

const char* setName(RegisterSet s) {
    switch (s) {
        case RegisterSet::Bool: return "bool";
        case RegisterSet::Int4: return "int4";
        case RegisterSet::Float4: return "float4";
        default: return "sampler";
    }
}

const char* dimName(TextureDimension d) {
    switch (d) {
        case TextureDimension::Tex1D: return "1D";
        case TextureDimension::Tex2D: return "2D";
        case TextureDimension::Tex3D: return "3D";
        default: return "Cube";
    }
}

struct Job {
    fs::path path;
    std::string name;
    // results
    std::string stage = "ok";  // parse | translate | dxil | spirv | ok
    std::string error;
    std::string kind;
    size_t hlslSize = 0, dxilSize = 0, spirvSize = 0;
    bool dxilSigned = false;
    double translateMs = 0, dxilMs = 0, spirvMs = 0;
    std::string iface;
    std::string warnings;
};

std::string interfaceJson(const Job& j, const ShaderInfo& info, const ShaderBindings& b) {
    std::ostringstream o;
    o << "{\"name\":\"" << jsonEscape(j.name) << "\",\"kind\":\"" << j.kind << "\"";
    char hash[32];
    std::snprintf(hash, sizeof(hash), "%016llX", (unsigned long long)info.ucodeHash);
    o << ",\"ucodeHash\":\"" << hash << "\",\"instructions\":" << info.physicalSize / 12;
    o << ",\"constants\":[";
    for (size_t i = 0; i < info.constants.size(); i++) {
        const auto& c = info.constants[i];
        o << (i ? "," : "") << "{\"name\":\"" << jsonEscape(c.name) << "\",\"set\":\"" << setName(c.registerSet)
          << "\",\"reg\":" << c.registerIndex << ",\"count\":" << c.registerCount << "}";
    }
    o << "],\"floatLiterals\":[";
    for (size_t i = 0; i < info.literals.size(); i++) o << (i ? "," : "") << info.literals[i].registerIndex;
    o << "],\"vertexInputs\":[";
    for (size_t i = 0; i < b.vertexBindings.size(); i++) {
        const auto& v = b.vertexBindings[i];
        if (v.raw) o << (i ? "," : "") << "\"fetchConstant" << int(v.fetchConstant) << "\"";
        else o << (i ? "," : "") << "\"" << declUsageName(v.usage) << int(v.usageIndex) << "\"";
    }
    o << "],\"samplers\":[";
    for (size_t i = 0; i < b.samplers.size(); i++) {
        const auto& s = b.samplers[i];
        o << (i ? "," : "") << "{\"slot\":" << int(s.slot) << ",\"dim\":\"" << dimName(s.dimension) << "\"}";
    }
    o << "],\"interpolators\":[";
    for (size_t i = 0; i < info.interpolators.size(); i++) {
        const auto& t = info.interpolators[i];
        o << (i ? "," : "") << "\"" << declUsageName(t.usage) << int(t.usageIndex) << "@r" << int(t.reg) << "\"";
    }
    o << "],\"pixelOutputs\":" << b.pixelOutputs << ",\"exportedInterpolators\":" << b.interpolators
      << ",\"tempRegisters\":" << b.tempRegisters << ",\"generalControlFlow\":" << (b.generalControlFlow ? "true" : "false")
      << ",\"dynamicRegisters\":" << (b.dynamicRegisters ? "true" : "false")
      << ",\"readsVPos\":" << (info.readsPixelPosition ? "true" : "false") << "}";
    return o.str();
}

int cmdRun(int argc, char** argv) {
    if (argc < 3) return std::fprintf(stderr, "run <fxo-folder> --dxc <dir> --out <dir>\n"), 2;
    fs::path in = argv[2];
    std::string dxcDir, filter;
    fs::path out = "fcshaders-out";
    unsigned jobs = defaultJobs();
    bool doDxil = true, doSpirv = true, keepHlsl = false;
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--dxc" && i + 1 < argc) dxcDir = argv[++i];
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--jobs" && i + 1 < argc) jobs = unsigned(std::max(1, std::atoi(argv[++i])));
        else if (a == "--filter" && i + 1 < argc) filter = argv[++i];
        else if (a == "--no-dxil") doDxil = false;
        else if (a == "--no-spirv") doSpirv = false;
        else if (a == "--hlsl") keepHlsl = true;
        else return std::fprintf(stderr, "unknown option %s\n", a.c_str()), 2;
    }
    std::string err;
    if ((doDxil || doSpirv) && !loadDxc(dxcDir, &err)) return std::fprintf(stderr, "DXC: %s\n", err.c_str()), 1;
    fs::create_directories(out / "failed");
    if (doDxil) fs::create_directories(out / "dxil");
    if (doSpirv) fs::create_directories(out / "spirv");
    if (keepHlsl) fs::create_directories(out / "hlsl");

    std::vector<Job> list;
    for (const auto& e : fs::directory_iterator(in)) {
        if (!e.is_regular_file() || e.path().extension() != ".fxo") continue;
        std::string n = e.path().stem().string();
        if (!filter.empty() && n.find(filter) == std::string::npos) continue;
        Job j;
        j.path = e.path();
        j.name = n;
        list.push_back(std::move(j));
    }
    std::sort(list.begin(), list.end(), [](const Job& a, const Job& b) { return a.name < b.name; });
    std::printf("fcshaders: %zu containers, %u threads, DXC %s\n", list.size(), jobs, dxcIdentity().c_str());

    // Distribution in name order. Measured: "largest first" was slower (32.7 instead of 24.4 s with
    // 32 threads), because then all expensive shaders run in DXC at the same time.
    std::atomic<size_t> next{0}, done{0};
    auto t0 = std::chrono::steady_clock::now();
    auto worker = [&] {
        Compiler compiler;
        for (;;) {
            size_t i = next++;
            if (i >= list.size()) break;
            Job& j = list[i];
            auto bytes = readFile(j.path);
            ParseResult p = parseContainer(bytes.data(), bytes.size());
            if (!p.ok) {
                j.stage = "parse";
                j.error = p.error;
                done++;
                continue;
            }
            j.kind = p.info.kind == ShaderKind::Vertex ? "VS" : "PS";
            auto a = std::chrono::steady_clock::now();
            TranslateResult t = translate(p.info);
            auto b = std::chrono::steady_clock::now();
            j.translateMs = std::chrono::duration<double, std::milli>(b - a).count();
            if (!t.ok) {
                j.stage = "translate";
                j.error = t.error;
                done++;
                continue;
            }
            for (const auto& w : t.bindings.warnings) j.warnings += (j.warnings.empty() ? "" : " | ") + w;
            j.hlslSize = t.hlsl.size();
            j.iface = interfaceJson(j, p.info, t.bindings);
            if (keepHlsl) writeFile(out / "hlsl" / (j.name + ".hlsl"), t.hlsl.data(), t.hlsl.size());
            if (doDxil) {
                a = std::chrono::steady_clock::now();
                CompileOutput c = compiler.compile(t.hlsl, p.info.kind, CompileTarget::Dxil);
                b = std::chrono::steady_clock::now();
                j.dxilMs = std::chrono::duration<double, std::milli>(b - a).count();
                if (!c.ok) {
                    j.stage = "dxil";
                    j.error = firstLine(c.messages);
                    writeFile(out / "failed" / (j.name + ".hlsl"), t.hlsl.data(), t.hlsl.size());
                    writeFile(out / "failed" / (j.name + ".dxil.txt"), c.messages.data(), c.messages.size());
                } else {
                    j.dxilSize = c.blob.size();
                    j.dxilSigned = isDxilSigned(c.blob);
                    writeFile(out / "dxil" / (j.name + ".dxil"), c.blob.data(), c.blob.size());
                }
            }
            if (doSpirv) {
                a = std::chrono::steady_clock::now();
                CompileOutput c = compiler.compile(t.hlsl, p.info.kind, CompileTarget::Spirv);
                b = std::chrono::steady_clock::now();
                j.spirvMs = std::chrono::duration<double, std::milli>(b - a).count();
                if (!c.ok) {
                    if (j.stage == "ok") j.stage = "spirv";
                    if (j.error.empty()) j.error = firstLine(c.messages);
                    writeFile(out / "failed" / (j.name + ".hlsl"), t.hlsl.data(), t.hlsl.size());
                    writeFile(out / "failed" / (j.name + ".spirv.txt"), c.messages.data(), c.messages.size());
                } else {
                    j.spirvSize = c.blob.size();
                    writeFile(out / "spirv" / (j.name + ".spv"), c.blob.data(), c.blob.size());
                }
            }
            size_t d = ++done;
            if (d % 250 == 0) std::fprintf(stderr, "  %zu/%zu\n", d, list.size());
        }
    };
    std::vector<std::thread> threads;
    for (unsigned k = 0; k < jobs; k++) threads.emplace_back(worker);
    for (auto& t : threads) t.join();
    double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::map<std::string, size_t> stages;
    std::map<std::string, size_t> causes;
    size_t dxilTotal = 0, spirvTotal = 0, hlslTotal = 0, signedCount = 0, warned = 0;
    double tMs = 0, dMs = 0, sMs = 0;
    std::ofstream res(out / "results.tsv");
    res << "name\tkind\tstage\thlsl\tdxil\tspirv\tsigned\ttranslate_ms\tdxil_ms\tspirv_ms\terror\twarnings\n";
    std::ofstream iface(out / "interface.jsonl");
    for (const auto& j : list) {
        stages[j.kind + " " + j.stage]++;
        if (j.stage != "ok") causes[j.stage + ": " + clean(j.error)]++;
        dxilTotal += j.dxilSize;
        spirvTotal += j.spirvSize;
        hlslTotal += j.hlslSize;
        signedCount += j.dxilSigned;
        warned += !j.warnings.empty();
        tMs += j.translateMs;
        dMs += j.dxilMs;
        sMs += j.spirvMs;
        res << j.name << '\t' << j.kind << '\t' << j.stage << '\t' << j.hlslSize << '\t' << j.dxilSize << '\t'
            << j.spirvSize << '\t' << j.dxilSigned << '\t' << j.translateMs << '\t' << j.dxilMs << '\t' << j.spirvMs
            << '\t' << clean(j.error) << '\t' << clean(j.warnings) << '\n';
        if (!j.iface.empty()) iface << j.iface << '\n';
    }
    std::printf("Run time %.2f s (wall), %u threads; total per step: translate %.1f s, DXIL %.1f s, SPIR-V %.1f s\n",
                wall, jobs, tMs / 1000, dMs / 1000, sMs / 1000);
    for (const auto& [k, v] : stages) std::printf("  %-14s %zu\n", k.c_str(), v);
    std::printf("Sizes: HLSL %zu, DXIL %zu (%zu signed), SPIR-V %zu bytes; shaders with warnings %zu\n", hlslTotal,
                dxilTotal, signedCount, spirvTotal, warned);
    std::vector<std::pair<size_t, std::string>> sorted;
    for (const auto& [k, v] : causes) sorted.push_back({v, k});
    std::sort(sorted.rbegin(), sorted.rend());
    for (const auto& [v, k] : sorted) std::printf("  %5zu  %s\n", v, k.c_str());
    return 0;
}

// fcshaders pack <fxo-folder> --dxc <dir> --out <file> [--target dxil|spirv] [--jobs N]
// Builds the disc pack of one target (pack.h): every container once (key containerHash), with reflection.
int cmdPack(int argc, char** argv) {
    if (argc < 3) return std::fprintf(stderr, "pack <fxo-folder> --dxc <dir> --out <file> [--target dxil|spirv]\n"), 2;
    fs::path in = argv[2];
    std::string dxcDir, outPath;
    PackTarget target = PackTarget::Spirv;
    unsigned jobs = defaultJobs();
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--dxc" && i + 1 < argc) dxcDir = argv[++i];
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--jobs" && i + 1 < argc) jobs = unsigned(std::max(1, std::atoi(argv[++i])));
        else if (a == "--target" && i + 1 < argc) {
            std::string t = argv[++i];
            if (t == "dxil") target = PackTarget::Dxil;
            else if (t == "spirv" || t == "spv") target = PackTarget::Spirv;
            else return std::fprintf(stderr, "unknown target %s\n", t.c_str()), 2;
        } else return std::fprintf(stderr, "unknown option %s\n", a.c_str()), 2;
    }
    if (outPath.empty()) return std::fprintf(stderr, "--out missing\n"), 2;
    std::string err;
    if (!loadDxc(dxcDir, &err)) return std::fprintf(stderr, "DXC: %s\n", err.c_str()), 1;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(in))
        if (e.is_regular_file() && e.path().extension() == ".fxo") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    std::printf("fcshaders pack: %zu containers, target %s, %u threads, DXC %s\n", files.size(), packTargetName(target),
                jobs, dxcIdentity().c_str());
    PackWriter writer(target, buildStamp());
    std::mutex mutex;
    std::atomic<size_t> next{0}, failed{0}, duplicates{0};
    std::vector<std::string> failures;
    auto t0 = std::chrono::steady_clock::now();
    auto worker = [&] {
        Compiler compiler;
        BuildOptions options;
        options.dxil = target == PackTarget::Dxil;
        options.spirv = target == PackTarget::Spirv;
        for (;;) {
            size_t i = next++;
            if (i >= files.size()) break;
            auto bytes = readFile(files[i]);
            BuildResult r = buildShader(bytes.data(), bytes.size(), &compiler, options);
            std::lock_guard lock(mutex);
            if (!r.ok) {
                failed++;
                failures.push_back(files[i].stem().string() + " (" + r.stage + "): " + clean(firstLine(r.error)));
                continue;
            }
            if (!writer.add(r.reflection.containerHash, r.reflection.kind, target == PackTarget::Dxil ? r.dxil : r.spirv,
                            r.reflection))
                duplicates++;
        }
    };
    std::vector<std::thread> threads;
    for (unsigned k = 0; k < jobs; k++) threads.emplace_back(worker);
    for (auto& t : threads) t.join();
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!writer.write(outPath, &err)) return std::fprintf(stderr, "%s\n", err.c_str()), 1;
    std::printf("Pack %s: %zu entries, %zu failed, %zu duplicates, %.2f s\n", outPath.c_str(), writer.count(),
                size_t(failed), size_t(duplicates), wall);
    for (const auto& f : failures) std::printf("  %s\n", f.c_str());
    return 0;
}

int cmdTranslate(int argc, char** argv) {
    if (argc < 3) return 2;
    auto bytes = readFile(argv[2]);
    ParseResult p = parseContainer(bytes.data(), bytes.size());
    if (!p.ok) return std::fprintf(stderr, "parse: %s\n", p.error.c_str()), 1;
    TranslateResult t = translate(p.info);
    if (!t.ok) return std::fprintf(stderr, "translate: %s\n", t.error.c_str()), 1;
    std::fwrite(t.hlsl.data(), 1, t.hlsl.size(), stdout);
    for (const auto& w : t.bindings.warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
    return 0;
}

int cmdCompile(int argc, char** argv) {
    if (argc < 4) return 2;
    std::string dxcDir;
    bool spirv = false;
    for (int i = 4; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--dxc" && i + 1 < argc) dxcDir = argv[++i];
        else if (a == "--spirv") spirv = true;
    }
    std::string err;
    if (!loadDxc(dxcDir, &err)) return std::fprintf(stderr, "DXC: %s\n", err.c_str()), 1;
    auto bytes = readFile(argv[2]);
    std::string hlsl(bytes.begin(), bytes.end());
    Compiler c;
    CompileOutput o = c.compile(hlsl, std::string(argv[3]) == "vs" ? ShaderKind::Vertex : ShaderKind::Pixel,
                                spirv ? CompileTarget::Spirv : CompileTarget::Dxil);
    std::printf("%s\n%s: %zu bytes\n", o.messages.c_str(), o.ok ? "ok" : "ERROR", o.blob.size());
    return o.ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "fcshaders run|translate|compile ...\n");
        return 2;
    }
    std::string c = argv[1];
    if (c == "run") return cmdRun(argc, argv);
    if (c == "pack") return cmdPack(argc, argv);
    if (c == "translate") return cmdTranslate(argc, argv);
    if (c == "compile") return cmdCompile(argc, argv);
    std::fprintf(stderr, "unknown command %s\n", c.c_str());
    return 2;
}
