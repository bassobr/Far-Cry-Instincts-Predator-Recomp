// Counters of the [fp_rules] slow paths (rex/ppc/fcp_fp_rules.h, rexglue/UPSTREAM.md change 70):
// how often a guest FP result needed the PowerPC NaN rule or the single rounding of fmadds, and how
// often that result differs from the plain host result.
#pragma once

namespace fcp::fp_stats {

/** Writes the totals and the busiest guest addresses to the log (one block at shutdown). */
void WriteSummary();

}  // namespace fcp::fp_stats
