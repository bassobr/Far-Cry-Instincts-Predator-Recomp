// SHA-256 and SHA-1 over the Windows CNG (bcrypt.dll): file checks of the first start (disc image, game code).
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace fcp::hash {

class Hasher {
 public:
  enum class Kind { kSha256, kSha1 };
  explicit Hasher(Kind kind = Kind::kSha256);
  ~Hasher();
  Hasher(const Hasher&) = delete;
  Hasher& operator=(const Hasher&) = delete;
  void Update(const void* data, size_t size);
  void Update(std::string_view text) { Update(text.data(), text.size()); }
  /** Upper case hex of the digest; the hasher cannot be used afterwards. */
  std::string HexDigest();

 private:
  void* alg_ = nullptr;
  void* hash_ = nullptr;
  size_t length_ = 0;
};

std::string Sha256(const void* data, size_t size);
inline std::string Sha256(std::string_view text) { return Sha256(text.data(), text.size()); }
std::string Sha1(const void* data, size_t size);
/** SHA-256 of a file's content, empty when it cannot be read. */
std::string Sha256File(const std::filesystem::path& path);

}  // namespace fcp::hash
