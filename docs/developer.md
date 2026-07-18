# Developer Guide

This document is a code-tour for `inotask`.
It is meant to help a developer quickly answer:

- where config text becomes structured data
- where validation happens
- how watches are built
- how runtime events become task launches
- which modules own which responsibilities

## Big picture

The current program flow is:

1. `inotask_main.c` starts the process
2. `inotask_load.c` reads the config file from disk
3. `inotask_parser.c` parses config text into an `it_config`
4. `inotask_validate.c` checks semantic and executable-path validity
5. `inotask_runtime.c` builds the runtime watch plan and opens `inotify`
6. `inotask_main.c` reads events, matches rules, expands placeholders, and forks children
7. `inotask_log.c` reports runtime diagnostics to stderr

## Module tour

### `inotask_config.h` and `inotask_config.c`

This is the core data-model layer.

It defines the main structs used everywhere else:

- `it_str` — owned heap string with explicit length
- `it_str_vec` — dynamic array of owned strings
- `it_task` — one named executable plus argument templates
- `it_rule` — one rule with watch path, event mask, optional include/exclude filters, and task names to run
- `it_watch` — one derived watch path plus merged event mask
- `it_config` — top-level config object containing watches, tasks, and rules

This layer is also responsible for:

- initializing and freeing config-owned memory
- adding tasks and rules into the config model
- merging rules into the derived watch list
- returning human-readable config validation error strings

### `inotask_lexer.h` and `inotask_lexer.c`

This is the tokenization layer.

It turns raw config text into tokens such as:

- identifiers
- strings
- `{` and `}`
- `[` and `]`
- `,`
- `=`

The lexer does not understand tasks or rules as concepts.
It only answers: “what is the next token in this input buffer?”

### `inotask_parser.h` and `inotask_parser.c`

This is the syntax layer.

It consumes lexer tokens and builds structured config objects.

It understands the grammar for:

- `task NAME { ... }`
- `rule NAME { ... }`
- field lists such as `args = [ ... ]`
- event lists such as `events = [ CREATE, CLOSE_WRITE ]`

The parser is responsible for syntactic correctness.
Examples of parser-level failures:

- missing `}`
- malformed list syntax
- unknown field shape
- bad token ordering

It reports line/column information through `it_parse_error`.

### `inotask_validate.h` and `inotask_validate.c`

This is the semantic validation layer.

It runs after parsing succeeds.

This layer checks things that require looking across the full parsed config,
including:

- duplicate task names
- duplicate rule names
- unknown task names in a rule `run` list
- duplicate task names inside one rule `run` list
- whether each task `exec` path exists
- whether each task `exec` path is a regular file
- whether each task `exec` path is executable

This layer is what turns “the file parsed” into “the config is actually usable.”

### `inotask_load.h` and `inotask_load.c`

This is the orchestration layer for config loading.

It handles the full startup sequence for configuration:

- read file contents
- parse into `it_config`
- validate the parsed result
- report user-facing load errors

If you want one entry point for “load the config file correctly,” this is it.

### `inotask_runtime.h` and `inotask_runtime.c`

This is the runtime watch layer.

It translates validated config data into an actual `inotify` session.

Key responsibilities:

- build a runtime plan from derived watches
- open one `inotify` file descriptor
- install one watch per derived target
- keep a mapping from inotify watch descriptor (`wd`) to runtime target
- convert between internal event masks and Linux `inotify` masks

Current design:

- one `inotify` fd
- many watch descriptors attached to that one fd
- blocking reads on that fd in the main loop

### `inotask_log.h` and `inotask_log.c`

This is a small stderr logging helper.

It provides severity-based logging:

- `ERROR`
- `WARN`
- `INFO`
- `DEBUG`

Runtime code uses this instead of sprinkling raw `fprintf(stderr, ...)`
through the project.

### `inotask_main.c`

This is the top-level runtime driver.

It currently owns the live daemon behavior:

- startup summary printing
- `SIGCHLD` handling and child reaping
- blocking reads from the `inotify` fd
- per-event rule matching
- include/exclude filename filtering with `fnmatch()`
- runtime placeholder expansion for task arguments
- `fork()` + `execv()` task launching
- child exit status logging

