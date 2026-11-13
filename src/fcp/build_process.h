// Child processes of the first-start build (codegen, compilers, linker, shader packer): all in one job object, so
// closing the job (or this process ending) ends them; their output goes to the build log.
// Adapted from src/app/game_code.cpp of Far Cry Instincts Recomp (MIT, same author).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace fcp::build {

class Processes {
 public:
  /** Appends to log (created when missing). cpus > 0: the processes run on the first cpus logical processors. */
  explicit Processes(const std::filesystem::path& log, int cpus = 0);
  ~Processes();
  Processes(const Processes&) = delete;
  Processes& operator=(const Processes&) = delete;

  /** A line in the build log. */
  void Note(const std::string& line);

  /** Starts a process (below normal priority, no console window); null when it did not start. extra_path is put
      in front of PATH for it (the folder with the SDK DLLs, for rexglue.exe). */
  void* Start(const std::wstring& command, const std::filesystem::path& dir,
              const std::filesystem::path& extra_path = {});

  /** Waits until the process ends; false when the player stopped the build (every process is ended then). */
  bool Wait(void* process);

  /** Exit code of an ended process; closes its handle. */
  static unsigned long ExitCode(void* process);

  /** CPU seconds a process has used so far. */
  static double CpuSeconds(void* process);

  /** Ends every process of the job. Returns false (for "return run.Cancel();"). */
  bool Cancel();

  const std::filesystem::path& Log() const { return log_path_; }

 private:
  void* job_ = nullptr;
  void* log_ = nullptr;
  std::filesystem::path log_path_;
  uint64_t affinity_ = 0;
};

std::wstring Quote(const std::filesystem::path& path);
std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& s);

}  // namespace fcp::build
