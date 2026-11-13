/**
 * @file        rex/ppc/guest_prelude.h
 * @brief       FCP: the SDK headers the generated code includes (from the codegen PCH header).
 *
 * @license     BSD 3-Clause License (same as the SDK). Local addition of the Far Cry Instincts
 *              Predator project (rexglue/UPSTREAM.md, change 140), not part of upstream ReXGlue.
 *
 * The codegen PCH header (resources/templates/codegen/pch_h.inja) included 13 SDK headers and 7
 * standard headers directly. It now includes this header, which keeps that list for hosted builds
 * and, with REX_GUEST_FREESTANDING, includes only what the generated functions use:
 * PPCContext, the FPU and VMX helpers, the [fp_rules] helpers, the MMIO slots and the few runtime
 * functions in rexppc.dll and rexbase.dll. That build runs with `-nostdlibinc` and the headers in
 * rex/guest_std, so the recompiled code compiles on a PC without Visual Studio or the Windows SDK
 * (the first-start build of predator_code.dll).
 */
#pragma once

#if defined(REX_GUEST_FREESTANDING)

#include <atomic>
#include <csetjmp>
#include <cstdlib>

#include <rex/chrono.h>
#include <rex/ppc/context.h>
#include <rex/ppc/fpu.h>
#include <rex/ppc/image.h>
#include <rex/ppc/mmio.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/vmx.h>
#include <rex/types.h>

#else

#include <rex/ppc/hook.h>
#include <rex/ppc/call.h>
#include <rex/ppc/context.h>
#include <rex/ppc/fpu.h>
#include <rex/ppc/stack.h>
#include <rex/ppc/vmx.h>
#include <rex/types.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/image.h>
#include <rex/logging/log_category.h>
#include <rex/chrono.h>
#include <rex/platform/perf.h>
#include <rex/ppc/mmio.h>

#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

#endif
