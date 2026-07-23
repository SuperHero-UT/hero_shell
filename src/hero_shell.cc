#include <grpcpp/client_context.h>
#include <editline/readline.h>
#include <superhero.grpc.pb.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "commands.hh"
#include "completions.hh"
#include "hero_shell_state.hh"
#include "shell_utils.hh"

std::atomic<bool> g_interrupted{false};
std::shared_ptr<grpc::Channel> g_channel = nullptr;
std::unique_ptr<superhero::CommunicationService::Stub> g_stub = nullptr;
ShellState g_current_state = ShellState::IDLE;
std::string g_current_endpoint{};
int g_router_count = 0;
int g_detector_count = 0;
std::vector<std::string> g_candidate{};
namespace {
const std::vector<ShellState> kAllStates = {ShellState::IDLE, ShellState::CONNECTED,
                                            ShellState::DEVICE_ADDED};
const std::vector<ShellState> kConnectedStates = {ShellState::CONNECTED, ShellState::DEVICE_ADDED};
const std::vector<ShellState> kDeviceStates = {ShellState::DEVICE_ADDED};
}  // namespace

const std::vector<CommandInfo> kCommands = {
    {"help", "General", kAllStates, "Show this list, or details for one command",
     R"(Usage: help [command]
  Show the list of available commands, or details for one command.
  Typical workflow:
    1. connect <host:port> to open the gRPC channel to CdTeDE.
    2. add_detector <logical> then answer target/reply prompts so HL knows the detector path.
    3. (Optional) get_hv / set_hv <logical> <raw> to inspect or ramp the HV DAC.
    4. set <parameter> <logical> <value> to configure the detector parameters.
    5. readout <duration> <file.bin> to start/stop HL data streaming and capture frames.
  A line starting with '@' runs a script file: @myscript.txt
  Example: help set)"},
    {"sleep", "General", kAllStates, "Pause the shell for a duration",
     R"(Usage: sleep <seconds>
  Pause the shell for the given number of seconds (Ctrl-C interrupts).
  Example: sleep 5)"},
    {"exit", "General", kAllStates, "Terminate the shell",
     "Usage: exit\n  Terminate the shell. 'quit' is an alias."},
    {"quit", "General", kAllStates, "Terminate the shell",
     "Usage: quit\n  Terminate the shell. 'exit' is an alias."},

    {"connect", "Connection", {ShellState::IDLE}, "Open the gRPC channel to the server",
     R"(Usage: connect <host:port>
  Open the gRPC channel to the CdTeDE server.
  Example: connect localhost:50051)"},

    {"add_detector", "Device Management", kConnectedStates,
     "Register a detector by logical address",
     R"(Usage: add_detector <logical_address> target_addresses... - reply_addresses...
  Register a detector at <logical_address> with its SpaceWire target and reply paths.
  Example: add_detector 0x35 0x02 0x35 - 0x03 0xFE)"},
    {"remove_detector", "Device Management", kConnectedStates, "Remove a registered detector",
     "Usage: remove_detector <logical_address>\n  Remove a registered detector."},
    {"add_router", "Device Management", kConnectedStates, "Register a router by logical address",
     R"(Usage: add_router <logical_address> target_addresses... - reply_addresses...
  Register a router at <logical_address> with its SpaceWire target and reply paths.
  Example: add_router 0x01 0x02 - 0x03)"},
    {"remove_router", "Device Management", kConnectedStates, "Remove a registered router",
     "Usage: remove_router <logical_address>\n  Remove a registered router."},
    {"remove_device", "Device Management", kConnectedStates, "Remove a registered device",
     "Usage: remove_device <logical_address>\n  Remove a registered device (detector or router)."},
    {"remove_all_devices", "Device Management", kConnectedStates, "Remove every registered device",
     "Usage: remove_all_devices\n  Remove every registered detector and router."},
    {"list_devices", "Device Management", kDeviceStates, "List all registered devices",
     "Usage: list_devices\n  List logical addresses of all registered devices."},
    {"list_detectors", "Device Management", kDeviceStates, "List registered detectors",
     "Usage: list_detectors\n  List logical addresses of registered detectors."},
    {"list_routers", "Device Management", kDeviceStates, "List registered routers",
     "Usage: list_routers\n  List logical addresses of registered routers."},

    {"set", "Configuration", kDeviceStates, "Write a register on device(s)",
     R"(Usage: set <address> <logical|[addr,...]|[all]> <value>
  Write <value> to register <address> on the given device(s).
  Example: set PeakingTime1 0x35 100)"},
    {"get", "Configuration", kDeviceStates, "Read a register from device(s)",
     R"(Usage: get <address> <logical|[addr,...]|[all]>
  Read register <address> from the given device(s).
  Example: get PeakingTime1 [all])"},
    {"configure_fpga", "Configuration", kDeviceStates, "Configure FPGA timing parameters",
     R"(Usage: configure_fpga <logical> key=value...
  Configure all FPGA timing parameters for a detector in one RPC. Required keys:
    peaking_time_nside=<value> peaking_time_pside=<value>
    adc_clock_period=<value> readout_clock_period=<value>
    readout_clock_delay=<value> trig_patlatch_timing=<value>
    reset_wait_time=<value> reset_wait_time2=<value>
  Example: configure_fpga 0x35 peaking_time_nside=10 peaking_time_pside=10 adc_clock_period=8 readout_clock_period=8 readout_clock_delay=2 trig_patlatch_timing=4 reset_wait_time=100 reset_wait_time2=100)"},
    {"set_vareg", "Configuration", kDeviceStates, "Upload a VAREG image to a detector",
     R"(Usage: set_vareg <logical> <filename>
  Upload a base64-encoded VAREG image (516 bytes decoded, CRC32-checked) to a detector.
  Example: set_vareg 0x35 vareg_default.b64)"},
    {"set_linkspeed", "Configuration", kConnectedStates, "Change the SpaceWire link speed",
     R"(Usage: set_linkspeed <10MHz|20MHz|25MHz|33MHz|50MHz|100MHz>
  Change the SpaceWire link speed.
  Example: set_linkspeed 50MHz)"},
    {"set_hv", "Configuration", kDeviceStates, "Ramp the HV DAC",
     "Usage: set_hv <logical> <raw>\n  Ramp the HV DAC. (Not implemented yet.)"},
    {"get_hv", "Configuration", kDeviceStates, "Inspect the HV DAC",
     "Usage: get_hv <logical>\n  Inspect the HV DAC. (Not implemented yet.)"},

    {"show", "Data Acquisition", kDeviceStates, "Dump status/timing registers",
     "Usage: show <logical>\n  Dump the common status/timing registers for a device."},
    {"readout", "Data Acquisition", kDeviceStates, "Start/stop HL data streaming",
     R"(Usage: readout <duration> <output_file_prefix>
  Start HL data streaming for <duration>, writing per-detector and HK files.
  <duration> accepts combined units, e.g. 10s, 90min, 1h30min.
  Example: readout 1h30min run001)"},
};

