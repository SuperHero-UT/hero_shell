#include "commands.hh"

#include <google/protobuf/descriptor.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <hlcommands.grpc.pb.h>
#include <hlcommands.pb.h>
#include <superhero.grpc.pb.h>
#include <superhero.pb.h>
#include <sys/xattr.h>

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

auto ensure_grpc_initialized() -> bool {
  if (!g_stub) {
    std::cout
        << "gRPC is not initialized. please connect to gRPC sever using `connect` command\n";
    return false;
  }
  return true;
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

auto format_yyMMdd_hhmm(std::chrono::system_clock::time_point tp) -> string {
  std::time_t t = std::chrono::system_clock::to_time_t(tp);
  std::tm tm{};
  localtime_r(&t, &tm);

  std::ostringstream oss;
  oss << std::put_time(&tm, "%y%m%d-%H%M");
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
      std::cerr << "Failed to set xattr '" << attr_name << "' on " << path << ": "
                << std::strerror(errno) << "\n";
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

}  // namespace

auto do_help(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() == 1) {
    std::cout << "Available commands:\n";
    for (const auto& [states, command] : kCommandList) {
      if (std::find(states.begin(), states.end(), g_current_state) != states.end()) {
        std::cout << "  \033[1m" << command << "\033[0m\n";
      } else {
        std::cout << "  " << command << "\n";
      }
    }
    std::cout << "Type 'help <command>' for more information.\n";
    return true;
  }
  if (tokens.size() == 2) {
    auto command = tokens[1];
    auto it = kHelps.find(command);
    if (it != kHelps.end()) {
      std::cout << it->second << "\n";
      return true;
    }
    std::cout << "Unknown command: " << command << "\n";
    return false;
  }
  return false;
}

auto do_sleep(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() != 2) {
    do_help({"help", "sleep"});
    return false;
  }
  double seconds = 0.0;
  try {
    seconds = std::stod(tokens[1]);
  } catch (const std::exception& e) {
    std::cout << "Error parsing duration: " << e.what() << "\n";
    return false;
  }

  auto sleep_duration = duration_cast<milliseconds>(duration<double>(seconds));
  auto deadline = steady_clock::now() + sleep_duration;

  if (seconds >= 3.0) {
    while (!g_interrupted.load(std::memory_order_relaxed)) {
      auto now = steady_clock::now();
      if (now >= deadline) {
        break;
      }
      auto remaining = duration_cast<std::chrono::seconds>(deadline - now);
      auto hrs = remaining.count() / 3600;
      auto mins = (remaining.count() % 3600) / 60;
      auto secs = remaining.count() % 60;
      std::cout << "\rremaining " << std::setfill('0') << std::setw(2) << hrs << ":"
                << std::setfill('0') << std::setw(2) << mins << ":" << std::setfill('0')
                << std::setw(2) << secs << std::flush;
      std::this_thread::sleep_for(1s);
    }
    std::cout << "\r" << std::string(30, ' ') << "\r";
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
  }

  return true;
}

auto do_connect(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() != 2) {
    do_help({"help", "connect"});
    return false;
  }
  std::cout << "Connecting to " << tokens[1] << "...\n";
  g_channel = grpc::CreateChannel(tokens[1], grpc::InsecureChannelCredentials());
  g_stub = superhero::CommunicationService::NewStub(g_channel);
  try {
    superhero::grpc::echo(*g_stub, "Hello, CdTeDE!");
    std::this_thread::sleep_for(100ms);
  } catch (const std::exception& e) {
    std::cout << "Connection failed: " << e.what() << "\n";
    return false;
  }
  g_current_endpoint = tokens[1];
  refresh_state_after_device_change();
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
    std::cout << "Error parsing logical address: " << e.what() << "\n";
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
    std::cout << "Error parsing logical address: " << e.what() << "\n";
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
    std::cout << "Error parsing logical address: " << e.what() << "\n";
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
  std::cout << "Connected detectors:\n";
  if (const auto detectors = get_detector_logical_addresses()) {
    for (const auto& addr : *detectors) {
      std::cout << "  Logical Address: " << shell::to_hex_string(addr) << "\n";
    }
  }
  std::cout << "Connected routers:\n";
  if (const auto routers = get_router_logical_addresses()) {
    for (const auto& addr : *routers) {
      std::cout << "  Logical Address: " << shell::to_hex_string(addr) << "\n";
    }
  }
  return true;
}

auto do_list_detectors(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() != 1) {
    do_help({"help", "list_devices"});
    return false;
  }
  if (!ensure_grpc_initialized()) {
    return false;
  }
  std::cout << "Connected devices:\n";
  if (const auto detectors = get_detector_logical_addresses()) {
    for (const auto& addr : *detectors) {
      std::cout << "  Logical Address: " << shell::to_hex_string(addr) << "\n";
    }
  }
  return true;
}

