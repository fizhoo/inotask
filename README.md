# inotask

`inotask` is a small Linux filesystem event runner built on `inotify`.
It watches configured paths, matches events against rules, and launches tasks
with `fork()` + `execv()` when rules fire.

It is aimed at simple, explicit per-file workflows where one matching event can
launch one new process immediately, with optional per-path settling for noisy
events such as `MODIFY`.

The current design is intentionally simple:

- rules are declared in a config file
- watches are derived from those rules
- matching events either launch immediately or update a per-path settle timer
- tasks run asynchronously
- exited child processes are reaped to avoid zombies
- task arguments can include runtime event placeholders

## Highlights

- Linux `inotify`-based event watching
- explicit `execv()` task launching without a shell
- rule-driven config with derived watch merging
- per-event placeholder expansion in task arguments
- glob-style `include` / `exclude` filename filtering
- optional per-rule `settle_ms` quiet window for noisy paths
- startup validation of executable paths
- `CLOSE_WRITE` support for ingestion-style workflows
- async child launching with zombie reaping
- graceful `SIGINT` / `SIGTERM` shutdown
- inotify queue overflow detection
- `--check` mode for parse/validate without opening watches
- launch logs include the expanded `execv()` argument vector

## Current status

`inotask` is usable today for per-file event handling, especially when you want
explicit filesystem events to trigger task launches with predictable argument
expansion.

Current supported event names:

- `CREATE`
- `MODIFY`
- `DELETE`
- `MOVE`
- `ATTRIB`
- `CLOSE_WRITE`

Current supported argument placeholders:

- `{watch_path}` — the watched path from the matching rule
- `{entry_name}` — the filename reported by the event, relative to the watched directory; if no filename is present, this becomes an empty string
- `{full_path}` — the watched path plus the entry name when one exists; if no entry name is present, this becomes the watched path
- `{event}` — a display-style event string such as `CREATE` or `CLOSE_WRITE`

Argument placeholders are runtime variables you can embed inside task `args`.
When a rule fires, `inotask` expands them using values from the actual event
before calling `execv()`.

For example:

```cfg
args = [ "{full_path}", "{event}" ]
```

might expand to:

```text
/tmp/report.txt CLOSE_WRITE
```

## Build

```sh
make
```

## Run

```sh
./inotask inotaskd.cfg
```

Check a config without opening runtime watches:

```sh
./inotask --check inotaskd.cfg
```

Helper make targets:

```sh
make check
make run
make scan
make san
make live
make edit
```

## Quick example

```cfg
task ingest_tmp_file {
    exec = "/usr/bin/echo"
    args = [ "TMP_READY", "{full_path}", "{entry_name}", "{event}" ]
}

rule tmp_ready {
    watch = "/tmp"
    events = [ CLOSE_WRITE ]
    run = [ "ingest_tmp_file" ]
}
```

If a writer closes `/tmp/report.txt`, the task receives arguments roughly like:

```text
TMP_READY /tmp/report.txt report.txt CLOSE_WRITE
```

## Why `CLOSE_WRITE` matters

For ingestion-style workflows, `CREATE` or `MODIFY` may fire while a file is
still being written. `CLOSE_WRITE` is usually the better trigger when you want
to process a file after the writing side closes it.

For `MODIFY`-driven workflows, use `settle_ms` when repeated writes should
become one task launch after the path is quiet:

```cfg
rule docs_changed {
    watch = "/project/docs"
    events = [ MODIFY ]
    include = [ "*.md" ]
    settle_ms = 250
    run = [ "rebuild_index" ]
}
```

If `settle_ms` is omitted, it defaults to `0`, preserving immediate dispatch.
For ingestion based on `CLOSE_WRITE`, a settle window is usually unnecessary.

## Configuration model

A config file defines:

- `task` blocks: named executables plus argument templates
- `rule` blocks: watched path, event list, optional `include` / `exclude`
  filters, optional `settle_ms`, and task names to run

At startup, task executables are validated before the runtime begins. Today that includes checking that each `exec` path is absolute, exists, is a regular file, and is executable.

See `docs/config.md` for the full format.

## Why not a shell command string?

`inotask` currently runs tasks with explicit `execv()` argument vectors instead
of shell command strings.

That keeps behavior more predictable:

- no shell quoting surprises
- no pipe/redirection parsing in the daemon
- safer handling of filenames with spaces or punctuation
- clearer mapping from config to actual process arguments

## Runtime model

At runtime:

- the config is parsed and validated
- derived watches are built from the configured rules
- one inotify file descriptor is opened
- each event is matched against rules
- optional filename filters are applied at the rule level
- rules with `settle_ms` wait until each matching full path is quiet
- each matching task is launched with `execv()`
- `SIGINT` or `SIGTERM` exits the event loop and runs cleanup

See `docs/design.md` for the architecture and design notes.

## Logging

`inotask` writes summary tables to standard output and diagnostic/runtime logs
to standard error.

Examples:

- config load failures
- rule matches
- task launches with expanded argv
- child reap status
- graceful shutdown requests
- inotify queue overflow errors

When run under `systemd`, stderr is typically captured by `journald`.

## Current limitations

Current behavior is intentionally direct and does not yet implement advanced
queueing or flood-control policy.

Notable current limitations:

- Linux-only (`inotify`)
- no recursive watch walking
- kernel inotify queue overflow means events may already have been lost before
  `inotask` can process them
- immediate rules launch one new child process per matching task
- no per-file queueing or throttling yet
- `settle_ms` coalesces repeated events for the same rule and full path
- no shell command strings; tasks use `execv()` with explicit args

## Documentation

- `docs/config.md` — config format, events, filters, placeholders, examples
- `docs/design.md` — architecture, runtime behavior, and design choices
- `docs/developer.md` — code tour, control flow, ownership, and extension notes
