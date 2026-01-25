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
  for (const auto& [states, command] : kCommandList) {
    if (std::find(states.begin(), states.end(), g_current_state) != states.end() &&
        command.find(text) == 0) {
      g_candidate.emplace_back(command);
    }
  }
  rl_attempted_completion_over = 1;
  return rl_completion_matches(text, completion_generator);
}

}  // namespace

auto repl_completion(const char* text, int start, int end) -> char** {
  g_candidate.clear();

  if (start == 0) {
    return command_completion(text);
  }

  const auto current_command = shell::split_shell_like(rl_line_buffer);
  const auto argc = current_command.size();
  const bool starting_new_token = (end == start);
  const bool editing_token = (end > start);

  if (((argc == 1 && starting_new_token) || (argc == 2 && editing_token)) &&
      !current_command.empty() &&
      (current_command[0] == "remove_device" || current_command[0] == "set_vareg" ||
       current_command[0] == "show")) {
    return device_address_completion(text);
  }

  if (((argc == 1 && starting_new_token) || (argc == 2 && editing_token)) &&
      !current_command.empty() && current_command[0] == "remove_detector") {
    return detector_address_completion(text);
  }

  if (((argc == 1 && starting_new_token) || (argc == 2 && editing_token)) &&
      !current_command.empty() && current_command[0] == "remove_router") {
    return router_address_completion(text);
  }

  if (((argc == 1 && starting_new_token) || (argc == 2 && editing_token)) &&
      !current_command.empty() && (current_command[0] == "get")) {
    return read_address_completion(text);
  }
  if (((argc == 1 && starting_new_token) || (argc == 2 && editing_token)) &&
      !current_command.empty() && (current_command[0] == "set")) {
    return write_address_completion(text);
  }
  if (((argc == 2 && starting_new_token) || (argc == 3 && editing_token)) &&
      !current_command.empty() && (current_command[0] == "set" || current_command[0] == "get")) {
    return device_address_completion(text);
  }

  if (((argc == 2 && starting_new_token) || (argc == 3 && editing_token)) &&
      !current_command.empty() && (current_command[0] == "set_vareg")) {
    return rl_completion_matches(text, rl_filename_completion_function);
  }

  if (((argc == 2 && starting_new_token) || (argc == 3 && editing_token)) &&
      !current_command.empty() && current_command[0] == "readout") {
    return rl_completion_matches(text, rl_filename_completion_function);
  }

  if (((argc == 1 && starting_new_token) || (argc == 2 && editing_token)) &&
      !current_command.empty() && current_command[0] == "set_linkspeed") {
    return link_speed_completion(text);
  }

  if (g_candidate.empty() && start > 0 && rl_line_buffer[start - 1] == '@') {
    return rl_completion_matches(text, rl_filename_completion_function);
  }

  rl_attempted_completion_over = 1;
  return rl_completion_matches(text, completion_generator);
}
