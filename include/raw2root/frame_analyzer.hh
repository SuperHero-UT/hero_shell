#pragma once

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>

namespace cdtedsd {

constexpr size_t kFrameSize = 1ULL << 15;  //  32768;

template <size_t ChannelNum = 64>
struct ASICData {
  std::bitset<5> header{};
  std::bitset<ChannelNum> chflag{};
  int16_t ref = 0;
  int16_t cmn = 0;
  std::array<int16_t, ChannelNum> adc_data{};

  void Reset() {
    header.reset();
    chflag.reset();
    ref = 0;
    cmn = 0;
    adc_data.fill(0);
  }
};

template <size_t ASICNUM = 4, size_t ChannelNum = 64>
struct EventData {
  uint32_t ti = 0;
  uint32_t livetime = 0;
  uint32_t integral_livetime = 0;
  uint32_t flag_trig_pat = 0;
  uint32_t event_counter = 0;
  uint32_t pseudo_counter = 0;
  bool is_pseudo_event = false;
  bool valid = true;
  std::array<ASICData<ChannelNum>, ASICNUM> asic_data{};

  void Reset() {
    ti = 0;
    livetime = 0;
    integral_livetime = 0;
    flag_trig_pat = 0;
    event_counter = 0;
    pseudo_counter = 0;
    valid = true;
    for (auto& asic : asic_data) {
      asic.Reset();
    }
  }
  constexpr void Invalidate() {
    Reset();
    valid = false;
  }
  constexpr operator bool() const { return valid; }
};

template <size_t ASICNUM = 4, size_t ChannelNum = 64>
class FrameAnalyzer {
 private:
  const uint8_t* raw_data_ = nullptr;
  size_t raw_data_size_ = 0;
  uint32_t head_ = 0;
  static constexpr size_t kAsicDataStartByte = 24;
  static constexpr std::array<uint8_t, 4> kEventHeader = {0x3C, 0x3C, 0x00,
                                                          0x00};
  static constexpr std::array<uint8_t, 4> kEventFooter = {0x00, 0x00, 0x77,
                                                          0x77};

 protected:
  template <size_t Big, size_t Len>
  auto GetValueFromRawData(uint32_t offset) {
    static_assert(Len == 1 || Len == 2 || Len == 4, "Len must be 1, 2, or 4");

    using ValueType =
        std::conditional_t<Len == 1, uint8_t,
                           std::conditional_t<Len == 2, uint16_t, uint32_t>>;

    if (raw_data_ == nullptr) {
      throw std::runtime_error("Raw data is not initialized.");
    }
    if (offset + Big + Len > raw_data_size_) {
      throw std::out_of_range("GetValueFromRawData: Offset out of range");
    }

    ValueType data{};
    std::memcpy(&data, raw_data_ + offset + Big, Len);  // NOLINT

    if constexpr (Len == 1) {
      return data;
    } else if constexpr (Len == 2) {
      return static_cast<uint16_t>((data >> 8) | (data << 8));
    } else {  // Len == 4
      return ((data & 0xFF000000u) >> 24) | ((data & 0x00FF0000u) >> 8) |
             ((data & 0x0000FF00u) << 8) | ((data & 0x000000FFu) << 24);
    }
  }

 public:
  FrameAnalyzer(const FrameAnalyzer&) = delete;
  FrameAnalyzer(FrameAnalyzer&&) = delete;
  auto operator=(const FrameAnalyzer&) -> FrameAnalyzer& = delete;
  auto operator=(FrameAnalyzer&&) -> FrameAnalyzer& = delete;
  ~FrameAnalyzer() = default;

  FrameAnalyzer() = default;
  FrameAnalyzer(const uint8_t* data, size_t size)
      : raw_data_(data), raw_data_size_(size) {
    if (size != kFrameSize) {
      // throw std::runtime_error("Data size must be 32768 bytes.");
      // Relaxed size check for robustness
    }
  }

