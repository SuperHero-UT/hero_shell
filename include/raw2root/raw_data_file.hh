#pragma once

#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "frame_analyzer.hh"

// Old Format Headers/Footers
static constexpr std::array<char, 4> kOldHkheader = {
    static_cast<char>(0xAB), static_cast<char>(0xCD), static_cast<char>(0xEF),
    static_cast<char>(0x03)};
static constexpr std::array<char, 4> kOldDataheader = {
    static_cast<char>(0xAB), static_cast<char>(0xCD), static_cast<char>(0xEF),
    static_cast<char>(0x02)};
static constexpr std::array<char, 4> kOldFooter = {
    static_cast<char>(0xFF), static_cast<char>(0xFF), static_cast<char>(0x01),
    static_cast<char>(0x23)};

class RawDataFile {
 public:
  explicit RawDataFile(std::string filename, bool enable_watch = true,
                       const volatile std::sig_atomic_t* abort_flag = nullptr,
                       bool block_on_wait = true)
      : filename_(std::move(filename)),
        file_(filename_, std::ios::binary),
        ignore_buffer_(8372),
        frame_(cdtedsd::kFrameSize),
        enable_watch_(enable_watch),
        abort_flag_(abort_flag),
        block_on_wait_(block_on_wait) {
    if (!file_.is_open()) {
      throw std::runtime_error("Error: Could not open file");
    }
    std::array<char, 4> first_4bytes{};
    ReadData(first_4bytes.data(), first_4bytes.size());
    is_old_format_ = (first_4bytes[0] == kOldHkheader[0] &&
                      first_4bytes[1] == kOldHkheader[1] &&
                      first_4bytes[2] == kOldHkheader[2]);
    file_.seekg(0, std::ios::beg);
  }

  ~RawDataFile() = default;
  RawDataFile(const RawDataFile&) = delete;
  RawDataFile(RawDataFile&&) = delete;
  auto operator=(const RawDataFile&) -> RawDataFile& = delete;
  auto operator=(RawDataFile&&) -> RawDataFile& = delete;

  auto GetNextFrame() -> bool {
    if (!is_old_format_) {
      return ReadData(frame_.data(), frame_.size());
    }

    std::array<char, 4> header{};
    std::array<char, 4> footer{};
    std::array<char, 4> unixtime{};

    while (true) {
      auto header_pos = file_.tellg();
      if (!ReadData(header.data(), header.size())) {
        return false;
      }

      if (std::equal(header.begin(), header.end(), kOldHkheader.begin(),
                     kOldHkheader.end())) {
        if (!ReadData(ignore_buffer_.data(), ignore_buffer_.size())) {
          file_.clear();
          file_.seekg(header_pos);
          return false;
        }
        continue;
      }

      if (std::equal(header.begin(), header.end(), kOldDataheader.begin(),
                     kOldDataheader.end())) {
        if (!ReadData(frame_.data(), frame_.size())) {
          file_.clear();
          file_.seekg(header_pos);
          return false;
        }
        if (!ReadData(unixtime.data(), unixtime.size())) {
          file_.clear();
          file_.seekg(header_pos);
          return false;
        }
        if (!ReadData(footer.data(), footer.size())) {
          file_.clear();
          file_.seekg(header_pos);
          return false;
        }
        if (!std::equal(footer.begin(), footer.end(), kOldFooter.begin(),
                        kOldFooter.end())) {
          file_.clear();
          file_.seekg(header_pos + std::streamoff(1));
          ++misalignment_count_;
          continue;
        }
        return true;
      }

      file_.clear();
      file_.seekg(header_pos + std::streamoff(1));
      ++misalignment_count_;
    }
  }

  auto GetFrame() const -> const std::vector<char>& { return frame_; }
  auto IsOldFormat() const -> bool { return is_old_format_; }
  auto GetMisalignmentCount() const -> uint64_t { return misalignment_count_; }

 private:
  auto ReadData(char* data, size_t length) -> bool {
    if (file_.eof()) {
      file_.clear();
    }
    auto pos = file_.tellg();
    if (pos < 0) {
      file_.clear();
      pos = file_.tellg();
      if (pos < 0) {
        pos = 0;
      }
    }
    if (!HasBytesAvailable(length)) {
      if (enable_watch_) {
        if (!block_on_wait_) {
          return false;
        }
        if (!WaitForAppend(length)) {
          return false;
        }
        return ReadData(data, length);
      }
      return false;
    }
    file_.read(reinterpret_cast<char*>(data),  // NOLINT
               static_cast<std::streamsize>(length));
    if (!file_) {
      if (enable_watch_) {
        file_.clear();
        file_.seekg(pos);
        if (!block_on_wait_) {
          return false;
        }
        if (!WaitForAppend(length)) {
          return false;
        }
        return ReadData(data, length);
      }
      return false;
    }
    return true;
  }

  auto HasBytesAvailable(size_t length) -> bool {
    if (file_.eof()) {
      file_.clear();
    }
    auto pos = file_.tellg();
    if (pos < 0) {
      file_.clear();
      pos = file_.tellg();
      if (pos < 0) {
        return false;
      }
    }
    file_.clear();
    file_.seekg(0, std::ios::end);
    const auto size = file_.tellg();
    file_.clear();
    file_.seekg(pos);
    if (size < 0) {
      return false;
    }
    return static_cast<uint64_t>(pos) + length <=
           static_cast<uint64_t>(size);
  }

  auto WaitForAppend(size_t length) -> bool {
    while (!HasBytesAvailable(length)) {
      if (abort_flag_ && *abort_flag_ != 0) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
  }

 private:
  std::string filename_;
  std::ifstream file_;
  std::vector<char> ignore_buffer_;  // For HK body
  std::vector<char> frame_;          // For data frame
  bool is_old_format_ = false;
  bool enable_watch_ = true;
  const volatile std::sig_atomic_t* abort_flag_ = nullptr;
  bool block_on_wait_ = true;
  uint64_t misalignment_count_ = 0;
};