auto find_command(const std::string& name) -> const CommandInfo* {
  for (const auto& info : kCommands) {
    if (info.name == name) {
      return &info;
    }
  }
  return nullptr;
}

auto command_available(const CommandInfo& info) -> bool {
  return std::find(info.states.begin(), info.states.end(), g_current_state) != info.states.end();
}

namespace {

constexpr const char* kHistoryFile = ".hero_shell_history";
// True once run_shell has loaded the history; guards against clobbering the
// history file with an empty list when `exit` runs in argv-script mode.
bool g_history_loaded = false;

void handle_sigint(int) {
  g_interrupted.store(true, std::memory_order_relaxed);
}

// Installed without SA_RESTART so a blocked readline read() returns EINTR.
void install_sigint_handler() {
  struct sigaction sa{};
  sa.sa_handler = handle_sigint;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
}

void rstrip(std::string& line) {
  auto pos = line.find_last_not_of(" \t\r");
  if (pos == std::string::npos) {
    line.clear();
  } else {
    line.erase(pos + 1);
  }
}

auto consume_line_continuation(std::string& line) -> bool {
  auto pos = line.find_last_not_of(" \t\r");
  if (pos == std::string::npos) {
    line.clear();
    return false;
  }
  if (line[pos] != '\\') {
    return false;
  }
  line.erase(pos);
  rstrip(line);
  return true;
}

}  // namespace

