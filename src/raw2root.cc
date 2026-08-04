#include <Compression.h>
#include <TFile.h>
#include <TH1.h>
#include <TH2.h>
#include <TH2D.h>
#include <TMath.h>
#include <TTree.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "detector_constants.hh"
#include "frame_analyzer.hh"
#include "progress_bar.hh"
#include "raw_data_file.hh"

struct ProcessResult {
  size_t total_frames = 0;
  size_t total_events = 0;
};

class DataFile {};

auto Analyze(const std::string& input_file) -> ProcessResult {
  std::error_code fs_error;
  const auto file_size =
      static_cast<size_t>(std::filesystem::file_size(input_file, fs_error));
  if (fs_error) {
    std::cerr << "Error: Could not read file size " << input_file << std::endl;
    return {};
  }
  RawDataFile raw_data(input_file, false);
  const bool is_old_format = raw_data.IsOldFormat();
  const size_t total_frames = is_old_format
                                  ? file_size / (cdtedsd::kFrameSize + 12)
                                  : file_size / cdtedsd::kFrameSize;

  std::string root_file_name = input_file + ".root";
  auto outfile = TFile(root_file_name.c_str(), "recreate");
  auto events = TTree("events", "events");

  outfile.SetCompressionAlgorithm(ROOT::RCompressionSetting::EAlgorithm::kZSTD);
  outfile.SetCompressionLevel(1);

  cdtedsd::EventData<kAsicNum, kChannelNum> event_data{};
  cdtedsd::FrameAnalyzer<kAsicNum, kChannelNum> frame_analyzer{};

  events.Branch("ti", &event_data.ti, "ti/i");
  events.Branch("livetime", &event_data.livetime, "livetime/i");
  events.Branch("integral_livetime", &event_data.integral_livetime,
                "integral_livetime/i");
  events.Branch("trighitpat", &event_data.flag_trig_pat, "trighitpat/i");
  events.Branch("event_counter", &event_data.event_counter, "event_counter/i");
  events.Branch("pseudo_counter", &event_data.pseudo_counter,
                "pseudo_counter/i");
  events.Branch("is_pseudo_event", &event_data.is_pseudo_event,
                "is_pseudo_event/O");

  for (size_t i = 0; i < kAsicNum; ++i) {
    std::stringstream cmn_name, cmn_type;
    cmn_name << "cmn" << i;
    cmn_type << "cmn" << i << "/S";
    events.Branch(cmn_name.str().c_str(), &event_data.asic_data.at(i).cmn,
                  cmn_type.str().c_str());

    std::stringstream adc_name, adc_type;
    adc_name << "adc" << i;
    adc_type << "adc" << i << "[" << kChannelNum << "]/S";
    events.Branch(adc_name.str().c_str(), &event_data.asic_data.at(i).adc_data,
                  adc_type.str().c_str());

    std::stringstream ref_name, ref_type;
    ref_name << "ref" << i;
    ref_type << "ref" << i << "/S";
    events.Branch(ref_name.str().c_str(), &event_data.asic_data.at(i).ref,
                  ref_type.str().c_str());
  }

  auto histall =
      TH2D("histall", "histall", 256, -0.5, -0.5 + 256, 1024, -0.5, 1023.5);
  auto histall_cmn = TH2D("histall_cmn", "histall_cmn", 256, -0.5, -0.5 + 256,
                          1024, -50.5, 1024.0 - 50.5);

  int event_counter = 0;
  ProgressBar progress_bar(total_frames);

  auto process_event =
      [&](cdtedsd::EventData<kAsicNum, kChannelNum>& analyzed_event,
          size_t frame_index) -> void {
    if (!analyzed_event.valid) {
      std::cout << "Warning: Invalid event at frame: " << frame_index
                << " and index: " << event_counter << ".";
      std::cout << "This might be caused by data corruption or misalignment."
                << std::endl;
      return;
    }
    for (size_t asic_index = 0; asic_index < kAsicNum; ++asic_index) {
      const auto& asic_data = analyzed_event.asic_data[asic_index];  // NOLINT
      const auto cmn = asic_data.cmn;
      for (size_t index = 0; index < kChannelNum; ++index) {
        if (!asic_data.chflag.test(index)) {
          continue;
        }
        const auto adc = asic_data.adc_data[index];  // NOLINT
        const auto global_index =
            static_cast<double>(asic_index * kChannelNum + index);
        histall.Fill(global_index, adc);
        histall_cmn.Fill(global_index, adc - cmn);
      }
    }
    events.Fill();
    event_counter++;
  };

  size_t frame_count = 0;
  while (raw_data.GetNextFrame()) {
    progress_bar.MaybeRender(frame_count);
    const auto& frame = raw_data.GetFrame();
    frame_analyzer.Initialize(
        reinterpret_cast<const uint8_t*>(frame.data()),  // NOLINT
        frame.size());
    while (frame_analyzer.UnpackNextEvent(event_data)) {
      process_event(event_data, frame_count);
    }
    frame_count++;
  }

  progress_bar.Finish();
  outfile.Write();
  return {.total_frames = total_frames,
          .total_events = static_cast<size_t>(event_counter)};
}

auto main(int argc, char** argv) -> int try {
  const std::vector<std::string> args(argv, argv + argc);  // NOLINT
  if (args.size() <= 1) {
    std::cout << "Usage: " << args.front() << " file name" << std::endl;
    return 1;
  }
  for (const auto& input_file :
       std::vector<std::string>(args.begin() + 1, args.end())) {
    const auto result = Analyze(input_file);
    std::cout << "[100.0%] total_frame: " << result.total_frames
              << " total_event: " << result.total_events << " " << input_file
              << std::endl;
  }
  return 0;
} catch (const std::exception& ex) {
  std::cerr << "Error: " << ex.what() << std::endl;
  return 1;
} catch (...) {
  std::cerr << "Error: Unknown exception" << std::endl;
  return 1;
}
