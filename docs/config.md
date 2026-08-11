# Configuration Reference

An `inotask` configuration contains named `task` and `rule` blocks. A task
defines a process to launch. A rule defines the filesystem events that launch
one or more tasks.

```cfg
task announce {
    exec = "/usr/bin/echo"
    args = [ "changed", "{full_path}", "{event}" ]
}

rule documents {
    watch = "/srv/documents"
    events = [ IN_CLOSE_WRITE ]
    include = [ "*.txt", "*.md" ]
    exclude = [ ".*", "*.tmp" ]
    run = [ "announce" ]
}
```

## Syntax

- Block and field names are identifiers.
- Paths, task references, patterns, and arguments are quoted strings.
- Lists use square brackets and commas.
- Trailing commas are not accepted.
- `#` starts a comment outside a quoted string.
- Quoted strings cannot contain escapes or span lines.
- Unknown fields and duplicate fields are errors.
- Block order is unrestricted; rule task references are resolved after parsing.

## Tasks

```cfg
task NAME {
    exec = "/absolute/path"
    args = [ "optional", "argument", "templates" ]
}
```

### `exec`

Required absolute path to an executable. Validation requires it to exist, be a
regular file, and be executable by the user starting `inotask`.

### `args`

Optional non-empty list of argument templates. Omit `args` when the executable
needs no additional arguments; an explicitly empty list is rejected.

The runtime calls `execv()` with:

```text
[ exec, expanded_args..., NULL ]
```

No shell is inserted. A task can technically execute a shell, but configuration
strings do not support escapes and event placeholders are substituted literally.
A wrapper executable is safer for shell behavior or untrusted filenames.

## Rules

```cfg
rule NAME {
    watch = "/absolute/path"
    events = [ EVENT, ... ]
    include = [ "optional", "patterns" ]
    exclude = [ "optional", "patterns" ]
    settle_ms = 250
    run = [ "task_name", ... ]
}
```

### `watch`

Required absolute path. The path must be watchable when runtime startup opens
the inotify session.

Rules that use the same path are merged into one kernel watch with the union of
their requested events. Rule matching and policy remain independent.

### `events`

Required non-empty list. Supported names are:

| Event | Meaning |
|---|---|
| `IN_ACCESS` | File was accessed |
| `IN_MODIFY` | File content was modified |
| `IN_ATTRIB` | Metadata changed |
| `IN_CLOSE_WRITE` | Writable file was closed |
| `IN_CLOSE_NOWRITE` | Non-writable file was closed |
| `IN_OPEN` | File was opened |
| `IN_MOVED_FROM` | Entry moved out of the watched directory |
| `IN_MOVED_TO` | Entry moved into the watched directory |
| `IN_CREATE` | Entry was created in the watched directory |
| `IN_DELETE` | Entry was deleted from the watched directory |
| `IN_DELETE_SELF` | Watched object was deleted |
| `IN_MOVE_SELF` | Watched object was moved |

The Linux aggregate masks `IN_CLOSE`, `IN_MOVE`, and `IN_ALL_EVENTS` are also
accepted. They expand to their constituent event bits in summaries and the
`{event}` placeholder.

Names and meanings match Linux inotify directly. For example, `IN_MOVED_TO`
does not also count as `IN_CREATE`. Use `IN_CLOSE_WRITE` for ingestion when
work should start after the writer closes the file. Use `IN_MODIFY` with
`settle_ms` when repeated modifications should
collapse into one launch after a quiet period.

### `run`

Required non-empty list of declared task names. Each task may appear only once
within a rule.

### `include`

Optional non-empty list of `fnmatch()`-style patterns. When present, the event's
`entry_name` must match at least one pattern.

### `exclude`

Optional non-empty list of `fnmatch()`-style patterns. A matching exclusion
rejects the event even when an inclusion matched.

```cfg
include = [ "*.xml", "*.json" ]
exclude = [ "#*", ".*", "*.swp", "*.tmp" ]
```

Filters apply to `entry_name`, not the full path. A filtered rule does not match
an event that has no entry name.

### `settle_ms`

Optional unsigned quiet period in milliseconds. The default is `0`, which
dispatches immediately.

For a nonzero value, pending work is keyed by rule and `full_path`. Every new
matching event updates the stored event mask and resets that path's deadline.
The rule runs once after the path remains quiet for the configured interval.

```cfg
rule source_changed {
    watch = "/srv/source"
    events = [ IN_MODIFY ]
    include = [ "*.c", "*.h" ]
    settle_ms = 250
    run = [ "rebuild" ]
}
```

`--check` warns when a rule watches `IN_MODIFY` with `settle_ms = 0` because active
writes may launch repeated tasks.

## Argument Placeholders

Each task argument may contain zero or more placeholders:

| Placeholder | Value |
|---|---|
| `{watch_path}` | Configured watch path from the matching rule |
| `{entry_name}` | Name reported relative to the watched directory, or empty |
| `{full_path}` | Watch path joined with the entry name, or the watch path itself |
| `{event}` | Exact event names such as `IN_CREATE` or `IN_CLOSE_WRITE` |

Placeholders may be embedded within other text:

```cfg
args = [ "path={full_path}", "event={event}" ]
```

Unknown placeholder names are left unchanged. Expansion produces literal argv
strings; no shell quoting or evaluation occurs.

## Validation

Use check mode before deploying a configuration:

```sh
./inotask --check inotaskd.cfg
```

Start from `inotaskd-sample.conf` for a fully commented example covering every
configuration field, placeholder, supported event name, and aggregate mask.

Check mode reads, parses, validates, builds the derived watch plan, prints the
startup summary, and exits without opening inotify watches.

Validation rejects:

- malformed syntax, unknown fields, duplicate fields, and empty explicit lists
- relative task executable or watch paths
- missing, non-regular, or non-executable task executables
- duplicate task or rule names
- unknown or repeated task names in a rule's `run` list
- missing required task or rule fields

Whether a configured watch path can actually be opened is checked only when the
runtime session starts.

## Complete Example

```cfg
task ingest_file {
    exec = "/usr/bin/echo"
    args = [ "READY", "{full_path}", "{entry_name}", "{event}" ]
}

task report_change {
    exec = "/usr/bin/echo"
    args = [ "CHANGED", "{full_path}" ]
}

rule files_ready {
    watch = "/srv/incoming"
    events = [ IN_CLOSE_WRITE ]
    exclude = [ ".*", "*.tmp", "*.swp" ]
    run = [ "ingest_file" ]
}

rule files_changing {
    watch = "/srv/incoming"
    events = [ IN_MODIFY ]
    exclude = [ ".*", "*.tmp", "*.swp" ]
    settle_ms = 250
    run = [ "report_change" ]
}
```
