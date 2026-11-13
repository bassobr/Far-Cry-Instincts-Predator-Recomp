#include "fcp/keyboard_text.h"

#include <windows.h>

#include <atomic>
#include <format>
#include <string>
#include <vector>

#include <rex/ppc/context.h>

#include "fcp/env.h"
#include "fcp/guest.h"
#include "fcp/log.h"

namespace fcp::keyboard_text {
namespace {

std::atomic<uint32_t> g_calls{0};

std::vector<std::string> Entries() {
  std::vector<std::string> out;
  const std::string all = fcp::env::Get("FCP_KEYBOARD_TEXT");
  size_t at = 0;
  while (at <= all.size()) {
    const size_t bar = all.find('|', at);
    out.push_back(all.substr(at, bar == std::string::npos ? std::string::npos : bar - at));
    if (bar == std::string::npos) break;
    at = bar + 1;
  }
  return out;
}

}  // namespace

bool Enabled() { return !fcp::env::Get("FCP_KEYBOARD_TEXT").empty(); }

void OnShowKeyboard(PPCContext& ctx, uint8_t* base) {
  const uint32_t n = g_calls.fetch_add(1) + 1;
  const std::vector<std::string> entries = Entries();
  if (entries.empty()) return;
  std::string text = entries[std::min<size_t>(n - 1, entries.size() - 1)];
  for (size_t p; (p = text.find("{n}")) != std::string::npos;) text.replace(p, 3, std::to_string(n));
  const uint32_t buffer = ctx.r8.u32, length = ctx.r9.u32;  // length in characters, the terminator included
  if (length < 2 || !fcp::guest::Readable(base, buffer, size_t(length) * 2)) {
    FCPLOG("FCP_KEYBOARD_TEXT: call {}: buffer {:08X} length {} not usable, title's default kept", n, buffer, length);
    return;
  }
  const int wn = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
  std::wstring wide(size_t(wn), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), wn);
  if (wide.size() > length - 1) wide.resize(length - 1);
  uint8_t* out = fcp::guest::Ptr(base, buffer);
  for (size_t i = 0; i <= wide.size(); ++i) {  // UTF-16 big endian, with the terminator
    const uint16_t c = i < wide.size() ? uint16_t(wide[i]) : 0;
    out[2 * i] = uint8_t(c >> 8);
    out[2 * i + 1] = uint8_t(c & 0xFF);
  }
  const uint32_t old_default = ctx.r5.u32;
  ctx.r5.u64 = buffer;  // the SDK copies the default text from the buffer itself
  FCPLOG("FCP_KEYBOARD_TEXT: call {}: default text \"{}\" ({} chars) instead of the title's ({:08X})", n, text,
         wide.size(), old_default);
}

}  // namespace fcp::keyboard_text