  void Initialize(const uint8_t* data, size_t size) {
    if (data == nullptr && size > 0) {
      throw std::runtime_error("Data pointer is null but size is > 0.");
    }
    raw_data_ = data;
    raw_data_size_ = size;
    head_ = 0;
  }

  auto UnpackNextEvent(EventData<ASICNUM, ChannelNum>& event) -> bool {
    if (raw_data_ == nullptr) {
      throw std::runtime_error("Raw data is not initialized.");
    }
    event.Reset();
    if (head_ + 4 > raw_data_size_) {
      return false;
    }
    const uint8_t* header = raw_data_ + head_;                    // NOLINT
    if (!std::equal(header, header + 4, kEventHeader.begin())) {  // NOLINT
      return false;
    }
    event.valid = true;

    event.ti = GetValueFromRawData<4, 4>(head_);
    event.livetime = GetValueFromRawData<8, 4>(head_);
    event.integral_livetime = GetValueFromRawData<12, 2>(head_);
    event.flag_trig_pat = GetValueFromRawData<14, 2>(head_);
    event.is_pseudo_event = (event.flag_trig_pat & 0x0001) == 0x0001;
    event.event_counter = GetValueFromRawData<16, 4>(head_);
    event.pseudo_counter = GetValueFromRawData<20, 4>(head_);
    head_ += 24;

    auto bit_offset = static_cast<size_t>(head_ * 8);
    for (size_t i = 0; i < ASICNUM; ++i) {
      event.asic_data[i].header = std::bitset<5>{};

      for (size_t j = 0; j < 5; ++j) {
        event.asic_data[i].header.set(j, TestBit(bit_offset + j));
      }

      bit_offset += 5;
      if (!event.asic_data[i].header.test(1)) {
        continue;
      }

      event.asic_data[i].chflag.reset();
      for (size_t j = 0; j < 64; ++j) {
        event.asic_data[i].chflag.set(j, TestBit(bit_offset + j));
      }
      bit_offset += 64;
      bit_offset += 1;  // <-- skip 1 bit ?

      event.asic_data[i].ref = Convert10bit(bit_offset);
      bit_offset += 10;

      for (size_t j = 0; j < ChannelNum; ++j) {
        if (event.asic_data[i].chflag.test(j)) {
          event.asic_data[i].adc_data[j] = Convert10bit(bit_offset);
          bit_offset += 10;
        } else {
          event.asic_data[i].adc_data[j] = -1;
        }
      }

      event.asic_data[i].cmn = Convert10bit(bit_offset);
      bit_offset += 10;
      bit_offset += 1;  // <-- skip 1 bit ?
    }
    head_ = ((bit_offset + 63) / 32) * 4;  // Align to the next 4-byte boundary
    if (head_ + 4 > raw_data_size_) {
      // Not enough space for footer
      event.Invalidate();
      return false;
    }
    const uint8_t* footer = raw_data_ + head_;                   // NOLINT
    if (std::equal(footer, footer + 4, kEventFooter.begin())) {  // NOLINT
      head_ += 4;
      return true;
    }
    footer = raw_data_ + head_;                                  // NOLINT
    if (std::equal(footer, footer + 4, kEventFooter.begin())) {  // NOLINT
      head_ += 4;
      return true;
    }
    event.Invalidate();
    while (head_ + 4 <= raw_data_size_) {
      footer = raw_data_ + head_;                                  // NOLINT
      if (std::equal(footer, footer + 4, kEventFooter.begin())) {  // NOLINT
        head_ += 4;
        return true;
      }
      head_++;
    }
    // Footer not found
    return false;
  }

  [[nodiscard]] auto GetAsicNum() const -> size_t { return ASICNUM; }

 private:
  [[nodiscard]] inline auto Convert10bit(size_t n) -> uint16_t {
    uint16_t ret = 0;
    for (size_t i = 0; i < 10; ++i) {
      if (TestBit(n + i)) {
        ret |= (1 << (i));
      }
    }
    return ret;
  }

