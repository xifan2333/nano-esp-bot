# Claude Code Guide

See [AGENTS.md](AGENTS.md) for architecture routing, progressive skill index, and core constraints.

## Progressive Skill Routing

Load skills as needed to avoid context pollution:
- **Starting an Issue or PR**: Load [`.agents/skills/issue-pr/SKILL.md`](.agents/skills/issue-pr/SKILL.md) for the mandatory 5-phase Draft PR lifecycle SOP.
- **Developing Firmware / Components**: Load [`.agents/skills/nano-bot-dev/SKILL.md`](.agents/skills/nano-bot-dev/SKILL.md) for ESP-IDF component architecture and hardware safety rules.

## Quick CLI Reference

```bash
mise run check:plan     # Preview execution plan for modified files
mise run check:changed  # Check modified files via native hk
mise run fix            # Auto-fix issues in modified files
mise run hw:build       # Compile ESP32-S3 firmware
mise run hw:flash       # Flash firmware to connected ESP32-S3
mise run hw:monitor     # Open serial monitor
mise run check          # Full repository validation (CI)
```

## Key Red Lines
- Always develop via feature branches and Draft PRs with unchecked `- [ ]` tasks (`issue-pr` skill).
- Monorepo confinement: all firmware lives under `hardware/`.
- Run `mise run check:changed` and `mise run hw:build` before pushing.
- All hardware pins/peripherals must be governed by `ledger.h`.
