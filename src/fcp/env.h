// FCP_* switches. Read once at start (never per call); every boot deviation from the original has one.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fcp::env {

/** Raw value of an environment variable, empty when unset. */
std::string Get(const char* name);

/** True for a set value other than "", "0", "false", "off". */
bool Flag(const char* name, bool fallback = false);

/** Integer value, fallback when unset or unreadable. */
int64_t Int(const char* name, int64_t fallback);

/** All FCP_* variables of the process, name and value, sorted by name. */
std::vector<std::pair<std::string, std::string>> AllFcp();

}  // namespace fcp::env
