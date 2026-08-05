#pragma once

#include <optional>
#include <string>
#include <vector>

auto do_help(const std::vector<std::string>& tokens) -> bool;
auto do_sleep(const std::vector<std::string>& tokens) -> bool;
auto do_connect(const std::vector<std::string>& tokens) -> bool;
auto do_add_detector(const std::vector<std::string>& tokens) -> bool;
auto do_remove_detector(const std::vector<std::string>& tokens) -> bool;
auto do_add_router(const std::vector<std::string>& tokens) -> bool;
auto do_remove_router(const std::vector<std::string>& tokens) -> bool;
auto do_remove_device(const std::vector<std::string>& tokens) -> bool;
auto do_reconnect_device(const std::vector<std::string>& tokens) -> bool;
auto do_remove_all_devices(const std::vector<std::string>& tokens) -> bool;
auto do_list_devices(const std::vector<std::string>& tokens) -> bool;
auto do_list_detectors(const std::vector<std::string>& tokens) -> bool;
auto do_list_routers(const std::vector<std::string>& tokens) -> bool;
auto do_set(const std::vector<std::string>& tokens) -> bool;
auto do_configure_fpga(const std::vector<std::string>& tokens) -> bool;
auto do_get(const std::vector<std::string>& tokens) -> bool;
auto do_set_vareg(const std::vector<std::string>& tokens) -> bool;
auto do_show(const std::vector<std::string>& tokens) -> bool;
auto do_readout(const std::vector<std::string>& tokens) -> bool;
auto do_pedcalib_readout(const std::vector<std::string>& tokens) -> bool;
auto readout_prompt_progress() -> std::optional<std::string>;
void emit_readout_message(const std::string& message, bool error = false);
void shutdown_readout();
auto do_set_linkspeed(const std::vector<std::string>& tokens) -> bool;
