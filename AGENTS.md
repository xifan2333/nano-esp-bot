# nano-esp-bot Agent Guide

Index, progressive disclosure routing, and core constraints for AI coding agents working on `nano-esp-bot`.

---

## 1. Monorepo Architecture & Code Routing

All hardware firmware code MUST be confined under `hardware/`. Never pollute the root directory.

| Target | Description | Local Path |
| :--- | :--- | :--- |
| **`hardware/` (Core Firmware)** | Pure native ESP-IDF 5.x firmware for ESP32-S3 | `hardware/` |
| **`hardware/components/ledger/`** | Hardware Resource Safety Ledger (pin tracking, auto-teardown) | `hardware/components/ledger/` |
| **`hardware/components/rpc/`** | Stream JSON-RPC 2.0 communication engine (USB-Serial/JTAG) | `hardware/components/rpc/` |
| **`hardware/components/chal/`** | C-HAL hardware abstraction layers (io, pulse, bus, net, input) | `hardware/components/chal/` |
| **`hardware/components/sandbox/`** | Embedded Lua 5.4 VM with memory limits and instruction quota hooks | `hardware/components/sandbox/` |
| **`hardware/components/storage/`** | LittleFS storage & application bundle manager | `hardware/components/storage/` |
| **`hardware/main/`** | Micro-Supervisor bootstrap & task lifecycle coordinator | `hardware/main/` |

---

## 2. Progressive Skill Index (Load on Demand)

To prevent context bloat, detailed SOPs and domain operational manuals are encapsulated into atomic skills. Load them progressively based on the current task:

| Task / Domain | Skill to Load | Purpose |
| :--- | :--- | :--- |
| **Issue + Draft PR Workflow** | [`.agents/skills/issue-pr/SKILL.md`](.agents/skills/issue-pr/SKILL.md) | **Mandatory SOP** for branching, Draft PR creation, single-item execution loops, quality gates, and squash-merging. |
| **Hardware & Firmware Dev** | [`.agents/skills/nano-bot-dev/SKILL.md`](.agents/skills/nano-bot-dev/SKILL.md) | ESP-IDF 5.x build commands, component interfaces, FreeRTOS tasks, and C-HAL safety conventions. |

---

## 3. Quick Quality Gate Commands

```bash
mise run check:plan     # Preview which linters will run on modified files
mise run check:changed  # Check modified files via native hk (fast local gate)
mise run fix            # Auto-fix formatting in modified files
mise run hw:download    # Download built firmware artifacts from GitHub Actions CI
mise run hw:flash       # Flash downloaded firmware to connected ESP32-S3 (via esptool)
mise run hw:monitor     # Open serial monitor
mise run check          # Full repository validation (for CI)
```

---

## 4. Inviolable Red Lines (Hard Constraints)

1. **No Direct Main Commits**: Always develop via feature branches and Draft PRs.
2. **Draft PR Pre-Creation**: Draft PR with unchecked items (`- [ ]`) MUST be opened before writing implementation code (see `issue-pr` skill).
3. **No Multi-Task Lump Commits**: Every commit must map to exactly one `- [ ]` task in the PR checklist.
4. **CI-Delegated Firmware Compilation (No Local hw:build)**: Local development machines MUST NOT run `hw:build` / `idf.py build` (to eliminate heavy toolchain constraints on low-spec hardware). Run only `mise run check:changed` locally before pushing. Firmware compilation and build verification are strictly delegated to CI (`hw-build` job). Verify builds via `gh pr checks` and inspect errors via `gh run view --log-failed`.
5. **Monorepo Confinement**: All firmware code strictly lives under `hardware/`.
6. **Zero Warning Suppression**: Never add `#pragma GCC diagnostic ignored`, `#pragma clang diagnostic ignored`, or `-Wno-*` in CMake. Fix root causes cleanly.
7. **Resource Ledger Governance**: All user-accessible pins/peripherals must register with `ledger.h` and implement safe teardown.
8. **Appliance-First Philosophy**: Firmware boots instantly into runtime; the JSON-RPC control plane runs on high priority and remains always reachable.
9. **AI Attribution**: Append standard disclosure to PR bodies and substantive reviews:
   `*AI-assisted — Tool: <tool>; model: <provider>/<model>; version: <version-or-unavailable>.*`
