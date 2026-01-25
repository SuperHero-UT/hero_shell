#pragma once
#include <array>
#include <cstdint>

namespace shell::crc {

inline auto rbit32(uint32_t value) -> uint32_t {
  uint32_t result = 0;
  for (int i = 0; i < 32; ++i) {
    result <<= 1;
    result |= (value & 1);
    value >>= 1;
  }
  return result;
}

inline auto rbit8(uint8_t value) -> uint32_t { return rbit32(static_cast<uint32_t>(value)) >> 24; }

constexpr auto crc32_tablex4(uint32_t polynomial) -> std::array<std::array<uint32_t, 256>, 4> {
  std::array<std::array<uint32_t, 256>, 4> table{};
  const auto poly = polynomial;

  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 256; ++j) {
      uint32_t crc =
          (i == 0) ? static_cast<uint32_t>(static_cast<uint32_t>(j) << 24) : table[i - 1][j];
      for (int k = 0; k < 8; ++k) {
        crc = (crc & 0x80000000u) ? static_cast<uint32_t>((crc << 1) ^ poly)
                                  : static_cast<uint32_t>(crc << 1);
      }
      table[i][j] = crc;
    }
  }
  return table;
}

constexpr auto zlib_crc32_tablex4 =
    crc32_tablex4(0x04C11DB7u /* x^32 + x^26 + x^23 + x^22 + x^16 + x^12 + x^11 + x^10 + x^8 + x^7 + x^5 + x^4 + x^2 + x + 1 */);

constexpr auto crc32(const uint8_t* data, size_t length) -> uint32_t {
  if (length % 4 != 0) {
    throw std::invalid_argument("Data length must be multiple of 4 for this CRC32.");
  }
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; i += 4) {
    uint8_t byte1 = data[i];      // NOLINT
    uint8_t byte2 = data[i + 1];  // NOLINT
    uint8_t byte3 = data[i + 2];  // NOLINT
    uint8_t byte4 = data[i + 3];  // NOLINT
    byte1 = rbit8(byte1);
    byte2 = rbit8(byte2);
    byte3 = rbit8(byte3);
    byte4 = rbit8(byte4);
    crc ^= static_cast<uint32_t>(byte1) << 24 ^ static_cast<uint32_t>(byte2) << 16 ^
           static_cast<uint32_t>(byte3) << 8 ^ static_cast<uint32_t>(byte4);
    crc = zlib_crc32_tablex4[3][(crc >> 24) & 0xFFu] ^ zlib_crc32_tablex4[2][(crc >> 16) & 0xFFu] ^
          zlib_crc32_tablex4[1][(crc >> 8) & 0xFFu] ^ zlib_crc32_tablex4[0][crc & 0xFFu];
  }
  crc = rbit32(crc);
  return crc ^ 0xFFFFFFFFu;
}

}  // namespace shell::crc
