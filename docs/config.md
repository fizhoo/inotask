# Configuration Guide

`inotask` uses a small block-based configuration format with two top-level
constructs:

- `task`
- `rule`

## Overview

A `task` defines what executable to run and which arguments to pass.
A `rule` defines which path and events trigger one or more tasks.

## Task blocks

A task has:

- a name
- an absolute executable path
- an `args` list

Example:

```cfg
task ingest_tmp_file {
    exec = "/usr/bin/echo"
    args = [ "TMP_READY", "{full_path}", "{entry_name}", "{event}" ]
}
```

### Task fields

#### `exec`

Absolute path to the executable.

At startup, `inotask` validates that the path is:

- absolute
- present on disk
- a regular file
- executable

Example:

```cfg
exec = "/usr/bin/echo"
```

#### `args`

Optional list of argument templates appended after `exec`.

Example:

```cfg
args = [ "hello", "world" ]
```

At runtime, `inotask` launches tasks as:

```text
[ exec, args..., NULL ]
```

There is no shell involved.

## Rule blocks

A rule has:

- a name
- a watched absolute path
- an event list
- optional `include` patterns
- optional `exclude` patterns
- a `run` list of task names

Example:

```cfg
rule tmp_ready {
    watch = "/tmp"
    events = [ CLOSE_WRITE ]
    run = [ "ingest_tmp_file" ]
}
```

### Rule fields

#### `watch`

Absolute watched path.

Example:

```cfg
watch = "/tmp"
```

#### `events`

One or more event names.

Example:

```cfg
events = [ CREATE, CLOSE_WRITE ]
```

#### `run`

One or more task names. Each name must refer to a declared `task`.

Example:

```cfg
run = [ "ingest_tmp_file", "audit_tmp_file" ]
```

#### `include`

Optional glob-style filename patterns. If present, the event entry name must
match at least one pattern.

Example:

```cfg
include = [ "*.xml", "*.json" ]
```

#### `exclude`

Optional glob-style filename patterns. If present, the event entry name must
not match any of them.

Example:

```cfg
exclude = [ "#*", ".*", "*.swp", "*.tmp" ]
```

## Supported event names

Current supported events are:

- `CREATE`
- `MODIFY`
- `DELETE`
- `MOVE`
- `ATTRIB`
- `CLOSE_WRITE`

### Event notes

#### `CREATE`
Triggers when a file or directory is created inside a watched directory.

#### `MODIFY`
Triggers on file modification activity.

#### `DELETE`
Triggers when a file or directory is deleted.

#### `MOVE`
Represents move/rename activity in the current simplified event model.

#### `ATTRIB`
Triggers on metadata changes such as permissions or timestamps.

#### `CLOSE_WRITE`
Triggers when a file that was opened for writing is closed.
This is often the best event for file ingestion pipelines.

## Argument placeholders

Each string in `args` may contain placeholders expanded at runtime.

Think of placeholders as event variables. They let one static config line pick
up values from the specific filesystem event that triggered the rule.

Supported placeholders:

- `{watch_path}`
- `{entry_name}`
- `{full_path}`
- `{event}`

### `{watch_path}`
The watched path from the matching rule.

### `{entry_name}`
The filename reported by the event, relative to the watched directory.
If no filename is present, this becomes an empty string.

### `{full_path}`
The watched path plus the entry name when one exists.
If no entry name is present, this becomes the watched path.

### `{event}`
A display-style event string such as `CREATE` or `CLOSE_WRITE`.

### Expansion example

Config:

```cfg
args = [ "READY", "{full_path}", "{event}" ]
```

Possible runtime argv tail:

```text
READY /tmp/report.txt CLOSE_WRITE
```

## Filename filtering

Rules may optionally filter events by `entry_name` using glob-style patterns.

Matching rules:

- if `include` is absent, the include check passes automatically
- if `include` is present, `entry_name` must match at least one pattern
- if `exclude` is present, `entry_name` must match none of the patterns
- if filters are present and the event has no entry name, the rule does not
  match

Examples:

- `*.xml`
- `*.json`
- `#*`
- `.*`
- `*.tmp`

This is useful for ignoring editor temp files, dotfiles, swap files, or other
unrelated noise in a watched directory.

## Example configs

### Ingest file after close

```cfg
task ingest_tmp_file {
    exec = "/usr/bin/echo"
    args = [ "TMP_READY", "{full_path}", "{entry_name}", "{event}" ]
}

rule tmp_ready {
    watch = "/tmp"
    events = [ CLOSE_WRITE ]
    exclude = [ "#*", ".*", "*.swp", "*.tmp" ]
    run = [ "ingest_tmp_file" ]
}
```

### Separate create and close-write handling

```cfg
task tmp_created {
    exec = "/usr/bin/echo"
    args = [ "TMP_CREATE", "{full_path}" ]
}

task ingest_tmp_file {
    exec = "/usr/bin/echo"
    args = [ "TMP_READY", "{full_path}" ]
}

rule tmp_created {
    watch = "/tmp"
    events = [ CREATE ]
    exclude = [ "#*", ".*", "*.swp", "*.tmp" ]
    run = [ "tmp_created" ]
}

rule tmp_ready {
    watch = "/tmp"
    events = [ CLOSE_WRITE ]
    exclude = [ "#*", ".*", "*.swp", "*.tmp" ]
    run = [ "ingest_tmp_file" ]
}
```

## Validation notes

After parsing succeeds, `inotask` performs semantic validation before runtime startup.

Current validation includes:

- watched paths must be absolute
- task executable paths must be absolute
- duplicate task names
- duplicate rule names
- unknown task names referenced by `run`
- duplicate task names inside a single rule `run` list
- empty event lists
- empty `run` lists
- invalid task `exec` paths

This means many configuration mistakes are rejected before any `inotify` watches are opened.

## Notes

- Tasks are launched with `execv()`, not through a shell.
- Unknown placeholders are currently left unchanged.
- One matching event currently launches one new child process per matching task.
