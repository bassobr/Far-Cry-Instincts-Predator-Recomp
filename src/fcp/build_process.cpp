// Child processes of the first-start build (fcp/build_process.h).
// Adapted from src/app/game_code.cpp of Far Cry Instincts Recomp (MIT, same author).
#include "fcp/build_process.h"

#include <windows.h>

#include <vector>

#include "fcp/log.h"
#include "fcp/progress_window.h"

namespace fcp::build {

std::wstring Quote(const std::filesystem::path& path) { return L"\"" + path.wstring() + L"\""; }

std::wstring Widen(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::string Narrow(const std::wstring& w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

Processes::Processes(const std::filesystem::path& log, int cpus) : log_path_(log) {
  job_ = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
  limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (cpus > 0) {
    // FCP_GAME_CODE_CPUS: the build's processes only on the first n logical processors (measuring smaller PCs)
    affinity_ = cpus >= 64 ? ~0ull : ((1ull << cpus) - 1);
    limit.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_AFFINITY;
    limit.BasicLimitInformation.Affinity = ULONG_PTR(affinity_);
  }
  SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
  std::error_code ec;
  std::filesystem::create_directories(log.parent_path(), ec);
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE h = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  log_ = h == INVALID_HANDLE_VALUE ? nullptr : h;
}

Processes::~Processes() {
  if (job_) CloseHandle(job_);
  if (log_) CloseHandle(log_);
}

void Processes::Note(const std::string& line) {
  DWORD n = 0;
  const std::string text = line + "\r\n";
  if (log_) WriteFile(log_, text.data(), DWORD(text.size()), &n, nullptr);
}

void* Processes::Start(const std::wstring& command, const std::filesystem::path& dir,
                       const std::filesystem::path& extra_path) {
  Note(Narrow(command));
  STARTUPINFOW si{sizeof(si)};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nullptr;
  si.hStdOutput = log_;
  si.hStdError = log_;
  PROCESS_INFORMATION pi{};
  // DETACHED_PROCESS: no console host for every compiler (the output goes to the log handle anyway).
  const DWORD flags = DETACHED_PROCESS | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS | CREATE_UNICODE_ENVIRONMENT;
  std::vector<wchar_t> env;
  if (!extra_path.empty()) {
    // this process's environment with PATH = extra_path;PATH
    const wchar_t* block = GetEnvironmentStringsW();
    bool path_done = false;
    for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
      std::wstring entry = p;
      if (_wcsnicmp(p, L"PATH=", 5) == 0) {
        entry = L"PATH=" + extra_path.wstring() + L";" + entry.substr(5);
        path_done = true;
      }
      env.insert(env.end(), entry.begin(), entry.end());
      env.push_back(0);
    }
    if (!path_done) {
      const std::wstring entry = L"PATH=" + extra_path.wstring();
      env.insert(env.end(), entry.begin(), entry.end());
      env.push_back(0);
    }
    env.push_back(0);
    FreeEnvironmentStringsW(const_cast<wchar_t*>(block));
  }
  std::wstring cmd = command;
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, flags, env.empty() ? nullptr : env.data(),
                      dir.c_str(), &si, &pi)) {
    FCPLOG("game code: could not start {} (error {})", Narrow(command), GetLastError());
    return nullptr;
  }
  if (!AssignProcessToJobObject(job_, pi.hProcess)) {
    FCPLOG("game code: a build process is not in the job (error {}); it is ended by hand if the build stops",
           GetLastError());
  }
  if (affinity_) SetProcessAffinityMask(pi.hProcess, DWORD_PTR(affinity_));
  if (ResumeThread(pi.hThread) == DWORD(-1)) {  // a process that never runs would hold the build forever
    FCPLOG("game code: could not resume {} (error {})", Narrow(command), GetLastError());
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return nullptr;
  }
  CloseHandle(pi.hThread);
  return pi.hProcess;
}

bool Processes::Wait(void* process) {
  while (WaitForSingleObject(process, 250) == WAIT_TIMEOUT) {
    if (fcp::progress::Cancelled()) return Cancel();
  }
  return true;
}

unsigned long Processes::ExitCode(void* process) {
  DWORD code = 1;
  GetExitCodeProcess(process, &code);
  CloseHandle(process);
  return code;
}

double Processes::CpuSeconds(void* process) {
  FILETIME created, ended, kernel, user;
  if (!GetProcessTimes(process, &created, &ended, &kernel, &user)) return 0;
  auto s = [](const FILETIME& f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7; };
  return s(kernel) + s(user);
}

bool Processes::Cancel() {
  TerminateJobObject(job_, 1);
  return false;
}

}  // namespace fcp::build
