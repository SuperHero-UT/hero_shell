# hero_shell

`hero_shell` is an interactive and scriptable gRPC shell to operate CdTeDE detector systems. It provides comprehensive capabilities for managing device topology, performing register access, configuring the FPGA, uploading VAREG files, and controlling data acquisition operations over a gRPC connection.

## Features

- **Interactive Shell**: Includes state-aware tab completion, history management, and categorized command help.
- **Batch Script Mode**: Supports robust script execution with fail-fast `file:line` errors and non-zero exit codes on failure for CI/CD or automated pipelines.
- **TTY-Aware Output**: Provides ANSI styling and a live progress display in interactive sessions, while automatically falling back to plain, clean output suitable for batch logs when redirected.
- **Comprehensive Readout**: Handles data streaming with per-detector binary files, dedicated HK files, extended attributes for metadata, and detailed acquisition summaries.

## Requirements & Build

### Prerequisites

- CMake 3.15+
- C++17 compatible compiler
- make (used by bundled ncurses/libedit builds)

### Steps

```sh
git submodule update --init --recursive

cmake -S . -B build
cmake --build build -j
```

The binary will be generated at `build/hero_shell`.

## Quick Start

### Interactive Session

Start the shell by running `build/hero_shell` and execute commands interactively:

```text
hero_shell[-]> connect localhost:50051
Connected to localhost:50051
hero_shell[localhost:50051(0,0)]> add_detector 0x35 0x02 0x35 - 0x03 0xFE
hero_shell[localhost:50051(0,1)]> configure_fpga 0x35 peaking_time_nside=10 peaking_time_pside=10 adc_clock_period=8 readout_clock_period=8 readout_clock_delay=2 trig_patlatch_timing=4 reset_wait_time=100 reset_wait_time2=100
hero_shell[localhost:50051(0,1)]> readout 10s run001
```

The prompt shows the connected endpoint and the registered `(router, detector)` counts.

### Batch Script

Create a script file (e.g., `run.txt`):

```text
# Connect to the server
connect localhost:50051

# Add detector and configure
add_detector 0x35 0x02 0x35 - 0x03 0xFE
configure_fpga 0x35 peaking_time_nside=10 peaking_time_pside=10 adc_clock_period=8 readout_clock_period=8 readout_clock_delay=2 trig_patlatch_timing=4 reset_wait_time=100 reset_wait_time2=100

# Run acquisition for 10 seconds
readout 10s run001
```

Run the script directly from the command line:

```sh
./build/hero_shell run.txt
```

*Note: The process exits with a status of `1` if execution fails, ensuring reliability in automated pipelines.*

## Command Overview

For detailed command syntax and behavior, please refer to the [Command Reference](docs/COMMANDS.md).

| Category | Command | Summary |
|----------|---------|---------|
| **General** | [`help`](docs/COMMANDS.md#help) | Print help or command list |
| | [`sleep`](docs/COMMANDS.md#sleep) | Pause execution |
| | [`exit` / `quit`](docs/COMMANDS.md#exit--quit) | Terminate the shell |
| **Connection** | [`connect`](docs/COMMANDS.md#connect) | Open a gRPC channel |
| **Device Management** | [`add_detector`](docs/COMMANDS.md#add_detector) | Register a detector |
| | [`remove_detector`](docs/COMMANDS.md#remove_detector) | Remove a registered detector |
| | [`add_router`](docs/COMMANDS.md#add_router) | Register a router |
| | [`remove_router`](docs/COMMANDS.md#remove_router) | Remove a registered router |
| | [`remove_device`](docs/COMMANDS.md#remove_device) | Remove a generic device |
| | [`remove_all_devices`](docs/COMMANDS.md#remove_all_devices) | Remove all registered devices |
| | [`list_devices`](docs/COMMANDS.md#list_devices) | List all registered devices |
| | [`list_detectors`](docs/COMMANDS.md#list_detectors) | List registered detectors |
| | [`list_routers`](docs/COMMANDS.md#list_routers) | List registered routers |
| **Configuration** | [`set`](docs/COMMANDS.md#set) | Write a register |
| | [`get`](docs/COMMANDS.md#get) | Read a register |
| | [`configure_fpga`](docs/COMMANDS.md#configure_fpga) | Configure FPGA parameters |
| | [`set_vareg`](docs/COMMANDS.md#set_vareg) | Upload a VAREG image |
| | [`set_linkspeed`](docs/COMMANDS.md#set_linkspeed) | Set SpaceWire link speed |
| | [`set_hv`](docs/COMMANDS.md#set_hv) | Set HV-DAC ramp (disabled) |
| | [`get_hv`](docs/COMMANDS.md#get_hv) | Get HV-DAC status (disabled) |
| **Data Acquisition** | [`show`](docs/COMMANDS.md#show) | Print device status registers |
| | [`readout`](docs/COMMANDS.md#readout) | Stream and save HL data |

## Scripting

`hero_shell` supports robust execution of sequential script files directly (`hero_shell script.txt`) or from within another interactive or batch session:

- **Interactive inclusion**: Use `@file` to execute a script inline.
- **Comments**: Lines whose first token begins with `#` are ignored.
- **Line continuation**: End a line with a backslash `\` to continue the command on the next line (trailing whitespace is ignored).
- **Nesting limit**: Scripts can be nested via `@other-file` to a maximum recursive depth of 10. Deeper nesting fails with an error.
- **Error handling**: Scripts execute sequentially and fail fast, stopping and returning an error on the first failing command.