void log_grpc_error(const std::string& api, const grpc::Status& status) {
  if (!status.ok()) {
    std::cout << "[" << api << "] RPC failed: " << status.error_message()
              << " (code=" << status.error_code() << ")\n";
  }
}

namespace {

// The proto carries logical addresses as uint32, but SpaceWire RMAP logical
// addresses are one byte. Validate at the server boundary instead of silently
// truncating (0x101 must not alias 0x01).
template <typename RepeatedField>
auto to_uint8_addresses(const std::string& api, const RepeatedField& raw_addresses)
    -> std::vector<uint8_t> {
  std::vector<uint8_t> addresses;
  for (const auto& addr : raw_addresses) {
    if (static_cast<uint32_t>(addr) > 0xFFu) {
      std::cerr << "[" << api << "] Ignoring out-of-range logical address " << addr
                << " (must fit in one byte)\n";
      continue;
    }
    addresses.push_back(static_cast<uint8_t>(addr));
  }
  return addresses;
}

}  // namespace

auto get_detector_logical_addresses() -> std::optional<std::vector<uint8_t>> {
  if (!g_stub) {
    return std::nullopt;
  }
  grpc::ClientContext context;
  superhero::GetDetectorListRequest request;
  superhero::GetDetectorListReply reply;
  auto status = g_stub->GetDetectorList(&context, request, &reply);
  log_grpc_error("GetDetectorList", status);
  if (!status.ok()) {
    return std::nullopt;
  }
  return to_uint8_addresses("GetDetectorList", reply.logical_address());
}

auto get_router_logical_addresses() -> std::optional<std::vector<uint8_t>> {
  if (!g_stub) {
    return std::nullopt;
  }
  grpc::ClientContext context;
  superhero::GetRouterListRequest request;
  superhero::GetRouterListReply reply;
  auto status = g_stub->GetRouterList(&context, request, &reply);
  log_grpc_error("GetRouterList", status);
  if (!status.ok()) {
    return std::nullopt;
  }
  return to_uint8_addresses("GetRouterList", reply.logical_address());
}

auto get_device_logical_addresses() -> std::optional<std::vector<uint8_t>> {
  if (!g_stub) {
    return std::nullopt;
  }
  grpc::ClientContext context;
  superhero::GetDeviceListRequest request;
  superhero::GetDeviceListReply reply;
  auto status = g_stub->GetDeviceList(&context, request, &reply);
  log_grpc_error("GetDeviceList", status);
  if (!status.ok()) {
    return std::nullopt;
  }
  return to_uint8_addresses("GetDeviceList", reply.logical_address());
}

void update_device_counts() {
  if (!g_stub) {
    g_router_count = 0;
    g_detector_count = 0;
    return;
  }
  auto routers = get_router_logical_addresses();
  auto detectors = get_detector_logical_addresses();
  g_router_count = routers ? static_cast<int>(routers->size()) : 0;
  g_detector_count = detectors ? static_cast<int>(detectors->size()) : 0;
}

void refresh_state_after_device_change() {
  update_device_counts();
  if (!g_stub) {
    g_current_state = ShellState::IDLE;
    g_current_endpoint.clear();
    return;
  }
  if (g_detector_count > 0 || g_router_count > 0) {
    g_current_state = ShellState::DEVICE_ADDED;
  } else {
    g_current_state = ShellState::CONNECTED;
  }
}

