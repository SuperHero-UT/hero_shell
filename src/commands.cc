#include "commands.hh"

#include <google/protobuf/descriptor.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <hlcommands.grpc.pb.h>
#include <hlcommands.pb.h>
#include <superhero.grpc.pb.h>
#include <superhero.pb.h>
#include <sys/xattr.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "base64.hh"
#include "crc.hh"
#include "grpc_funcs.hh"
#include "hero_shell_state.hh"
#include "shell_utils.hh"

using std::string;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace {

[[maybe_unused]] void readFileIntoVec(std::vector<uint8_t>& out, const std::string& filename) {
  std::ifstream ifs(filename);
  if (!ifs.is_open()) {
    throw std::runtime_error("Failed to open file: " + filename);
  }

  std::string line;
  while (std::getline(ifs, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }

    if (line.size() < 4) {
      throw std::runtime_error("Invalid hex line: " + line);
    }

    uint8_t b1 = static_cast<uint8_t>(std::stoul(line.substr(0, 2), nullptr, 16));
    uint8_t b2 = static_cast<uint8_t>(std::stoul(line.substr(2, 2), nullptr, 16));

    out.push_back(b1);
    out.push_back(b2);
  }
}

auto trim_copy(const std::string& input) -> std::string {
  const auto is_space = [](unsigned char c) -> bool { return std::isspace(c) != 0; };
  auto first = std::find_if_not(input.begin(), input.end(), is_space);
  if (first == input.end()) {
    return "";
  }
  auto last = std::find_if_not(input.rbegin(), input.rend(), is_space).base();
  if (first >= last) {
    return "";
  }
  return {first, last};
}

std::string g_last_set_vareg_path = "N/A";

struct ReadoutStatus {
  std::chrono::nanoseconds duration{};
  std::chrono::steady_clock::time_point start_time{};
  std::map<uint8_t, size_t> frame_counters;
  std::vector<std::string> messages;
  std::string job_name = "readout";
  std::string file_prefix;
  std::string hk_filename;
  std::string register_filename;
  size_t suppressed_message_count = 0;
  bool started = false;
  bool has_result = false;
  bool succeeded = false;
  bool stop_requested = false;
};

std::mutex g_readout_status_mutex;
ReadoutStatus g_readout_status;
constexpr size_t kMaxReadoutMessages = 20;

void reset_readout_status(std::chrono::nanoseconds duration,
                          const std::string& job_name = "readout") {
  std::lock_guard<std::mutex> lock(g_readout_status_mutex);
  g_readout_status = {};
  g_readout_status.duration = duration;
  g_readout_status.job_name = job_name;
}

void mark_readout_started() {
  std::lock_guard<std::mutex> lock(g_readout_status_mutex);
  g_readout_status.start_time = std::chrono::steady_clock::now();
  g_readout_status.started = true;
}

void set_readout_outputs(const std::string& file_prefix, const std::string& hk_filename,
                         const std::vector<uint8_t>& detector_addresses) {
  std::lock_guard<std::mutex> lock(g_readout_status_mutex);
  g_readout_status.file_prefix = file_prefix;
  g_readout_status.hk_filename = hk_filename;
  for (const auto address : detector_addresses) {
    g_readout_status.frame_counters.try_emplace(address, 0);
  }
}

void set_readout_register_output(const std::string& register_filename) {
  std::lock_guard<std::mutex> lock(g_readout_status_mutex);
  g_readout_status.register_filename = register_filename;
}

void record_readout_message(const std::string& message) {
  std::lock_guard<std::mutex> lock(g_readout_status_mutex);
  if (g_readout_status.messages.size() < kMaxReadoutMessages) {
    g_readout_status.messages.push_back(message);
  } else {
    ++g_readout_status.suppressed_message_count;
  }
}

void increment_readout_frame_count(uint8_t logical_address) {
  std::lock_guard<std::mutex> lock(g_readout_status_mutex);
  g_readout_status.frame_counters[logical_address] += 1;
}

void finish_readout_status(bool succeeded, bool stop_requested) {
  std::lock_guard<std::mutex> lock(g_readout_status_mutex);
  g_readout_status.has_result = true;
  g_readout_status.succeeded = succeeded;
  g_readout_status.stop_requested = stop_requested;
}

auto readout_status_snapshot() -> ReadoutStatus {
  std::lock_guard<std::mutex> lock(g_readout_status_mutex);
  return g_readout_status;
}

auto format_elapsed_time(std::chrono::steady_clock::duration elapsed) -> std::string {
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
  std::ostringstream out;
  out << std::setfill('0') << std::setw(2) << seconds / 3600 << ':' << std::setw(2)
      << (seconds % 3600) / 60 << ':' << std::setw(2) << seconds % 60;
  return out.str();
}

auto format_prompt_duration(std::chrono::nanoseconds value) -> std::string {
  value = std::max(value, std::chrono::nanoseconds::zero());
  const auto centiseconds = std::chrono::ceil<std::chrono::duration<int64_t, std::centi>>(value);
  const auto count = centiseconds.count();
  std::ostringstream out;
  out << std::setfill('0') << std::setw(2) << count / 6000 << ':' << std::setw(2)
      << (count / 100) % 60 << '.' << std::setw(2) << count % 100;
  return out.str();
}

auto ensure_grpc_initialized() -> bool {
  if (!g_stub) {
    std::cout
        << "gRPC is not initialized. please connect to gRPC sever using `connect` command\n";
    return false;
  }
  return true;
}

auto device_type_name(superhero::DeviceType type) -> const char* {
  switch (type) {
    case superhero::DeviceType_DETECTOR:
      return "detector";
    case superhero::DeviceType_ROUTER:
      return "router";
    case superhero::DeviceType_UNKNOWN:
    default:
      return "unknown";
  }
}

auto parse_logical_address_spec(const std::string& spec) -> std::vector<uint8_t> {
  auto trimmed = trim_copy(spec);
  if (trimmed.empty()) {
    throw std::invalid_argument("Logical address is required.");
  }

  if (trimmed.front() == '[') {
    if (trimmed.back() != ']') {
      throw std::invalid_argument("Logical address list must end with ']'.");
    }
    auto inner = trim_copy(trimmed.substr(1, trimmed.size() - 2));
    if (inner.empty()) {
      throw std::invalid_argument("Logical address list cannot be empty.");
    }

    if (shell::to_lower(inner) == "all") {
      auto devices = get_device_logical_addresses();
      if (!devices.has_value()) {
        throw std::runtime_error("Failed to fetch device list.");
      }
      if (devices->empty()) {
        throw std::runtime_error("No devices have been added.");
      }
      return *devices;
    }

    std::vector<uint8_t> parsed;
    size_t start = 0;
    while (start < inner.size()) {
      size_t comma = inner.find(',', start);
      auto token = trim_copy(inner.substr(start, comma - start));
      if (token.empty()) {
        throw std::invalid_argument("Empty logical address found in list.");
      }
      parsed.push_back(static_cast<uint8_t>(shell::parse_uint8(token)));
      if (comma == std::string::npos) {
        break;
      }
      start = comma + 1;
    }
    return parsed;
  }

  return {static_cast<uint8_t>(shell::parse_uint8(trimmed))};
}

void validate_logical_address(uint8_t logical_address) {
  auto devices = get_device_logical_addresses();
  if (!devices.has_value()) {
    throw std::runtime_error("Failed to fetch device list.");
  }
  if (std::find(devices->begin(), devices->end(), logical_address) == devices->end()) {
    throw std::runtime_error("Device with logical address " +
                             shell::to_hex_string(logical_address) + " not found.");
  }
}

auto format_yyMMdd_hhmmss(std::chrono::system_clock::time_point tp) -> string {
  std::time_t t = std::chrono::system_clock::to_time_t(tp);
  std::tm tm{};
  localtime_r(&t, &tm);

  std::ostringstream oss;
  oss << std::put_time(&tm, "%y%m%d-%H%M%S");
  return oss.str();
}

auto format_iso8601(std::chrono::system_clock::time_point tp) -> string {
  std::time_t t = std::chrono::system_clock::to_time_t(tp);
  std::tm tm{};
  localtime_r(&t, &tm);

  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
  return oss.str();
}

void apply_xattr_to_file(const std::string& path,
                         const std::map<std::string, std::string>& attributes) {
  for (const auto& [key, value] : attributes) {
#if defined(__linux__)
    const std::string attr_name = "user." + key;
#else
    const std::string attr_name = key;
#endif
#if defined(__APPLE__)
    const int result = setxattr(path.c_str(), attr_name.c_str(), value.data(), value.size(), 0, 0);
#else
    const int result = setxattr(path.c_str(), attr_name.c_str(), value.data(), value.size(), 0);
#endif
    if (result != 0) {
      emit_readout_message("Failed to set xattr '" + attr_name + "' on " + path + ": " +
                               std::strerror(errno),
                           true);
    }
  }
}

auto parse_link_speed_token(std::string token) -> std::optional<superhero::SpwLinkSpeed> {
  token.erase(std::remove_if(token.begin(), token.end(),
                             [](unsigned char c) -> bool { return std::isspace(c) != 0; }),
              token.end());
  std::string lowered = token;
  std::transform(
      lowered.begin(), lowered.end(), lowered.begin(),
      [](unsigned char c) -> unsigned char { return static_cast<unsigned char>(std::tolower(c)); });
  for (std::string suffix : {"mhz", "mbps"}) {
    if (auto pos = lowered.find(suffix); pos != std::string::npos) {
      lowered.erase(pos);
    }
  }
  if (lowered == "10") return superhero::SpwLinkSpeed_10MHz;
  if (lowered == "20") return superhero::SpwLinkSpeed_20MHz;
  if (lowered == "25") return superhero::SpwLinkSpeed_25MHz;
  if (lowered == "33") return superhero::SpwLinkSpeed_33MHz;
  if (lowered == "50") return superhero::SpwLinkSpeed_50MHz;
  if (lowered == "100") return superhero::SpwLinkSpeed_100MHz;
  return std::nullopt;
}

auto shell_quote(const std::string& value) -> std::string {
  std::string quoted = "'";
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted += character;
    }
  }
  quoted += "'";
  return quoted;
}

