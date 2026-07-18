# Design Notes

This document explains the current architecture and design choices behind
`inotask`.

## High-level model

`inotask` is a rules-first filesystem event runner.

The main concepts are:

- `task` — named executable plus argument templates
- `rule` — watched path, event list, and task names to run
- derived watch — merged runtime watch built from configured rules

## Why rules-first

The configuration is centered on rules because that matches how users usually
think about the problem:

- watch this path
- for these events
- run these tasks

Internally, multiple rules that reference the same path are merged into one
derived watch with the union of required event bits.

This keeps the user-facing config simple while reducing duplicate watch setup in
the runtime. Matching still happens later at the rule level, so two rules can
share one watched path but still differ by event mask, include patterns,
exclude patterns, or run-list.

## Runtime flow

Current runtime flow:

1. read config file
2. parse config
3. validate config
4. build derived watch plan
5. open one inotify instance
6. install one or more watches on that instance
7. read events from the inotify file descriptor
8. match events against rules
9. launch matching tasks with `fork()` + `execv()`
10. reap exited child processes

## One inotify fd, many watches

`inotask` currently uses one inotify instance and one inotify file descriptor.
Many watched paths are attached to that one fd.

This means:

- the process blocks on one fd
- any watched path can wake the read
- each returned event includes a watch descriptor (`wd`)
- `wd` is mapped back to the runtime watch target

This is the normal Linux inotify pattern for a daemon of this size.

## Event model

The current internal event model is intentionally smaller than raw Linux
`inotify`, but it now includes one ingestion-critical event:

- `CREATE`
- `MODIFY`
- `DELETE`
- `MOVE`
- `ATTRIB`
- `CLOSE_WRITE`

A key recent change is that `CLOSE_WRITE` is its own first-class event rather
than being silently folded into `MODIFY`.

That matters because ingestion workflows often want to wait until a file opened
for writing has been closed before trying to parse it.

## Task launching model

Tasks are launched with:

- `fork()`
- `execv()`

There is no shell command-string layer in the current model.

This was chosen to keep runtime behavior explicit and to avoid shell parsing
surprises.

### Why `execv()`

Benefits of the current approach:

- explicit executable path
- explicit argument vector
- no shell quoting rules
- safer handling of filenames with spaces and punctuation
- easier to reason about exact process arguments

## Argument expansion

Configured task args are stored as templates.
At dispatch time, placeholders are expanded using event-specific values.

Supported placeholders:

- `{watch_path}`
- `{entry_name}`
- `{full_path}`
- `{event}`

Rules may also constrain which events reach this stage by applying glob-style
`include` and `exclude` filters to `entry_name`.

This gives the convenience of runtime metadata injection while keeping the
execution model shell-free.

In other words, the config stores argument templates, and the runtime turns
those templates into concrete argv strings using the actual event being
handled.

## Async-only execution

The current runtime is async-only.
`inotask` does not wait synchronously for a task to finish before returning to
the main event loop.

This decision was made because a filesystem event daemon should continue reading
new events rather than blocking on long-running child processes.

## Child reaping

Because tasks run asynchronously, the daemon must reap exited child processes to
avoid zombies.

Current behavior:

- `SIGCHLD` sets a reap flag
- the main loop calls non-blocking `waitpid(..., WNOHANG)`
- child exit status is logged

This keeps the event loop responsive while still handling child lifecycle
correctly.

## Current event-processing policy

Current policy is intentionally simple:

- one incoming event record
- zero or more matching rules
- one new task launch per matched task

There is currently:

- no queue
- no throttling
- no coalescing
- no per-file flood protection
- no bounded child-process pool

This matches the current project goal of straightforward per-event launching.

## Logging

`inotask` uses a small logging layer that writes diagnostics to stderr.
Summary tables printed at startup still go to stdout.

Logged events currently include:

- config/load errors
- runtime watch startup issues
- event matches
- task launches
- reaped child statuses

When run under `systemd`, stderr is typically captured into `journald`.

## Current limitations

Important limitations in the current design:

- Linux-only
- no recursive watch walking
- no advanced queueing policy yet
- no per-file dedupe or bounded worker pool yet
- no full raw-inotify event surface yet
- current move handling is simplified compared with raw `IN_MOVED_FROM` and
  `IN_MOVED_TO`

## Why these tradeoffs are acceptable right now

The current implementation is optimized for clarity and incremental progress.
It gives a working end-to-end daemon with:

- explicit config
- explicit runtime execution
- shell-free task launching
- child reaping
- useful ingestion-ready `CLOSE_WRITE` support

More advanced scheduling, queueing, and flood-control behavior can be layered on
later without having to discard the current model.

## Market need and roadmap

`inotask` is best understood as a small, predictable middle ground between
`incron`, `systemd.path`, shell loops around `inotifywait`, and larger
developer-oriented watchers such as Watchman or `watchexec`.

The recurring need is not simply "cron, but for files." Users want a filesystem
event runner that can answer a few practical questions reliably:

- which path changed?
- which event happened?
- which rule matched?
- which exact command ran?
- what happens if many events arrive quickly?
- what happens if the task writes back into the watched tree?
- what happens if the daemon is run under `systemd`?

Those questions are where existing tools tend to become uncomfortable.

### Evidence from adjacent tools

`incron` proves there is demand for filesystem-triggered automation, but it
also exposes several design traps:

- one watched path per incrontab table entry limits natural rule composition
- recursive watches have been a long-standing user request
- shell-style command execution makes quoting and special characters risky
- task output and child lifecycle behavior can be difficult to reason about
- commands that write into watched paths can accidentally trigger loops
- project documentation and maintenance history have been uneven