auto build_prompt() -> PromptInfo {
  auto wrap_nonprinting = [](const char* code) -> std::string {
    std::string result;
    result.push_back('\001');  // RL_PROMPT_START_IGNORE
    result += code;
    result.push_back('\002');  // RL_PROMPT_END_IGNORE
    return result;
  };

  const std::string bold_on_display = "\033[1m";
  const std::string bold_off_display = "\033[0m";
  const std::string router_color_display = "\033[32m";
  const std::string detector_color_display = "\033[34m";

  const std::string bold_on_readline = wrap_nonprinting("\033[1m");
  const std::string bold_off_readline = wrap_nonprinting("\033[0m");
  const std::string router_color_readline = wrap_nonprinting("\033[32m");
  const std::string detector_color_readline = wrap_nonprinting("\033[34m");

  PromptInfo prompt{};
  if (g_current_state == ShellState::IDLE || g_current_endpoint.empty()) {
    const std::string plain_prompt = "hero_shell[-]> ";
    prompt.visible_length = plain_prompt.size();
    prompt.display_text =
        bold_on_display + "hero_shell[" + router_color_display + "-" + bold_off_display + "]> ";
    prompt.readline_text =
        bold_on_readline + "hero_shell[" + router_color_readline + "-" + bold_off_readline + "]> ";
    return prompt;
  }

  std::string plain_prompt = "hero_shell[" + g_current_endpoint + "(" +
                             std::to_string(g_router_count) + "," +
                             std::to_string(g_detector_count) + ")]> ";
  prompt.visible_length = plain_prompt.size();
  prompt.display_text = bold_on_display + "hero_shell[" + g_current_endpoint + "(" +
                        router_color_display + std::to_string(g_router_count) + bold_off_display +
                        "," + detector_color_display + std::to_string(g_detector_count) +
                        bold_off_display + ")]> ";
  prompt.readline_text = bold_on_readline + "hero_shell[" + g_current_endpoint + "(" +
                         router_color_readline + std::to_string(g_router_count) +
                         bold_off_readline + "," + detector_color_readline +
                         std::to_string(g_detector_count) + bold_off_readline + ")]> ";
  return prompt;
}

auto load_script(const std::string& filename, int depth) -> bool;
auto execute_command(const std::string& line, int depth) -> bool;

auto load_script(const std::string& filename, int depth) -> bool {
  std::ifstream script_file(filename);
  if (!script_file.is_open()) {
    std::cout << "Failed to open script file: " << filename << "\n";
    return false;
  }
  std::string raw_line;
  int line_number = 0;
  while (std::getline(script_file, raw_line)) {
    ++line_number;
    const int command_line = line_number;
    if (g_interrupted.load(std::memory_order_relaxed)) {
      std::cout << "^C\n";
      g_interrupted.store(false, std::memory_order_relaxed);
      return false;
    }
    if (!raw_line.empty() && raw_line.back() == '\r') {
      raw_line.pop_back();
    }
    std::string line = raw_line;
    while (consume_line_continuation(line)) {
      std::string continuation;
      if (!std::getline(script_file, continuation)) {
        std::cout << filename << ":" << line_number
                  << ": unexpected EOF while reading continued command\n";
        break;
      }
      ++line_number;
      if (!continuation.empty() && continuation.back() == '\r') {
        continuation.pop_back();
      }
      if (!line.empty()) {
        line.push_back(' ');
      }
      line += continuation;
    }
    auto token = shell::split_shell_like(line);
    if (token.empty() || token[0].empty() || token[0][0] == '#') {
      continue;
    }
    const auto prompt = build_prompt();
    std::cout << prompt.display_text << line << "\n";
    if (!execute_command(line, depth + 1)) {
      std::cout << filename << ":" << command_line << ": error executing: " << line << "\n";
      return false;
    }
  }
  return true;
}