auto executable_directory() -> std::optional<std::filesystem::path> {
  std::error_code error;
#if defined(__APPLE__)
  uint32_t size = 0;
  (void)_NSGetExecutablePath(nullptr, &size);
  std::vector<char> path(size);
  if (_NSGetExecutablePath(path.data(), &size) != 0) {
    return std::nullopt;
  }
  auto executable = std::filesystem::weakly_canonical(path.data(), error);
#elif defined(__linux__)
  auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
#else
  return std::nullopt;
#endif
  if (error) {
    return std::nullopt;
  }
  return executable.parent_path();
}

auto find_auxiliary_file(const std::string& filename, bool must_be_executable)
    -> std::optional<std::string> {
  std::vector<std::filesystem::path> candidates;
  if (const auto directory = executable_directory(); directory.has_value()) {
    candidates.push_back(*directory / filename);
  }
  candidates.emplace_back(std::filesystem::path("scripts") / filename);
  for (const auto& candidate : candidates) {
    if (std::filesystem::is_regular_file(candidate) &&
        (!must_be_executable || access(candidate.c_str(), X_OK) == 0)) {
      return candidate.string();
    }
  }
  if (must_be_executable &&
      std::system(("command -v " + filename + " >/dev/null 2>&1").c_str()) == 0) {
    return filename;
  }
  return std::nullopt;
}

}  // namespace

void emit_readout_message(const std::string& message, bool error) {
  if (g_interactive_shell && std::this_thread::get_id() != g_shell_thread_id) {
    record_readout_message(message);
    return;
  }
  auto& output = error ? std::cerr : std::cout;
  output << message << "\n";
}

auto do_help(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() == 1) {
    std::cout << "Available commands:\n";
    const std::string* current_category = nullptr;
    for (const auto& info : kCommands) {
      if (!command_available(info)) {
        continue;
      }
      if (!current_category || *current_category != info.category) {
        current_category = &info.category;
        std::cout << "\n" << info.category << ":\n";
      }
      if (shell::stdout_is_tty()) {
        std::cout << "  \033[1m" << std::left << std::setw(20) << info.name << "\033[0m"
                  << info.summary << "\n";
      } else {
        std::cout << "  " << std::left << std::setw(20) << info.name << info.summary << "\n";
      }
    }
    std::cout << "\nType 'help <command>' for more information.\n";
    std::cout << "Use '@<file>' to run a script file, and 'exit'/'quit' to leave the shell.\n";
    return true;
  }
  if (tokens.size() == 2) {
    if (const auto* info = find_command(tokens[1])) {
      std::cout << info->help << "\n";
      return true;
    }
    std::cout << "Unknown command: " << tokens[1] << "\n";
    return false;
  }
  return false;
}

auto do_sleep(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() != 2) {
    do_help({"help", "sleep"});
    return false;
  }
  // A bare number (no unit) means seconds; otherwise accept duration units
  // like 5min, 1h30min, 500ms (same grammar as readout).
  std::string duration_spec = tokens[1];
  if (!duration_spec.empty() &&
      std::isalpha(static_cast<unsigned char>(duration_spec.back())) == 0) {
    duration_spec += "s";
  }

  std::chrono::nanoseconds sleep_duration{};
  try {
    sleep_duration = shell::parse_duration(duration_spec);
  } catch (const std::exception& e) {
    std::cout << "Error parsing duration: " << e.what() << "\n";
    return false;
  }
  double seconds = duration<double>(sleep_duration).count();
  auto deadline = steady_clock::now() + sleep_duration;

  if (seconds >= 3.0) {
    auto next_print = steady_clock::now();
    while (!g_interrupted.load(std::memory_order_relaxed)) {
      auto now = steady_clock::now();
      if (now >= deadline) {
        break;
      }
      if (now >= next_print && shell::stdout_is_tty()) {
        auto remaining = std::chrono::ceil<std::chrono::seconds>(deadline - now);
        auto hrs = remaining.count() / 3600;
        auto mins = (remaining.count() % 3600) / 60;
        auto secs = remaining.count() % 60;
        std::cout << "\rremaining " << std::setfill('0') << std::setw(2) << hrs << ":"
                  << std::setfill('0') << std::setw(2) << mins << ":" << std::setfill('0')
                  << std::setw(2) << secs << std::flush;
        next_print = now + 1s;
      }
      auto chunk = std::min(duration_cast<milliseconds>(deadline - now), 100ms);
      std::this_thread::sleep_for(chunk);
    }
    if (shell::stdout_is_tty()) {
      std::cout << "\r" << std::string(30, ' ') << "\r";
    }
  } else {
    while (!g_interrupted.load(std::memory_order_relaxed)) {
      auto now = steady_clock::now();
      if (now >= deadline) {
        break;
      }
      auto chunk = std::min(duration_cast<milliseconds>(deadline - now), 100ms);
      std::this_thread::sleep_for(chunk);
    }
  }

  if (g_interrupted.load(std::memory_order_relaxed)) {
    std::cout << "Sleep interrupted by SIGINT\n";
    g_interrupted.store(false, std::memory_order_relaxed);
    return false;
  }

  return true;
}

auto do_connect(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() != 2) {
    do_help({"help", "connect"});
    return false;
  }
  std::cout << "Connecting to " << tokens[1] << "...\n";
  // Keep the new channel local until its Echo succeeds so an interrupted
  // connection never leaves the shell in a partially connected state.
  auto channel = grpc::CreateChannel(tokens[1], grpc::InsecureChannelCredentials());
  auto stub = superhero::CommunicationService::NewStub(channel);

  superhero::EchoRequest request;
  request.set_message("Hello, CdTeDE!");
  superhero::EchoReply reply;
  grpc::ClientContext context;
  grpc::CompletionQueue completion_queue;
  auto rpc = stub->AsyncEcho(&context, request, &completion_queue);
  grpc::Status status;
  void* tag = nullptr;
  bool ok = false;
  rpc->Finish(&reply, &status, &tag);

  bool cancelled = false;
  while (true) {
    const auto next_status = completion_queue.AsyncNext(
        &tag, &ok, std::chrono::system_clock::now() + std::chrono::milliseconds(100));
    if (next_status == grpc::CompletionQueue::GOT_EVENT) {
      break;
    }
    if (g_interrupted.load(std::memory_order_relaxed) && !cancelled) {
      context.TryCancel();
      cancelled = true;
    }
  }
  completion_queue.Shutdown();

  if (g_interrupted.load(std::memory_order_relaxed)) {
    std::cout << "Connection interrupted by SIGINT\n";
    g_interrupted.store(false, std::memory_order_relaxed);
    return false;
  }
  if (!ok || !status.ok()) {
    std::cout << "Connection failed: " << status.error_message() << "\n";
    return false;
  }

  g_channel = std::move(channel);
  g_stub = std::move(stub);
  g_current_endpoint = tokens[1];
  refresh_state_after_device_change();
  std::cout << "Connected to " << tokens[1] << "\n";
  return true;
}