If you want to understand what happens after an event arrives, this is the file
that matters most.

## Important control flows

### Config startup flow

The startup path is roughly:

1. initialize `it_config`
2. call `it_load_config_file()`
3. print startup summary tables
4. build runtime plan
5. open runtime session
6. enter the event loop

This means config problems are caught early, before any watches are installed.

### Rule-to-watch derivation

Users write rules, not low-level watch objects.
Internally, rules that point at the same `watch` path are merged into one
derived watch with a combined event mask.

That gives two benefits:

- config stays rule-oriented and easy to read
- runtime avoids installing duplicate watches for the same path when only the event mask needs to be widened

Important detail: matching still happens at the rule level later.
The derived watch only controls what the kernel reports to us.

### Event loop flow

At runtime, the event loop does this:

1. block in `read()` on the single `inotify` fd
2. receive one or more raw `struct inotify_event` records
3. map each event `wd` back to a watched path target
4. convert the Linux mask to the internal `it_event_mask`
5. log the event summary
6. scan configured rules for matches
7. for each matching task, build argv and launch a child
8. periodically reap dead children when `SIGCHLD` has fired

### Rule matching flow

A rule matches an event only if all of these pass:

- watched path matches the target path
- event masks overlap
- `include` patterns match, if any are configured
- `exclude` patterns do not match, if any are configured

The `include` and `exclude` filters are evaluated against `entry_name`, not the
full path.

Important edge case:
if a rule has `include` or `exclude` filters, but the event has no
`entry_name`, that rule does not match.

### Placeholder expansion flow

Configured task args are stored as templates.
Before `execv()`, `inotask_main.c` expands placeholders using the current
filesystem event.

Current supported placeholders are:

- `{watch_path}`
- `{entry_name}`
- `{full_path}`
- `{event}`

Example:

```cfg
args = [ "READY", "{full_path}", "{event}" ]
```

A matching close-write event for `/tmp/report.txt` becomes roughly:

```text
READY /tmp/report.txt CLOSE_WRITE
```

### Task launch flow

For each matched task:

1. resolve the named `task` from the config
2. expand its `args` templates for the current event
3. build `argv` as `[ exec, expanded_args..., NULL ]`
4. `fork()`
5. in the child, call `execv()`
6. in the parent, log the child pid and continue the event loop

There is no shell involved.
That means no shell quoting, no pipelines, and no redirection syntax in task
config.

### Child reaping flow

Because children run asynchronously, the parent must reap them.

Current model:

- `SIGCHLD` handler sets a flag
- the main loop notices the flag
- `waitpid(-1, &status, WNOHANG)` is called until no reapable children remain
- exit/signal status is logged

This prevents zombie accumulation in a long-running daemon.

## Memory ownership notes

### `it_str`

`it_str` owns a heap string and stores its length.
It is the basic owned-string type for the config model.

### `it_str_vec`

`it_str_vec` is a growable array of `it_str` values.
This is used for things like:

- task args
- rule include patterns
- rule exclude patterns
- rule run lists

### Why helpers like `str_vec_push()` exist

Helpers such as `str_vec_push()` handle the repetitive but important work of:

- growing capacity with `realloc()`
- copying incoming text into owned storage
- updating vector length only on success

That gives the parser and config-building code one safe path for “append this
string to the vector.”

In other words, it is not parser-specific logic.
It is shared config-storage infrastructure used by the parser/config layer.

## Current safety model

The current implementation already makes a few important safety choices:

- task execution is shell-free via `execv()`
- config is validated before watches are opened
- executable paths are checked for existence, type, and execute permission
- child processes are reaped
- logging is explicit and separated from the summary table output

## Current limitations

These are important for any contributor to understand:

- Linux-only via `inotify`
- no recursive watch walking
- no queueing or flood control yet
- no worker pool or concurrency limit yet
- one matching event can launch one new child immediately
- no config live-reload yet
- filtering is filename-based glob matching, not regex matching

## Good next places to extend

If we keep building from today’s foundation, the most natural next layers are:

- `IN_Q_OVERFLOW` handling and reporting
- bounded concurrency / worker limits
- optional event coalescing modes
- config live-reload with parse-validate-swap
- systemd service examples and hardening guidance
- optional per-task user/group execution controls
