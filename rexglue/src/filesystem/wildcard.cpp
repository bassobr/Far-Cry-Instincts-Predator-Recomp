#include "filesystem/wildcard.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include <rex/string/text.h>

namespace rex::filesystem {

namespace {

// FCP (UPSTREAM 341): a pattern ending in ".*" also matches names without a dot, as on Windows and the
// console ("0*.*" finds the folder "00_training"). Far Cry Instincts Predator lists its levels with
// "Levels\0*.*" .. "Levels\3*.*" (level selection) and "Levels\M*.*" (multiplayer maps); the folders have no
// dot, so the lists stayed empty. The final dot becomes the DOS dot '"' (a dot or the end of the name).
// FCP_FIND_STAR_DOT_STRICT=1 restores the literal dot.
std::string Normalize(std::string_view pattern) {
  if (pattern == "*.*") {
    return "*";
  }
  std::string p(pattern);
  static const bool strict = [] {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const char* v = std::getenv("FCP_FIND_STAR_DOT_STRICT");  // read only
#pragma clang diagnostic pop
    return v && v[0] == '1';
  }();
  if (!strict && p.size() >= 2 && p.ends_with(".*")) {
    p[p.size() - 2] = '"';
  }
  return p;
}

}  // namespace

WildcardMatcher::WildcardMatcher(std::string_view pattern) : pattern_(Normalize(pattern)) {}

bool WildcardMatcher::Match(std::string_view name) const {
  if (pattern_.empty()) {
    return true;
  }
  const size_t last_dot = name.rfind('.');
  const size_t count = pattern_.size();
  std::vector<bool> active(count + 1);
  std::vector<bool> next(count + 1);
  active[0] = true;
  for (size_t n = 0;; ++n) {
    const bool at_end = n == name.size();
    const char c = at_end ? '\0' : name[n];
    for (size_t p = 0; p < count; ++p) {
      const char token = pattern_[p];
      if (active[p] && (token == '*' || token == '<' || (token == '>' && (at_end || c == '.')) ||
                        (token == '"' && at_end))) {
        active[p + 1] = true;
      }
    }
    if (at_end) {
      return active[count];
    }
    std::ranges::fill(next, false);
    for (size_t p = 0; p < count; ++p) {
      if (!active[p]) {
        continue;
      }
      switch (pattern_[p]) {
        case '*':
          next[p] = true;
          break;
        case '<':
          next[p] = next[p] || c != '.' || n != last_dot;
          break;
        case '?':
          next[p + 1] = true;
          break;
        case '>':
          next[p + 1] = next[p + 1] || c != '.';
          break;
        case '"':
          next[p + 1] = next[p + 1] || c == '.';
          break;
        default:
          next[p + 1] = next[p + 1] || string::utf8_equal_case({&pattern_[p], 1}, {&c, 1});
          break;
      }
    }
    std::swap(active, next);
  }
}

}  // namespace rex::filesystem
