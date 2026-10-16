# nano-esp-bot

[![CI](https://github.com/xifan2333/nano-esp-bot/actions/workflows/ci.yml/badge.svg)](https://github.com/xifan2333/nano-esp-bot/actions/workflows/ci.yml)

Minimal core harness and physical execution runtime for ESP32-S3.

`nano-esp-bot` turns an ESP32-S3 into a high-reliability physical agent appliance. Following the minimalist tool philosophy of Pi (Coding Agent), the firmware eschews monolithic microservice RPC bloat in favor of **7 orthogonal atomic primitives** served over USB-Serial/JTAG with $O(1)$ table-driven dispatch.

---

## 1. Minimal Agent Primitives

The control plane is permanently frozen to 7 orthogonal verbs (plus a heartbeat `ping`), eliminating custom RPC interface expansion:

| Primitive | Analogue | Description | Request Example |
| :--- | :--- | :--- | :--- |
| **`exec`** | `bash` / tool execution | Execute dynamic Lua 5.4 script to control hardware peripherals or run logic | `{"jsonrpc":"2.0","id":1,"method":"exec","params":"return 1+1"}` |
| **`read`** | `read` | Read a file from `/storage` (SPIFFS flash) | `{"jsonrpc":"2.0","id":2,"method":"read","params":"init.lua"}` |
| **`write`** | `write` | Write or overwrite a file in `/storage` | `{"jsonrpc":"2.0","id":3,"method":"write","params":["init.lua", "print('hello')"]}` |
| **`delete`** | `rm` | Delete a file from `/storage` | `{"jsonrpc":"2.0","id":4,"method":"delete","params":"init.lua"}` |
| **`list`** | `ls` | Enumerate all files stored on `/storage` | `{"jsonrpc":"2.0","id":5,"method":"list"}` |
| **`break`** | `cancel` / abort | Emergency circuit breaker: interrupts running Lua VM and tears down claimed GPIOs | `{"jsonrpc":"2.0","id":6,"method":"break"}` |
| **`info`** | `get_state` | Single-shot comprehensive snapshot of system, heap, stack, storage, sandbox, and ledger | `{"jsonrpc":"2.0","id":7,"method":"info"}` |
| **`ping`** | heartbeat | Heartbeat ping returning uptime | `{"jsonrpc":"2.0","id":8,"method":"ping"}` |

### Flexible Parameter Extraction
All primitives support both positional arrays (e.g. `["init.lua"]`) and named object dictionaries (e.g. `{"path": "init.lua"}`), as well as bare string literals where unambiguous.

---

## 2. Table-Driven $O(1)$ Dispatch

- **Compile-Time Hash**: The RPC dispatcher uses a collision-free perfect hash `((uint8_t)method[0] + strlen(method)) & 0x0F` mapped directly to a static read-only dispatch table `S_PRIMITIVE_TABLE[16]`.
- **Zero Lock Contention**: Method lookup requires zero mutex acquisition and zero dynamic allocations.
- **Hardware Configuration Tables**: Peripheral drivers (such as `chal_gpio.c`) use static mode mapping tables (`GPIO_MODE_TABLE[]`) to eliminate cascading `if-else` branching.

---

## 3. Hardware Resource Safety & Appliance Boot

- **Ledger Governance**: Every script-driven GPIO pin is verified and tracked by `ledger.h`. Unowned or reserved pins (e.g., Octal-SPI Flash/PSRAM pads) are blocked at the hardware boundary.
- **Auto-Teardown**: Any script interruption, Ctrl-C (`0x03`), or `break` call automatically resets claimed pins to floating high-impedance state.
- **Appliance-First Boot**: If `/storage/init.lua` exists, the supervisor immediately boots into the script on Core 1 while keeping the control plane live on Core 0.

---

## 4. Development & Build

```bash
mise run check:plan     # Preview linter execution plan
mise run check:changed  # Lint modified files via native hk
mise run hw:build       # Compile firmware via ESP-IDF 5.4
mise run hw:flash       # Flash connected ESP32-S3
mise run hw:monitor     # Open USB serial monitor
```
