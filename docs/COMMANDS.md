# hero_shell Command Reference

## Invocation

```text
hero_shell
```

Starts the interactive shell.

```text
hero_shell <script>
```

Runs `<script>` as a command script, then exits. The process exits with status `1` if opening or executing the script fails; scripts stop at the first failing command.

```text
hero_shell -h
hero_shell --help
```

Print command-line help and exits successfully.

```text
hero_shell -v
hero_shell --version
```

Prints `hero_shell <version>` and exits successfully.

Unknown command-line options exit with status `1`.

## Shell States and Command Availability

The command metadata drives `help` listings and first-word tab completion. During an active
readout, both are further limited to the commands allowed while acquiring. Command dispatch itself
does not otherwise enforce the connection/device states; commands may instead fail when their
required gRPC connection or devices are absent.

| State | Meaning |
|---|---|
| `IDLE` | No active gRPC connection. |
| `CONNECTED` | Connected to the server, with no registered detector or router. |
| `DEVICE_ADDED` | Connected, with at least one registered detector or router. |

| Available in | Commands |
|---|---|
| All states | `help`, `sleep`, `exit`, `quit` |
| `IDLE` | `connect` |
| `CONNECTED`, `DEVICE_ADDED` | `add_detector`, `remove_detector`, `add_router`, `remove_router`, `remove_device`, `remove_all_devices`, `set_linkspeed` |
| `DEVICE_ADDED` | `list_devices`, `list_detectors`, `list_routers`, `set`, `get`, `configure_fpga`, `set_vareg`, `show`, `readout`, `pedcalib_readout` |

## General Commands

### `help`

```text
help [command]
```

Lists commands grouped by category, or prints detailed help for one command.

Example:

```text
help readout
```

### `sleep`

```text
sleep <duration>
```

Pauses for the given duration. `Ctrl-C` interrupts it. A bare number is
interpreted as seconds; duration units (same grammar as `readout`) are also
accepted.

Examples:

```text
sleep 2.5
sleep 500ms
sleep 5min
sleep 1h30min
```

For delays of at least three seconds, an interactive terminal shows a countdown.

### `exit` / `quit`

```text
exit
quit
```

Terminates the shell. The two commands are aliases.

## Connection Commands

### `connect`

```text
connect <host:port>
```

Opens an insecure gRPC channel to the CdTeDE server and verifies it with an echo request.
Press `Ctrl-C` to cancel a pending connection attempt.

Example:

```text
connect localhost:50051
```

A failed or interrupted connection leaves the shell disconnected.

## Device Management Commands

Logical addresses accept decimal or `0x`/`0X` hexadecimal values and must fit in one byte (`0`–`255`).

### `add_detector`

```text
add_detector <logical_address> <target_address>... - <reply_address>...
```

Registers a detector with its SpaceWire target and reply paths.

Example:

```text
add_detector 0x35 0x02 0x35 - 0x03 0xFE
```

The logical address must not already be registered.

### `remove_detector`

```text
remove_detector <logical_address>
```

Removes a registered detector.

Example:

```text
remove_detector 0x35
```

### `add_router`

```text
add_router <logical_address> <target_address>... - <reply_address>...
```

Registers a router with its SpaceWire target and reply paths.

Example:

```text
add_router 0x01 0x02 - 0x03
```

The logical address must not already be registered.

### `remove_router`

```text
remove_router <logical_address>
```

Removes a registered router.

Example:

```text
remove_router 0x01
```

### `remove_device`

```text
remove_device <logical_address>
```

Removes a registered detector or router.

Example:

```text
remove_device 0x35
```

### `remove_all_devices`

```text
remove_all_devices
```

Removes every registered device, stopping if any removal fails.

### `list_devices`

```text
list_devices
```

Lists registered detectors and routers by logical address.

### `list_detectors`

```text
list_detectors
```

Lists registered detector logical addresses.

### `list_routers`

```text
list_routers
```

Lists registered router logical addresses.

## Configuration Commands

### `set`

```text
set <address> <logical|[addr,...]|[all]> <value>
```

Writes a 32-bit register value to one or more registered devices.

Examples:

```text
set PeakingTime1 0x35 100
set PeakingTime1 [0x35, 0x36] 100
set PeakingTime1 [all] 100
```

`<value>` accepts decimal or hexadecimal unsigned 32-bit values and is sent as four big-endian bytes. For a multi-address write, failures are reported per device and the command returns failure if any write fails.

### `get`

```text
get <address> <logical|[addr,...]|[all]>
```

Reads a four-byte register from one or more registered devices.

Examples:

```text
get PeakingTime1 0x35
get PeakingTime1 [0x35,0x36]
get PeakingTime1 [all]
```

Four-byte values are printed as both hexadecimal and decimal:

```text
0x35: 0x00000064  (100)
```

### `configure_fpga`

```text
configure_fpga <logical> key=value...
```

Configures all FPGA timing parameters for one registered device in a single RPC. All eight keys are required:

```text
peaking_time_nside
peaking_time_pside
adc_clock_period
readout_clock_period
readout_clock_delay
trig_patlatch_timing
reset_wait_time
reset_wait_time2
```

Example:

```text
configure_fpga 0x35 peaking_time_nside=10 peaking_time_pside=10 adc_clock_period=8 readout_clock_period=8 readout_clock_delay=2 trig_patlatch_timing=4 reset_wait_time=100 reset_wait_time2=100
```

Keys are case-insensitive; values are unsigned 32-bit decimal or hexadecimal values.

### `set_vareg`

```text
set_vareg <logical> <filename>
```

Uploads a VAREG image to one registered device.

Example:

```text
set_vareg 0x35 vareg_default.b64
```

The file must be base64-encoded. After decoding, it must be exactly 516 bytes:

- Bytes `0..511` are the VAREG payload.
- Bytes `512..515` are the expected CRC32, stored big-endian.
- The CRC32 is verified over the first 512 bytes before upload.
- The uploaded payload is zero-padded to 4096 bytes.

The filename is recorded as VAREG provenance only after the server accepts the upload.

### `set_linkspeed`

```text
set_linkspeed <10MHz|20MHz|25MHz|33MHz|50MHz|100MHz>
```

Sets the SpaceWire link speed.

Example:

```text
set_linkspeed 50MHz
```

Input is case-insensitive and also accepts forms such as `50`, `50mbps`, or `50 MHz` only when tokenized as one argument; use the documented forms in scripts.

## Data Acquisition Commands

### `show`

```text
show <logical>
```

Reads and prints the shell’s fixed set of common status and timing registers for one registered device.

Example:

```text
show 0x35
```

Each normal four-byte register is printed as a left-aligned register name followed by hexadecimal and decimal values.

### `readout`

```text
readout <duration> <output_file_prefix>
readout status
readout stop
```

Starts HL data streaming, records data for the requested duration, then stops the stream. In an
interactive shell the acquisition runs in the background: `get`, `show`, and device-list commands
remain available. Use `readout status` to check whether it is active and `readout stop` to request
an early stop. Configuration, device-management, connection, and additional readout-start commands
are rejected while a readout is active; `@script` is rejected as well. `help`, `sleep`, `exit`, and
`quit` remain available. `exit`/`quit` requests a stop and waits for the readout worker to finish.
The timestamped data and HK output paths are printed when the background job starts.

`readout status` reports whether the worker is starting, running, stopping, or finished. While it
is running, it shows the total and per-detector frame counts, output paths, elapsed time, and
remaining time. Background diagnostics are queued and shown here instead of being printed over an
active input prompt. On completion it distinguishes a normal completion, an operator stop, and a
failure. `readout stop` is asynchronous: it
requests shutdown. A second `readout <duration> <prefix>` is rejected until the active worker has
finished.

The interactive prompt keeps its existing endpoint and `(router, detector)` fields and appends a
live `[remaining/total]` countdown while readout is active:

```text
hero_shell[localhost:50051(0,1)][00:52.58/01:00.00]>
```

The countdown updates without replacing the command being edited and disappears when readout
finishes or is stopped.

If the initial stream-start RPC does not respond within 10 seconds, the readout fails rather than
leaving the interactive shell unable to stop or exit.

Example:

```text
readout 1h30min run001
```

Durations accept one or more adjacent number-and-unit components, including decimal values. Examples:

```text
readout 10s run001
readout 90min run001
readout 1h30min run001
readout 10s500ms run001
```

Supported units are:

| Unit forms | Meaning |
|---|---|
| `h`, `hr`, `hrs`, `hour`, `hours` | hours |
| `m`, `min`, `mins`, `minute`, `minutes` | minutes |
| `s`, `sec`, `secs`, `second`, `seconds` | seconds |
| `ms`, `msec`, `millisecond`, `milliseconds` | milliseconds |

Output names use the local acquisition timestamp:

```text
<prefix>_yyMMdd-HHmmss_<addr>
<prefix>_yyMMdd-HHmmss_hk
```

For example, a prefix of `runs/run001` can produce:

```text
runs/run001_260723-143015_0x35
runs/run001_260723-143015_hk
runs/log.txt
```