  [[nodiscard]] inline auto TestBit(size_t n) const -> bool {
    const size_t byte_index = n / 8;
    const uint8_t byte = raw_data_[byte_index];  // NOLINT
    const auto mask = static_cast<uint8_t>(0x80u >> (n & 7u));
    return (byte & mask) != 0;
  }
};

template <size_t ASICNUM = 4, size_t ChannelNum = 64, bool OldFormat = false>
class Analyzer {
  EventData<ASICNUM, ChannelNum> event_data_{};
  FrameAnalyzer<ASICNUM, ChannelNum> frame_analyzer_{};
  std::ifstream file_{};
  std::array<uint8_t, kFrameSize> frame_buffer_{};

 public:
  explicit Analyzer(std::string fname) {
    file_.open(fname, std::ios::binary);
    if (!file_.is_open()) {
      throw std::runtime_error("Could not open file: " + fname);
    }
  }

  auto GetEventData() -> EventData<ASICNUM, ChannelNum>& { return event_data_; }

  template <typename Func>
  auto Analyze(const Func& func) -> void {
    static_assert(
        std::is_invocable_v<Func, EventData<ASICNUM, ChannelNum>&, size_t>,
        "Func must be callable with EventData<ASICNUM, CHANNEL_NUM>& as "
        "argument.");
    if constexpr (OldFormat) {
      static std::array<uint8_t, 4> header_buffer{};
      static std::array<uint8_t, 4> footer_buffer{};
      static constexpr std::array<uint8_t, 4> kOldHkheader = {
          0xAB, 0xCD, 0xEF, 0x03};
      static constexpr std::array<uint8_t, 4> kOldDataheader = {
          0xAB, 0xCD, 0xEF, 0x02};
      static constexpr std::array<uint8_t, 4> kOldFooter = {
          0xFF, 0xFF, 0x01, 0x23};
      size_t frame_count = 0;
      while (
          file_.read(reinterpret_cast<char*>(header_buffer.data()),  // NOLINT
                     4)) {
        if (std::equal(header_buffer.begin(), header_buffer.end(),
                       kOldHkheader.begin())) {
          file_.seekg(8372, std::ios::cur);
          continue;
        } else if (std::equal(header_buffer.begin(), header_buffer.end(),
                              kOldDataheader.begin())) {
          // Read frame data // NOLINTNEXTLINE
          file_.read(reinterpret_cast<char*>(frame_buffer_.data()), kFrameSize);
          size_t bytes_read = file_.gcount();
          // Check footer
          file_.seekg(4, std::ios::cur);  // Skip UnixTime NOLINTNEXTLINE
          file_.read(reinterpret_cast<char*>(footer_buffer.data()), 4);
          if (!std::equal(footer_buffer.begin(), footer_buffer.end(),
                          kOldFooter.begin())) {
            throw std::runtime_error("Invalid footer in old format.");
          }
          frame_analyzer_.Initialize(
              // NOLINTNEXTLINE
              reinterpret_cast<uint8_t*>(frame_buffer_.data()), bytes_read);
          try {
            while (frame_analyzer_.UnpackNextEvent(event_data_)) {
              func(event_data_, frame_count);
            }
          } catch (const std::runtime_error& e) {
            std::cerr << "\tError unpacking event: " << e.what() << std::endl;
            std::cerr << "\tFile position: " << file_.tellg() << std::endl;
          }
          frame_count++;
        } else {
          throw std::runtime_error("Invalid header in old format.");
        }
      }
    } else {
      size_t frame_count = 0;
      while (
          file_.read(reinterpret_cast<char*>(frame_buffer_.data()),  // NOLINT
                     frame_buffer_.size()) ||
          file_.gcount() > 0) {
        size_t bytes_read = file_.gcount();
        frame_analyzer_.Initialize(
            reinterpret_cast<uint8_t*>(frame_buffer_.data()),  // NOLINT
            bytes_read);
        while (frame_analyzer_.UnpackNextEvent(event_data_)) {
          func(event_data_, frame_count);
        }
        frame_count++;
      }
    }
  }
};

};  // namespace cdtedsd
