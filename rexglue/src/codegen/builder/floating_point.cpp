/**
 * @file        codegen/builder/floating_point.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builder_context.h"
#include "builder.h"

#include <dis-asm.h>
#include <ppc-inst.h>

#include <format>
#include <string>

namespace rex::codegen {

template <>
bool Build<PPC_INST_FABS>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.u64 = {}.u64 & ~0x8000000000000000;", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FNABS>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.u64 = {}.u64 | 0x8000000000000000;", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FNEG>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.u64 = {}.u64 ^ 0x8000000000000000;", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FMR>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = {}.f64;", ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FCFID>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64({}.s64);", ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FCTID>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  // FCP fctid_sat: f64(LLONG_MAX) is 2^63, so 2^63 itself must saturate too (upstream used >).
  if (FcpOriginal("fctid_sat")) {
    ctx.Println(
        "\t{0}.s64 = std::isnan({1}.f64) ? i64(0x8000000000000000ULL) : "
        "({1}.f64 > f64(LLONG_MAX)) ? LLONG_MAX : "
        "simde_mm_cvtsd_si64(simde_mm_load_sd(&{1}.f64));",
        ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]));
    return true;
  }
  ctx.Println(
      "\t{0}.s64 = std::isnan({1}.f64) ? i64(0x8000000000000000ULL) : "
      "({1}.f64 >= f64(LLONG_MAX)) ? LLONG_MAX : "
      "simde_mm_cvtsd_si64(simde_mm_load_sd(&{1}.f64));",
      ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FCTIDZ>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  // FCP fctid_sat: f64(LLONG_MAX) is 2^63, so 2^63 itself must saturate too (upstream used >).
  if (FcpOriginal("fctid_sat")) {
    ctx.Println(
        "\t{0}.s64 = std::isnan({1}.f64) ? i64(0x8000000000000000ULL) : "
        "({1}.f64 > f64(LLONG_MAX)) ? LLONG_MAX : "
        "simde_mm_cvttsd_si64(simde_mm_load_sd(&{1}.f64));",
        ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]));
    return true;
  }
  ctx.Println(
      "\t{0}.s64 = std::isnan({1}.f64) ? i64(0x8000000000000000ULL) : "
      "({1}.f64 >= f64(LLONG_MAX)) ? LLONG_MAX : "
      "simde_mm_cvttsd_si64(simde_mm_load_sd(&{1}.f64));",
      ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FCTIW>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println(
      "\t{0}.s64 = std::isnan({1}.f64) ? i64(INT_MIN) : "
      "({1}.f64 >= f64(INT_MAX)) ? INT_MAX : "
      "simde_mm_cvtsd_si32(simde_mm_load_sd(&{1}.f64));",
      ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FCTIWZ>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println(
      "\t{0}.s64 = std::isnan({1}.f64) ? i64(INT_MIN) : "
      "({1}.f64 >= f64(INT_MAX)) ? INT_MAX : "
      "simde_mm_cvttsd_si32(simde_mm_load_sd(&{1}.f64));",
      ctx.F(ctx.insn.operands[0]), ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FRSP>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = f64(f32({}.f64));", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FCMPU>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.Compare({}.f64, {}.f64);", ctx.CR(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]));
  return true;
}

template <>
bool Build<PPC_INST_FCMPO>(BuilderContext& ctx) {
  // fcmpo matches fcmpu here, its FPSCR exception flags for SNaN operands are not emulated.
  return Build<PPC_INST_FCMPU>(ctx);
}

namespace {

// FCP [fp_rules] (rexglue/UPSTREAM.md, change 70): NaN results after the
// PowerPC rules and the single rounding of the fmadds family, both off by default. The helpers live in
// rex/ppc/fcp_fp_rules.h. FCP_CODEGEN_ORIGINAL=fp_nan / fmadds_single switches back at codegen time.
bool FpNan(BuilderContext& ctx) {
  return ctx.GetConfig().fp_rules.nan && !FcpOriginal("fp_nan");
}

bool FmaddsSingle(BuilderContext& ctx) {
  return ctx.GetConfig().fp_rules.fmadds_single && !FcpOriginal("fmadds_single");
}

/** frD = a <op> b; nan_a/nan_b: operands in PowerPC NaN priority order. */
bool EmitFpBinary(BuilderContext& ctx, const char* op, bool single) {
  const auto d = ctx.F(ctx.insn.operands[0]);
  const auto a = ctx.F(ctx.insn.operands[1]);
  const auto b = ctx.F(ctx.insn.operands[2]);
  ctx.EmitSetFlushMode(false);
  const std::string expr = single ? std::format("f64(f32({}.f64 {} {}.f64))", a, op, b)
                                  : std::format("{}.f64 {} {}.f64", a, op, b);
  if (FpNan(ctx)) {
    ctx.Println("	{}.f64 = rex::ppc::fcp_fp_rules::Nan2({}, {}.f64, {}.f64, {}, 0x{:X});", d, expr, a,
                b, single ? 1 : 0, ctx.base);
  } else {
    ctx.Println("	{}.f64 = {};", d, expr);
  }
  return true;
}