auto execute_command(const std::string& line, int depth) -> bool {
  if (depth > 10) {
    std::cout << "Maximum script recursion depth (10) exceeded.\n";
    return false;
  }
  auto tokens = shell::split_shell_like(line);
  if (tokens.empty() || tokens[0].empty()) {
    return true;
  }
  if (tokens[0] == "help") {
    return do_help(tokens);
  }
  if (tokens[0] == "sleep") {
    return do_sleep(tokens);
  }
  if (tokens[0] == "connect") {
    return do_connect(tokens);
  }

  if (tokens[0] == "add_detector") {
    return do_add_detector(tokens);
  }
  if (tokens[0] == "add_router") {
    return do_add_router(tokens);
  }

  if (tokens[0] == "remove_detector") {
    return do_remove_detector(tokens);
  }
  if (tokens[0] == "remove_router") {
    return do_remove_router(tokens);
  }
  if (tokens[0] == "remove_device") {
    return do_remove_device(tokens);
  }

  if (tokens[0] == "remove_all_devices") {
    return do_remove_all_devices(tokens);
  }
  if (tokens[0] == "list_devices") {
    return do_list_devices(tokens);
  }
  if (tokens[0] == "list_detectors") {
    return do_list_detectors(tokens);
  }
  if (tokens[0] == "list_routers") {
    return do_list_routers(tokens);
  }

  if (tokens[0] == "set") {
    return do_set(tokens);
  }
  if (tokens[0] == "configure_fpga") {
    return do_configure_fpga(tokens);
  }
  if (tokens[0] == "get") {
    return do_get(tokens);
  }
  if (tokens[0] == "set_vareg") {
    return do_set_vareg(tokens);
  }
  if (tokens[0] == "show") {
    return do_show(tokens);
  }
  if (tokens[0] == "readout") {
    return do_readout(tokens);
  }
  if (tokens[0] == "set_linkspeed") {
    return do_set_linkspeed(tokens);
  }
  // if (tokens[0] == "set_hv") {
  //   return do_set_hv(tokens);
  // }
  // if (tokens[0] == "get_hv") {
  //   return do_get_hv(tokens);
  // }
  if (tokens[0][0] == '#') {
    return true;
  }
  if (tokens[0][0] == '@') {
    std::string script_file = tokens[0].substr(1);
    return load_script(script_file, depth);
  }
  if (tokens[0] == "exit" || tokens[0] == "quit") {
    // std::exit skips local destructors, so persist history explicitly.
    if (g_history_loaded) {
      write_history(kHistoryFile);
    }
    std::exit(0);
  }

  std::cout << "Unknown command: " << tokens[0] << ". Type 'help' to see available commands.\n";
  return false;
}

auto run_shell() -> int {
  rl_catch_signals = 0;

  rl_attempted_completion_function = repl_completion;
  rl_variable_bind("show-all-if-ambiguous", "on");

  shell::defer write_history_on_exit([]() -> void { write_history(kHistoryFile); });

  using_history();
  read_history(kHistoryFile);
  stifle_history(1000);
  g_history_loaded = true;

  while (true) {
    g_interrupted.store(false, std::memory_order_relaxed);
    errno = 0;

    const auto prompt = build_prompt();
    const std::string& main_prompt = prompt.readline_text;
    const std::string continuation_prompt =
        (prompt.visible_length > 2 ? std::string(prompt.visible_length - 2, ' ') : std::string()) +
        "> ";

    char* raw_input = readline(main_prompt.c_str());
    shell::defer free_input([raw_input]() -> void {
      if (raw_input) {
        free(raw_input);
      }
    });
    if (!raw_input) {
      if (g_interrupted.load(std::memory_order_relaxed)) {
        std::cout << "^C\n";
        g_interrupted.store(false, std::memory_order_relaxed);
        continue;
      }
      std::cout << "\n";
      break;
    }
    std::string line(raw_input);
    while (consume_line_continuation(line)) {
      char* continued_input = readline(continuation_prompt.c_str());
      if (!continued_input) {
        if (g_interrupted.load(std::memory_order_relaxed)) {
          std::cout << "^C\n";
          g_interrupted.store(false, std::memory_order_relaxed);
        } else {
          std::cout << "\n";
        }
        line.clear();
        break;
      }
      std::string continued_line(continued_input);
      free(continued_input);
      if (!line.empty()) {
        line.push_back(' ');
      }
      line += continued_line;
    }
    if (line.empty()) {
      continue;
    }
    add_history(line.c_str());
    execute_command(line, 0);
  }

  return 0;
}

auto main(int argc, char** argv) -> int {
  install_sigint_handler();
  if (argc > 1) {
    std::string script_file = argv[1];
    if (!execute_command("@" + script_file, 0)) {
      std::cerr << "Error executing script file: " << script_file << "\n";
      return 1;
    }
    return 0;
  }

  return run_shell();
}