auto do_list_routers(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() != 1) {
    do_help({"help", "list_devices"});
    return false;
  }
  if (!ensure_grpc_initialized()) {
    return false;
  }
  std::cout << "Connected devices:\n";
  if (const auto routers = get_router_logical_addresses()) {
    for (const auto& addr : *routers) {
      std::cout << "  Logical Address: " << shell::to_hex_string(addr) << "\n";
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
    int value = shell::parse_uint32(tokens.back());
    value_bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    value_bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    value_bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    value_bytes.push_back(static_cast<uint8_t>(value & 0xFF));
  } catch (const std::exception& e) {
    std::cout << "Error parsing value: " << e.what() << "\n";
    return false;
  }

  for (auto logical_address : logical_addresses) {
    try {
      validate_logical_address(logical_address);
    } catch (const std::exception& e) {
      std::cout << e.what() << "\n";
      continue;
    }
    try {
      superhero::grpc::rmapWrite(*g_stub, logical_address, address, value_bytes);
    } catch (const std::exception& e) {
      std::cout << "Failed to set parameter for device " << shell::to_hex_string(logical_address)
                << ": " << e.what() << "\n";
    }
  }
  return true;
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
    std::cout << "Error parsing logical address: " << e.what() << "\n";
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
    std::cout << "Error parsing logical address: " << e.what() << "\n";
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
  auto data = shell::base64::base64_decode(
      std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()));
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
  log_grpc_error("SetVaRegister", g_stub->SetVaRegister(&context, request, &reply));
  if (!reply.accepted()) {
    std::cout << "Failed to set VAREG: " << reply.message() << "\n";
    return false;
  }
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
    std::cout << "Error parsing logical address: " << e.what() << "\n";
    return false;
  }

  std::array<::superhero::CdTeDSDAddress, 37> show_addresses = {
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
      std::cout << "\t" << name << ":\t<unexpected " << data.size() << " bytes>\n";
      continue;
    }
    uint32_t value = (static_cast<uint32_t>(data[0]) << 24) |
                     (static_cast<uint32_t>(data[1]) << 16) |
                     (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
    std::cout << "\t" << name << ":\t0x" << shell::to_hex_string(value) << "\n";
  }

  return true;
}

