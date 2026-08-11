#!/bin/sh

set -eu

fail()
{
    printf 'integration test failed: %s\n' "$1" >&2
    exit 1
}

binary=${1:-./inotask}
case "$binary" in
    /*) ;;
    *) binary=$(cd "$(dirname "$binary")" && pwd)/$(basename "$binary") ;;
esac

test_root=$(mktemp -d "${TMPDIR:-/tmp}/inotask-tests.XXXXXX")
daemon_pid=

cleanup()
{
    if [ -n "$daemon_pid" ] && kill -0 "$daemon_pid" 2>/dev/null; then
        kill -TERM "$daemon_pid" 2>/dev/null || true
        wait "$daemon_pid" 2>/dev/null || true
    fi
    rm -rf "$test_root"
}

trap cleanup EXIT HUP INT TERM

wait_for_pattern()
{
    pattern=$1
    file=$2
    attempts=0
    while [ "$attempts" -lt 100 ]; do
        if [ -f "$file" ] && grep -F -q -- "$pattern" "$file"; then
            return 0
        fi
        sleep 0.05
        attempts=$((attempts + 1))
    done
    if [ -f "$file" ]; then cat "$file" >&2; fi
    fail "timed out waiting for '$pattern' in $file"
}

wait_for_lines()
{
    expected=$1
    file=$2
    attempts=0
    while [ "$attempts" -lt 100 ]; do
        count=0
        if [ -f "$file" ]; then count=$(wc -l < "$file"); fi
        if [ "$count" -ge "$expected" ]; then return 0; fi
        sleep 0.05
        attempts=$((attempts + 1))
    done
    if [ -f "$file" ]; then cat "$file" >&2; fi
    fail "timed out waiting for $expected line(s) in $file"
}

start_daemon()
{
    level=$1
    config=$2
    output_prefix=$3
    env INOTASK_LOG_LEVEL="$level" "$binary" "$config" \
        > "$output_prefix.stdout" 2> "$output_prefix.stderr" &
    daemon_pid=$!
    wait_for_pattern "watching for filesystem events" "$output_prefix.stderr"
}

stop_daemon()
{
    stopped_pid=$daemon_pid
    kill -TERM "$stopped_pid"
    if ! wait "$stopped_pid"; then
        fail "daemon did not exit successfully after SIGTERM"
    fi
    daemon_pid=
}

record_task=$test_root/record-task
cat > "$record_task" <<'EOF'
#!/bin/sh
printf '%s|%s\n' "$2" "$3" >> "$1"
EOF
chmod 0755 "$record_task"

fail_task=$test_root/fail-task
cat > "$fail_task" <<'EOF'
#!/bin/sh
exit 7
EOF
chmod 0755 "$fail_task"

watch_dir=$test_root/watch
mkdir "$watch_dir"

close_config=$test_root/close.cfg
close_result=$test_root/close.result
cat > "$close_config" <<EOF
task record {
    exec = "$record_task"
    args = [ "$close_result", "{full_path}", "{event}" ]
}

rule close_ready {
    watch = "$watch_dir"
    events = [ IN_CLOSE_WRITE ]
    exclude = [ "#*" ]
    run = [ "record" ]
}
EOF

if ! "$binary" --check "$close_config" > "$test_root/check.stdout" \
    2> "$test_root/check.stderr"; then
    cat "$test_root/check.stderr" >&2
    fail "valid config failed check mode"
fi
grep -F -q "Config check passed" "$test_root/check.stdout" ||
    fail "valid config did not pass check mode"

exact_config=$test_root/exact-events.cfg
cat > "$exact_config" <<EOF
task record {
    exec = "$record_task"
}

rule exact_events {
    watch = "$watch_dir"
    events = [ IN_ACCESS, IN_MODIFY, IN_ATTRIB, IN_CLOSE_WRITE, IN_CLOSE_NOWRITE, IN_OPEN, IN_MOVED_FROM, IN_MOVED_TO, IN_CREATE, IN_DELETE, IN_DELETE_SELF, IN_MOVE_SELF ]
    run = [ "record" ]
}
EOF
"$binary" --check "$exact_config" > "$test_root/exact.stdout" \
    2> "$test_root/exact.stderr" || fail "exact inotify event names were rejected"

alias_config=$test_root/alias.cfg
cat > "$alias_config" <<EOF
task record {
    exec = "$record_task"
}

rule old_alias {
    watch = "$watch_dir"
    events = [ CREATE ]
    run = [ "record" ]
}
EOF
if "$binary" --check "$alias_config" > "$test_root/alias.stdout" \
    2> "$test_root/alias.stderr"; then
    fail "made-up event alias unexpectedly passed"
fi
grep -F -q "unknown event 'CREATE'" "$test_root/alias.stderr" ||
    fail "made-up event alias error was not reported"

bad_config=$test_root/bad.cfg
cat > "$bad_config" <<EOF
task broken {
    exex = "$record_task"
}
EOF
if "$binary" --check "$bad_config" > "$test_root/bad.stdout" \
    2> "$test_root/bad.stderr"; then
    fail "invalid config unexpectedly passed"
fi
grep -F -q "unknown task field 'exex'" "$test_root/bad.stderr" ||
    fail "invalid config error was not reported"

if env INOTASK_LOG_LEVEL=loud "$binary" --check "$close_config" \
    > "$test_root/level.stdout" 2> "$test_root/level.stderr"; then
    fail "invalid log level unexpectedly passed"
else
    level_status=$?
fi
[ "$level_status" -eq 2 ] || fail "invalid log level returned $level_status"

info_prefix=$test_root/info
start_daemon info "$close_config" "$info_prefix"
printf 'accepted\n' > "$watch_dir/accepted.txt"
wait_for_lines 1 "$close_result"
stop_daemon
grep -F -q "$watch_dir/accepted.txt|IN_CLOSE_WRITE" "$close_result" ||
    fail "close-write task received unexpected arguments"
if grep -F -q "DEBUG:" "$info_prefix.stderr"; then
    fail "info logging emitted debug messages"
fi
grep -F -q "shutdown requested" "$info_prefix.stderr" ||
    fail "graceful shutdown was not logged"

debug_result=$test_root/debug.result
sed "s|$close_result|$debug_result|" "$close_config" > "$test_root/debug.cfg"
debug_prefix=$test_root/debug
start_daemon debug "$test_root/debug.cfg" "$debug_prefix"
printf 'ignored\n' > "$watch_dir/#ignored"
printf 'accepted\n' > "$watch_dir/debug.txt"
wait_for_lines 1 "$debug_result"
wait_for_pattern "DEBUG: no rule matched" "$debug_prefix.stderr"
stop_daemon
grep -F -q "DEBUG: watch added" "$debug_prefix.stderr" ||
    fail "debug watch binding was not logged"
grep -F -q "DEBUG: inotify wd=" "$debug_prefix.stderr" ||
    fail "raw debug event was not logged"
grep -F -q "DEBUG: rule close_ready matched" "$debug_prefix.stderr" ||
    fail "debug rule match was not logged"
if grep -F -q "#ignored" "$debug_result"; then
    fail "excluded file launched a task"
fi

move_dir=$test_root/move-watch
move_source_dir=$test_root/move-source
move_result=$test_root/move.result
create_result=$test_root/create.result
move_config=$test_root/move.cfg
mkdir "$move_dir" "$move_source_dir"
cat > "$move_config" <<EOF
task record_move {
    exec = "$record_task"
    args = [ "$move_result", "{full_path}", "{event}" ]
}

task record_create {
    exec = "$record_task"
    args = [ "$create_result", "{full_path}", "{event}" ]
}

rule moved_in {
    watch = "$move_dir"
    events = [ IN_MOVED_TO ]
    run = [ "record_move" ]
}

rule created_here {
    watch = "$move_dir"
    events = [ IN_CREATE ]
    run = [ "record_create" ]
}
EOF

printf 'moved\n' > "$move_source_dir/moved.txt"
move_prefix=$test_root/move
start_daemon debug "$move_config" "$move_prefix"
mv "$move_source_dir/moved.txt" "$move_dir/moved.txt"
wait_for_lines 1 "$move_result"
sleep 0.15
stop_daemon
grep -F -q "$move_dir/moved.txt|IN_MOVED_TO" "$move_result" ||
    fail "move-in task received unexpected arguments"
if [ -s "$create_result" ]; then
    fail "IN_MOVED_TO incorrectly matched IN_CREATE"
fi

settle_dir=$test_root/settle-watch
settle_result=$test_root/settle.result
settle_config=$test_root/settle.cfg
mkdir "$settle_dir"
cat > "$settle_config" <<EOF
task record {
    exec = "$record_task"
    args = [ "$settle_result", "{full_path}", "{event}" ]
}

rule modify_settled {
    watch = "$settle_dir"
    events = [ IN_MODIFY ]
    settle_ms = 150
    run = [ "record" ]
}
EOF

settle_prefix=$test_root/settle
start_daemon debug "$settle_config" "$settle_prefix"
printf 'one\n' > "$settle_dir/source.txt"
printf 'two\n' >> "$settle_dir/source.txt"
printf 'three\n' >> "$settle_dir/source.txt"
wait_for_pattern "DEBUG: settling rule=modify_settled" "$settle_prefix.stderr"
wait_for_lines 1 "$settle_result"
sleep 0.35
settle_count=$(wc -l < "$settle_result")
[ "$settle_count" -eq 1 ] ||
    fail "settled modifications launched $settle_count tasks"
grep -F -q "$settle_dir/source.txt|IN_MODIFY" "$settle_result" ||
    fail "settled task received unexpected arguments"
stop_daemon

failure_dir=$test_root/failure-watch
failure_config=$test_root/failure.cfg
mkdir "$failure_dir"
cat > "$failure_config" <<EOF
task fail {
    exec = "$fail_task"
}

rule child_failure {
    watch = "$failure_dir"
    events = [ IN_CLOSE_WRITE ]
    run = [ "fail" ]
}
EOF

failure_prefix=$test_root/failure
start_daemon info "$failure_config" "$failure_prefix"
printf 'fail\n' > "$failure_dir/fail.txt"
wait_for_pattern "WARN: reaped child" "$failure_prefix.stderr"
wait_for_pattern "exited with status 7" "$failure_prefix.stderr"
stop_daemon

printf 'integration tests passed\n'
