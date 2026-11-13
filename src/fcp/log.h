// Asynchronous log: callers only append to an in-memory queue, a dedicated thread writes the file.
// No guest thread ever waits on file I/O (all logging goes through the log thread).
#pragma once

#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace fcp::log {

/** Opens <folder>/<name> (the previous file is kept as <name>.prev) and starts the writer thread. */
bool Start(const std::filesystem::path& folder, std::string_view name);

/** <folder>/logs: dumps and traces go there. Empty before Start. */
const std::filesystem::path& Folder();

/** Appends one line (a newline is added). Thread safe, never blocks on I/O. */
void Line(std::string_view text);

/** Waits until the writer has written everything queued so far, at most timeout. */
void Drain(std::chrono::milliseconds timeout);

/** Drains, stops the writer thread and closes the file. */
void Stop();

/** Connects the SDK logger: every SDK message goes into this log as well. */
void AttachSdkLogger();

/** Lines written and dropped (queue limit) so far. */
uint64_t LinesWritten();
uint64_t LinesDropped();

template <typename... Args>
void Info(std::format_string<Args...> fmt, Args&&... args) {
  Line(std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace fcp::log

#define FCPLOG(...) ::fcp::log::Info(__VA_ARGS__)
