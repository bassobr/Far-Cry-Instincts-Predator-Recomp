/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <thread>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>

#include <format>

#include <rex/filesystem/mount.h>
#include <rex/filesystem/file_system.h>
#include <rex/filesystem/stfs.h>
#include <rex/kernel/types/content.h>
#include <rex/logging/log_category.h>
#include <rex/platform/file.h>
#include <rex/kernel/module.h>
#include <rex/string/text.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/object/object_pin.h>
#include <rex/kernel/object/object_table.h>
#include <rex/xam/app_manager.h>
#include <rex/xam/device.h>
#include <rex/xam/user_profile.h>
#include <rex/kernel/object/file_object.h>
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/object/object.h>

#include "xam/content_manager.h"
#include <rex/kernel/types/xex.h>
#include <rex/xam/title_lifecycle.h>
#include <rex/filesystem/file_system_factory.h>

namespace rex::xam {

ContentPackage::ContentPackage(filesystem::IFileSystem& file_system, u32 device_index,
                               const std::string_view root_name,
                               const kernel::XCONTENT_AGGREGATE_DATA& data,
                               const std::filesystem::path& package_path)
    : file_system_(file_system), root_name_(root_name), package_path_(package_path), license_(0) {
  device_path_ = std::format("\\Device\\Content\\{0}\\", device_index);
  content_data_ = data;

  auto* fs = &file_system_;
  std::error_code ec;
  const bool folder = std::filesystem::is_directory(package_path, ec);
  filesystem::MountEntry entry;
  entry.path = device_path_;
  entry.type = folder ? filesystem::ProviderType::kHostDirectory : filesystem::ProviderType::kSTFS;
  entry.source = package_path;
  entry.flags = folder ? filesystem::MountFlags::kWritable : filesystem::MountFlags::kNone;
  if (auto mounted = fs->Mount(entry); !mounted) {
    REXSYS_WARN("content package '{}' is not mounted, {}", rex::string::PathToUTF8(package_path),
                mounted.error().message);
  }
  if (auto linked = fs->Link(root_name_ + ":", device_path_); !linked) {
    REXSYS_WARN("content root '{}:' is not linked, {}", root_name_, linked.error().message);
  }
}

ContentPackage::~ContentPackage() {
  log::NoThrow(log::system, [this] {
    auto* fs = &file_system_;
    if (auto unlinked = fs->Unlink(root_name_ + ":"); !unlinked) {
      REXSYS_DEBUG("{}", unlinked.error().message);
    }
    if (auto unmounted = fs->Unmount(device_path_); !unmounted) {
      REXSYS_DEBUG("{}", unmounted.error().message);
    }
  });
}

void ContentPackage::LoadPackageLicenseMask(const std::filesystem::path& header_path) {
  if (!std::filesystem::exists(header_path)) {
    return;
  }

  std::ifstream file(header_path, std::ios::binary);
  if (!file) {
    return;
  }

  std::error_code ec;
  auto file_size = std::filesystem::file_size(header_path, ec);
  if (ec || file_size < sizeof(kernel::XCONTENT_AGGREGATE_DATA) + sizeof(license_)) {
    return;
  }

  file.seekg(sizeof(kernel::XCONTENT_AGGREGATE_DATA));
  file.read(reinterpret_cast<char*>(&license_), sizeof(license_));
}

namespace detail {

bool IsReplacementFolderName(std::string_view name) {
  const auto ends = [name](std::string_view suffix) {
    return name.size() > suffix.size() &&
           rex::string::utf8_equal_case(name.substr(name.size() - suffix.size()), suffix);
  };
  return ends(kStagingSuffix) || ends(kReadySuffix) || ends(kPreviousSuffix);
}

bool AtomicReplaceEnabled() {
  static const bool enabled = [] {
    const auto off = [](const char* name) {
      const std::optional<std::string> v = rex::platform::GetEnvironmentValue(name);
      return v && !v->empty() && (*v)[0] == '0';
    };
    return !off("FCP_SAVE_ATOMIC") && !off("FCP_SAVE_GUARD");
  }();
  return enabled;
}

}  // namespace detail

namespace {

constexpr std::string_view kGameUserContentDirName = "profile";

std::filesystem::path WithSuffix(std::filesystem::path path, std::string_view suffix) {
  path += rex::string::to_path(suffix);
  return path;
}

// FCP: change 330: a rename that a short lived handle (a virus scanner, the indexer) may block for a moment.
bool RenameRetry(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    ec.clear();
    std::filesystem::rename(from, to, ec);
    if (!ec) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

// FCP: change 330: <final>.saving (closed) becomes <final>; see detail::kStagingSuffix.
Result<void> CommitReplacement(const std::filesystem::path& staging, const std::filesystem::path& final_path) {
  const std::filesystem::path ready = WithSuffix(final_path, detail::kReadySuffix);
  const std::filesystem::path old = WithSuffix(final_path, detail::kPreviousSuffix);
  std::error_code ec;
  std::filesystem::remove_all(ready, ec);
  if (!RenameRetry(staging, ready, ec)) {
    return Err(ErrorCategory::kIO, std::format("cannot rename '{}' to '{}': {}", rex::string::PathToUTF8(staging),
                                               rex::string::PathToUTF8(ready), ec.message()));
  }
  std::filesystem::remove_all(old, ec);
  if (std::filesystem::exists(final_path, ec) && !RenameRetry(final_path, old, ec)) {
    return Err(ErrorCategory::kIO,
               std::format("cannot move the previous '{}' aside: {} (the new content waits in '{}')",
                           rex::string::PathToUTF8(final_path), ec.message(), rex::string::PathToUTF8(ready)));
  }
  if (!RenameRetry(ready, final_path, ec)) {
    std::error_code back;
    std::filesystem::rename(old, final_path, back);
    return Err(ErrorCategory::kIO, std::format("cannot rename '{}' to '{}': {}", rex::string::PathToUTF8(ready),
                                               rex::string::PathToUTF8(final_path), ec.message()));
  }
  std::filesystem::remove_all(old, ec);
  return Ok();
}

}  // namespace

ContentManager::ContentManager(const XAMTitle& title, rex::xam::UserProfile& user_profile)
    : title_(title), root_path_(title.user_data_root), user_profile_(user_profile) {}

ContentManager::~ContentManager() = default;

std::filesystem::path ContentManager::ResolvePackageRoot(u64 xuid,
                                                         rex::kernel::XContentType content_type,
                                                         u32 title_id) const {
  if (title_id == kernel::kRunningTitleID) {
    title_id = this->GetTitleID();
  }
  auto xuid_str = std::format("{:016X}", xuid);
  auto title_id_str = std::format("{:08X}", title_id);
  auto content_type_str = std::format("{:08X}", static_cast<u32>(content_type));

  // Layout is content_root/xuid/title_id/content_type/
  return detail::ResolveChain(root_path_, xuid_str, title_id_str, content_type_str);
}

std::filesystem::path ContentManager::ResolvePackagePath(
    u64 xuid, const kernel::XCONTENT_AGGREGATE_DATA& data) const {
  u64 used_xuid =
      (data.xuid != static_cast<u64>(-1) && data.xuid != 0) ? static_cast<u64>(data.xuid) : xuid;

  if (data.content_type == rex::kernel::XContentType::kMarketplaceContent) {
    used_xuid = 0;
  }

  auto package_root = ResolvePackageRoot(used_xuid, data.content_type, data.title_id);
  return rex::platform::ResolveEntryCase(package_root, data.GetFileName());
}

std::filesystem::path ContentManager::ResolvePackageHeaderPath(
    const std::string_view file_name, u64 xuid, u32 title_id,
    rex::kernel::XContentType content_type) const {
  if (title_id == kernel::kRunningTitleID) {
    title_id = this->GetTitleID();
  }

  if (content_type == rex::kernel::XContentType::kMarketplaceContent) {
    xuid = 0;
  }

  auto xuid_str = std::format("{:016X}", xuid);
  auto title_id_str = std::format("{:08X}", title_id);
  auto content_type_str = std::format("{:08X}", static_cast<u32>(content_type));
  std::string final_name = std::string(file_name) + ".header";

  // Layout is content_root/xuid/title_id/Headers/content_type/filename.header
  return detail::ResolveChain(root_path_, xuid_str, title_id_str, detail::kGameContentHeaderDirName,
                              content_type_str, final_name);
}

std::filesystem::path ContentManager::ResolveHeaderPathFor(
    u64 xuid, const kernel::XCONTENT_AGGREGATE_DATA& data) const {
  const u64 used_xuid =
      (data.xuid != static_cast<u64>(-1) && data.xuid != 0) ? static_cast<u64>(data.xuid) : xuid;
  return ResolvePackageHeaderPath(data.GetFileName(), used_xuid, data.title_id, data.content_type);
}

std::vector<kernel::XCONTENT_AGGREGATE_DATA> ContentManager::ListContent(
    u32 device_id, u64 xuid, rex::kernel::XContentType content_type, u32 title_id) {
  std::vector<kernel::XCONTENT_AGGREGATE_DATA> result;

  if (title_id == kernel::kRunningTitleID) {
    title_id = this->GetTitleID();
  }

  auto package_root = ResolvePackageRoot(xuid, content_type, title_id);
  std::error_code ec;
  for (const auto& child : std::filesystem::directory_iterator(package_root, ec)) {
    if (!child.is_directory(ec)) {
      continue;
    }
    const std::filesystem::path name = child.path().filename();
    if (detail::IsReplacementFolderName(rex::string::PathToUTF8(name))) {
      continue;  // FCP: change 330, a replacement in progress or cut, no content of its own
    }
    if (!IsContentEnabled(title_id, content_type, rex::string::PathToUTF8(name))) {
      continue;
    }

    kernel::XCONTENT_AGGREGATE_DATA content_data{};
    if (auto header =
            ReadContentHeaderFile(rex::string::PathToUTF8(name), xuid, title_id, content_type)) {
      result.emplace_back(*header);
    } else {
      content_data.device_id = device_id;
      content_data.content_type = content_type;
      content_data.SetDisplayName(rex::string::PathToUTF16(name));
      content_data.SetFileName(rex::string::PathToUTF8(name));
      content_data.title_id = title_id;
      content_data.xuid = xuid;
      result.emplace_back(content_data);
    }
  }

  return result;
}

std::unique_ptr<ContentPackage> ContentManager::ResolvePackage(
    const std::string_view root_name, u64 xuid, const kernel::XCONTENT_AGGREGATE_DATA& data) {
  auto package_path = ResolvePackagePath(xuid, data);
  if (!std::filesystem::exists(package_path)) {
    return nullptr;
  }
  if (const std::optional<std::filesystem::path> source = FindInPlaceSource(xuid, data)) {
    if (!std::filesystem::exists(*source)) {
      return nullptr;
    }
    package_path = *source;
  }
  auto package = std::make_unique<ContentPackage>(title_.file_system, ++device_count_, root_name,
                                                  data, package_path);
  return package;
}

bool ContentManager::HasContent(u64 xuid, const kernel::XCONTENT_AGGREGATE_DATA& data) {
  const u32 title_id =
      data.title_id == kernel::kRunningTitleID ? GetTitleID() : static_cast<u32>(data.title_id);
  if (!IsContentEnabled(title_id, data.content_type, data.GetFileName())) {
    return false;
  }
  auto path = ResolvePackagePath(xuid, data);
  return std::filesystem::exists(path);
}

Result<void> ContentManager::WriteContentHeaderFile(u64 xuid, kernel::XCONTENT_AGGREGATE_DATA data,
                                                    u32 license_mask) {
  if (data.title_id == static_cast<u32>(-1)) {
    data.title_id = this->GetTitleID();
  }
  if (data.xuid == static_cast<u64>(-1)) {
    data.xuid = xuid;
  }
  auto header_path = ResolveHeaderPathFor(xuid, data);
  auto parent_path = header_path.parent_path();

  if (!std::filesystem::exists(parent_path)) {
    if (!std::filesystem::create_directories(parent_path)) {
      return Err(kernel::XResultError(kernel::X_ERROR_ACCESS_DENIED));
    }
  }

  std::vector<u8> header(sizeof(kernel::XCONTENT_AGGREGATE_DATA) +
                         (license_mask != 0 ? sizeof(license_mask) : 0));
  std::memcpy(header.data(), &data, sizeof(kernel::XCONTENT_AGGREGATE_DATA));
  if (license_mask != 0) {
    std::memcpy(header.data() + sizeof(kernel::XCONTENT_AGGREGATE_DATA), &license_mask,
                sizeof(license_mask));
  }
  if (!platform::WriteHostFile(header_path, header)) {
    return Err(kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
  }
  return Ok();
}

Result<kernel::XCONTENT_AGGREGATE_DATA> ContentManager::ReadContentHeaderFile(
    const std::string_view file_name, u64 xuid, u32 title_id,
    rex::kernel::XContentType content_type) const {
  auto header_file_path = ResolvePackageHeaderPath(file_name, xuid, title_id, content_type);
  constexpr u32 kHeaderSize = sizeof(kernel::XCONTENT_AGGREGATE_DATA);

  if (!std::filesystem::exists(header_file_path)) {
    return Err<kernel::XCONTENT_AGGREGATE_DATA>(
        kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
  }

  std::ifstream file(header_file_path, std::ios::binary);
  if (!file) {
    return Err<kernel::XCONTENT_AGGREGATE_DATA>(
        kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
  }

  std::array<u8, kHeaderSize> buffer;
  if (!file.read(reinterpret_cast<char*>(buffer.data()), kHeaderSize)) {
    return Err<kernel::XCONTENT_AGGREGATE_DATA>(
        kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
  }

  kernel::XCONTENT_AGGREGATE_DATA data;
  std::memcpy(&data, buffer.data(), buffer.size());
  return data;
}

Result<void> ContentManager::CreateContent(const std::string_view root_name, u64 xuid,
                                           const kernel::XCONTENT_AGGREGATE_DATA& data) {
  {
    std::lock_guard lock(mutex_);
    if (open_packages_.count(string::StringKeyIgnoreCase(root_name))) {
      return Err(kernel::XResultError(kernel::X_ERROR_ALREADY_EXISTS));
    }
  }

  auto package_path = ResolvePackagePath(xuid, data);
  if (std::filesystem::exists(package_path)) {
    return Err(kernel::XResultError(kernel::X_ERROR_ALREADY_EXISTS));
  }
  if (!std::filesystem::create_directories(package_path)) {
    return Err(kernel::XResultError(kernel::X_ERROR_ACCESS_DENIED));
  }
  auto package = ResolvePackage(root_name, xuid, data);
  REX_ASSERT(package != nullptr);

  {
    std::lock_guard lock(mutex_);
    if (open_packages_.count(string::StringKeyIgnoreCase(root_name))) {
      return Err(kernel::XResultError(kernel::X_ERROR_ALREADY_EXISTS));
    }
    open_packages_.emplace(string::StringKeyIgnoreCase::Create(root_name), std::move(package));
  }
  return Ok();
}

Result<void> ContentManager::ReplaceContent(const std::string_view root_name, u64 xuid,
                                            const kernel::XCONTENT_AGGREGATE_DATA& data) {
  {
    std::lock_guard lock(mutex_);
    if (open_packages_.count(string::StringKeyIgnoreCase(root_name))) {
      return Err(kernel::XResultError(kernel::X_ERROR_ALREADY_EXISTS));
    }
    if (IsContentOpen(data)) {
      return Err(kernel::XResultError(kernel::X_ERROR_ACCESS_DENIED));
    }
  }
  const std::filesystem::path final_path = ResolvePackagePath(xuid, data);
  std::error_code ec;
  if (!std::filesystem::is_directory(final_path, ec) || FindInPlaceSource(xuid, data)) {
    return Err(ErrorCategory::kNotImplemented,
               std::format("'{}' is no host folder package", rex::string::PathToUTF8(final_path)));
  }
  const std::filesystem::path staging = WithSuffix(final_path, detail::kStagingSuffix);
  if (std::filesystem::exists(staging, ec)) {
    // left by a process that ended in the middle of a replacement after the start check (the check moves such a
    // folder aside): its content never was complete
    REXSYS_WARN("content: '{}' from an unfinished replacement is removed", rex::string::PathToUTF8(staging));
    std::filesystem::remove_all(staging, ec);
  }
  if (!std::filesystem::create_directories(staging, ec)) {
    return Err(kernel::XResultError(kernel::X_ERROR_ACCESS_DENIED));
  }
  auto package = std::make_unique<ContentPackage>(title_.file_system, ++device_count_, root_name, data, staging);
  package->SetReplaces(final_path);
  {
    std::lock_guard lock(mutex_);
    if (open_packages_.count(string::StringKeyIgnoreCase(root_name))) {
      return Err(kernel::XResultError(kernel::X_ERROR_ALREADY_EXISTS));
    }
    open_packages_.emplace(string::StringKeyIgnoreCase::Create(root_name), std::move(package));
  }
  return Ok();
}

std::filesystem::path ContentManager::WritablePackagePath(u64 xuid,
                                                          const kernel::XCONTENT_AGGREGATE_DATA& data) const {
  for (const auto& [name, package] : open_packages_) {
    if (!package->GetReplaces().empty() && package->GetPackageContentData() == data) {
      return package->GetPackagePath();
    }
  }
  return ResolvePackagePath(xuid, data);
}

Result<u32> ContentManager::OpenContent(const std::string_view root_name, u64 xuid,
                                        const kernel::XCONTENT_AGGREGATE_DATA& data) {
  {
    std::lock_guard lock(mutex_);
    if (open_packages_.count(string::StringKeyIgnoreCase(root_name))) {
      return Err<u32>(kernel::XResultError(kernel::X_ERROR_ALREADY_EXISTS));
    }
  }

  const u32 title_id =
      data.title_id == kernel::kRunningTitleID ? GetTitleID() : static_cast<u32>(data.title_id);
  auto package_path = ResolvePackagePath(xuid, data);
  if (!std::filesystem::exists(package_path) ||
      !IsContentEnabled(title_id, data.content_type, data.GetFileName())) {
    return Err<u32>(kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
  }
  auto package = ResolvePackage(root_name, xuid, data);
  if (!package) {
    return Err<u32>(kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
  }
  package->LoadPackageLicenseMask(ResolveHeaderPathFor(xuid, data));
  const u32 license = package->GetPackageLicense();

  {
    std::lock_guard lock(mutex_);
    if (open_packages_.count(string::StringKeyIgnoreCase(root_name))) {
      return Err<u32>(kernel::XResultError(kernel::X_ERROR_ALREADY_EXISTS));
    }
    open_packages_.emplace(string::StringKeyIgnoreCase::Create(root_name), std::move(package));
  }
  return license;
}

Result<void> ContentManager::CloseContent(const std::string_view root_name) {
  std::string open_root_name;
  {
    std::lock_guard lock(mutex_);
    auto it = open_packages_.find(string::StringKeyIgnoreCase(root_name));
    if (it == open_packages_.end()) {
      return Err(kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
    }
    open_root_name = it->first.GetView();
  }
  CloseOpenedFilesFromContent(open_root_name);
  std::unique_ptr<ContentPackage> package = DetachPackage(open_root_name);
  if (package && !package->GetReplaces().empty()) {
    // FCP: change 330: unmount first (the folder is renamed), then the new content takes the old one's place
    const std::filesystem::path staging = package->GetPackagePath();
    const std::filesystem::path final_path = package->GetReplaces();
    package.reset();
    if (auto committed = CommitReplacement(staging, final_path); !committed) {
      REXSYS_WARN("content: replacing '{}' failed, {}", rex::string::PathToUTF8(final_path),
                  committed.error().message);
      return Err(kernel::XResultError(kernel::X_ERROR_ACCESS_DENIED));
    }
    REXSYS_DEBUG("content: '{}' replaced", rex::string::PathToUTF8(final_path));
  }
  return Ok();
}

Result<std::vector<u8>> ContentManager::GetContentThumbnail(
    u64 xuid, const kernel::XCONTENT_AGGREGATE_DATA& data) {
  auto package_path = ResolvePackagePath(xuid, data);
  auto thumb_path = package_path / detail::kThumbnailFileName;
  std::lock_guard lock(mutex_);
  if (std::filesystem::exists(thumb_path)) {
    return platform::ReadHostFile(thumb_path);
  } else {
    return Err<std::vector<u8>>(kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
  }
}

Result<void> ContentManager::SetContentThumbnail(u64 xuid,
                                                 const kernel::XCONTENT_AGGREGATE_DATA& data,
                                                 std::vector<u8> buffer) {
  std::lock_guard lock(mutex_);
  auto package_path = WritablePackagePath(xuid, data);  // FCP: change 330: into a replacement while it is open
  std::filesystem::create_directories(package_path);
  if (std::filesystem::exists(package_path)) {
    auto thumb_path = package_path / detail::kThumbnailFileName;
    if (!platform::WriteHostFile(thumb_path, buffer)) {
      return Err(kernel::XResultError(kernel::X_ERROR_ACCESS_DENIED));
    }
    return Ok();
  } else {
    return Err(kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
  }
}

Result<void> ContentManager::DeleteContent(u64 xuid, const kernel::XCONTENT_AGGREGATE_DATA& data) {
  auto package_path = ResolvePackagePath(xuid, data);
  u64 used_xuid =
      (data.xuid != static_cast<u64>(-1) && data.xuid != 0) ? static_cast<u64>(data.xuid) : xuid;
  auto header_path =
      ResolvePackageHeaderPath(data.GetFileName(), used_xuid, data.title_id, data.content_type);
  const std::filesystem::path manifest_path = ResolveManifestPath(used_xuid, data);
  const u32 title_id =
      data.title_id == kernel::kRunningTitleID ? GetTitleID() : static_cast<u32>(data.title_id);

  {
    std::lock_guard lock(mutex_);

    if (IsContentOpen(data)) {
      return Err(kernel::XResultError(kernel::X_ERROR_ACCESS_DENIED));
    }

    std::error_code ec;
    auto dir_removed = std::filesystem::remove_all(package_path, ec);
    if (ec ||
        std::filesystem::status(package_path, ec).type() != std::filesystem::file_type::not_found) {
      return Err(kernel::XResultError(kernel::X_ERROR_ACCESS_DENIED));
    }

    std::error_code ec2;
    // FCP: change 330: what a cut replacement of this package left
    for (std::string_view suffix : {detail::kStagingSuffix, detail::kReadySuffix, detail::kPreviousSuffix}) {
      std::filesystem::remove_all(WithSuffix(package_path, suffix), ec2);
    }
    bool header_removed = std::filesystem::remove(header_path, ec2);
    std::filesystem::remove(manifest_path, ec2);

    std::vector<EnableEntry>& entries = LoadEnableEntriesLocked();
    const std::string file_name = data.GetFileName();
    if (std::erase_if(entries, [&](const EnableEntry& entry) {
          return entry.title_id == title_id && entry.content_type == data.content_type &&
                 entry.file_name == file_name;
        }) != 0) {
      SaveEnableEntriesLocked();
    }

    if (dir_removed == 0 && !header_removed) {
      return Err(kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
    }
  }
  NotifyContentChanged();
  return Ok();
}

std::filesystem::path ContentManager::ResolveGameUserContentPath() {
  auto title_id = std::format("{:08X}", this->GetTitleID());
  auto user_name = rex::string::to_path(user_profile_.GetName());

  return root_path_ / title_id / kGameUserContentDirName / user_name;
}

std::unique_ptr<ContentPackage> ContentManager::DetachPackage(const std::string_view root_name) {
  std::lock_guard lock(mutex_);
  auto it = open_packages_.find(string::StringKeyIgnoreCase(root_name));
  if (it == open_packages_.end()) {
    return nullptr;
  }
  std::unique_ptr<ContentPackage> package = std::move(it->second);
  open_packages_.erase(it);
  return package;
}

bool ContentManager::IsContentOpen(const kernel::XCONTENT_AGGREGATE_DATA& data) const {
  return std::any_of(open_packages_.cbegin(), open_packages_.cend(), [&data](const auto& content) {
    return data == content.second->GetPackageContentData();
  });
}

void ContentManager::CloseOpenedFilesFromContent(const std::string_view root_name) {
  const std::optional<std::string> resolved_path =
      title_.file_system.FindLink(std::string(root_name) + ':');
  if (!resolved_path || resolved_path->empty()) {
    return;
  }

  const auto is_separator = [](char c) { return c == '\\' || c == '/'; };
  const std::string_view root = *resolved_path;
  const std::vector<kernel::Pinned<kernel::IOpenedFile>> all_files_handles =
      title_.objects.All<kernel::IOpenedFile>();
  for (const kernel::Pinned<kernel::IOpenedFile>& file : all_files_handles) {
    const std::string file_path = file->GetAbsolutePath();
    if (!rex::string::utf8_starts_with_case(file_path, root)) {
      continue;
    }
    const std::string_view rest = std::string_view(file_path).substr(root.size());
    if (rest.empty() || is_separator(rest.front()) || is_separator(root.back())) {
      if (file.GetHandle()) {
        kernel::CloseOwned(title_.objects, file.GetHandle());
      }
    }
  }
}

Result<u32> ContentManager::GetContentDeviceID(const std::string_view root_name) const {
  std::lock_guard lock(mutex_);
  auto it = open_packages_.find(string::StringKeyIgnoreCase(root_name));
  if (it == open_packages_.end()) {
    return Err<u32>(kernel::XResultError(kernel::X_ERROR_FILE_NOT_FOUND));
  }
  return it->second->GetPackageContentData().device_id;
}

u32 ContentManager::GetTitleID() const {
  auto module =
      title_.objects.Get<kernel::ILoadedModule>(title_.modules.GetExecutable().value_or(0));
  REX_ASSERT(module);

  kernel::XEXOptExecutionInfo* exec_info = 0;
  module->OptHeader(kernel::kXEXHeaderExecutionInfo, &exec_info);

  if (exec_info) {
    return exec_info->title_id;
  }

  return 0;
}

std::unique_ptr<rex::filesystem::IXDBFTitleData> ContentManager::GetTitleXDBF() const {
  auto module =
      title_.objects.Get<kernel::ILoadedModule>(title_.modules.GetExecutable().value_or(0));
  REX_ASSERT(module);
  return GetModuleXDBF(*module);
}

std::unique_ptr<rex::filesystem::IXDBFTitleData> ContentManager::GetModuleXDBF(
    const kernel::ILoadedModule& exec_module) const {
  u32 resource_data = 0;
  u32 resource_size = 0;
  if (kernel::XSUCCEEDED(exec_module.GetSection(std::format("{:08X}", exec_module.GetTitleID()),
                                                &resource_data, &resource_size))) {
    return title_.file_system_factory.OpenXDBF(title_.membase.TranslateVirtual(resource_data),
                                               resource_size);
  }
  return title_.file_system_factory.OpenXDBF(nullptr, resource_size);
}

}  // namespace rex::xam
