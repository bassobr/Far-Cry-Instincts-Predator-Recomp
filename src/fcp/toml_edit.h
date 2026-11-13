// One value of a small TOML file changed in place (predator.toml, predator_input.toml): every other line stays,
// comments too, also the comment after the value. Written over a .tmp file and MoveFileEx (as Alt+Enter did, the
// idea from FCI/FCE). Own code of this project.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace fcp::toml {

/**
 * Sets `key = <literal>` in [section] (names without case). The literal is written as given (quote strings
 * yourself: Quote). A missing key goes after the section's last value, a missing section to the end of the file;
 * a missing file is created. Thread-safe within this process.
 */
bool SetValue(const std::filesystem::path& file, std::string_view section, std::string_view key,
              std::string_view literal);

/** "text" for a TOML string (no escapes inside: the readers take it as written, Windows paths included). */
std::string Quote(std::string_view text);

}  // namespace fcp::toml
