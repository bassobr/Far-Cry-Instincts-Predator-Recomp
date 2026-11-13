// fcp_semtool: helper of the semantic test.
//
//   fcp_semtool decode <words.bin> <base_hex> <out.tsv>
//       Decodes every big endian word with the decoder of our codegen (binutils derived ppc-dis.c in
//       rexglue/thirdparty/disasm, Xenon dialect). One line per word:
//       address, word, PPC_INST id, mnemonic, operand text ("-" when the decoder knows no opcode).
//
//   fcp_semtool emit <code.bin> <base_hex> <symbols.txt> <title.toml> <out_dir> <shards> <pch>
//       Recompiles the functions named in symbols.txt ("<hex address> <name>" per line, sorted, each
//       function ends at the next one) with our codegen and the codegen settings of the title
//       configuration, the same way `rexglue recompile-tests` does, but without its fixed test
//       settings (exact vector math, forwarded stack stores). Writes <out_dir>/sem_funcs_<n>.cpp
//       (n < shards) that include <pch> (the generated predator_pch.h, so the macros are the ones of the
//       game build) and sem_table.cpp with the table of entry points.
//
// Copyright (c) 2026 the Far Cry Instincts Predator Recomp project. Uses the ReXGlue SDK (BSD 3-Clause).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include <rex/platform/file.h>
#include <rex/types.h>

#include "codegen/binary_view.h"
#include "codegen/codegen_context.h"
#include "codegen/config.h"
#include "codegen/function_graph.h"
#include "codegen/module_stub.h"
#include "codegen/ppc/disasm.h"
#include "codegen/ppc/instruction.h"

namespace fs = std::filesystem;
namespace codegen = rex::codegen;