auto do_add_detector(const std::vector<std::string>& tokens) -> bool {
  std::vector<uint16_t> target_addresses;
  std::vector<uint16_t> reply_addresses;

  if (tokens.size() < 4 || std::find(tokens.begin(), tokens.end(), "-") == tokens.end()) {
    do_help({"help", "add_detector"});
    return false;
  }

  uint8_t logical_address = 0;
  try {
    logical_address = static_cast<uint16_t>(shell::parse_uint8(tokens[1]));
  } catch (const std::exception& e) {
    std::cout << "Error parsing logical address: " << e.what() << "\n";
    return false;
  }
  if (!ensure_grpc_initialized()) {
    return false;
  }
  auto devices = get_device_logical_addresses();
  if (!devices.has_value()) {
    return false;
  }
  if (std::find(devices->begin(), devices->end(), logical_address) != devices->end()) {
    std::cout << "Device with logical address " << shell::to_hex_string(logical_address)
              << " already exists.\n";
    return false;
  }
  try {
    for (size_t i = 2; i < tokens.size(); ++i) {
      if (tokens[i] == "-") {
        for (size_t j = i + 1; j < tokens.size(); ++j) {
          reply_addresses.push_back(shell::parse_uint8(tokens[j]));
        }
        break;
      } else {
        target_addresses.push_back(shell::parse_uint8(tokens[i]));
      }
    }
  } catch (const std::exception& e) {
    std::cout << "Error parsing addresses: " << e.what() << "\n";
    return false;
  }

  grpc::ClientContext context;
  superhero::AddDetectorRequest request;
  superhero::AddDetectorReply reply;

  request.set_logical_address(logical_address);

  for (const auto& addr : target_addresses) {
    request.add_target_address(addr);
  }
  for (const auto& addr : reply_addresses) {
    request.add_reply_address(addr);
  }

  auto status = g_stub->AddDetector(&context, request, &reply);
  log_grpc_error("AddDetector", status);
  if (!status.ok()) {
    return false;
  }

  if (!reply.accepted()) {
    std::cout << "Failed to add detector: " << reply.message() << "\n";
    return false;
  }
  refresh_state_after_device_change();
  return true;
}

auto do_remove_detector(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() != 2) {
    do_help({"help", "remove_detector"});
    return false;
  }
  uint8_t logical_address = 0;
  try {
    logical_address = static_cast<uint16_t>(shell::parse_uint8(tokens[1]));
  } catch (const std::exception& e) {
    std::cout << "Error parsing logical address: " << e.what() << "\n";
    return false;
  }

  if (!ensure_grpc_initialized()) {
    return false;
  }

  try {
    validate_logical_address(logical_address);
  } catch (const std::exception& e) {
    std::cout << e.what() << "\n";
    return false;
  }

  grpc::ClientContext context;
  superhero::RemoveDetectorRequest request;
  superhero::RemoveDetectorReply reply;

  request.set_logical_address(logical_address);

  auto status = g_stub->RemoveDetector(&context, request, &reply);
  log_grpc_error("RemoveDetector", status);
  if (!status.ok()) {
    return false;
  }

  if (!reply.accepted()) {
    std::cout << "Failed to remove detector: " << reply.message() << "\n";
    return false;
  }
  refresh_state_after_device_change();
  return true;
}

auto do_add_router(const std::vector<std::string>& tokens) -> bool {
  std::vector<uint16_t> target_addresses;
  std::vector<uint16_t> reply_addresses;

  if (tokens.size() < 4 || std::find(tokens.begin(), tokens.end(), "-") == tokens.end()) {
    do_help({"help", "add_router"});
    return false;
  }

  uint8_t logical_address = 0;
  try {
    logical_address = static_cast<uint16_t>(shell::parse_uint8(tokens[1]));
  } catch (const std::exception& e) {
    std::cout << "Error parsing logical address: " << e.what() << "\n";
    return false;
  }

  if (!ensure_grpc_initialized()) {
    return false;
  }
  auto devices = get_device_logical_addresses();
  if (!devices.has_value()) {
    return false;
  }
  if (std::find(devices->begin(), devices->end(), logical_address) != devices->end()) {
    std::cout << "Device with logical address " << shell::to_hex_string(logical_address)
              << " already exists.\n";
    return false;
  }
  try {
    for (size_t i = 2; i < tokens.size(); ++i) {
      if (tokens[i] == "-") {
        for (size_t j = i + 1; j < tokens.size(); ++j) {
          reply_addresses.push_back(shell::parse_uint8(tokens[j]));
        }
        break;
      } else {
        target_addresses.push_back(shell::parse_uint8(tokens[i]));
      }
    }
  } catch (const std::exception& e) {
    std::cout << "Error parsing addresses: " << e.what() << "\n";
    return false;
  }

  grpc::ClientContext context;
  superhero::AddRouterRequest request;
  superhero::AddRouterReply reply;

  request.set_logical_address(logical_address);

  for (const auto& addr : target_addresses) {
    request.add_target_address(addr);
  }
  for (const auto& addr : reply_addresses) {
    request.add_reply_address(addr);
  }

  auto status = g_stub->AddRouter(&context, request, &reply);
  log_grpc_error("AddRouter", status);
  if (!status.ok()) {
    return false;
  }

  if (!reply.accepted()) {
    std::cout << "Failed to add router: " << reply.message() << "\n";
    return false;
  }
  refresh_state_after_device_change();
  return true;
}

auto do_remove_router(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() != 2) {
    do_help({"help", "remove_router"});
    return false;
  }
  uint8_t logical_address = 0;
  try {
    logical_address = static_cast<uint16_t>(shell::parse_uint8(tokens[1]));
  } catch (const std::exception& e) {
    std::cout << "Error parsing logical address: " << e.what() << "\n";
    return false;
  }

  if (!ensure_grpc_initialized()) {
    return false;
  }

  try {
    validate_logical_address(logical_address);
  } catch (const std::exception& e) {
    std::cout << e.what() << "\n";
    return false;
  }

  grpc::ClientContext context;
  superhero::RemoveRouterRequest request;
  superhero::RemoveRouterReply reply;

  request.set_logical_address(logical_address);

  auto status = g_stub->RemoveRouter(&context, request, &reply);
  log_grpc_error("RemoveRouter", status);
  if (!status.ok()) {
    return false;
  }

  if (!reply.accepted()) {
    std::cout << "Failed to remove router: " << reply.message() << "\n";
    return false;
  }
  refresh_state_after_device_change();
  return true;
}

auto do_remove_device(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() != 2) {
    do_help({"help", "remove_device"});
    return false;
  }
  uint8_t logical_address = 0;
  try {
    logical_address = static_cast<uint16_t>(shell::parse_uint8(tokens[1]));
  } catch (const std::exception& e) {
    std::cout << "Error parsing logical address: " << e.what() << "\n";
    return false;
  }

  if (!ensure_grpc_initialized()) {
    return false;
  }

  try {
    validate_logical_address(logical_address);
  } catch (const std::exception& e) {
    std::cout << e.what() << "\n";
    return false;
  }

  grpc::ClientContext context;
  superhero::RemoveDeviceRequest request;
  superhero::RemoveDeviceReply reply;

  request.set_logical_address(logical_address);

  auto status = g_stub->RemoveDevice(&context, request, &reply);
  log_grpc_error("RemoveDevice", status);
  if (!status.ok()) {
    return false;
  }

  if (!reply.accepted()) {
    std::cout << "Failed to remove device: " << reply.message() << "\n";
    return false;
  }
  refresh_state_after_device_change();
  return true;
}

auto do_reconnect_device(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() != 2) {
    do_help({"help", "reconnect_device"});
    return false;
  }

  uint8_t logical_address = 0;
  try {
    logical_address = shell::parse_uint8(tokens[1]);
  } catch (const std::exception& e) {
    std::cout << "Error parsing logical address: " << e.what() << "\n";
    return false;
  }

  if (!ensure_grpc_initialized()) {
    return false;
  }

  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
  superhero::ReconnectDeviceRequest request;
  superhero::ReconnectDeviceReply reply;
  request.set_logical_address(logical_address);

  const auto status = g_stub->ReconnectDevice(&context, request, &reply);
  log_grpc_error("ReconnectDevice", status);
  if (!status.ok()) {
    return false;
  }
  if (!reply.accepted()) {
    std::cout << "Failed to reconnect device " << shell::to_hex_string(logical_address) << ": "
              << reply.message() << "\n";
    return false;
  }

  const auto device_statuses = get_device_statuses();
  if (!device_statuses) {
    std::cout << "Failed to reconnect device " << shell::to_hex_string(logical_address)
              << ": could not verify device status with list_devices.\n";
    return false;
  }
  const auto reconnected_device =
      std::find_if(device_statuses->begin(), device_statuses->end(),
                   [logical_address](const auto& device) {
                     return device.logical_address == logical_address;
                   });
  if (reconnected_device == device_statuses->end()) {
    std::cout << "Failed to reconnect device " << shell::to_hex_string(logical_address)
              << ": device is missing from list_devices.\n";
    return false;
  }
  if (!reconnected_device->enabled) {
    std::cout << "Failed to reconnect device " << shell::to_hex_string(logical_address)
              << ": Status: disabled.\n";
    return false;
  }

  std::cout << "Reconnected device " << shell::to_hex_string(logical_address)
            << ". Status: enabled.\n";
  return true;
}

