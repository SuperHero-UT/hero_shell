#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "detector_constants.hh"
#include "frame_analyzer.hh"
#include "raw_data_file.hh"

namespace {

constexpr size_t kMaxEvents = 8192;
using PedestalSamples =
    std::array<std::array<std::vector<int16_t>, kChannelNum>, kAsicNum>;

auto median(std::vector<int16_t>& samples) -> double {
  if (samples.empty()) {
    throw std::runtime_error("No pedestal samples for one or more channels");
  }

  const auto middle = samples.begin() + static_cast<std::ptrdiff_t>(samples.size() / 2U);
  std::nth_element(samples.begin(), middle, samples.end());
  if (samples.size() % 2U != 0) {
    return *middle;
  }
  const auto lower = std::max_element(samples.begin(), middle);
  return (static_cast<double>(*lower) + *middle) / 2.0;
}

}  // namespace

auto main(int argc, char** argv) -> int try {
  if (argc == 2 && std::string(argv[1]) == "--check") {
    return 0;
  }
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " raw_file\n";
    return 1;
  }

  PedestalSamples samples;
  RawDataFile raw(argv[1], false);
  cdtedsd::EventData<kAsicNum, kChannelNum> event{};
  cdtedsd::FrameAnalyzer<kAsicNum, kChannelNum> analyzer{};
  size_t event_count = 0;

  while (event_count < kMaxEvents && raw.GetNextFrame()) {
    const auto& frame = raw.GetFrame();
    analyzer.Initialize(reinterpret_cast<const uint8_t*>(frame.data()), frame.size());
    while (event_count < kMaxEvents && analyzer.UnpackNextEvent(event)) {
      if (!event.valid) {
        continue;
      }
      ++event_count;
      for (size_t asic = 0; asic < kAsicNum; ++asic) {
        const auto& data = event.asic_data[asic];
        for (size_t channel = 0; channel < kChannelNum; ++channel) {
          if (data.chflag.test(channel)) {
            samples[asic][channel].push_back(data.adc_data[channel] - data.cmn);
          }
        }
      }
    }
  }

  for (auto& asic : samples) {
    for (size_t channel = 0; channel < asic.size(); ++channel) {
      std::cout << median(asic[channel]) << (channel + 1U == asic.size() ? '\n' : ' ');
    }
  }
  return 0;
} catch (const std::exception& error) {
  std::cerr << "Error: " << error.what() << "\n";
  return 1;
}
