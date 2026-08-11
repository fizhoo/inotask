# inotask

`inotask` is a small Linux filesystem event runner built on `inotify`. It
watches configured paths, matches events against rules, and launches explicit
argument vectors with `fork()` and `execv()`.

It is designed for ingestion and sysadmin workflows that need more event detail
than `systemd.path` without growing into a shell loop around `inotifywait`.

## Features

- Exact Linux inotify event names and masks
- Shell-free task execution with explicit executable paths and arguments
- Runtime placeholders for event paths and names
- Glob-style filename inclusion and exclusion
- Per-rule `settle_ms` quiet windows for noisy events
- Asynchronous children with zombie-safe reaping
- Config validation through `--check`
- Graceful `SIGINT` and `SIGTERM` shutdown
- Inotify queue-overflow detection
- Environment-controlled log verbosity
- systemd unit and install targets

## Build and Run

```sh
make
cp inotaskd-sample.conf inotaskd.cfg
# Edit inotaskd.cfg for this machine.
./inotask --check inotaskd.cfg
./inotask inotaskd.cfg
```

`inotaskd-sample.conf` is the maintained, fully commented configuration
reference. Local `inotaskd.cfg` files are ignored by Git. Make targets use the
local file when it exists and fall back to the sample otherwise.

Useful development targets:

```sh
make check
make test
make run
make scan
make san
make live
```

## Example

```cfg
task ingest_file {
    exec = "/usr/bin/echo"
    args = [ "READY", "{full_path}", "{event}" ]
}

rule files_ready {
    watch = "/srv/incoming"
    events = [ IN_CLOSE_WRITE ]
    exclude = [ ".*", "*.tmp", "*.swp" ]
    run = [ "ingest_file" ]
}
```

Closing `/srv/incoming/report.csv` after writing launches approximately:

```text
/usr/bin/echo READY /srv/incoming/report.csv IN_CLOSE_WRITE
```

`IN_CLOSE_WRITE` is usually preferable to `IN_CREATE` or `IN_MODIFY` for
ingestion because it indicates that the writing side closed the file.

## Configuration

A configuration contains two block types:

- `task`: a named executable and argument templates
- `rule`: a watched path, event mask, filters, settle policy, and tasks to run

Task arguments may contain:

- `{watch_path}`: configured watch path
- `{entry_name}`: event name relative to the watched directory
- `{full_path}`: watch path joined with the entry name
- `{event}`: exact inotify event names

Rules watching the same path share one merged inotify watch while retaining
their own events, filters, settle policy, and task list.

See `inotaskd-sample.conf` for an annotated configuration and `docs/config.md`
for the complete format and validation rules.

## Runtime Behavior

At startup, `inotask` parses and validates the configuration, derives merged
watches, prints a summary, and opens one inotify instance. Matching tasks run
asynchronously; the daemon continues reading events and reaps children when
they exit.

Rules dispatch immediately unless `settle_ms` is nonzero. Settling is tracked
per rule and full path. Repeated matching events reset that path's timer, and
the rule runs once after the path remains quiet for the configured interval.

There is no shell command layer, quoting syntax, pipeline parsing, or
redirection handling.

See `docs/design.md` for architecture and policy details.

## Logging

Startup summaries go to stdout. Diagnostics and runtime logs go to stderr.

```sh
INOTASK_LOG_LEVEL=debug ./inotask inotaskd.cfg
```

Accepted levels are cumulative:

- `error`: failures only
- `warn`: errors plus recoverable or suspicious conditions
- `info`: service lifecycle, task launches, and child exits; the default
- `debug`: watch bindings, raw events, rule decisions, and settle updates

Fatal event-loop failures return nonzero. Recoverable failures such as queue
overflow or an individual task launch failure are logged while the daemon
continues when possible.

systemd captures stdout and stderr in the journal without a special logging
mode. See `docs/systemd.md` for installation and operation.

## Limitations

- Linux-only
- Non-recursive watches
- No queue, concurrency bound, or worker pool
- No queue-overflow recovery beyond reporting lost-event risk
- No live config reload
- Move cookies are not yet exposed to tasks
- One child is launched per matching task unless settling coalesces the event

Planned work is tracked in `docs/roadmap.md`.

## Documentation

- `docs/config.md`: configuration reference
- `docs/design.md`: architecture and design decisions
- `docs/developer.md`: code organization and control flow
- `docs/systemd.md`: service installation and operation
- `docs/roadmap.md`: ordered feature roadmap
- `docs/releasing.md`: release checklist
- `inotaskd-sample.conf`: fully commented sample configuration