auto do_remove_all_devices(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() != 1) {
    do_help({"help", "remove_all_devices"});
    return false;
  }
  if (!ensure_grpc_initialized()) {
    return false;
  }
  auto devices = get_device_logical_addresses();
  if (!devices.has_value()) {
    return false;
  }
  for (const auto& addr : *devices) {
    grpc::ClientContext context;
    superhero::RemoveDeviceRequest request;
    superhero::RemoveDeviceReply reply;
    request.set_logical_address(addr);
    auto status = g_stub->RemoveDevice(&context, request, &reply);
    log_grpc_error("RemoveDevice", status);
    if (!status.ok()) {
      return false;
    }
    if (!reply.accepted()) {
      std::cout << "Failed to remove device " << shell::to_hex_string(addr) << ": "
                << reply.message() << "\n";
      return false;
    }
  }
  refresh_state_after_device_change();
  return true;
}

auto do_list_devices(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() != 1) {
    do_help({"help", "list_devices"});
    return false;
  }
  if (!ensure_grpc_initialized()) {
    return false;
  }
  const auto device_statuses = get_device_statuses();
  if (!device_statuses) {
    return false;
  }
  std::cout << "Registered devices:\n";
  for (const auto& device : *device_statuses) {
    std::cout << "  Logical Address: " << shell::to_hex_string(device.logical_address)
              << ", Type: " << device_type_name(device.type)
              << ", Status: " << (device.enabled ? "enabled" : "disabled") << "\n";
  }
  return true;
}

auto do_list_detectors(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() != 1) {
    do_help({"help", "list_detectors"});
    return false;
  }
  if (!ensure_grpc_initialized()) {
    return false;
  }
  const auto device_statuses = get_device_statuses();
  if (!device_statuses) {
    return false;
  }
  std::cout << "Registered detectors:\n";
  for (const auto& device : *device_statuses) {
    if (device.type == superhero::DeviceType_DETECTOR) {
      std::cout << "  Logical Address: " << shell::to_hex_string(device.logical_address)
                << ", Status: " << (device.enabled ? "enabled" : "disabled") << "\n";
    }
  }
  return true;
}

auto do_list_routers(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() != 1) {
    do_help({"help", "list_routers"});
    return false;
  }
  if (!ensure_grpc_initialized()) {
    return false;
  }
  const auto device_statuses = get_device_statuses();
  if (!device_statuses) {
    return false;
  }
  std::cout << "Registered routers:\n";
  for (const auto& device : *device_statuses) {
    if (device.type == superhero::DeviceType_ROUTER) {
      std::cout << "  Logical Address: " << shell::to_hex_string(device.logical_address)
                << ", Status: " << (device.enabled ? "enabled" : "disabled") << "\n";
    }
  }
  return true;
}

auto do_set(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() < 4) {
    do_help({"help", "set"});
    return false;
  }
  std::string address_str = "CdTeDSDAddress_" + tokens[1];
  const google::protobuf::EnumDescriptor* descriptor = superhero::CdTeDSDAddress_descriptor();
  const google::protobuf::EnumValueDescriptor* value_descriptor =
      descriptor->FindValueByName(address_str);
  if (!value_descriptor) {
    std::cout << "Invalid address name: " << tokens[1] << "\n";
    return false;
  }
  auto address = static_cast<superhero::CdTeDSDAddress>(value_descriptor->number());

  if (!ensure_grpc_initialized()) {
    return false;
  }

  std::string logical_spec;
  for (size_t i = 2; i + 1 < tokens.size(); ++i) {
    if (!logical_spec.empty()) {
      logical_spec.push_back(' ');
    }
    logical_spec += tokens[i];
  }
  if (logical_spec.empty()) {
    std::cout << "Logical address is required.\n";
    return false;
  }

  std::vector<uint8_t> logical_addresses;
  try {
    logical_addresses = parse_logical_address_spec(logical_spec);
  } catch (const std::exception& e) {
    std::cout << "Error parsing logical addresses: " << e.what() << "\n";
    return false;
  }

  std::vector<uint8_t> value_bytes;
  try {
    uint32_t value = shell::parse_uint32(tokens.back());
    value_bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    value_bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    value_bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    value_bytes.push_back(static_cast<uint8_t>(value & 0xFF));
  } catch (const std::exception& e) {
    std::cout << "Error parsing value: " << e.what() << "\n";
    return false;
  }

  bool all_ok = true;
  for (auto logical_address : logical_addresses) {
    try {
      validate_logical_address(logical_address);
    } catch (const std::exception& e) {
      std::cout << e.what() << "\n";
      all_ok = false;
      continue;
    }
    try {
      superhero::grpc::rmapWrite(*g_stub, logical_address, address, value_bytes);
    } catch (const std::exception& e) {
      std::cout << "Failed to set parameter for device " << shell::to_hex_string(logical_address)
                << ": " << e.what() << "\n";
      all_ok = false;
    }
  }
  return all_ok;
}

auto do_configure_fpga(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() < 3) {
    do_help({"help", "configure_fpga"});
    return false;
  }
  if (!ensure_grpc_initialized()) {
    return false;
  }
  uint8_t logical_address = 0;
  try {
    logical_address = static_cast<uint8_t>(shell::parse_uint8(tokens[1]));
    validate_logical_address(logical_address);
  } catch (const std::exception& e) {
    std::cout << "Invalid logical address: " << e.what() << "\n";
    return false;
  }

  struct FieldState {
    uint32_t value = 0;
    bool set = false;
  };
  std::map<std::string, FieldState> fields = {
      {"peaking_time_nside", {}},   {"peaking_time_pside", {}},  {"adc_clock_period", {}},
      {"readout_clock_period", {}}, {"readout_clock_delay", {}}, {"trig_patlatch_timing", {}},
      {"reset_wait_time", {}},      {"reset_wait_time2", {}}};

  try {
    for (size_t i = 2; i < tokens.size(); ++i) {
      const auto& token = tokens[i];
      auto eq_pos = token.find('=');
      if (eq_pos == std::string::npos || eq_pos == 0 || eq_pos + 1 >= token.size()) {
        std::cout << "Invalid parameter format, expected key=value: " << token << "\n";
        return false;
      }
      std::string key = token.substr(0, eq_pos);
      std::transform(key.begin(), key.end(), key.begin(),
                     [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); });
      auto it = fields.find(key);
      if (it == fields.end()) {
        std::cout << "Unknown parameter: " << key << "\n";
        return false;
      }
      try {
        it->second.value = shell::parse_uint32(token.substr(eq_pos + 1));
        it->second.set = true;
      } catch (const std::exception& e) {
        std::cout << "Error parsing value for " << key << ": " << e.what() << "\n";
        return false;
      }
    }
  } catch (const std::exception& e) {
    std::cout << "Error parsing parameters: " << e.what() << "\n";
    return false;
  }

  for (const auto& [name, field] : fields) {
    if (!field.set) {
      std::cout << "Missing parameter: " << name << "\n";
      return false;
    }
  }

  ::grpc::ClientContext context;
  ::superhero::ConfigureFPGARequest req;
  ::superhero::ConfigureFPGAReply rep;
  req.set_logical_address(logical_address);
  req.set_peaking_time_nside(fields.at("peaking_time_nside").value);
  req.set_peaking_time_pside(fields.at("peaking_time_pside").value);
  req.set_adc_clock_period(fields.at("adc_clock_period").value);
  req.set_readout_clock_period(fields.at("readout_clock_period").value);
  req.set_readout_clock_delay(fields.at("readout_clock_delay").value);
  req.set_trig_patlatch_timing(fields.at("trig_patlatch_timing").value);
  req.set_reset_wait_time(fields.at("reset_wait_time").value);
  req.set_reset_wait_time2(fields.at("reset_wait_time2").value);

  auto status = g_stub->ConfigureFPGA(&context, req, &rep);
  log_grpc_error("ConfigureFPGA", status);
  if (!status.ok()) {
    return false;
  }
  if (!rep.accepted()) {
    std::cout << "ConfigureFPGA rejected: " << rep.message() << "\n";
    return false;
  }
  return true;
}

auto do_get(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() < 3) {
    do_help({"help", "get"});
    return false;
  }
  std::string address_str = "CdTeDSDAddress_" + tokens[1];
  const google::protobuf::EnumDescriptor* descriptor = superhero::CdTeDSDAddress_descriptor();
  const google::protobuf::EnumValueDescriptor* value_descriptor =
      descriptor->FindValueByName(address_str);
  if (!value_descriptor) {
    std::cout << "Invalid address name: " << tokens[1] << "\n";
    return false;
  }
  auto address = value_descriptor->number();

  if (!ensure_grpc_initialized()) {
    return false;
  }

  std::string logical_spec;
  for (size_t i = 2; i < tokens.size(); ++i) {
    if (!logical_spec.empty()) {
      logical_spec.push_back(' ');
    }
    logical_spec += tokens[i];
  }
  if (logical_spec.empty()) {
    std::cout << "Logical address is required.\n";
    return false;
  }

  std::vector<uint8_t> logical_addresses;
  try {
    logical_addresses = parse_logical_address_spec(logical_spec);
  } catch (const std::exception& e) {
    std::cout << "Error parsing logical addresses: " << e.what() << "\n";
    return false;
  }

  try {
    for (auto logical_address : logical_addresses) {
      validate_logical_address(logical_address);
    }
  } catch (const std::exception& e) {
    std::cout << e.what() << "\n";
    return false;
  }

  if (!ensure_grpc_initialized()) {
    return false;
  }

  for (auto logical_address : logical_addresses) {
    std::vector<uint8_t> data;
    try {
      data = superhero::grpc::rmapRead(*g_stub, logical_address,
                                       static_cast<superhero::CdTeDSDAddress>(address), 4);
    } catch (const std::exception& e) {
      std::cout << "Failed to read RMAP for " << shell::to_hex_string(logical_address) << ": "
                << e.what() << "\n";
      return false;
    }
    std::cout << shell::to_hex_string(logical_address) << ": ";
    if (data.empty()) {
      std::cout << "<no data>\n";
      continue;
    }
    if (data.size() == 4) {
      // Same presentation as `show`: one 32-bit big-endian value.
      uint32_t value = (static_cast<uint32_t>(data[0]) << 24) |
                       (static_cast<uint32_t>(data[1]) << 16) |
                       (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
      std::cout << shell::to_hex_string(value) << "  (" << value << ")\n";
      continue;
    }
    for (unsigned char i : data) {
      std::cout << shell::to_hex_string(i) << " ";
    }
    std::cout << "\n";
  }
  return true;
}

