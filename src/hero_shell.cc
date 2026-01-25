#include <grpcpp/client_context.h>
#include <editline/readline.h>
#include <superhero.grpc.pb.h>

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
const std::vector<std::pair<std::vector<ShellState>, std::string>> kCommandList = {
    {{ShellState::IDLE, ShellState::CONNECTED, ShellState::DEVICE_ADDED}, "help"},
    {{ShellState::IDLE, ShellState::CONNECTED, ShellState::DEVICE_ADDED}, "sleep"},
    {{ShellState::IDLE}, "connect"},
    {{ShellState::CONNECTED, ShellState::DEVICE_ADDED}, "add_detector"},
    {{ShellState::CONNECTED, ShellState::DEVICE_ADDED}, "remove_detector"},
    {{ShellState::CONNECTED, ShellState::DEVICE_ADDED}, "add_router"},
    {{ShellState::CONNECTED, ShellState::DEVICE_ADDED}, "remove_router"},
    {{ShellState::CONNECTED, ShellState::DEVICE_ADDED}, "remove_all_devices"},
    {{ShellState::DEVICE_ADDED}, "list_devices"},
    {{ShellState::DEVICE_ADDED}, "set"},
    {{ShellState::DEVICE_ADDED}, "configure_fpga"},
    {{ShellState::DEVICE_ADDED}, "get"},
    {{ShellState::DEVICE_ADDED}, "set_vareg"},
    {{ShellState::DEVICE_ADDED}, "show"},
    {{ShellState::DEVICE_ADDED}, "readout"},
    {{ShellState::CONNECTED, ShellState::DEVICE_ADDED}, "set_linkspeed"},
    {{ShellState::DEVICE_ADDED}, "set_hv"},
    {{ShellState::DEVICE_ADDED}, "get_hv"},
};

const std::map<std::string, std::string> kHelps = {
    {"help",
     R"(
1. connect <host:port> to open the gRPC channel to CdTeDE.
2. add_detector <logical> then answer target/reply prompts so HL knows the detector path.
3. (Optional) get_hv / set_hv <logical> <raw> to inspect or ramp the HV DAC.
4. set <logical> <parameter> <value> to configure the detector parameters.
5. readout <duration> <file.bin> to start/stop HL data streaming and capture frames;
   use rmap_read/write for ad-hoc LL pokes (addresses from cdtedsd_address.hh).)"},

    {"sleep", "Usage: sleep <seconds>"},
    {"connect", "Usage: connect <host:port>"},

    {"add_detector",
     "Usage: add_detector <logical_address> target_addresses... - reply_addresses..."},
    {"add_router", "Usage: add_router <logical_address> target_addresses... - reply_addresses..."},

    {"remove_detector", "Usage: remove_detector <logical_address>"},
    {"remove_router", "Usage: remove_router <logical_address>"},
    {"remove_device", "Usage: remove_device <logical_address>"},

    {"remove_all_devices", "Usage: remove_all_devices"},

    {"list_detectors", "Usage: list_detectors"},
    {"list_routers", "Usage: list_routers"},
    {"list_devices", "Usage: list_devices"},

    {"set", "Usage: set <address> <logical|[addr,...]|[all]> <value>"},
    {"configure_fpga",
     R"(Usage: configure_fpga <logical>
  peaking_time_nside=<value> peaking_time_pside=<value>
  adc_clock_period=<value> readout_clock_period=<value>
  readout_clock_delay=<value> trip_patlatch_timing=<value>
  reset_wait_time=<value> reset_wait_time2=<value>)"},
    {"get", "Usage: get <address> <logical|[addr,...]|[all]>"},
    {"set_vareg", "Usage: set_vareg <logical> <filename with {}>"},
    {"show", "Usage: show <logical>"},
    {"readout", "Usage: readout <duration_seconds> <output_file.bin>"},
    {"set_linkspeed", "Usage: set_linkspeed <10MHz|20MHz|25MHz|33MHz|50MHz|100MHz>"},
};

namespace {

void handle_sigint(int) {
  g_interrupted.store(true, std::memory_order_relaxed);
  rl_done = 1;
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
  std::vector<uint8_t> addresses;
  for (const auto& addr : reply.logical_address()) {
    addresses.push_back(addr);
  }
  return addresses;
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
  std::vector<uint8_t> addresses;
  for (const auto& addr : reply.logical_address()) {
    addresses.push_back(addr);
  }
  return addresses;
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
  std::vector<uint8_t> addresses;
  for (const auto& addr : reply.logical_address()) {
    addresses.push_back(addr);
  }
  return addresses;
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

auto load_script(const std::string& filename) -> bool;
auto execute_command(const std::string& line, int depth) -> bool;

auto load_script(const std::string& filename) -> bool {
  std::ifstream script_file(filename);
  if (!script_file.is_open()) {
    std::cout << "Failed to open script file: " << filename << "\n";
    return true;
  }
  std::string raw_line;
  while (std::getline(script_file, raw_line)) {
    if (g_interrupted.load(std::memory_order_relaxed)) {
      std::cout << "^C\n";
      g_interrupted.store(false, std::memory_order_relaxed);
      break;
    }
    if (!raw_line.empty() && raw_line.back() == '\r') {
      raw_line.pop_back();
    }
    std::string line = raw_line;
    while (consume_line_continuation(line)) {
      std::string continuation;
      if (!std::getline(script_file, continuation)) {
        std::cout << "Unexpected EOF while reading continued command\n";
        break;
      }
      if (!continuation.empty() && continuation.back() == '\r') {
        continuation.pop_back();
      }
      if (!line.empty()) {
        line.push_back(' ');
      }
      line += continuation;
    }
    auto token = shell::split_shell_like(line);
    if (token.empty() || token[0][0] == '#') {
      continue;
    }
    const auto prompt = build_prompt();
    std::cout << prompt.display_text << line << "\n";
    if (!execute_command(line, 1)) {
      std::cout << "Error executing command in script: " << line << "\n";
      break;
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
  if (tokens.empty()) {
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
    return load_script(script_file);
  }
  if (tokens[0] == "exit" || tokens[0] == "quit") {
    std::exit(0);
  }

  std::cout << "Unknown command: " << tokens[0] << "\n";
  return false;
}

auto run_shell() -> int {
  rl_catch_signals = 0;
  std::signal(SIGINT, handle_sigint);

  rl_attempted_completion_function = repl_completion;
  rl_variable_bind("show-all-if-ambiguous", "on");

  const char* history_file = ".hero_shell_history";
  shell::defer write_history_on_exit([history_file]() -> void { write_history(history_file); });

  using_history();
  read_history(history_file);

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

  write_history(history_file);
  return 0;
}

auto main(int argc, char** argv) -> int {
  if (argc > 1) {
    std::string script_file = argv[1];
    if (!execute_command("@" + script_file, 0)) {
      std::cerr << "Error executing script file: " << script_file << "\n";
      return 1;
    }
  }

  return run_shell();
}
