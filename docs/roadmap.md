# Roadmap

This document tracks ordered project work. It describes direction, not released
behavior; current behavior is documented in `README.md` and `config.md`.

## Current Foundation

The current codebase includes:

- rules-first configuration and merged watches
- shell-free task execution and event placeholders
- filename filters and per-path settling
- direct inotify event model
- async child reaping and graceful shutdown
- config check mode and executable validation
- queue-overflow detection
- severity-controlled stderr logging
- systemd service and install targets
- nonzero exits for fatal event-loop failures
- source-based unit and integration test harness

## Immediate: Release Readiness

Goal: establish a trustworthy `v0.1.0` baseline before adding runtime policy.

- finish the documentation consolidation
- soak the development build under systemd
- exercise `info` and `debug` logging
- run the checklist in `releasing.md`
- fast-forward `main` and tag `v0.1.0`

## Next: Backpressure

Unbounded child creation is the largest operational risk in the current model.
Define policy before recursive watching increases event volume.

Design decisions:

- whether `max_concurrency` belongs to rules, tasks, or both
- whether the compatibility default remains unlimited
- how busy work is identified: rule, task, path, or a combination
- behavior for `parallel`, `drop`, and `queue`
- queue bounds and observability
- shutdown behavior for queued and running work

Implementation should include explicit logs and burst-oriented tests.

## Then: Coalescing and Loop Protection

Build on bounded execution with optional controls for noisy or self-triggering
workflows:

- duplicate suppression beyond the existing settle window
- cooldown or debounce policy
- bounded aggregate work after a burst
- clearer repeated-rule diagnostics
- documented patterns for excluding task outputs

These controls should remain opt-in and preserve simple immediate dispatch.

## Then: Recursive Watching

Recursive support must be opt-in and treated as watch-tree management, not a
one-time directory walk.

Required work:

- `recursive = true` configuration and validation
- separate configured roots from concrete watched directories
- startup traversal without following symlinks by default
- dynamic watches for created and moved-in directories
- removal of ignored, deleted, or moved-out bindings
- scalable watch-descriptor lookup
- clear handling of `fs.inotify.max_user_watches`
- watch-tree rebuild after queue overflow
- tests for creation, move, deletion, and startup races

## Later

- expose move cookies to tasks
- expose output-only flags such as `IN_ISDIR`
- config reload through parse-validate-swap
- per-task user and group controls
- deployment-specific systemd hardening examples
- optional metrics or structured event summaries

## Non-Goals for the Near Term

- becoming a general workflow engine
- implicit shell command parsing
- cross-platform filesystem abstraction
- hiding overload or event-loss behavior behind silent defaults