auto do_set_vareg(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() != 3) {
    do_help({"help", "set_vareg"});
    return false;
  }
  uint8_t logical_address = 0;
  try {
    logical_address = shell::parse_uint8(tokens[1]);
  } catch (const std::exception& e) {
    std::cout << "Error parsing logical address: " << e.what() << "\n";
    return false;
  }

  if (!ensure_grpc_initialized()) {
    return false;
  }

  try {
    validate_logical_address(logical_address);
  } catch (const std::exception& e) {
    std::cout << e.what() << "\n";
    return false;
  }
  std::cout << "Setting VAREG for device " << shell::to_hex_string(logical_address) << " from file "
            << tokens[2] << "...\n";
  std::string filename = tokens[2];
  std::ifstream f(filename, std::ios::binary);
  if (!f) {
    std::cout << "Error opening file " << filename << "\n";
    return false;
  }
  std::vector<uint8_t> data;
  try {
    data = shell::base64::base64_decode(
        std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()));
  } catch (const std::exception& e) {
    std::cout << "Error decoding base64 in " << filename << ": " << e.what() << "\n";
    return false;
  }
  if (data.size() != 516) {
    std::cout << "Error: VAREG file must be exactly 516 bytes after base64 decoding, but got "
              << data.size() << " bytes.\n";
    return false;
  }
  std::vector<uint8_t> expected_crc{};
  for (size_t i = 512; i < 516; ++i) {
    expected_crc.push_back(data[i]);
  }
  auto calculated_crc = shell::crc::crc32(data.data(), 512);
  if (calculated_crc !=
      (static_cast<uint32_t>(expected_crc[0]) << 24 | static_cast<uint32_t>(expected_crc[1]) << 16 |
       static_cast<uint32_t>(expected_crc[2]) << 8 | static_cast<uint32_t>(expected_crc[3]))) {
    std::cout << "Error: VAREG file CRC32 mismatch. Expected "
              << shell::to_hex_string(static_cast<uint32_t>(expected_crc[0]) << 24 |
                                      static_cast<uint32_t>(expected_crc[1]) << 16 |
                                      static_cast<uint32_t>(expected_crc[2]) << 8 |
                                      static_cast<uint32_t>(expected_crc[3]))
              << ", but calculated " << shell::to_hex_string(calculated_crc) << ".\n";
    return false;
  }
  grpc::ClientContext context;
  superhero::SetVaRegisterRequest request;
  superhero::SetVaRegisterReply reply;
  request.set_logical_address(logical_address);
  std::vector<uint8_t> payload;
  for (size_t i = 0; i < 512; ++i) {
    payload.push_back(data[i]);
  }
  payload.resize(4096);
  request.set_data(payload.data(), payload.size());
  auto status = g_stub->SetVaRegister(&context, request, &reply);
  log_grpc_error("SetVaRegister", status);
  if (!status.ok()) {
    return false;
  }
  if (!reply.accepted()) {
    std::cout << "Failed to set VAREG: " << reply.message() << "\n";
    return false;
  }
  // Record provenance only after the server accepted the upload.
  g_last_set_vareg_path = tokens[2];
  return true;
}

auto do_show(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() != 2) {
    do_help({"help", "show"});
    return false;
  }

  uint8_t logical_address = 0;
  try {
    logical_address = shell::parse_uint8(tokens[1]);
  } catch (const std::exception& e) {
    std::cout << "Error parsing logical address: " << e.what() << "\n";
    return false;
  }

  if (!ensure_grpc_initialized()) {
    return false;
  }

  try {
    validate_logical_address(logical_address);
  } catch (const std::exception& e) {
    std::cout << e.what() << "\n";
    return false;
  }

  // Size is deduced from the list so it cannot drift from the entry count
  // (a hardcoded size that was too large left zero-initialized VaStatus
  // entries duplicated at the end).
  const ::superhero::CdTeDSDAddress show_addresses[] = {
      ::superhero::CdTeDSDAddress_VaStatus,
      ::superhero::CdTeDSDAddress_ModuleStatus,
      ::superhero::CdTeDSDAddress_VaFlag,
      ::superhero::CdTeDSDAddress_SetUpModeFlag,
      ::superhero::CdTeDSDAddress_ObsmodeFlag,
      ::superhero::CdTeDSDAddress_ForcetrigFlag,
      ::superhero::CdTeDSDAddress_EnableFlag,
      ::superhero::CdTeDSDAddress_ExtSignalModeFlag,
      ::superhero::CdTeDSDAddress_PeakingTime1,
      ::superhero::CdTeDSDAddress_AdcClockPeriod,
      ::superhero::CdTeDSDAddress_ReadOutClockPeriod,
      ::superhero::CdTeDSDAddress_TrigPatLatchTiming,
      ::superhero::CdTeDSDAddress_ResetWaitTime,
      ::superhero::CdTeDSDAddress_ResetWaitTime2,
      ::superhero::CdTeDSDAddress_TiTime,
      ::superhero::CdTeDSDAddress_IntegralLiveTime,
      ::superhero::CdTeDSDAddress_DeadTime,
      ::superhero::CdTeDSDAddress_RmapTest,
      ::superhero::CdTeDSDAddress_CaldTrigReq,
      ::superhero::CdTeDSDAddress_CaldPulseWidth,
      ::superhero::CdTeDSDAddress_CaldPulseVetoWidth,
      ::superhero::CdTeDSDAddress_PeakingTime2,
      ::superhero::CdTeDSDAddress_DRAMWritePointer,
      ::superhero::CdTeDSDAddress_DRAMWritePointerResetReq,
      ::superhero::CdTeDSDAddress_TIUpper32bit,
      ::superhero::CdTeDSDAddress_TILower32bit,
      ::superhero::CdTeDSDAddress_TIUpper32bitNext,
      ::superhero::CdTeDSDAddress_Timecode,
      ::superhero::CdTeDSDAddress_Ext1TIUpper32bit,
      ::superhero::CdTeDSDAddress_Ext1TILower32bit,
      ::superhero::CdTeDSDAddress_Ext2TIUpper32bit,
      ::superhero::CdTeDSDAddress_Ext2TILower32bit,
      ::superhero::CdTeDSDAddress_PseudoONOFF,
      ::superhero::CdTeDSDAddress_PseudoRate,
      ::superhero::CdTeDSDAddress_PseudoCounter,
  };

  for (const auto& addr : show_addresses) {
    std::vector<uint8_t> data;
    try {
      data = superhero::grpc::rmapRead(*g_stub, logical_address, addr, 4);
    } catch (const std::exception& e) {
      std::cout << "Failed to read RMAP: " << e.what() << "\n";
      return false;
    }
    const google::protobuf::EnumDescriptor* descriptor = superhero::CdTeDSDAddress_descriptor();
    const google::protobuf::EnumValueDescriptor* value_descriptor =
        descriptor->FindValueByNumber(static_cast<int>(addr));
    auto name = value_descriptor
                    ? value_descriptor->name().substr(std::string("CdTeDSDAddress_").size())
                    : "Unknown";
    if (data.size() != 4) {
      std::cout << "  " << std::left << std::setw(26) << name << "<unexpected " << data.size()
                << " bytes>\n";
      continue;
    }
    uint32_t value = (static_cast<uint32_t>(data[0]) << 24) |
                     (static_cast<uint32_t>(data[1]) << 16) |
                     (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
    std::cout << "  " << std::left << std::setw(26) << name << shell::to_hex_string(value)
              << "  (" << value << ")\n";
  }

  return true;
}

struct ReadoutSetup {
  std::chrono::nanoseconds duration;
  std::chrono::system_clock::time_point acquisition_time;
  std::vector<uint8_t> detector_addresses;
};

