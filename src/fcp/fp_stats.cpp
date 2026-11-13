#include "fcp/fp_stats.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include <rex/ppc/fcp_fp_rules.h>

#include "fcp/game_code.h"
#include "fcp/log.h"

namespace fcp::fp_stats {

void WriteSummary() {
  namespace r = rex::ppc::fcp_fp_rules;
  static const char* const kNames[r::kKindCount] = {"scalar_nan", "vmx_nan", "vmx_minmax", "fmadds_tie"};
  // The counters of predator_code.dll, where the generated code runs (the EXE's own r::g_stats stays 0).
  auto* dll_stats = static_cast<r::Stats*>(fcp::game_code::FpStats());
  auto& st = dll_stats ? *dll_stats : r::g_stats;
  uint64_t total = 0;
  for (uint32_t k = 0; k < r::kKindCount; ++k) {
    total += st.hits[k].load(std::memory_order_relaxed);
  }
  // Without [fp_rules] in the title configuration no slow path exists and every counter stays 0; the
  // counters run only with FCP_FP_STATS=1.
  if (!r::StatsOn()) {
    return;
  }
  FCPLOG("fp_rules: slow paths {} (scalar_nan {}/{} differ, vmx_nan {}/{}, vmx_minmax {}/{}, fmadds_tie {}/{}), "
         "sites dropped {}",
         total, st.hits[0].load(), st.differs[0].load(), st.hits[1].load(), st.differs[1].load(),
         st.hits[2].load(), st.differs[2].load(), st.hits[3].load(), st.differs[3].load(),
         st.sites_dropped.load());
  if (total == 0) {
    return;
  }
  struct Row {
    uint32_t address, kind;
    uint64_t hits, differs;
  };
  std::vector<Row> rows;
  for (const auto& s : st.sites) {
    const uint32_t key = s.key.load(std::memory_order_relaxed);
    if (key != 0) {
      rows.push_back({key & ~3u, key & 3u, s.hits.load(), s.differs.load()});
    }
  }
  // sites with differing results first, then by hits
  std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) {
    return x.differs != y.differs ? x.differs > y.differs : x.hits > y.hits;
  });
  FCPLOG("fp_rules: {} guest addresses", rows.size());
  // every NaN site (from the static cross reference of the image), the 40 busiest fmadds ties
  size_t ties = 0;
  for (size_t i = 0; i < rows.size(); ++i) {
    const Row& w = rows[i];
    if (w.kind == r::kFmaddsTie && w.differs == 0 && ++ties > 40) {
      continue;
    }
    FCPLOG("  fp_rules site {:08X} {} hits {} differ {}", w.address,
           w.kind < r::kKindCount ? kNames[w.kind] : "?", w.hits, w.differs);
  }
}

}  // namespace fcp::fp_stats