One binary data file is created per registered detector; HK data is written to the `_hk` file. `log.txt` is appended in the directory containing the prefix (or the current directory when the prefix has no directory component). It records one line per detector containing the data filename, acquisition time, exposure seconds, last accepted VAREG filename, and whether `ForcetrigFlag` was nonzero.

Data frames must be 32,768 bytes and HK frames must be 1,024 bytes; malformed or unregistered-address frames are dropped. Data and HK output files receive extended attributes for acquisition date, exposure seconds, and logical address where supported by the platform.

In interactive mode, `Ctrl-C` both cancels the current input and requests shutdown of an active
readout. Check `readout status` for its final result. In script mode, readout remains foreground
and `Ctrl-C` aborts that command.

### `pedcalib_readout`

```text
pedcalib_readout <duration> <output_file_prefix> <register_output>
pedcalib_readout status
pedcalib_readout stop
```

Acquires pedestal data from exactly one registered detector. In an interactive shell it runs in the
background and uses the same status, stop, frame counters, `log.txt` entry, and prompt countdown as
`readout`. The timestamped data/HK paths and register output path are printed at startup. Status also
shows the register output path and calibration messages. Before acquisition, the command verifies
that the sibling or `PATH`-visible `calc_pedestal` binary can run, that Python can load the bundled
`vareg.py`, and that `set_vareg` has successfully loaded a readable VAREG file.

After the normal raw and HK files are closed, it runs `calc_pedestal` directly on the raw file.
The small C++ program only uses `FrameAnalyzer` to emit each channel's median `ADC-CMN`. It uses
at most the first 8192 valid events and ignores later events; the median is calculated with
`std::nth_element`. Its output is piped to `set_delreg.py`, which uses `vareg.py` to set
`Del_reg` to align the channels to that ASIC's highest pedestal. Because the measured data already
includes the current setting, it first subtracts the current `Del_reg` from each median, then sets
the new value to the difference from the highest reconstructed pedestal (clamped to `0..63`). It
writes a CRC-correct VAREG image to `<register_output>`.

Example:

```text
pedcalib_readout 100sec output reg_output
```

## Scripting

Run a script interactively with:

```text
@file
```

Or run it directly:

```text
hero_shell file
```

Script behavior:

- Commands execute sequentially and fail fast.
- A failure reports `file:line: error executing: <command>` and stops that script.
- Nested scripts are supported through `@other-file`.
- Script recursion is limited to depth 10; deeper nesting fails.
- A line whose first token begins with `#` is a comment.
- A trailing backslash (`\`), ignoring trailing whitespace, continues the command on the next line. The joined lines are separated by one space.
- A backslash continuation at end-of-file reports an unexpected-EOF error.
- Script commands are echoed with the current shell prompt before execution.
- `Ctrl-C` aborts script processing.

The tokenizer supports whitespace-separated tokens, double quotes, and backslash escaping of the next character. It does not diagnose unmatched double quotes; they simply remain in quote mode until end of line.

## Interactive Behavior

Tab completion is state-aware for command names and includes:

- Command names available in the current declared state.
- `help` command names.
- Register names for `get` (readable four-byte registers) and `set` (writable four-byte registers).
- Registered device, detector, or router logical addresses where applicable.
- `[all]` for the device argument of `set` and `get`.
- FPGA configuration keys, completing as `key=`.
- Link-speed values.
- `status` and `stop` for `readout` and `pedcalib_readout`.
- Filenames for `set_vareg`, `readout`, and `@script`.

History is loaded from and written to `.hero_shell_history`, with the in-memory history limited to 1000 entries.

`Ctrl-C` interrupts blocked interactive input, `sleep`, and script execution. When an interactive
readout is active, it also requests that readout stop. At the prompt it prints `^C` and returns to a
fresh prompt.

## TTY and Redirected Output

When standard output is a TTY, the shell uses ANSI styling in prompts and `help`, and shows live
countdown/readout progress using carriage-return updates for foreground (scripted) readout. An
interactive background readout updates only the prompt countdown from the shell thread. Its worker
does not write directly to the terminal, so startup diagnostics, stream warnings, and completion
cannot overwrite the command being typed; use `readout status` instead.

When standard output is redirected, styling and live progress are suppressed so output remains suitable for logs. Script prompts are plain when redirected.

Foreground/script readout emits a final, durable summary in this format:

```text
Readout summary: <total_frames> frames in <elapsed_seconds>s
  <addr>: <frames> frames -> <prefix>_yyMMdd-HHmmss_<addr>
  HK -> <prefix>_yyMMdd-HHmmss_hk
```
