# PbmsTools

A Linux command-line tool for communicating with **Pace BMS** hardware over a serial port. Reverse-engineered from `PbmsTools V2.5` (the original Windows GUI).

The focus is on battery health and balancing control.

## Building

Requires CMake ≥ 3.14 and a C++17 compiler. No external dependencies.

```sh
cmake -B pbmstool/build pbmstool && cmake --build pbmstool/build
```

Binary: `pbmstool/build/pbmstool`

## Usage

```
pbmstool --port <dev> [--baud <N>] [--timeout <ms>] [--pack <N>] <subcommand>
```

**Global flags:**

| Flag | Default | Description |
|---|---|---|
| `--port` | *(required)* | Serial device, e.g. `/dev/ttyUSB0` |
| `--baud` | `9600` | Baud rate |
| `--timeout` | `2000` | Read timeout in milliseconds |
| `--pack` | `1` | Target pack address (1–15); ignored by `discover` |

## Supported Subcommands

| Subcommand | Description |
|---|---|
| `discover` | Query pack count → prints `pack_count=N` |
| `info` | Read firmware version string → prints `pack=N`, `version=...` |
| `analog` | Read live data → cell voltages (mV), temperatures (°C), current (A), total voltage (mV), remaining/full/design capacity (mAh), cycle count |
| `alarm` | Read alarm/status flags → per-cell voltage alarms, temperature alarms, charge/discharge current state, 9 status bytes |
| `params get` | Read balancer parameters → `balance_threshold_v`, `balance_delta_mv` |
| `params set balance-threshold=X.XXX [balance-delta=N]` | Write balancer parameters (reads current values for any omitted param) |
| `tui` | Interactive ncurses view of live data, alarms and parameters |
| `daemon` | Read `analog` + `alarm` every interval and publish them to an MQTT broker (see below) |

All output is plain-text `key=value` lines on stdout. Errors go to stderr with a non-zero exit code.

## Examples

```sh
# Detect how many packs are connected
pbmstool --port /dev/ttyUSB0 discover

# Read live analog data from pack 2
pbmstool --port /dev/ttyUSB0 --pack 2 analog

# Get current balance settings
pbmstool --port /dev/ttyUSB0 params get

# Set balance threshold to 3.400 V, keep current delta
pbmstool --port /dev/ttyUSB0 params set balance-threshold=3.400

# Set both threshold and delta
pbmstool --port /dev/ttyUSB0 params set balance-threshold=3.400 balance-delta=30
```

## MQTT daemon

```
pbmstool --port <dev> [--baud <N>] [--timeout <ms>] [--pack <N>] daemon \
         --mqtt-host <host> [--mqtt-port <N>] [--mqtt-prefix <prefix>] [--interval <s>]
```

| Flag | Default | Description |
|---|---|---|
| `--mqtt-host` | *(required)* | Broker hostname or IP address |
| `--mqtt-port` | `1883` | Broker TCP port |
| `--mqtt-prefix` | `bms` | Topic prefix; each field goes to `<prefix>/<field>`. A trailing `/` is ignored; `--mqtt-prefix ""` publishes to bare `<field>` topics |
| `--interval` | `5` | Seconds between read-and-publish cycles; fractional values are accepted |

The daemon runs in the foreground (for systemd `Type=simple`) and stops cleanly with exit code 0 on SIGINT or SIGTERM. Every cycle it reads `analog` and `alarm` for `--pack` and publishes each field as its own topic. The payload is the value exactly as the `analog`/`alarm` subcommands print it (e.g. `3328`, `25.9`, `0`). Messages use QoS 0 and are not retained. The client connects with a clean session, no authentication and no TLS, using the client id `pbmstool-<hostname>`.

Published fields: `cell_N_mv`, `temp_N_c`, `cycle`, `cell_N_volt_alarm`, `temp_N_alarm`, `charge_curr_state`, `total_volt_state`, `discharge_curr_state`, every status flag (`protect_*`, `alarm_*`, `fault_*`, switch states) and `cell_N_balancing`. The cell and temperature counts come from the BMS. These keys are not published: `pack`, `cell_count`, `temp_count`, `current_a`, `total_voltage_mv`, `remain_cap_mah`, `full_cap_mah`, `design_cap_mah`.

Failure handling:

- If a BMS read fails (timeout, checksum), the daemon skips that read's fields for the cycle and closes the serial port. It reopens the port on the next cycle, so a replugged USB adapter or a missing device node recovers on its own.
- The daemon reconnects to the broker with exponential backoff (1 s up to 60 s) when the broker is unavailable at startup or the connection drops. It sends a PINGREQ every keepalive/2 and treats an unanswered PINGREQ as a dead connection. The keepalive is 30 s, or longer when `--timeout` is large.
- Log output is one line per state change: startup configuration, connects and disconnects, the first occurrence of each distinct error, and recovery. Errors go to stderr.

`pbmstool.service` (next to this repository) is an example systemd unit. The service user needs read/write access to the serial device, which on Raspberry Pi OS / Debian means membership of the `dialout` group.

## Protocol

Pace ASCII protocol over RS-232/RS-485. Frames are delimited by `0x7E` (start) and `0x0D` (end), with all payload bytes hex-encoded as uppercase ASCII pairs. The tool implements checksum validation and retries up to 2 times on timeout or checksum mismatch.