/**
 * Fused forms frD = [-](frA*frC [+-] frB). operands: frD, frA, frC, frB. sub: frB negated, neg:
 * result negated (not for a NaN, PowerPC rule).
 */
bool EmitFpFused(BuilderContext& ctx, bool sub, bool neg, bool single) {
  const auto d = ctx.F(ctx.insn.operands[0]);
  const auto a = ctx.F(ctx.insn.operands[1]);
  const auto c = ctx.F(ctx.insn.operands[2]);
  const auto b = ctx.F(ctx.insn.operands[3]);
  ctx.EmitSetFlushMode(false);
  const std::string addend = std::format("{}{}.f64", sub ? "-" : "", b);
  const std::string fma = std::format("std::fma({}.f64, {}.f64, {})", a, c, addend);
  std::string expr;
  if (!single) {
    expr = std::format("{}{}", neg ? "-" : "", fma);
  } else if (FmaddsSingle(ctx)) {
    // rounded once to single, then negated (fnmadds = -(fmadds), PowerPC ISA)
    expr = std::format("{}rex::ppc::fcp_fp_rules::FmaddsRound({}, {}.f64, {}.f64, {}, 0x{:X})",
                       neg ? "-" : "", fma, a, c, addend, ctx.base);
  } else {
    expr = std::format("f64(f32({}{}))", neg ? "-" : "", fma);
  }
  if (FpNan(ctx)) {
    ctx.Println("	{}.f64 = rex::ppc::fcp_fp_rules::Nan3({}, {}.f64, {}.f64, {}.f64, {}, 0x{:X});", d,
                expr, a, b, c, single ? 1 : 0, ctx.base);
  } else {
    ctx.Println("	{}.f64 = {};", d, expr);
  }
  return true;
}

bool EmitFpSqrt(BuilderContext& ctx, bool single) {
  const auto d = ctx.F(ctx.insn.operands[0]);
  const auto b = ctx.F(ctx.insn.operands[1]);
  ctx.EmitSetFlushMode(false);
  const std::string expr =
      single ? std::format("f64(f32(sqrt({}.f64)))", b) : std::format("sqrt({}.f64)", b);
  if (FpNan(ctx)) {
    ctx.Println("	{}.f64 = rex::ppc::fcp_fp_rules::Nan1({}, {}.f64, {}, 0x{:X});", d, expr, b,
                single ? 1 : 0, ctx.base);
  } else {
    ctx.Println("	{}.f64 = {};", d, expr);
  }
  return true;
}

}  // namespace

template <>
bool Build<PPC_INST_FADD>(BuilderContext& ctx) {
  return EmitFpBinary(ctx, "+", false);
}

template <>
bool Build<PPC_INST_FADDS>(BuilderContext& ctx) {
  return EmitFpBinary(ctx, "+", true);
}

