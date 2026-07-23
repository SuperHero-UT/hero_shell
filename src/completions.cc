#include "completions.hh"

#include <google/protobuf/descriptor.h>
#include <editline/readline.h>
#include <superhero.grpc.pb.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "hero_shell_state.hh"
#include "shell_utils.hh"

namespace {

struct AddressSpec {
  superhero::CdTeDSDAddress proto_addr;
  bool readable;
  bool writable;
  int length;
};

constexpr std::array<AddressSpec, 52> kAddressSpecs = {{
    {superhero::CdTeDSDAddress_VaStatus, true, false, 4},
    {superhero::CdTeDSDAddress_ModuleStatus, true, false, 4},
    {superhero::CdTeDSDAddress_VaFlag, true, true, 4},
    {superhero::CdTeDSDAddress_SetUpModeFlag, true, true, 4},
    {superhero::CdTeDSDAddress_ObsmodeFlag, true, true, 4},
    {superhero::CdTeDSDAddress_ForcetrigFlag, true, true, 4},
    {superhero::CdTeDSDAddress_EnableFlag, true, true, 4},
    {superhero::CdTeDSDAddress_ExtSignalModeFlag, true, true, 4},
    {superhero::CdTeDSDAddress_PeakingTime1, true, true, 4},
    {superhero::CdTeDSDAddress_AdcClockPeriod, true, true, 4},
    {superhero::CdTeDSDAddress_ReadOutClockPeriod, true, true, 4},
    {superhero::CdTeDSDAddress_FastGoLatchTimingAddress, false, false, 0},
    {superhero::CdTeDSDAddress_HitPatLatchTimingAddress, false, false, 0},
    {superhero::CdTeDSDAddress_TrigPatLatchTiming, true, true, 4},
    {superhero::CdTeDSDAddress_ResetWaitTime, true, true, 4},
    {superhero::CdTeDSDAddress_ResetWaitTime2, true, true, 4},
    {superhero::CdTeDSDAddress_TiTime, true, false, 4},
    {superhero::CdTeDSDAddress_IntegralLiveTime, true, false, 4},
    {superhero::CdTeDSDAddress_DeadTime, true, false, 4},
    {superhero::CdTeDSDAddress_RmapTest, true, true, 4},
    {superhero::CdTeDSDAddress_CaldTrigReq, true, true, 4},
    {superhero::CdTeDSDAddress_CaldPulseWidth, true, true, 4},
    {superhero::CdTeDSDAddress_CaldPulseVetoWidth, true, true, 4},
    {superhero::CdTeDSDAddress_HV, false, false, 0},
    {superhero::CdTeDSDAddress_HVValue, false, false, 0},
    {superhero::CdTeDSDAddress_CoinTrigMode, false, false, 0},
    {superhero::CdTeDSDAddress_PeakingTime2, true, true, 4},
    {superhero::CdTeDSDAddress_DRAMWritePointer, true, false, 4},
    {superhero::CdTeDSDAddress_DRAMWritePointerResetReq, true, true, 4},
    {superhero::CdTeDSDAddress_AlmostFullSize, true, true, 4},
    {superhero::CdTeDSDAddress_TIUpper32bit, true, false, 4},
    {superhero::CdTeDSDAddress_TILower32bit, true, false, 4},
    {superhero::CdTeDSDAddress_TIUpper32bitNext, true, true, 4},
    {superhero::CdTeDSDAddress_Timecode, true, false, 4},
    {superhero::CdTeDSDAddress_Ext1TIUpper32bit, true, false, 4},
    {superhero::CdTeDSDAddress_Ext1TILower32bit, true, false, 4},
    {superhero::CdTeDSDAddress_Ext2TIUpper32bit, true, false, 4},
    {superhero::CdTeDSDAddress_Ext2TILower32bit, true, false, 4},
    {superhero::CdTeDSDAddress_PseudoONOFF, true, true, 4},
    {superhero::CdTeDSDAddress_PseudoRate, true, true, 4},
    {superhero::CdTeDSDAddress_PseudoCounter, true, false, 4},
    {superhero::CdTeDSDAddress_VaRegPointer, false, true, 4096},
    {superhero::CdTeDSDAddress_VaRegReadBackPointer, true, false, 4096},
    {superhero::CdTeDSDAddress_TimeCodeLookupTable, true, false, 72},
    {superhero::CdTeDSDAddress_HVDacExec, true, true, 4},
    {superhero::CdTeDSDAddress_HVDacValue, true, true, 4},
    {superhero::CdTeDSDAddress_Port1ConfigAddress, true, true, 4},
    {superhero::CdTeDSDAddress_Port6ConfigAddress, true, true, 4},
    {superhero::CdTeDSDAddress_Port7ConfigAddress, true, true, 4},
    {superhero::CdTeDSDAddress_Port8ConfigAddress, true, true, 4},
    {superhero::CdTeDSDAddress_Port9ConfigAddress, true, true, 4},
    {superhero::CdTeDSDAddress_Port10ConfigAddress, true, true, 4},
}};

constexpr const AddressSpec* find_address_spec(superhero::CdTeDSDAddress addr) {
  for (const auto& spec : kAddressSpecs) {
    if (spec.proto_addr == addr) {
      return &spec;
    }
  }
  return nullptr;
}

auto completion_generator(const char* /* text */, int state) -> char* {
  if (state < static_cast<int>(g_candidate.size())) {
    return strdup(g_candidate[state].c_str());
  }
  return nullptr;
}

[[maybe_unused]] auto address_completion(const char* text) -> char** {
  const google::protobuf::EnumDescriptor* descriptor = superhero::CdTeDSDAddress_descriptor();

  for (int i = 0; i < descriptor->value_count(); ++i) {
    const google::protobuf::EnumValueDescriptor* v = descriptor->value(i);
    auto name = v->name().substr(std::string("CdTeDSDAddress_").size());
    if (name.find(text) == 0) {
      g_candidate.emplace_back(name);
    }
  }
  return rl_completion_matches(text, completion_generator);
}

auto read_address_completion(const char* text) -> char** {
  const google::protobuf::EnumDescriptor* descriptor = superhero::CdTeDSDAddress_descriptor();

  for (int i = 0; i < descriptor->value_count(); ++i) {
    const google::protobuf::EnumValueDescriptor* v = descriptor->value(i);
    auto val = static_cast<superhero::CdTeDSDAddress>(v->number());
    if (const auto* spec = find_address_spec(val)) {
      if (spec->readable && spec->length == 4) {
        auto name = v->name().substr(std::string("CdTeDSDAddress_").size());
        if (name.find(text) == 0) {
          g_candidate.emplace_back(name);
        }
      }
    }
  }
  return rl_completion_matches(text, completion_generator);
}

auto write_address_completion(const char* text) -> char** {
  const google::protobuf::EnumDescriptor* descriptor = superhero::CdTeDSDAddress_descriptor();

  for (int i = 0; i < descriptor->value_count(); ++i) {
    const google::protobuf::EnumValueDescriptor* v = descriptor->value(i);
    auto val = static_cast<superhero::CdTeDSDAddress>(v->number());
    if (const auto* spec = find_address_spec(val)) {
      if (spec->writable && spec->length == 4) {
        auto name = v->name().substr(std::string("CdTeDSDAddress_").size());
        if (name.find(text) == 0) {
          g_candidate.emplace_back(name);
        }
      }
    }
  }
  return rl_completion_matches(text, completion_generator);
}

auto device_address_completion(const char* text) -> char** {
  if (const auto devices = get_device_logical_addresses()) {
    for (const auto& addr : *devices) {
      auto addr_str = shell::to_hex_string(addr);
      if (addr_str.find(text) == 0) {
        g_candidate.emplace_back(addr_str);
      }
    }
  }
  rl_attempted_completion_over = 1;
  return rl_completion_matches(text, completion_generator);
}

auto detector_address_completion(const char* text) -> char** {
  if (const auto detectors = get_detector_logical_addresses()) {
    for (const auto& addr : *detectors) {
      auto addr_str = shell::to_hex_string(addr);
      if (addr_str.find(text) == 0) {
        g_candidate.emplace_back(addr_str);
      }
    }
  }
  rl_attempted_completion_over = 1;
  return rl_completion_matches(text, completion_generator);
}

auto router_address_completion(const char* text) -> char** {
  if (const auto routers = get_router_logical_addresses()) {
    for (const auto& addr : *routers) {
      auto addr_str = shell::to_hex_string(addr);
      if (addr_str.find(text) == 0) {
        g_candidate.emplace_back(addr_str);
      }
    }
  }
  rl_attempted_completion_over = 1;
  return rl_completion_matches(text, completion_generator);
}

auto help_command_completion(const char* text) -> char** {
  for (const auto& info : kCommands) {
    if (info.name.find(text) == 0) {
      g_candidate.emplace_back(info.name);
    }
  }
  rl_attempted_completion_over = 1;
  return rl_completion_matches(text, completion_generator);
}

auto set_get_device_completion(const char* text) -> char** {
  if (const auto devices = get_device_logical_addresses()) {
    for (const auto& addr : *devices) {
      auto addr_str = shell::to_hex_string(addr);
      if (addr_str.find(text) == 0) {
        g_candidate.emplace_back(addr_str);
      }
    }
  }
  static constexpr std::string_view kAllLiteral = "[all]";
  if (kAllLiteral.find(text) == 0) {
    g_candidate.emplace_back(kAllLiteral);
  }
  rl_attempted_completion_over = 1;
  return rl_completion_matches(text, completion_generator);
}

auto configure_fpga_key_completion(const char* text) -> char** {
  static constexpr std::array<const char*, 8> kKeys{
      "peaking_time_nside",   "peaking_time_pside",  "adc_clock_period",
      "readout_clock_period", "readout_clock_delay", "trig_patlatch_timing",
      "reset_wait_time",      "reset_wait_time2"};
  const std::string_view current_text(text);
  if (current_text.find('=') != std::string_view::npos) {
    // Already has a value part; do not offer key= or filename completions.
    rl_attempted_completion_over = 1;
    return rl_completion_matches(text, completion_generator);
  }
  for (const auto* key : kKeys) {
    if (std::string_view(key).find(current_text) == 0) {
      g_candidate.emplace_back(std::string(key) + "=");
    }
  }
  rl_attempted_completion_over = 1;
  rl_completion_append_character = '\0';
  return rl_completion_matches(text, completion_generator);
}

auto link_speed_completion(const char* text) -> char** {
  static constexpr std::array<const char*, 6> kOptions{"10MHz", "20MHz", "25MHz",
                                                       "33MHz", "50MHz", "100MHz"};
  for (const auto* opt : kOptions) {
    if (std::string_view(opt).find(text) == 0) {
      g_candidate.emplace_back(opt);
    }
  }
  rl_attempted_completion_over = 1;
  return rl_completion_matches(text, completion_generator);
}

auto command_completion(const char* text) -> char** {
  for (const auto& info : kCommands) {
    if (command_available(info) && info.name.find(text) == 0) {
      g_candidate.emplace_back(info.name);
    }
  }
  rl_attempted_completion_over = 1;
  return rl_completion_matches(text, completion_generator);
}

}  // namespace