auto prepare_readout(const std::vector<std::string>& tokens) -> std::optional<ReadoutSetup> {
  if (tokens.size() != 3) {
    do_help({"help", "readout"});
    return std::nullopt;
  }

  std::chrono::nanoseconds duration;
  try {
    duration = shell::parse_duration(tokens[1]);
  } catch (const std::exception& e) {
    emit_readout_message("Error parsing duration: " + std::string(e.what()), true);
    return std::nullopt;
  }

  if (!ensure_grpc_initialized()) {
    return std::nullopt;
  }

  auto detector_addresses = get_detector_logical_addresses();
  if (!detector_addresses.has_value()) {
    return std::nullopt;
  }
  if (detector_addresses->empty()) {
    emit_readout_message("No detectors registered for readout.", true);
    return std::nullopt;
  }

  return ReadoutSetup{duration, std::chrono::system_clock::now(), *detector_addresses};
}

auto readout_file_prefix(const std::string& output_prefix, const ReadoutSetup& setup)
    -> std::string {
  return output_prefix + "_" + format_yyMMdd_hhmmss(setup.acquisition_time);
}

void print_readout_outputs(const std::string& file_prefix, const ReadoutSetup& setup,
                           const std::optional<std::string>& register_output = std::nullopt) {
  for (const auto address : setup.detector_addresses) {
    std::cout << "  Data " << shell::to_hex_string(address) << ": " << file_prefix << "_"
              << shell::to_hex_string(address) << "\n";
  }
  std::cout << "  HK: " << file_prefix << "_hk\n";
  if (register_output.has_value()) {
    std::cout << "  Register: " << *register_output << "\n";
  }
}

auto do_readout_foreground(const std::vector<std::string>& tokens,
                           const std::optional<ReadoutSetup>& prepared_setup = std::nullopt)
    -> bool {
  const auto setup = prepared_setup.has_value() ? prepared_setup : prepare_readout(tokens);
  if (!setup.has_value()) {
    return false;
  }

  std::string output_datafileprefix = tokens[2];
  std::map<uint8_t, std::unique_ptr<std::ofstream>> output_datafiles;
  std::unique_ptr<std::ofstream> output_hkfile;

  std::mutex frame_counter_mutex;
  std::map<uint8_t, size_t> frame_counters;
  const auto acquisition_time = setup->acquisition_time;
  const auto duration = setup->duration;
  const std::string file_prefix = readout_file_prefix(output_datafileprefix, *setup);

  const auto acquired_date_value = format_iso8601(acquisition_time);
  std::ostringstream exposure_seconds_stream;
  exposure_seconds_stream << std::chrono::duration<double>(duration).count();
  const auto exposure_seconds_value = exposure_seconds_stream.str();
  auto build_xattr_map =
      [&](std::optional<uint8_t> logical_address) -> std::map<std::string, std::string> {
    std::map<std::string, std::string> attributes;
    attributes["acquired_date"] = acquired_date_value;
    attributes["exposure_sec"] = exposure_seconds_value;
    attributes["logical_address"] =
        logical_address.has_value() ? shell::to_hex_string(logical_address.value()) : "N/A";
    return attributes;
  };

  const std::string hk_filename = file_prefix + "_hk";
  output_hkfile = std::make_unique<std::ofstream>(hk_filename, std::ios::binary);
  if (!output_hkfile->is_open()) {
    emit_readout_message("Failed to open output file: " + hk_filename, true);
    return false;
  }
  apply_xattr_to_file(hk_filename, build_xattr_map(std::nullopt));

  for (const auto& addr : setup->detector_addresses) {
    std::string datafilename = file_prefix + "_" + shell::to_hex_string(addr);
    output_datafiles[addr] = std::make_unique<std::ofstream>(datafilename, std::ios::binary);
    {
      std::lock_guard<std::mutex> lock(frame_counter_mutex);
      frame_counters[addr] = 0;
    }
    if (!output_datafiles[addr]->is_open()) {
      emit_readout_message("Failed to open output file: " + datafilename, true);
      return false;
    }
    apply_xattr_to_file(datafilename, build_xattr_map(addr));
  }
  set_readout_outputs(file_prefix, hk_filename, setup->detector_addresses);
  if (!g_interactive_shell) {
    emit_readout_message("Output data files created with prefix: " + file_prefix);
  }

  {
    std::vector<uint8_t> sorted_addresses = setup->detector_addresses;
    std::sort(sorted_addresses.begin(), sorted_addresses.end());
    std::filesystem::path prefix_path(output_datafileprefix);
    auto parent_dir = prefix_path.parent_path();
    std::string log_filename = parent_dir.empty() ? "log.txt" : (parent_dir / "log.txt").string();
    std::ofstream readout_log(log_filename, std::ios::app);
    if (!readout_log.is_open()) {
      emit_readout_message("Failed to open readout log: " + log_filename, true);
      return false;
    }
    const std::filesystem::path log_base = parent_dir;
    for (const auto& addr : sorted_addresses) {
      const auto datafilename = file_prefix + "_" + shell::to_hex_string(addr);
      bool force_flag = false;
      try {
        auto data = superhero::grpc::rmapRead(
            *g_stub, addr, ::superhero::CdTeDSDAddress_ForcetrigFlag, 4);
        if (data.size() != 4) {
          emit_readout_message("Unexpected ForcetrigFlag size for " +
                                   shell::to_hex_string(addr) + ": " +
                                   std::to_string(data.size()) + " bytes",
                               true);
          return false;
        }
        uint32_t value = (static_cast<uint32_t>(data[0]) << 24) |
                         (static_cast<uint32_t>(data[1]) << 16) |
                         (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
        force_flag = value != 0;
      } catch (const std::exception& e) {
        emit_readout_message("Failed to read ForcetrigFlag for " +
                                 shell::to_hex_string(addr) + ": " + e.what(),
                             true);
        return false;
      }
      std::filesystem::path relative_path = datafilename;
      try {
        const auto base = log_base.empty() ? std::filesystem::path(".") : log_base;
        relative_path = std::filesystem::relative(datafilename, base);
      } catch (const std::exception&) {
        relative_path = datafilename;
      }
      readout_log << relative_path.string() << " " << acquired_date_value << " "
                  << exposure_seconds_value << " " << g_last_set_vareg_path << " "
                  << (force_flag ? "true" : "false") << "\n";
    }
  }

  try {
    ::superhero::StartDataStreamRequest req;
    ::superhero::StartDataStreamReply rep;
    ::grpc::ClientContext context;
    // A background readout must not make `readout stop` or shell exit wait
    // forever if the server becomes unresponsive before the stream starts.
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    const auto status = g_stub->StartDataStream(&context, req, &rep);
    if (!status.ok()) {
      throw std::runtime_error("StartDataStream RPC failed: " + status.error_message());
    }
    if (!rep.accepted()) {
      throw std::runtime_error("Failed to start data stream: " + rep.message());
    }
    std::this_thread::sleep_for(100ms);
  } catch (const std::exception& e) {
    emit_readout_message("Failed to start data stream: " + std::string(e.what()), true);
    return false;
  }

  // Stream context lives outside the reader thread so the main thread can
  // TryCancel() a blocked Read() if the server does not close the stream.
  ::grpc::ClientContext stream_context;
  std::atomic<bool> reader_done{false};
  std::atomic<bool> readout_failed{false};

  auto readout_thread = std::thread([stub = g_stub.get(), &output_datafiles, &frame_counters,
                                     &frame_counter_mutex, &output_hkfile, &stream_context,
                                     &reader_done, &readout_failed]() -> void {
    ::superhero::DataStreamRequest req;
    ::superhero::DataStreamReply rep;

    auto reader = std::unique_ptr<::grpc::ClientReader<::superhero::DataStreamReply>>(
        stub->DataStream(&stream_context, req));
    while (reader->Read(&rep)) {
      const auto logical_address_raw = static_cast<uint32_t>(rep.logical_address());
      if (logical_address_raw > std::numeric_limits<uint8_t>::max()) {
        emit_readout_message("Received DataStream frame with unsupported logical address " +
                                 shell::to_hex_string(logical_address_raw) + ", dropping frame",
                             true);
        continue;
      }
      const auto logical_address = static_cast<uint8_t>(logical_address_raw);

      switch (rep.type()) {
        case superhero::DataStreamType::DataStreamType_FrameData: {
          auto data = rep.value();
          if (data.size() != 32768) {
            emit_readout_message("Received DataStream frame with unexpected size: " +
                                     std::to_string(data.size()) + ", expected: 32768",
                                 true);
            continue;
          }
          {
            std::lock_guard<std::mutex> lock(frame_counter_mutex);
            if (frame_counters.find(logical_address) == frame_counters.end()) {
              emit_readout_message("Received data frame for unregistered logical address " +
                                       shell::to_hex_string(logical_address) +
                                       ", dropping frame data",
                                   true);
              continue;
            }
          }
          auto datafile_it = output_datafiles.find(logical_address);
          if (datafile_it == output_datafiles.end() || !datafile_it->second ||
              !datafile_it->second->is_open()) {
            emit_readout_message("Output file for logical address " +
                                     shell::to_hex_string(logical_address) +
                                     " is not available, dropping frame data",
                                 true);
            continue;
          }
          auto raw_data = data.Flatten();
          *(datafile_it->second) << raw_data;
          *(datafile_it->second) << std::flush;
          if (!datafile_it->second->good()) {
            emit_readout_message("Failed to write frame data for logical address " +
                                     shell::to_hex_string(logical_address),
                                 true);
            readout_failed.store(true, std::memory_order_relaxed);
            continue;
          }
          // Count only frames that were actually written to disk.
          {
            std::lock_guard<std::mutex> lock(frame_counter_mutex);
            frame_counters[logical_address] += 1;
          }
          increment_readout_frame_count(logical_address);
          break;
        }
        case superhero::DataStreamType::DataStreamType_HKData: {
          auto data = rep.value();
          if (data.size() != 1024) {
            emit_readout_message("Received HK DataStream frame with unexpected size: " +
                                     std::to_string(data.size()) + ", expected: 1024",
                                 true);
            continue;
          }
          (*output_hkfile) << data.Flatten() << std::flush;
          if (!output_hkfile->good()) {
            emit_readout_message("Failed to write HK data", true);
            readout_failed.store(true, std::memory_order_relaxed);
          }
          break;
        }
        default: {
          emit_readout_message("Received DataStream frame with unknown type: " +
                                   std::to_string(rep.type()),
                               true);
          continue;
        }
      }
      if (g_readout_stop_requested.load(std::memory_order_relaxed) ||
          g_interrupted.load(std::memory_order_relaxed)) {
        break;
      }
    }
    auto finish_status = reader->Finish();
    if (!finish_status.ok() && finish_status.error_code() != ::grpc::StatusCode::CANCELLED) {
      emit_readout_message("DataStream terminated with error: " + finish_status.error_message(),
                           true);
      readout_failed.store(true, std::memory_order_relaxed);
    }
    reader_done.store(true, std::memory_order_relaxed);
  });

  // Progress display runs on the main thread; it owns the shutdown sequence.
  auto start_time = std::chrono::steady_clock::now();
  mark_readout_started();
  while (std::chrono::steady_clock::now() - start_time < duration) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    // Interactive readout is a background job; emitting a carriage-return
    // progress line there would overwrite the user's current readline input.
    if (shell::stdout_is_tty() && !g_interactive_shell) {
      std::map<uint8_t, size_t> counters_snapshot;
      {
        std::lock_guard<std::mutex> lock(frame_counter_mutex);
        counters_snapshot = frame_counters;
      }
      size_t total_frames = 0;
      for (const auto& [addr, count] : counters_snapshot) {
        total_frames += count;
      }
      std::cout << "\r\tFrames: " << total_frames << " | ";
      for (const auto& [addr, count] : counters_snapshot) {
        std::cout << "Addr " << shell::to_hex_string(addr) << ": " << count << "  ";
      }

      auto elapsed = std::chrono::steady_clock::now() - start_time;
      auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
      auto hours = seconds / 3600;
      auto minutes = (seconds % 3600) / 60;
      seconds = seconds % 60;
      std::cout << "| Elapsed Time: " << std::setfill('0') << std::setw(2) << hours << ":"
                << std::setfill('0') << std::setw(2) << minutes << ":" << std::setfill('0')
                << std::setw(2) << seconds << "  " << std::flush;
    }
    if (g_readout_stop_requested.load(std::memory_order_relaxed) ||
        g_interrupted.load(std::memory_order_relaxed)) {
      if (!g_interactive_shell) {
        std::cout << "\nAcquisition stop requested\n";
      }
      break;
    }
    if (reader_done.load(std::memory_order_relaxed)) {
      if (!g_interactive_shell && shell::stdout_is_tty()) {
        std::cout << "\n";
      }
      emit_readout_message("Data stream closed by server before the requested duration.", true);
      readout_failed.store(true, std::memory_order_relaxed);
      break;
    }
  }
  if (shell::stdout_is_tty() && !g_interactive_shell) {
    std::cout << "\n";
  }
  // Acquisition time only — measured before the stop/cancel sequence.
  const auto acquisition_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - start_time);

  bool stop_ok = true;
  {
    ::superhero::StopDataStreamRequest stop_req;
    ::superhero::StopDataStreamReply stop_rep;
    ::grpc::ClientContext stop_context;
    // Headroom over the server's own internal 5s stop timeout, so we receive
    // its timeout message instead of a bare DEADLINE_EXCEEDED.
    stop_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(7));
    const auto stop_status = g_stub->StopDataStream(&stop_context, stop_req, &stop_rep);
    if (!stop_status.ok()) {
      emit_readout_message("Failed to stop data stream: " + stop_status.error_message(), true);
      stop_ok = false;
    } else if (!stop_rep.accepted()) {
      emit_readout_message("Failed to stop data stream: " + stop_rep.message(), true);
      stop_ok = false;
    }
  }

  // Give the reader a moment to drain, then cancel a possibly stuck Read().
  // TryCancel is harmless if the stream already finished. Note: cancelling a
  // stream that was NOT gracefully stopped can wedge the server in observation
  // mode (CdTeDE known issue C-2) — warn so the operator can check DE status.
  std::this_thread::sleep_for(1s);
  if (!stop_ok) {
    emit_readout_message("Warning: cancelling data stream without a confirmed stop; "
                         "the server may remain in observation mode.",
                         true);
  }
  stream_context.TryCancel();
  readout_thread.join();

  // Final summary: the durable record of foreground/script acquisitions.
  // Interactive readout keeps this data for `readout status` instead, so a
  // background worker never writes over a readline prompt at completion.
  if (!g_interactive_shell) {
    size_t total_frames = 0;
    for (const auto& [addr, count] : frame_counters) {
      total_frames += count;
    }
    std::cout << "Readout summary: " << total_frames << " frames in "
              << acquisition_elapsed.count() << "s\n";
    for (const auto& [addr, count] : frame_counters) {
      std::cout << "  " << shell::to_hex_string(addr) << ": " << count << " frames -> "
                << file_prefix << "_" << shell::to_hex_string(addr) << "\n";
    }
    std::cout << "  HK -> " << hk_filename << "\n";
  }

  if (g_readout_stop_requested.load(std::memory_order_relaxed) ||
      g_interrupted.load(std::memory_order_relaxed)) {
    return false;
  }
  return stop_ok && !readout_failed.load(std::memory_order_relaxed);
}

