// SHA-256 (FIPS 180-4), used by the inventory tool to fingerprint local files.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace pcport {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t n);
    std::string hexDigest();  // finalizes
private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t bufLen_ = 0;
    uint64_t total_ = 0;
};

std::string sha256Hex(const std::string& s);
bool sha256File(const std::string& path, std::string& hexOut, uint64_t& sizeOut);

}  // namespace pcport