auto repl_completion(const char* text, int start, int end) -> char** {
  g_candidate.clear();
  // Reset the '=' trick from a previous configure_fpga completion so it does
  // not leak into unrelated completions later in the session.
  rl_completion_append_character = ' ';

  if (start == 0) {
    return command_completion(text);
  }

  const auto current_command = shell::split_shell_like(rl_line_buffer);
  const auto argc = current_command.size();
  const bool starting_new_token = (end == start);
  const bool editing_token = (end > start);

  // arg_index_is(n): true when the token being completed is the n-th
  // argument (1-based, excluding the command name itself), i.e. either a
  // brand-new token being started right after n-1 existing arguments, or an
  // in-progress edit of the n-th argument.
  auto arg_index_is = [&](std::size_t n) -> bool {
    return (argc == n && starting_new_token) || (argc == n + 1 && editing_token);
  };

  if (!current_command.empty()) {
    const auto& command = current_command[0];

    if (arg_index_is(1) &&
        (command == "remove_device" || command == "set_vareg" || command == "show")) {
      return device_address_completion(text);
    }

    if (arg_index_is(1) && command == "remove_detector") {
      return detector_address_completion(text);
    }

    if (arg_index_is(1) && command == "remove_router") {
      return router_address_completion(text);
    }

    if (arg_index_is(1) && command == "get") {
      return read_address_completion(text);
    }
    if (arg_index_is(1) && command == "set") {
      return write_address_completion(text);
    }
    if (arg_index_is(2) && (command == "set" || command == "get")) {
      return set_get_device_completion(text);
    }

    if (arg_index_is(2) && command == "set_vareg") {
      return rl_completion_matches(text, rl_filename_completion_function);
    }

    if (arg_index_is(2) && command == "readout") {
      return rl_completion_matches(text, rl_filename_completion_function);
    }

    if (arg_index_is(1) && command == "set_linkspeed") {
      return link_speed_completion(text);
    }

    if (arg_index_is(1) && command == "configure_fpga") {
      return device_address_completion(text);
    }
    if (command == "configure_fpga" &&
        ((argc >= 2 && starting_new_token) || (argc >= 3 && editing_token))) {
      return configure_fpga_key_completion(text);
    }

    if (arg_index_is(1) && command == "help") {
      return help_command_completion(text);
    }
  }

  if (g_candidate.empty() && start > 0 && rl_line_buffer[start - 1] == '@') {
    return rl_completion_matches(text, rl_filename_completion_function);
  }

  rl_attempted_completion_over = 1;
  return rl_completion_matches(text, completion_generator);
}