struct PedcalibSetup {
  ReadoutSetup readout;
  std::string calc_pedestal;
  std::string set_delreg;
  std::string vareg_input;
};

auto prepare_pedcalib(const std::vector<std::string>& tokens) -> std::optional<PedcalibSetup> {
  if (tokens.size() != 4) {
    do_help({"help", "pedcalib_readout"});
    return std::nullopt;
  }

  const auto calc_pedestal = find_auxiliary_file("calc_pedestal", true);
  const auto set_delreg = find_auxiliary_file("set_delreg.py", false);
  const bool calc_pedestal_ready =
      calc_pedestal.has_value() &&
      std::system((shell_quote(*calc_pedestal) + " --check >/dev/null 2>&1").c_str()) == 0;
  const bool python_ready =
      set_delreg.has_value() && std::system("command -v python3 >/dev/null 2>&1") == 0 &&
      std::system(("python3 " + shell_quote(*set_delreg) + " --check >/dev/null 2>&1").c_str()) ==
          0;
  if (!calc_pedestal_ready || !python_ready) {
    std::cerr << "pedcalib_readout requires an executable calc_pedestal and a Python "
                 "environment able to run vareg.py. Build/activate them, then retry.\n";
    return std::nullopt;
  }

  if (g_last_set_vareg_path == "N/A" ||
      !std::filesystem::is_regular_file(g_last_set_vareg_path)) {
    std::cerr << "pedcalib_readout requires a readable VAREG file accepted by set_vareg first.\n";
    return std::nullopt;
  }
  const auto readout = prepare_readout({"readout", tokens[1], tokens[2]});
  if (!readout.has_value()) {
    return std::nullopt;
  }
  if (readout->detector_addresses.size() != 1) {
    std::cerr << "pedcalib_readout requires exactly one registered detector.\n";
    return std::nullopt;
  }

  return PedcalibSetup{*readout, *calc_pedestal, *set_delreg, g_last_set_vareg_path};
}

auto do_pedcalib_readout_foreground(const std::vector<std::string>& tokens,
                                    const PedcalibSetup& setup) -> bool {
  if (!do_readout_foreground({"readout", tokens[1], tokens[2]}, setup.readout)) {
    return false;
  }

  const auto status = readout_status_snapshot();
  const std::string raw_file =
      status.file_prefix + "_" + shell::to_hex_string(setup.readout.detector_addresses.front());
  emit_readout_message("Calculating median pedestals...");
  const std::string command = shell_quote(setup.calc_pedestal) + " " + shell_quote(raw_file) +
                              " 2>/dev/null | python3 " + shell_quote(setup.set_delreg) + " --in " +
                              shell_quote(setup.vareg_input) + " --out " + shell_quote(tokens[3]) +
                              " >/dev/null 2>&1";
  if (std::system(command.c_str()) != 0) {
    emit_readout_message("Pedestal register generation failed for " + raw_file, true);
    return false;
  }
  emit_readout_message("Pedestal register written to " + tokens[3]);
  return true;
}

