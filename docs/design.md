# Design

This document describes the architecture and intentional runtime policies of
`inotask`. Configuration syntax belongs in `config.md`; implementation details
belong in `developer.md`; future work belongs in `roadmap.md`.

## Goals

`inotask` is a small, predictable filesystem event runner. Its design favors:

- explicit rules and event names
- explicit executable paths and argument vectors
- early configuration validation
- continued event ingestion while tasks run
- observable service and child-process behavior
- small policy surfaces that can be extended deliberately

It is Linux-specific because its event model is built directly on inotify.

## Configuration Model

Users define:

- a `task`: named executable plus argument templates
- a `rule`: path, events, filters, settle policy, and task names

The configuration layer derives a third object:

- a `watch`: path plus the union of events required by every rule on that path

This rules-first model matches user intent while avoiding duplicate kernel
watches. Rules sharing a path still match independently, so they may use
different events, filters, settle windows, and task lists.

## Startup

Startup proceeds in a fixed order:

1. read and parse the configuration
2. validate names, references, paths, and task executables
3. derive the merged watch plan
4. print configuration and runtime-plan summaries
5. open one inotify instance
6. install each derived watch
7. install signal handlers
8. enter the event loop

`--check` stops after step 4. This makes it suitable for preflight checks
without consuming inotify resources.

## Inotify Model

One inotify instance and file descriptor serve every configured path. Each
installed watch receives a kernel watch descriptor (`wd`), which the runtime
maps back to its concrete watch target.

Configuration event names and masks map directly to Linux inotify. The twelve
primitive bits in `IN_ALL_EVENTS` are accepted by their exact `IN_*` names, as
are the Linux aggregate masks `IN_CLOSE`, `IN_MOVE`, and `IN_ALL_EVENTS`.

There is no normalization layer. `IN_CREATE` matches only `IN_CREATE`, while
`IN_MOVED_TO` matches only `IN_MOVED_TO`. Move cookies and output-only flags
such as `IN_ISDIR` are not yet exposed through task placeholders.

## Event Dispatch

For each raw event record, the main loop:

1. handles `IN_Q_OVERFLOW` before watch lookup
2. resolves `wd` to a runtime target
3. retains the raw event's `IN_ALL_EVENTS` bits
4. constructs event paths and placeholders
5. scans rules that use the target path
6. checks event overlap and filename filters
7. launches immediately or updates a settle timer

Filename filters use `entry_name`, not `full_path`. A rule with filters does not
match an event that lacks an entry name.

## Task Execution

Tasks run through `fork()` and `execv()`. The daemon builds argv explicitly as:

```text
[ exec, expanded_args..., NULL ]
```

There is no implicit shell. This avoids shell quoting and expansion surprises,
especially for event paths containing spaces or punctuation. A configuration
may invoke a shell explicitly, but literal event substitution makes wrapper
executables safer for nontrivial commands or untrusted filenames.

Task arguments are templates expanded immediately before launch. Expansion is
literal: placeholder substitution does not trigger further parsing.

## Asynchronous Children

The parent does not wait for a launched task before returning to event
processing. `SIGCHLD` sets a flag, and the main loop drains exited children with
`waitpid(..., WNOHANG)`.

This prevents zombies and keeps the event loop responsive, but the current
policy permits unbounded concurrent children. There is no worker pool, queue,
or busy-task policy yet.

## Settling

`settle_ms` provides opt-in, per-path coalescing for noisy events. Pending work
is keyed by rule and full path. A repeated match updates the accumulated event
mask and resets the deadline. The rule runs once after that path remains quiet.

Settling is intentionally not a general queue. Different rules or paths retain
independent timers, and immediate rules continue to launch once per match.

## Signals and Shutdown

Signal handlers only set `sig_atomic_t` flags:

- `SIGCHLD` requests child reaping
- `SIGINT` and `SIGTERM` request shutdown

The event loop handles the request after an interrupted system call, then frees
pending events, closes the inotify session, releases the watch plan, and frees
the configuration. A requested shutdown returns zero.

Fatal polling, inotify read, stream closure, or descriptor failures use the
same cleanup path and return nonzero so a service manager can restart the
daemon.

## Failure Policy

Startup failures return nonzero before event processing begins. During runtime:

- a task allocation, `fork()`, or child `execv()` failure is logged and event
  processing continues
- a child failure does not terminate the daemon
- queue overflow is logged and processing continues because future events are
  still useful
- a broken event source terminates the daemon with a nonzero status

After `IN_Q_OVERFLOW`, the kernel cannot identify the lost events. The current
runtime reports that uncertainty but does not rebuild state.

## Logging

Configuration summaries go to stdout. Diagnostics go to stderr through a small
severity-filtered logger.

- `INFO` covers service and task lifecycle
- `DEBUG` covers watch bindings, raw records, exact event masks, matching, and
  settle updates
- `WARN` and `ERROR` identify suspicious and failed operations

systemd captures both streams in the journal. The logger does not depend on
systemd or emit journal-specific metadata.

## Current Boundaries

The current implementation deliberately does not provide:

- recursive directory management
- bounded concurrency or queue policy
- queue-overflow recovery
- config live reload
- per-task credentials
- loop detection
- move cookies and output-only inotify flags in task placeholders

The order and rationale for extending these boundaries are maintained in
`roadmap.md`.