`systemd.path` is a strong service-management primitive, but it is intentionally
coarse:

- it is good for starting a unit when "something changed"
- it is less good when the task needs the specific changed filename
- path units rate-limit failures instead of modeling filesystem-event workload
- users often need wrapper scripts to recover event details

`inotifywait` is useful as a diagnostic and scripting primitive, but production
workflows built from shell loops usually need to reinvent:

- argument quoting
- process supervision
- duplicate suppression
- restart behavior
- queue overflow handling
- logging conventions

Watchman and `watchexec` show that users value filtering, debouncing,
coalescing, ignores, restart behavior, and diagnostics. They also aim at a
broader developer-tooling space than `inotask` needs to occupy.

The opportunity for `inotask` is therefore narrow but real: be the boring,
explicit, Linux-native filesystem event runner for ingestion and sysadmin
automation.

### Product thesis

`inotask` should not try to be a general job scheduler, a build tool, or a
cross-platform file watcher.

Its focused thesis should be:

> Run exact commands from exact filesystem events with exact event metadata,
> while making overload, recursion, and child-process behavior explicit.

This favors:

- explicit argv execution by default
- first-class event placeholders
- rule-level filtering
- startup validation
- clear logs
- bounded concurrency
- deliberate queueing policy
- opt-in recursive watching

It also means `inotask` can remain small while still solving real problems that
are awkward in `incron`, `systemd.path`, and ad-hoc shell loops.

### Core user needs

The strongest user needs to design around are:

- ingestion readiness: trigger after writers close files, especially with
  `CLOSE_WRITE`
- metadata handoff: pass `{full_path}`, `{entry_name}`, `{watch_path}`, and
  `{event}` without shell parsing
- safe execution: avoid surprise shell expansion and filename quoting bugs
- multiple workflows per path: let several rules share one watched directory
- noise control: filter include/exclude patterns before launching tasks
- loop control: detect or prevent self-triggering workflows
- overload control: define what happens when events arrive faster than tasks
  finish
- service friendliness: behave predictably under `systemd` and `journald`
- diagnosability: log what matched, what launched, and how children exited

The current implementation already covers several of these needs:

- rules-first configuration
- merged derived watches
- `CLOSE_WRITE`
- placeholder expansion
- include/exclude filters
- shell-free `execv()`
- async task launch
- zombie-safe child reaping
- stderr logging for service use

### Roadmap

The roadmap should preserve the current simple event-runner model while adding
control knobs where users actually feel pain.

#### Phase 1: Reliability and clarity

Near-term work should make current behavior easier to trust:

- add a dry-run or validate-only mode
- log expanded argv before task launch
- include watch descriptor, rule name, task name, and event mask in debug logs
- document deliberate `/bin/sh -c` usage for users who need shell features
- add example `systemd` service units
- add examples for ingestion, media processing, backup sync, and config reloads

#### Phase 2: Backpressure

The next major feature area should be overload behavior:

- per-rule `max_concurrency`
- per-rule or per-task `on_busy = parallel | drop | queue`
- optional queue size limits
- explicit logs for dropped or delayed events
- child exit status policy hooks
- flood tests for rapid create/modify/delete workloads

This is more important than adding many new event names because uncontrolled
process spawning is one of the fastest ways for a useful watcher to become
dangerous.

#### Phase 3: Debounce and coalescing

Many filesystem operations produce bursts rather than one clean event.
`inotask` should eventually support:

- per-rule debounce windows
- per-file duplicate suppression
- aggregate task mode for "run once after the burst"
- event summaries passed to aggregate tasks through a file or environment

This should be opt-in. The current one-event-one-launch model is still useful
and should remain easy to understand.

#### Phase 4: Loop protection

Self-triggering workflows are common when tasks write logs, transformed files,
temporary files, or state back into a watched tree.

Useful protections include:

- documented include/exclude patterns for output directories
- optional per-rule cooldown
- optional ignore patterns for task-created outputs
- clear logs when the same rule fires repeatedly on the same path

The goal is not to magically prove intent. The goal is to make accidental loops
visible and easy to prevent.

#### Phase 5: Recursive watching

Recursive watching should be added carefully, not as a casual default.

Required design points:

- opt-in `recursive = true`
- startup directory walk
- automatic watch insertion for newly created directories
- clear errors when kernel watch limits are reached
- documentation for `fs.inotify.max_user_watches`
- recovery behavior for queue overflow
- tests for directory creation races

Recursive support is valuable, but it is also where inotify tools become most
surprising. `inotask` should treat it as a reliability feature, not a checkbox.

#### Phase 6: Richer event surface

After the runtime policy is stronger, expand the event model:

- split `MOVE` into moved-from and moved-to events
- expose move cookies where available
- add delete-self, move-self, open, close-nowrite, and ignored events as needed
- preserve simple aliases for users who do not need raw inotify detail

The design should keep friendly event names while allowing advanced users to
reason about raw Linux behavior when they need it.

### Positioning statement

`inotask` is a small Linux filesystem-event runner for predictable ingestion
and sysadmin automation.

It watches explicit paths, matches explicit rules, passes explicit event
metadata, and launches explicit argument vectors without a shell by default.

It should become the tool users reach for when `systemd.path` is too coarse,
`incron` is too brittle, and an `inotifywait` shell loop has grown teeth.

## Likely future directions

Possible next design areas include:

- per-file queueing
- bounded concurrency
- per-file duplicate suppression
- richer Linux event coverage
- recursive directory management
- optional aggregate/coalesced task modes