namespace {

std::mutex g_readout_mutex;
std::thread g_readout_worker;

void join_completed_readout_locked() {
  if (!g_readout_active.load(std::memory_order_relaxed) && g_readout_worker.joinable()) {
    g_readout_worker.join();
  }
}

}  // namespace

auto readout_prompt_progress() -> std::optional<std::string> {
  if (!g_readout_active.load(std::memory_order_relaxed)) {
    return std::nullopt;
  }

  const auto status = readout_status_snapshot();
  if (status.duration <= std::chrono::nanoseconds::zero()) {
    return std::nullopt;
  }

  auto remaining = status.duration;
  if (status.started) {
    const auto elapsed = std::chrono::steady_clock::now() - status.start_time;
    remaining = elapsed < status.duration ? status.duration - elapsed
                                          : std::chrono::nanoseconds::zero();
  }
  return format_prompt_duration(remaining) + "/" + format_prompt_duration(status.duration);
}

auto do_readout(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() == 2 && tokens[1] == "status") {
    const auto status = readout_status_snapshot();
    const bool active = g_readout_active.load(std::memory_order_relaxed);
    const std::string operation =
        status.job_name == "pedcalib_readout" ? "Pedestal calibration" : "Readout";
    const std::string operation_lower =
        status.job_name == "pedcalib_readout" ? "pedestal calibration" : "readout";
    size_t total_frames = 0;
    for (const auto& [_, count] : status.frame_counters) {
      total_frames += count;
    }
    if (!active) {
      std::cout << operation << " is not running.";
      if (status.has_result) {
        if (status.succeeded) {
          std::cout << " Last " << operation_lower << " completed.";
        } else if (status.stop_requested) {
          std::cout << " Last " << operation_lower << " was stopped.";
        } else {
          std::cout << " Last " << operation_lower << " failed.";
        }
      }
      std::cout << "\n";
    } else if (!status.started) {
      std::cout << operation << " is starting.\n";
    } else {
      const auto elapsed = std::chrono::steady_clock::now() - status.start_time;
      const auto remaining = elapsed < status.duration ? status.duration - elapsed
                                                        : std::chrono::nanoseconds::zero();
      std::cout << operation << " is "
                << (g_readout_stop_requested.load(std::memory_order_relaxed) ? "stopping" : "running")
                << ": " << total_frames << " frames | elapsed " << format_elapsed_time(elapsed)
                << " | remaining " << format_elapsed_time(remaining) << "\n";
    }
    if (!status.file_prefix.empty()) {
      std::cout << "  Output prefix: " << status.file_prefix << "\n";
    }
    for (const auto& [addr, count] : status.frame_counters) {
      std::cout << "  " << shell::to_hex_string(addr) << ": " << count << " frames\n";
    }
    if (!status.hk_filename.empty()) {
      std::cout << "  HK: " << status.hk_filename << "\n";
    }
    if (!status.register_filename.empty()) {
      std::cout << "  Register output: " << status.register_filename << "\n";
    }
    if (!status.messages.empty()) {
      std::cout << "  Messages:\n";
      for (const auto& message : status.messages) {
        std::cout << "    " << message << "\n";
      }
    }
    if (status.suppressed_message_count > 0) {
      std::cout << "    " << status.suppressed_message_count
                << " additional messages suppressed\n";
    }
    return true;
  }
  if (tokens.size() == 2 && tokens[1] == "stop") {
    if (!g_readout_active.load(std::memory_order_relaxed)) {
      std::cout << "No acquisition is running.\n";
      return false;
    }
    g_readout_stop_requested.store(true, std::memory_order_relaxed);
    std::cout << "Acquisition stop requested.\n";
    return true;
  }

  if (!g_interactive_shell) {
    return do_readout_foreground(tokens);
  }
  if (tokens.size() != 3) {
    do_help({"help", "readout"});
    return false;
  }

  std::lock_guard<std::mutex> lock(g_readout_mutex);
  if (g_readout_active.load(std::memory_order_relaxed)) {
    std::cout << "An acquisition is already running. Use 'readout status' or 'readout stop'.\n";
    return false;
  }
  join_completed_readout_locked();
  const auto setup = prepare_readout(tokens);
  if (!setup.has_value()) {
    return false;
  }
  const auto file_prefix = readout_file_prefix(tokens[2], *setup);
  reset_readout_status(setup->duration);
  set_readout_outputs(file_prefix, file_prefix + "_hk", setup->detector_addresses);
  g_readout_stop_requested.store(false, std::memory_order_relaxed);
  g_readout_active.store(true, std::memory_order_relaxed);
  g_readout_worker = std::thread([tokens, setup = *setup]() {
    const bool success = do_readout_foreground(tokens, setup);
    finish_readout_status(success, g_readout_stop_requested.load(std::memory_order_relaxed));
    g_readout_active.store(false, std::memory_order_relaxed);
  });
  std::cout << "Readout started in the background. Use 'readout status' or 'readout stop'.\n";
  print_readout_outputs(file_prefix, *setup);
  return true;
}

auto do_pedcalib_readout(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() == 2 && (tokens[1] == "status" || tokens[1] == "stop")) {
    return do_readout({"readout", tokens[1]});
  }

  if (!g_interactive_shell) {
    const auto setup = prepare_pedcalib(tokens);
    if (!setup.has_value()) {
      return false;
    }
    reset_readout_status(setup->readout.duration, "pedcalib_readout");
    set_readout_register_output(tokens[3]);
    g_readout_stop_requested.store(false, std::memory_order_relaxed);
    const bool success = do_pedcalib_readout_foreground(tokens, *setup);
    finish_readout_status(success, g_readout_stop_requested.load(std::memory_order_relaxed));
    return success;
  }

  std::lock_guard<std::mutex> lock(g_readout_mutex);
  if (g_readout_active.load(std::memory_order_relaxed)) {
    std::cout << "An acquisition is already running. Use 'pedcalib_readout status' or "
                 "'pedcalib_readout stop'.\n";
    return false;
  }
  join_completed_readout_locked();
  const auto setup = prepare_pedcalib(tokens);
  if (!setup.has_value()) {
    return false;
  }

  const auto file_prefix = readout_file_prefix(tokens[2], setup->readout);
  reset_readout_status(setup->readout.duration, "pedcalib_readout");
  set_readout_outputs(file_prefix, file_prefix + "_hk", setup->readout.detector_addresses);
  set_readout_register_output(tokens[3]);
  g_readout_stop_requested.store(false, std::memory_order_relaxed);
  g_readout_active.store(true, std::memory_order_relaxed);
  g_readout_worker = std::thread([tokens, setup = *setup]() {
    const bool success = do_pedcalib_readout_foreground(tokens, setup);
    finish_readout_status(success, g_readout_stop_requested.load(std::memory_order_relaxed));
    g_readout_active.store(false, std::memory_order_relaxed);
  });
  std::cout << "Pedestal calibration started in the background. Use 'pedcalib_readout status' "
               "or 'pedcalib_readout stop'.\n";
  print_readout_outputs(file_prefix, setup->readout, tokens[3]);
  return true;
}

void shutdown_readout() {
  std::thread worker;
  {
    std::lock_guard<std::mutex> lock(g_readout_mutex);
    if (!g_readout_worker.joinable()) {
      return;
    }
    if (g_readout_active.load(std::memory_order_relaxed)) {
      std::cout << "Stopping active acquisition before exit...\n";
      g_readout_stop_requested.store(true, std::memory_order_relaxed);
    }
    worker = std::move(g_readout_worker);
  }
  worker.join();
}

auto do_set_linkspeed(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() != 2) {
    do_help({"help", "set_linkspeed"});
    return false;
  }
  if (!ensure_grpc_initialized()) {
    return false;
  }
  auto target_speed = parse_link_speed_token(tokens[1]);
  if (!target_speed.has_value()) {
    std::cout << "Unsupported link speed: " << tokens[1] << " (valid: 10/20/25/33/50/100 MHz)"
              << std::endl;
    return false;
  }

  ::grpc::ClientContext context;
  ::superhero::SetLinkSpeedRequest req;
  ::superhero::SetLinkSpeedReply rep;
  req.set_speed(target_speed.value());
  auto status = g_stub->SetLinkSpeed(&context, req, &rep);
  log_grpc_error("SetLinkSpeed", status);
  if (!status.ok()) {
    std::cout << "SetLinkSpeed RPC failed: " << status.error_message() << "\n";
    return false;
  }
  if (!rep.accepted()) {
    std::cout << "Failed to set link speed: " << rep.message() << "\n";
    return false;
  }
  return true;
}
