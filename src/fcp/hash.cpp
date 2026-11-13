#include "fcp/hash.h"

#include <windows.h>
#include <bcrypt.h>

#include <fstream>
#include <vector>

namespace fcp::hash {

Hasher::Hasher(Kind kind) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCryptOpenAlgorithmProvider(&alg, kind == Kind::kSha256 ? BCRYPT_SHA256_ALGORITHM : BCRYPT_SHA1_ALGORITHM,
                              nullptr, 0);
  BCRYPT_HASH_HANDLE h = nullptr;
  if (alg) BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
  alg_ = alg;
  hash_ = h;
  length_ = kind == Kind::kSha256 ? 32 : 20;
}

Hasher::~Hasher() {
  if (hash_) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
  if (alg_) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(alg_), 0);
}

void Hasher::Update(const void* data, size_t size) {
  if (!hash_) return;
  const auto* p = static_cast<const UCHAR*>(data);
  while (size) {
    const ULONG n = size > 0x40000000 ? 0x40000000 : static_cast<ULONG>(size);
    BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(hash_), const_cast<UCHAR*>(p), n, 0);
    p += n;
    size -= n;
  }
}

std::string Hasher::HexDigest() {
  if (!hash_) return {};
  unsigned char digest[32] = {};
  BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(hash_), digest, static_cast<ULONG>(length_), 0);
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (size_t i = 0; i < length_; ++i) {
    out += kHex[digest[i] >> 4];
    out += kHex[digest[i] & 15];
  }
  return out;
}

std::string Sha256(const void* data, size_t size) {
  Hasher h(Hasher::Kind::kSha256);
  h.Update(data, size);
  return h.HexDigest();
}

std::string Sha1(const void* data, size_t size) {
  Hasher h(Hasher::Kind::kSha1);
  h.Update(data, size);
  return h.HexDigest();
}

std::string Sha256File(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  Hasher h(Hasher::Kind::kSha256);
  std::vector<char> buffer(1 << 20);
  while (f) {
    f.read(buffer.data(), std::streamsize(buffer.size()));
    if (f.gcount() > 0) h.Update(buffer.data(), size_t(f.gcount()));
  }
  return h.HexDigest();
}

}  // namespace fcp::hash
