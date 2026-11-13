// fcp/toml_edit.h. Own code of this project (the line walk of user_settings.cpp's former StoreMode, generalised).
#include "fcp/toml_edit.h"

#include <windows.h>

#include <cctype>
#include <format>
#include <fstream>
#include <mutex>
#include <vector>

namespace fcp::toml {
namespace {

std::mutex g_mutex;

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Trim(std::string_view s) {
  const size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
  return b == std::string_view::npos ? std::string() : std::string(s.substr(b, e - b + 1));
}

// Up to a '#' outside quotes.
size_t CodeEnd(const std::string& line) {
  char quote = 0;
  for (size_t i = 0; i < line.size(); ++i) {
    if ((line[i] == '"' || line[i] == '\'') && (quote == 0 || quote == line[i])) quote = quote ? 0 : line[i];
    if (line[i] == '#' && !quote) return i;
  }
  return line.size();
}

}  // namespace

std::string Quote(std::string_view text) { return std::format("\"{}\"", text); }

bool SetValue(const std::filesystem::path& file, std::string_view section, std::string_view key,
              std::string_view literal) {
  std::lock_guard lock(g_mutex);
  std::vector<std::string> lines;
  bool bom = false;
  {
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(std::move(line));
    }
    if (!lines.empty() && lines[0].starts_with("\xEF\xBB\xBF")) {
      lines[0].erase(0, 3);
      bom = true;
    }
  }
  const std::string want_section = Lower(std::string(section)), want_key = Lower(std::string(key));
  std::string current;
  int insert_at = -1;  // after the section's last value line
  bool done = false;
  for (size_t i = 0; i < lines.size() && !done; ++i) {
    const size_t code_end = CodeEnd(lines[i]);
    const std::string t = Trim(std::string_view(lines[i]).substr(0, code_end));
    if (t.empty()) continue;
    if (t.front() == '[' && t.back() == ']') {
      current = Lower(Trim(std::string_view(t).substr(1, t.size() - 2)));
      if (current == want_section) insert_at = int(i) + 1;
      continue;
    }
    if (current != want_section) continue;
    insert_at = int(i) + 1;
    const size_t eq = t.find('=');
    if (eq == std::string::npos || Lower(Trim(std::string_view(t).substr(0, eq))) != want_key) continue;
    // keep the comment after the value, aligned as before when the new value is not longer
    std::string head = std::format("{} = {}", Trim(std::string_view(t).substr(0, eq)), literal);
    if (code_end < lines[i].size()) {
      const std::string old_code = lines[i].substr(0, code_end);
      const size_t pad = old_code.size() > head.size() + 1 ? old_code.size() - head.size() : 2;
      head += std::string(pad, ' ') + lines[i].substr(code_end);
    }
    lines[i] = head;
    done = true;
  }
  if (!done) {
    const std::string line = std::format("{} = {}", key, literal);
    if (insert_at < 0) {
      if (!lines.empty() && !Trim(lines.back()).empty()) lines.push_back("");
      lines.push_back(std::format("[{}]", section));
      lines.push_back(line);
    } else {
      lines.insert(lines.begin() + insert_at, line);
    }
  }
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  std::filesystem::path tmp = file;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (bom) out << "\xEF\xBB\xBF";
    for (const std::string& l : lines) out << l << "\n";
    if (!out) return false;
  }
  return MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

}  // namespace fcp::toml
