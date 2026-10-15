---
name: nano-bot-dev
description: Master development, architecture, code hygiene, Issue+Draft PR lifecycle, and hardware build guide for nano-esp-bot. Use when developing ESP32-S3 firmware under hardware/, implementing components (ledger, rpc, chal, sandbox, storage), running quality gates (mise run check:plan, check:changed, fix, hw:build), or following the strict Chronological Issue + Draft PR workflow (SOP). Trigger on Chinese requests such as 开发 nano-esp-bot、改底层固件、编写组件、根据 issue 拆解开发、开 PR/提 PR、代码体检与格式化自检、编译固件 hw:build.
---

# nano-esp-bot Unified Development Skill

This skill is the master operational manual for developing, contributing, performing code health checks, and managing workflows across the `nano-esp-bot` repository.

---

## 1. Quick Repository Routing & Architecture

Verify target directories before making changes:

| Target | Description | Local Path |
| :--- | :--- | :--- |
| **`hardware/` (Core Firmware)** | Pure native ESP-IDF 5.x firmware for ESP32-S3 | `hardware/` |
| **`hardware/components/ledger/`** | Hardware Resource Safety Ledger (pin tracking, auto-teardown) | `hardware/components/ledger/` |
| **`hardware/components/rpc/`** | Stream JSON-RPC 2.0 communication engine (USB-Serial/JTAG) | `hardware/components/rpc/` |
| **`hardware/components/chal/`** | C-HAL hardware abstraction (io, pulse, bus, net, input) | `hardware/components/chal/` |
| **`hardware/components/sandbox/`** | Embedded Lua 5.4 VM runtime with quota & break hooks | `hardware/components/sandbox/` |
| **`hardware/components/storage/`** | LittleFS filesystem mount & app bundle manager | `hardware/components/storage/` |
| **`hardware/main/`** | Micro-Supervisor main entry point | `hardware/main/` |

**Monorepo Separation Iron Rule**: All hardware firmware code MUST be contained under `hardware/`. Never place firmware source files directly in the repository root!

---

## 2. Universal Code Quality Gate (`hk` / `mise`)

Always run verification before committing or reporting task completion:

```bash
# 1. Preview which steps match edited files
mise run check:plan

# 2. Check changed files safely
mise run check:changed

# 3. Automatically fix formatting issues
mise run fix

# 4. Verify ESP-IDF firmware compilation
mise run hw:build

# 5. Full repository validation
mise run check
```

---

## 3. Workflow: Strict Issue + Draft PR Lifecycle

All development MUST strictly follow the chronological **Pre-Code Draft PR -> Single-Item Loop -> Merge** process defined in the atomic [`issue-pr` Skill](../issue-pr/SKILL.md).

Never write implementation code directly on `main` or without an open Draft PR! Refer directly to [`.agents/skills/issue-pr/SKILL.md`](../issue-pr/SKILL.md) for the complete 5-phase SOP and bash snippets.

---

## 4. Agent Hard Constraints (Red Lines)

1. **No Direct Main Commits**: Never push implementation code directly to `main`.
2. **Draft PR Pre-Creation**: Draft PR with unchecked items (`- [ ]`) MUST be created before writing implementation code.
3. **No Multi-Task Lump Commits**: Every commit must map to exactly one `- [ ]` task in the PR checklist.
4. **No Push Without Quality Gate**: Never push code before running `mise run check:changed` and `mise run hw:build`.
5. **Transparent Progress Tracking**: Report progress based on the PR checklist (e.g. "Completed 2 of 4 tasks; currently implementing task 3").
6. **Zero-Tolerance on Suppressing Diagnostics**: Never add `#pragma GCC diagnostic ignored`, `#pragma clang diagnostic ignored`, or `-Wno-*` compiler flags in CMakeLists.txt to silence warnings. Clean up code so compilation passes cleanly.
7. **Resource Safety & Ledger Governance**: Any hardware peripheral or pin exposed to user scripts must be claimed and registered through `ledger.h`. Teardown hooks must restore safe high-impedance floating state.
8. **AI Attribution & Disclosure**: When an AI agent authors PR descriptions or review comments, append standard disclosure:
   `*AI-assisted — Tool: <tool>; model: <provider>/<model>; version: <version-or-unavailable>.*`