namespace {

std::vector<u8> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "fcp_semtool: cannot read %s\n", path.c_str());
    std::exit(2);
  }
  return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool WriteIfChanged(const fs::path& path, const std::string& text) {
  {
    std::ifstream in(path, std::ios::binary);
    if (in) {
      std::string old((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      if (old == text)
        return true;
    }
  }
  std::ofstream out(path, std::ios::binary);
  out << text;
  return static_cast<bool>(out);
}

int Decode(int argc, char** argv) {
  if (argc != 5) {
    std::fprintf(stderr, "usage: fcp_semtool decode <words.bin> <base_hex> <out.tsv>\n");
    return 2;
  }
  auto data = ReadFile(argv[2]);
  const u32 base = static_cast<u32>(std::strtoul(argv[3], nullptr, 16));
  std::string out;
  out.reserve(data.size() * 12);
  for (size_t i = 0; i + 4 <= data.size(); i += 4) {
    ppc_insn insn{};
    const u32 pc = base + static_cast<u32>(i);
    const u32 word = (u32(data[i]) << 24) | (u32(data[i + 1]) << 16) | (u32(data[i + 2]) << 8) |
                     u32(data[i + 3]);
    rex::codegen::detail::Disassemble(data.data() + i, 4, pc, insn);
    if (insn.opcode == nullptr) {
      out += std::format("{:08X}\t{:08X}\t0\t-\t-\n", pc, word);
    } else {
      out += std::format("{:08X}\t{:08X}\t{}\t{}\t{}\n", pc, word, insn.opcode->id, insn.opcode->name,
                         insn.op_str);
    }
  }
  std::ofstream f(argv[4], std::ios::binary);
  f << out;
  return f ? 0 : 1;
}

int Emit(int argc, char** argv) {
  if (argc != 9) {
    std::fprintf(stderr,
                 "usage: fcp_semtool emit <code.bin> <base_hex> <symbols.txt> <title.toml> <out_dir> "
                 "<shards> <pch>\n");
    return 2;
  }
  auto data = ReadFile(argv[2]);
  const u32 base = static_cast<u32>(std::strtoul(argv[3], nullptr, 16));
  struct Sym {
    u32 size;
    std::string name;
  };
  std::map<u32, Sym> symbols;
  {
    std::ifstream in(argv[4]);
    std::string addr, size, name;
    while (in >> addr >> size >> name)
      symbols[static_cast<u32>(std::strtoul(addr.c_str(), nullptr, 16))] = {
          static_cast<u32>(std::strtoul(size.c_str(), nullptr, 16)), name};
  }
  const fs::path out_dir = argv[6];
  const size_t shards = std::max<size_t>(1, std::strtoul(argv[7], nullptr, 10));
  const std::string pch = argv[8];

  // Codegen settings of the game: defaults plus the title configuration (skips the finalize checks
  // that need a XEX, like the shared layer of a manifest).
  codegen::RecompilerConfig config;
  {
    toml::table tbl;
    try {
      tbl = toml::parse_file(argv[5]);
    } catch (const toml::parse_error& e) {
      std::fprintf(stderr, "fcp_semtool: %s: %s\n", argv[5], e.what());
      return 2;
    }
    if (!config.LoadShared(tbl, fs::path(argv[5]).parent_path(), argv[5])) {
      std::fprintf(stderr, "fcp_semtool: cannot load %s\n", argv[5]);
      return 2;
    }
  }
  config.out_directory_path = out_dir.string();

  codegen::StubModule module;
  module.Load(base, data.data(), data.size());
  module.SetName("sem");
  auto ctx = codegen::CodegenContext::Create(codegen::BinaryView::FromModule(module), std::move(config));
  // As codegen::AnalyzeTestBinary, but with the sizes from the symbol file (the generator puts a zero
  // word between functions) and every symbol a function.
  for (const auto& [addr, sym] : symbols) {
    auto* node = ctx.graph.AddFunction(addr, sym.size, codegen::FunctionAuthority::kDiscovered, true);
    if (node) {
      node->Discover({{addr, sym.size}}, {}, {});
      ctx.graph.SetFunctionName(addr, std::format("sem_{:X}", addr));
      node->Seal();
    }
  }
  for (const auto& [addr, sym] : symbols) {
    for (u32 offset = 0; offset < sym.size; offset += 4) {
      const u32 pc = addr + offset;
      const u32 raw = (u32(data[pc - base]) << 24) | (u32(data[pc - base + 1]) << 16) |
                      (u32(data[pc - base + 2]) << 8) | u32(data[pc - base + 3]);
      auto decoded = codegen::detail::DecodeInstruction(pc, raw);
      const bool is_tail_branch = decoded.opcode == codegen::detail::Opcode::b &&
                                  decoded.branch_target.has_value() &&
                                  (*decoded.branch_target < addr || *decoded.branch_target >= addr + sym.size);
      if ((decoded.IsCall() || is_tail_branch) && decoded.branch_target.has_value()) {
        if (auto* target = ctx.graph.GetFunction(*decoded.branch_target))
          ctx.graph.AddCallToFunction(addr, pc, codegen::CallTarget::MakeFunction(target));
      }
    }
  }

  std::vector<const codegen::FunctionNode*> functions;
  for (const auto& [addr, node] : ctx.graph.GetFunctions())
    functions.push_back(node.get());
  std::sort(functions.begin(), functions.end(),
            [](const auto* a, const auto* b) { return a->GetBase() < b->GetBase(); });

  codegen::EmitContext emit_ctx{ctx.GetBinary(), ctx.GetConfig(), ctx.graph, ctx.GetLimits(), 0, nullptr};

  std::vector<std::string> shard_code(shards);
  std::string decls;
  std::string table;
  size_t index = 0;
  size_t emitted = 0;
  for (const auto* fn : functions) {
    std::string code = fn->EmitCpp(emit_ctx);
    if (code.empty())
      continue;
    const std::string name = std::format("sem_{:X}", fn->GetBase());
    decls += std::format("DECLARE_REX_FUNC({});\n", name);
    table += std::format("  {{0x{:08X}u, &{}}},\n", fn->GetBase(), name);
    shard_code[index++ % shards] += code + "\n";
    ++emitted;
  }

  std::string header = std::format(
      "// Generated by fcp_semtool emit. DO NOT EDIT.\n#include \"{}\"\n#include \"sem_decls.h\"\n\n", pch);
  bool ok = WriteIfChanged(out_dir / "sem_decls.h",
                           std::format("#pragma once\n#include \"{}\"\n{}", pch, decls));
  for (size_t i = 0; i < shards; ++i)
    ok &= WriteIfChanged(out_dir / std::format("sem_funcs_{}.cpp", i), header + shard_code[i]);
  ok &= WriteIfChanged(
      out_dir / "sem_table.cpp",
      std::format("// Generated by fcp_semtool emit. DO NOT EDIT.\n#include \"{}\"\n#include "
                  "\"sem_decls.h\"\n#include \"sem_runner_api.h\"\n\nextern \"C\" const FcpSemFunc "
                  "fcp_sem_funcs[] = {{\n{}  {{0u, nullptr}}\n}};\n",
                  pch, table));
  std::printf("fcp_semtool emit: %zu functions, %zu symbols\n", emitted, symbols.size());
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "decode")
    return Decode(argc, argv);
  if (argc >= 2 && std::string(argv[1]) == "emit")
    return Emit(argc, argv);
  std::fprintf(stderr, "usage: fcp_semtool decode|emit ...\n");
  return 2;
}
