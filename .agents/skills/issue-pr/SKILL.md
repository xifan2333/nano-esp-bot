---
name: issue-pr
description: Issue and Draft PR driven development workflow SOP. Use when creating an issue, starting development via Draft PR, executing single-item task loops, running quality gates (check:plan, check:changed, fix), pushing commits, verifying CI builds (gh pr checks, gh run view --log-failed), handling review bot triage (CodeRabbit, Greptile), or performing squash-merges.
---

# Issue + PR Driven Development Workflow (SOP)

Development in this repository follows a strict, chronological **Pre-Code Draft PR -> Single-Item Loop -> Merge** process.

---

## 1. Dual-Planning Model

To maintain clear separation of concerns between functional task design and toolchain validation:

1. **Phase A: Task Planning (Pre-development - BEFORE CODING)**:
   - Defining *what* to build: Issue analysis, module boundaries, architectural design, and opening the Draft PR with an unchecked `- [ ]` checklist.
2. **Phase B: Quality Gate Pre-check (Post-edit - AFTER EDIT)**:
   - Previewing *which* linter and compiler steps will execute on edited files via `mise run check:plan` before committing.

---

## 2. Chronological Lifecycle

```
+------------------------------------------------------------------------+
| 1. Pre-Code Initialization (MANDATORY BEFORE ANY CODE IS WRITTEN)       |
|    gh issue view <id>                                                  |
|    git checkout -b <type>/issue-<id>-<name>                            |
|    git commit --allow-empty -m "chore: initialize draft pr for #<id>"   |
|    git push -u origin <type>/issue-<id>-<name>                         |
|    gh pr create --draft (ALL tasks unchecked: - [ ])                   |
+-----------------------------------+------------------------------------+
                                    |
                +-------------------v-------------------+
                | 2. Single-Item Focused Development    |
                |    Only implement the first - [ ]     |
                +-------------------+-------------------+
                                    |
                +-------------------v-------------------+
                | 3. Local Quality Gate (Fast Check)    |
                |    mise run check:plan (preview steps)|
                |    mise run check:changed             |
                |    mise run fix (if needed)           |
                |    (DO NOT run hw:build locally)      |
                +-------------------+-------------------+
                                    |
                +-------------------v-------------------+
                | 4. Local Atomic Commit                |
                |    git add <files>                    |
                |    git commit -m "<type>(<scope>): ..."|
                |    (Keep commit local)                |
                +-------------------+-------------------+
                                    | (Remaining tasks?)
                                    +-------- Yes -------+
                                    | No                 |
+-----------------------------------v-------------------+|
| 5. Unified Push, Checks & Merge                       ||
|    git push origin <branch>                           ||
|    gh pr edit --body (check all - [x])                ||
|    gh pr checks (verify CI hw-build passes)           ||
|    gh run view --log-failed (if failed, fix in PR)    ||
|    gh pr ready (mark as ready for review)             ||
|    gh pr merge --squash --delete-branch               ||
+-------------------------------------------------------+|
                                    ^                    |
                                    +--------------------+
```

---

## 3. Detailed Execution Steps

### Phase 1: Pre-Code Initialization (MANDATORY BEFORE ANY CODE)
```bash
# 1. Inspect issue details and requirements
gh issue view <issue_id>

# 2. Create a standardized feature/fix branch
git checkout -b <type>/issue-<issue_id>-<short-description>
# Examples:
#   git checkout -b feat/issue-1-littlefs-storage
#   git checkout -b fix/issue-2-rpc-buffer-overflow

# 3. Initialize branch with an empty commit and push to remote
git commit --allow-empty -m "chore: initialize draft pr for issue #<issue_id>"
git push -u origin <type>/issue-<issue_id>-<short-description>

# 4. Open Draft PR with ALL tasks UNCHECKED (- [ ])
gh pr create --draft \
  --title "<type>(<scope>): <concise description> (#<issue_id>)" \
  --body "Closes #<issue_id>

### Implementation Tasks
- [ ] 1. Core component interface & data structures
- [ ] 2. Implementation logic & hardware driver integration
- [ ] 3. Quality checks, formatting & firmware build verification"
```

---

### Phase 2: Single-Item Execution Loop
For each unchecked `- [ ]` task in order:
1. **Code ONLY Task N**: Focus strictly on the topmost unchecked `- [ ]` item. Do not start work on subsequent tasks early.
2. **Quality Gate & Pre-check**:
   ```bash
   # Preview execution plan for modified files
   mise run check:plan

   # Run linters/checkers across changed/staged/untracked files (fast local gate)
   mise run check:changed

   # Auto-fix formatting if needed
   mise run fix

   # For structured diagnostics:
   hk run check --safe --format jsonl

   # NOTE: DO NOT run hw:build / idf.py build locally!
   # Firmware compilation is strictly offloaded to GitHub Actions CI.
   ```
3. **Local Atomic Commit**:
   Keep commits strictly atomic (one commit per `- [ ]` task) and keep them **local** during intermediate steps:
   ```bash
   git add <modified_files>
   git commit -m "<type>(<scope>): complete task N (#<issue_id>)"
   ```

---

### Phase 3: Finalize, Unified Push & Merge
Once all tasks in the checklist are completed:
```bash
# 1. Push all completed atomic commits in one unified push
git push origin <branch>

# 2. Update Draft PR body to check off all completed tasks (- [x])
gh pr edit --body "Closes #<issue_id>

### Implementation Tasks
- [x] 1. Core component interface & data structures
- [x] 2. Implementation logic & hardware driver integration
- [x] 3. Quality checks, formatting & firmware build verification"

# 3. Mark PR ready for review (activates review bots: CodeRabbit, Greptile)
gh pr ready

# 4. Inspect review bot comments and verify CI checks
gh pr checks
gh pr view --comments
```

---

### Phase 4: Automated Review Triage & Fix Loop
Once marked ready, monitor CI and review bot outputs:
```bash
# 1. Check CI status
gh pr checks

# 2. Inspect review bot comments (top-level and line-level threads)
gh pr view --comments
gh api repos/:owner/:repo/pulls/<pr_id>/comments
```

- **CodeRabbit**: Look for `> Prompt for AI Agents` blocks and treat them as candidate repair instructions.
- **Greptile**: Address cross-file architectural warnings when Confidence $\ge$ 4.
- **Fix & Push**:
  Verify findings against actual code, make minimal edits, run `mise run check:changed`, then commit:
  ```bash
  git add <modified_files>
  git commit -m "fix(review): address review feedback (#<issue_id>)"
  git push origin <branch>
  ```

---

### Phase 5: CI Build Triage Loop (If CI Fails)
If `gh pr checks` indicates `hw-build` or `check` failure on GitHub Actions:
```bash
# 1. Fetch failure logs
gh run view --log-failed

# 2. Fix compiler/linter errors locally, verify formatting via mise run check:changed
git add <modified_files>
git commit -m "fix(build): resolve compilation error reported by CI (#<issue_id>)"
git push origin <branch>

# 3. Re-verify CI checks until all green
gh pr checks --watch
```

---

### Phase 6: Final Squash-Merge
```bash
# 1. Verify all checks pass
gh pr checks

# 2. Perform squash-merge and delete remote/local branch
gh pr merge --squash --delete-branch
```
