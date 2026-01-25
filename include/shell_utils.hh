#pragma once

#include <functional>
#include <string>
#include <vector>

namespace shell {

class defer {
 public:
  explicit defer(std::function<void()> func) : func_(std::move(func)) {}

  ~defer() {
    if (active_) {
      func_();
    }
  }
  defer(const defer&) = delete;
  auto operator=(const defer&) -> defer& = delete;
  defer(defer&& other) noexcept : func_(std::move(other.func_)), active_(other.active_) {
    other.active_ = false;
  }
  auto operator=(defer&& other) noexcept -> defer& {
    if (this != &other) {
      if (active_) {
        func_();
      }
      func_ = std::move(other.func_);
      active_ = other.active_;
      other.active_ = false;
    }
    return *this;
  }

 private:
  std::function<void()> func_;
  bool active_{};
};

inline auto split_shell_like(const std::string& input) -> std::vector<std::string> {
  std::vector<std::string> tokens;
  std::string cur;
  bool in_quote = false;

  for (size_t i = 0; i < input.size(); ++i) {
    char c = input[i];

    if (c == '\\') {
      if (i + 1 < input.size()) {
        cur.push_back(input[i + 1]);
        ++i;
      }
      continue;
    }

    if (c == '"') {
      in_quote = !in_quote;
      continue;
    }

    if (!in_quote && std::isspace(static_cast<unsigned char>(c))) {
      if (!cur.empty()) {
        tokens.push_back(cur);
        cur.clear();
      }
      continue;
    }

    cur.push_back(c);
  }

  if (!cur.empty()) {
    tokens.push_back(cur);
  }

  return tokens;
}

inline auto to_hex_string(uint8_t value) -> std::string {
  std::array<char, 16> hex_chars = {'0', '1', '2', '3', '4', '5', '6', '7',
                                    '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
  std::string result;
  result.reserve(4);
  result.push_back('0');
  result.push_back('x');
  result.push_back(hex_chars.at((value >> 4) & 0x0F));
  result.push_back(hex_chars.at(value & 0x0F));
  return result;
}

inline auto to_hex_string(uint32_t value) -> std::string {
  std::array<char, 16> hex_chars = {'0', '1', '2', '3', '4', '5', '6', '7',
                                    '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
  std::string result;
  result.reserve(10);
  result.push_back('0');
  result.push_back('x');
  for (int i = 7; i >= 0; --i) {
    result.push_back(hex_chars.at((value >> (i * 4)) & 0x0F));
  }
  return result;
}

inline void trim(std::string_view& sv) {
  auto is_space = [](unsigned char c) -> bool { return std::isspace(c) != 0; };
  while (!sv.empty() && is_space(static_cast<unsigned char>(sv.front()))) sv.remove_prefix(1);
  while (!sv.empty() && is_space(static_cast<unsigned char>(sv.back()))) sv.remove_suffix(1);
}

inline auto to_lower(std::string_view sv) -> std::string {
  std::string s(sv);
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); });
  return s;
}

// 例: "1h30min", "10s500ms", "2h", "90min", "1m20s"
inline auto parse_duration(std::string_view sv) -> std::chrono::nanoseconds {
  using namespace std::chrono;

  trim(sv);
  if (sv.empty()) throw std::invalid_argument("empty duration string");

  nanoseconds total{0};
  std::size_t pos = 0;

  while (pos < sv.size()) {
    // --- 数値を読む ---
    std::size_t start_num = pos;
    bool seen_dot = false;

    // +/- を許容
    if (pos < sv.size() && (sv[pos] == '+' || sv[pos] == '-')) pos++;

    while (pos < sv.size()) {
      char c = sv[pos];
      if (std::isdigit(static_cast<unsigned char>(c))) {
        pos++;
      } else if (c == '.' && !seen_dot) {
        seen_dot = true;
        pos++;
      } else {
        break;
      }
    }
    if (pos == start_num) throw std::invalid_argument("duration: missing numeric part");

    double value = std::stod(std::string(sv.substr(start_num, pos - start_num)));

    std::size_t start_unit = pos;
    while (pos < sv.size() && std::isalpha(static_cast<unsigned char>(sv[pos]))) pos++;

    std::string unit = to_lower(sv.substr(start_unit, pos - start_unit));
    if (unit.empty()) throw std::invalid_argument("duration: missing unit after number");

    double seconds = 0.0;
    if (unit == "h" || unit == "hr" || unit == "hrs" || unit == "hour" || unit == "hours") {
      seconds = value * 3600.0;
    } else if (unit == "m" || unit == "min" || unit == "mins" || unit == "minute" ||
               unit == "minutes") {
      seconds = value * 60.0;
    } else if (unit == "s" || unit == "sec" || unit == "secs" || unit == "second" ||
               unit == "seconds") {
      seconds = value;
    } else if (unit == "ms" || unit == "msec" || unit == "millisecond" || unit == "milliseconds") {
      seconds = value / 1000.0;
    } else {
      throw std::invalid_argument("duration: unknown unit '" + unit + "'");
    }

    total += duration_cast<nanoseconds>(duration<double>(seconds));
  }

  return total;
}

inline auto parse_uint8(const std::string& s) -> int {
  size_t idx = 0;
  int base = 10;

  if (s.size() > 2 && (s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
  }

  int value = std::stoi(s, &idx, base);

  if (value < 0 || value > 255) {
    throw std::out_of_range("value does not fit in uint8_t: " + s);
  }
  if (idx != s.size()) {
    throw std::invalid_argument("invalid numeric string: " + s);
  }

  return value;
}

inline auto parse_uint32(const std::string& s) -> std::uint32_t {
  size_t idx = 0;
  int base = 10;

  if (s.size() >= 3 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
  }

  unsigned long v = std::stoul(s, &idx, base);

  if (idx != s.size()) {
    throw std::invalid_argument("invalid numeric string: " + s);
  }
  if (v > 0xFFFFFFFFul) {
    throw std::out_of_range("value does not fit in uint32_t: " + s);
  }

  return static_cast<std::uint32_t>(v);
}

}  // namespace shell
