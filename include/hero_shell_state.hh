#pragma once

#include <grpcpp/channel.h>
#include <superhero.grpc.pb.h>

#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

enum class ShellState {
  IDLE,
  CONNECTED,
  DEVICE_ADDED,
};

extern std::atomic<bool> g_interrupted;
extern std::shared_ptr<grpc::Channel> g_channel;
extern std::unique_ptr<superhero::CommunicationService::Stub> g_stub;
extern ShellState g_current_state;
extern std::string g_current_endpoint;
extern int g_router_count;
extern int g_detector_count;
extern std::vector<std::string> g_candidate;
extern const std::vector<std::pair<std::vector<ShellState>, std::string>> kCommandList;
extern const std::map<std::string, std::string> kHelps;

struct PromptInfo {
  std::string readline_text;
  std::string display_text;
  std::size_t visible_length;
};

void log_grpc_error(const std::string& api, const grpc::Status& status);
void update_device_counts();
void refresh_state_after_device_change();
auto build_prompt() -> PromptInfo;
auto get_detector_logical_addresses() -> std::optional<std::vector<uint8_t>>;
auto get_router_logical_addresses() -> std::optional<std::vector<uint8_t>>;
auto get_device_logical_addresses() -> std::optional<std::vector<uint8_t>>;
