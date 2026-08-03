#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

class ProgressBar {
 public:
  explicit ProgressBar(size_t total_frames, size_t interval = 100,
                       std::size_t bar_width = 50)
      : total_frames_(total_frames),
        interval_(interval),
        bar_width_(bar_width),
        show_progress_(total_frames_ > 0),
        display_total_frames_(show_progress_ ? total_frames_ : 1),
        frame_counter_width_(
            static_cast<int>(std::to_string(display_total_frames_).size())) {}

  auto MaybeRender(size_t frame_index) -> void {
    if (!show_progress_ || (frame_index % interval_ != 0)) {
      return;
    }
    Render(frame_index);
  }

  auto Finish() -> void {
    if (!show_progress_) {
      return;
    }
    Render(display_total_frames_);
    std::cout << "\r\033[2K" << std::flush;
    RestoreCursor();
  }

 private:
  auto EnsureCursorHidden() -> void {
    if (!cursor_hidden_) {
      std::cout << "\033[?25l" << std::flush;
      cursor_hidden_ = true;
    }
  }

  auto RestoreCursor() -> void {
    if (cursor_hidden_) {
      std::cout << "\033[?25h" << std::flush;
      cursor_hidden_ = false;
    }
  }

  auto Render(size_t frame_index) -> void {
    EnsureCursorHidden();
    const size_t clamped = std::min(frame_index, display_total_frames_);
    const double ratio = display_total_frames_ == 0
                             ? 1.0
                             : static_cast<double>(clamped) /
                                   static_cast<double>(display_total_frames_);
    const double percent = ratio * 100.0;
    const size_t total_subunits = bar_width_ * 8;
    size_t filled_subunits = std::min(
        total_subunits, static_cast<size_t>(std::lround(
                            ratio * static_cast<double>(total_subunits))));
    static constexpr std::array<const char*, 9> kBlockChars = {
        " ", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█"};
    std::string bar;
    bar.reserve(bar_width_ * 3);
    for (size_t i = 0; i < bar_width_; ++i) {
      const size_t units = std::min<size_t>(filled_subunits, 8);
      bar += kBlockChars[units];  // NOLINT
      filled_subunits = (filled_subunits >= 8) ? (filled_subunits - 8) : 0;
    }
    std::ostringstream oss;
    oss << '[' << bar << "] " << std::fixed << std::setprecision(1)
        << std::setw(5) << percent << "% " << std::setw(frame_counter_width_)
        << clamped << '/' << std::setw(frame_counter_width_)
        << display_total_frames_;
    std::cout << '\r' << oss.str() << std::flush;
  }

  size_t total_frames_ = 0;
  size_t interval_ = 100;
  std::size_t bar_width_ = 50;
  bool show_progress_ = false;
  size_t display_total_frames_ = 1;
  int frame_counter_width_ = 1;
  bool cursor_hidden_ = false;
};