template <>
bool Build<PPC_INST_FSUB>(BuilderContext& ctx) {
  return EmitFpBinary(ctx, "-", false);
}

template <>
bool Build<PPC_INST_FSUBS>(BuilderContext& ctx) {
  return EmitFpBinary(ctx, "-", true);
}

template <>
bool Build<PPC_INST_FMUL>(BuilderContext& ctx) {
  return EmitFpBinary(ctx, "*", false);  // operands frD, frA, frC: NaN priority frA, frC
}

template <>
bool Build<PPC_INST_FMULS>(BuilderContext& ctx) {
  return EmitFpBinary(ctx, "*", true);
}

template <>
bool Build<PPC_INST_FDIV>(BuilderContext& ctx) {
  return EmitFpBinary(ctx, "/", false);
}

template <>
bool Build<PPC_INST_FDIVS>(BuilderContext& ctx) {
  return EmitFpBinary(ctx, "/", true);
}

template <>
bool Build<PPC_INST_FMADD>(BuilderContext& ctx) {
  return EmitFpFused(ctx, false, false, false);
}

template <>
bool Build<PPC_INST_FMADDS>(BuilderContext& ctx) {
  return EmitFpFused(ctx, false, false, true);
}

template <>
bool Build<PPC_INST_FMSUB>(BuilderContext& ctx) {
  return EmitFpFused(ctx, true, false, false);
}

template <>
bool Build<PPC_INST_FMSUBS>(BuilderContext& ctx) {
  return EmitFpFused(ctx, true, false, true);
}

template <>
bool Build<PPC_INST_FNMADD>(BuilderContext& ctx) {
  return EmitFpFused(ctx, false, true, false);
}

template <>
bool Build<PPC_INST_FNMADDS>(BuilderContext& ctx) {
  return EmitFpFused(ctx, false, true, true);
}

template <>
bool Build<PPC_INST_FNMSUB>(BuilderContext& ctx) {
  return EmitFpFused(ctx, true, true, false);
}

template <>
bool Build<PPC_INST_FNMSUBS>(BuilderContext& ctx) {
  return EmitFpFused(ctx, true, true, true);
}

template <>
bool Build<PPC_INST_FRES>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  if (ctx.GetConfig().vector_math.fres == VectorMath::kFast) {
    ctx.Println("	{}.f64 = f64(f32(1.0 / {}.f64));", ctx.F(ctx.insn.operands[0]),
                ctx.F(ctx.insn.operands[1]));
    return true;
  }
  ctx.Println("\t{}.f64 = rex::ppc::ReciprocalEstimate({}.f64);", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FRSQRTE>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  if (ctx.GetConfig().vector_math.frsqrte == VectorMath::kFast) {
    ctx.Println("	{}.f64 = f64(f32(1.0 / std::sqrt({}.f64)));", ctx.F(ctx.insn.operands[0]),
                ctx.F(ctx.insn.operands[1]));
    return true;
  }
  ctx.Println("\t{}.f64 = rex::ppc::ReciprocalSqrtEstimate({}.f64);", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]));
  return true;
}

template <>
bool Build<PPC_INST_FSQRT>(BuilderContext& ctx) {
  return EmitFpSqrt(ctx, false);
}

template <>
bool Build<PPC_INST_FSQRTS>(BuilderContext& ctx) {
  return EmitFpSqrt(ctx, true);
}

template <>
bool Build<PPC_INST_FSEL>(BuilderContext& ctx) {
  ctx.EmitSetFlushMode(false);
  ctx.Println("\t{}.f64 = {}.f64 >= 0.0 ? {}.f64 : {}.f64;", ctx.F(ctx.insn.operands[0]),
              ctx.F(ctx.insn.operands[1]), ctx.F(ctx.insn.operands[2]),
              ctx.F(ctx.insn.operands[3]));
  return true;
}

}  // namespace rex::codegen
