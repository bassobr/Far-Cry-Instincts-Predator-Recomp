#include "fcp/env.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include <windows.h>

namespace fcp::env {

std::string Get(const char* name) {
  char buffer[4096];
  const DWORD n = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
  if (n == 0 || n >= sizeof(buffer)) {
    return {};
  }
  return std::string(buffer, n);
}

bool Flag(const char* name, bool fallback) {
  std::string v = Get(name);
  if (v.empty()) {
    return fallback;
  }
  std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return !(v == "0" || v == "false" || v == "off" || v == "no");
}

int64_t Int(const char* name, int64_t fallback) {
  const std::string v = Get(name);
  if (v.empty()) {
    return fallback;
  }
  char* end = nullptr;
  const long long parsed = std::strtoll(v.c_str(), &end, 0);
  return end && end != v.c_str() ? parsed : fallback;
}

std::vector<std::pair<std::string, std::string>> AllFcp() {
  std::vector<std::pair<std::string, std::string>> out;
  LPCH block = GetEnvironmentStringsA();
  if (!block) {
    return out;
  }
  for (const char* p = block; *p; p += std::strlen(p) + 1) {
    std::string_view entry(p);
    if (entry.size() > 4 && (entry.starts_with("FCP_") || entry.starts_with("REX_"))) {
      const size_t eq = entry.find('=');
      if (eq != std::string_view::npos) {
        out.emplace_back(std::string(entry.substr(0, eq)), std::string(entry.substr(eq + 1)));
      }
    }
  }
  FreeEnvironmentStringsA(block);
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace fcp::env