auto do_readout(const std::vector<std::string>& tokens) -> bool {
  if (tokens.size() != 3) {
    do_help({"help", "readout"});
    return false;
  }

  std::chrono::nanoseconds duration;
  try {
    duration = shell::parse_duration(tokens[1]);
  } catch (const std::exception& e) {
    std::cout << "Error parsing duration: " << e.what() << "\n";
    return false;
  }

  if (!ensure_grpc_initialized()) {
    return false;
  }

  std::string output_datafileprefix = tokens[2];
  std::map<uint8_t, std::unique_ptr<std::ofstream>> output_datafiles;
  std::unique_ptr<std::ofstream> output_hkfile;

  std::mutex frame_counter_mutex;
  std::map<uint8_t, size_t> frame_counters;
  auto acquisition_time = std::chrono::system_clock::now();
  std::string file_prefix = output_datafileprefix + "_" + format_yyMMdd_hhmm(acquisition_time);

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
    std::cout << "Failed to open output file: " << hk_filename << "\n";
    return false;
  }
  apply_xattr_to_file(hk_filename, build_xattr_map(std::nullopt));

  auto detector_addresses = get_detector_logical_addresses();
  if (!detector_addresses.has_value()) {
    return false;
  }
  for (const auto& addr : *detector_addresses) {
    std::string datafilename = file_prefix + "_" + shell::to_hex_string(addr);
    output_datafiles[addr] = std::make_unique<std::ofstream>(datafilename, std::ios::binary);
    {
      std::lock_guard<std::mutex> lock(frame_counter_mutex);
      frame_counters[addr] = 0;
    }
    if (!output_datafiles[addr]->is_open()) {
      std::cout << "Failed to open output file: " << datafilename << "\n";
      return false;
    }
    apply_xattr_to_file(datafilename, build_xattr_map(addr));
  }
  std::cout << "\tOutput data files created with prefix: " << file_prefix << "\n";

  {
    std::vector<uint8_t> sorted_addresses = *detector_addresses;
    std::sort(sorted_addresses.begin(), sorted_addresses.end());
    std::ofstream readout_log("readoutlog.txt", std::ios::app);
    if (!readout_log.is_open()) {
      std::cout << "Failed to open readout log: readoutlog.txt\n";
      return false;
    }
    const std::filesystem::path log_base = std::filesystem::path("readoutlog.txt").parent_path();
    for (const auto& addr : sorted_addresses) {
      const auto datafilename = file_prefix + "_" + shell::to_hex_string(addr);
      std::filesystem::path relative_path = datafilename;
      try {
        const auto base = log_base.empty() ? std::filesystem::path(".") : log_base;
        relative_path = std::filesystem::relative(datafilename, base);
      } catch (const std::exception&) {
        relative_path = datafilename;
      }
      readout_log << relative_path.string() << " " << exposure_seconds_value << " "
                  << acquired_date_value << "\n";
    }
  }

  try {
    ::superhero::StartDataStreamRequest req;
    ::superhero::StartDataStreamReply rep;
    ::grpc::ClientContext context;
    const auto status = g_stub->StartDataStream(&context, req, &rep);
    log_grpc_error("StartDataStream", status);
    if (!status.ok()) {
      throw std::runtime_error("StartDataStream RPC failed: " + status.error_message());
    }
    if (!rep.accepted()) {
      throw std::runtime_error("Failed to start data stream: " + rep.message());
    }
    std::this_thread::sleep_for(100ms);
  } catch (const std::exception& e) {
    std::cout << "Failed to start data stream: " << e.what() << "\n";
    return false;
  }

  auto readout_thread = std::thread([stub = g_stub.get(), &output_datafiles, &frame_counters,
                                     &frame_counter_mutex, &output_hkfile]() -> void {
    ::superhero::DataStreamRequest req;
    ::superhero::DataStreamReply rep;
    ::grpc::ClientContext context;

    auto reader = std::unique_ptr<::grpc::ClientReader<::superhero::DataStreamReply>>(
        stub->DataStream(&context, req));
    while (reader->Read(&rep)) {
      const auto logical_address_raw = static_cast<uint32_t>(rep.logical_address());
      if (logical_address_raw > std::numeric_limits<uint8_t>::max()) {
        std::cerr << "Received DataStream frame with unsupported logical address "
                  << shell::to_hex_string(logical_address_raw) << ", dropping frame" << std::endl;
        continue;
      }
      const auto logical_address = static_cast<uint8_t>(logical_address_raw);

      switch (rep.type()) {
        case superhero::DataStreamType::DataStreamType_FrameData: {
          auto data = rep.value();
          if (data.size() != 32768) {
            std::cerr << "Received DataStream frame with unexpected size: " << data.size()
                      << ", expected: 32768" << std::endl;
            continue;
          }
          bool counter_updated = false;
          {
            std::lock_guard<std::mutex> lock(frame_counter_mutex);
            auto counter_it = frame_counters.find(logical_address);
            if (counter_it != frame_counters.end()) {
              counter_it->second += 1;
              counter_updated = true;
            }
          }
          if (!counter_updated) {
            std::cerr << "Received data frame for unregistered logical address "
                      << shell::to_hex_string(logical_address) << ", dropping frame data"
                      << std::endl;
            continue;
          }
          auto datafile_it = output_datafiles.find(logical_address);
          if (datafile_it == output_datafiles.end() || !datafile_it->second ||
              !datafile_it->second->is_open()) {
            std::cerr << "Output file for logical address " << shell::to_hex_string(logical_address)
                      << " is not available, dropping frame data" << std::endl;
            continue;
          }
          auto raw_data = data.Flatten();
          *(datafile_it->second) << raw_data;
          break;
        }
        case superhero::DataStreamType::DataStreamType_HKData: {
          auto data = rep.value();
          if (data.size() != 1024) {
            std::cerr << "Received HK DataStream frame with unexpected size: " << data.size()
                      << ", expected: 1024" << std::endl;
            continue;
          }
          (*output_hkfile) << data.Flatten();
          break;
        }
        default: {
          std::cerr << "Received DataStream frame with unknown type: " << rep.type() << std::endl;
          continue;
        }
      }
      if (g_interrupted.load(std::memory_order_relaxed)) {
        break;
      }
    }
    auto finish_status = reader->Finish();
    log_grpc_error("DataStream", finish_status);
    if (!finish_status.ok()) {
      std::cerr << "DataStream terminated with error: " << finish_status.error_message() << "\n";
    }
  });

  auto status_thread = std::thread([&frame_counters, duration, &frame_counter_mutex]() -> void {
    auto start_time = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start_time < duration) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      std::lock_guard<std::mutex> lock(frame_counter_mutex);
      size_t total_frames = 0;
      for (const auto& [addr, count] : frame_counters) {
        total_frames += count;
      }
      std::cout << "\r\tFrames: " << total_frames << " | ";
      for (const auto& [addr, count] : frame_counters) {
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
      if (g_interrupted.load(std::memory_order_relaxed)) {
        std::cout << "\nReadout interrupted by SIGINT\n";
        break;
      }
    }
    std::cout << "\n";
    ::superhero::StopDataStreamRequest stop_req;
    ::superhero::StopDataStreamReply stop_rep;
    ::grpc::ClientContext stop_context;
    const auto stop_status = g_stub->StopDataStream(&stop_context, stop_req, &stop_rep);
    log_grpc_error("StopDataStream", stop_status);
    if (!stop_status.ok()) {
      std::cerr << "Failed to stop data stream: " << stop_status.error_message() << "\n";
    } else if (!stop_rep.accepted()) {
      std::cerr << "Failed to stop data stream: " << stop_rep.message() << "\n";
    }
  });

  status_thread.join();
  readout_thread.join();
  std::this_thread::sleep_for(std::chrono::seconds(1));
  return true;
}

auto do_set_hv([[maybe_unused]] const std::vector<std::string>& tokens) -> bool {
  std::cout << "set_hv command not implemented yet.\n";
  return true;
}

auto do_get_hv([[maybe_unused]] const std::vector<std::string>& tokens) -> bool {
  std::cout << "get_hv command not implemented yet.\n";
  return true;
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
