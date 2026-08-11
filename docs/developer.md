# Developer Guide

This guide maps responsibilities, ownership, and runtime control flow. Read
`design.md` first for policy and `config.md` for user-facing behavior.

## Program Flow

```text
config file
  -> loader
  -> lexer and parser
  -> config model
  -> validator
  -> runtime watch plan
  -> inotify session
  -> event matching and task launch
```

`inotask_main.c` coordinates the process. Other modules keep parsing, storage,
validation, watch management, and logging separate.

## Modules

### `inotask_config.h` / `inotask_config.c`

Owns the configuration data model and its memory:

- `it_str` and `it_str_vec`
- `it_task`, `it_rule`, and `it_watch`
- `it_config`
- task and rule insertion
- derived-watch merging
- configuration error strings

Adding a rule also merges its path and event mask into `cfg->watches`. The
derived list is runtime input; configured rules remain the source for matching.

### `inotask_lexer.h` / `inotask_lexer.c`

Converts source text into identifiers, strings, punctuation, and end-of-file
tokens. It does not understand task or rule semantics.

### `inotask_parser.h` / `inotask_parser.c`

Consumes tokens and populates `it_config`. It owns grammar rules, required and
optional fields, list syntax, duplicate-field detection, and line/column parse
errors.

### `inotask_validate.h` / `inotask_validate.c`

Checks relationships and filesystem properties after parsing:

- duplicate task and rule names
- unknown or repeated task references
- task executable existence, type, and execute permission

Absolute-path and empty-list checks occur while config objects are added.

### `inotask_load.h` / `inotask_load.c`

Provides the file-oriented loading entry point. It reads the file, invokes the
parser and validator, and converts their failures into user-facing log messages.

### `inotask_runtime.h` / `inotask_runtime.c`

Owns watch planning and the live inotify session:

- concrete `it_watch_target` values
- watch-descriptor bindings
- one inotify file descriptor
- installation and cleanup of watches
- direct preservation of configured inotify event masks

The current lookup is a linear scan of watch bindings. This is sufficient for
the current non-recursive scale but should be reconsidered with recursion.

### `inotask_log.h` / `inotask_log.c`

Provides severity filtering and stderr output. `INOTASK_LOG_LEVEL` is parsed
once during startup. Runtime code should use `it_log_*()` rather than writing
diagnostics directly.

Use levels consistently:

- `ERROR`: failed operation or lost correctness
- `WARN`: recoverable but suspicious condition
- `INFO`: service or task lifecycle
- `DEBUG`: per-watch, per-event, matching, or timer detail

### `inotask_main.c`

Owns top-level runtime policy:

- CLI handling and startup summaries
- signal flags and child reaping
- the `poll()` and `read()` event loop
- event path construction and rule matching
- filename filtering
- settle timer storage and expiry
- placeholder expansion and argv construction
- `fork()` and `execv()`
- final cleanup and process status

This file is intentionally the policy boundary. Generic watch-session behavior
belongs in `inotask_runtime.c`; generic config behavior belongs in config,
parser, or validation modules.

## Ownership

### Configuration

`it_config` owns every task, rule, derived watch, string, and string vector
inserted into it. Release the complete graph with `it_config_free()`.

Insertion helpers follow a commit-on-success pattern: vector lengths advance
only after all storage for the new value has been acquired.

### Runtime Plan

`it_runtime_plan` owns copied target paths. It does not borrow path storage from
the configuration. Release it with `it_runtime_plan_free()`.

### Runtime Session

`it_runtime_session` owns the inotify file descriptor and watch bindings.
`it_runtime_session_free()` closes the descriptor and releases bindings.

### Pending Events

The pending-event vector in `inotask_main.c` owns copied entry and full paths.
Removing or freeing a pending item must release both strings.

### Task Arguments

Each launch builds a temporary expanded argv. The parent frees its copy after
`fork()`. The child uses its inherited copy until `execv()` replaces the process
image or fails.

## Control Flows

### Startup

1. Parse CLI arguments and logging environment.
2. Load and validate the configuration.
3. Initialize and build the runtime plan.
4. Print summaries.
5. Return early for `--check`.
6. Open watches and install signal handlers.
7. Enter the event loop.

All failures before step 7 return nonzero after releasing initialized state.

### Event Loop

Each iteration:

1. reaps children when requested
2. launches expired settled events
3. computes the nearest settle deadline
4. polls the inotify descriptor
5. reads and bounds-checks raw event records
6. handles queue overflow before watch lookup
7. resolves ordinary events and retains their exact inotify bits
8. dispatches matching rules

`SIGINT` or `SIGTERM` interrupts polling and leads to a zero-status cleanup.
Fatal event-source failures set a nonzero final status before using the same
cleanup path.

### Rule Matching

A rule matches when:

1. its configured path equals the runtime target path
2. its event mask overlaps the raw event's `IN_ALL_EVENTS` bits
3. inclusion patterns pass, when present
4. exclusion patterns do not match

Filters use `entry_name`. This exact-path assumption is the main dispatch
boundary recursive watching must redesign.

### Settling

Pending work is keyed by rule index and full path. A repeated match merges the
event mask and replaces the due time. Expiry reconstructs event variables from
the stored paths and launches the rule once.

### Child Lifecycle

The parent logs each launch and immediately resumes event processing. `SIGCHLD`
only sets a flag. The main loop calls `waitpid(-1, ..., WNOHANG)` until no exited
children remain and logs each status.

## Extending the Code

### Add a Configuration Field

1. Add storage and defaults to the config model.
2. Parse the field and reject duplicates.
3. Add semantic validation when needed.
4. Update summary output if operationally useful.
5. Implement runtime behavior.
6. Update `config.md`, design notes, and tests.

### Expose More Event Metadata

Configurable event names come directly from Linux inotify. To expose metadata
such as move cookies or output-only flags, extend event variables, placeholder
expansion, diagnostics, documentation, and integration tests without inventing
an alternate event vocabulary.

### Add Recursive Watching

Do not only append subdirectory paths. Runtime targets must distinguish the
configured rule root from the concrete watched directory. Recursive work also
needs dynamic insertion/removal, symlink policy, queue-overflow recovery, and a
nonlinear watch-descriptor lookup.

## Validation Workflow

The repository includes two maintained test layers:

- `tests/test_runtime.c` checks exact event-mask preservation, config errors, derived
  watch merging, and runtime-plan construction
- `tests/integration.sh` launches the real daemon against temporary directories
  and checks config errors, filtering, settling, logging, child failures, and
  graceful shutdown

Before publishing a change:

```sh
make clean
make
make check
make test
git diff --check
```

Use `make scan`, `make san`, and the checks in `releasing.md` for release work
or changes involving ownership, signals, parsing, or event-record handling.

## Safety Boundaries

- Task execution is shell-free unless the config explicitly invokes a shell.
- Configuration is validated before watches open.
- Signal handlers do not allocate or log.
- Children are reaped asynchronously.
- Runtime ownership has explicit init/free pairs.
- Queue overflow is visible but not recoverable yet.

See `roadmap.md` for planned policy and reliability work.
