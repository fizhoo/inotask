# Release Checklist

Run release checks from a clean checkout of the intended release commit. Do not
build a release from a working tree containing local configuration experiments.

## 1. Confirm Scope

```sh
git status --short
git log -5 --oneline --decorate
git diff --check
```

- Confirm documentation matches the code.
- Confirm no generated binaries, object files, or analyzer reports are staged.
- Review changes since the previous release or initial baseline.

## 2. Build and Check

```sh
make clean
make
make check
make test
```

Also check any release example configurations explicitly:

```sh
./inotask --check inotaskd-sample.conf
./inotask --check inotaskd.cfg
```

## 3. Static Analysis

```sh
make scan
```

Review every analyzer finding. Do not treat a completed command as proof that
the report is empty.

## 4. Sanitizers

```sh
make san
./inotask --check inotaskd.cfg
```

Run the daemon against a temporary watched directory, trigger at least one
matching event, allow the child to exit, and stop the daemon with `SIGTERM` or
Ctrl-C. Confirm AddressSanitizer and UndefinedBehaviorSanitizer stay quiet.

Rebuild normally afterward:

```sh
make clean
make
```

## 5. Valgrind

When Valgrind is available, run:

```sh
valgrind --leak-check=full --show-leak-kinds=all \
    --track-origins=yes ./inotask inotaskd.cfg
```

Trigger an event and stop with Ctrl-C so normal cleanup runs. Review definite
and indirect leaks plus invalid reads, writes, and file-descriptor handling.

## 6. Runtime Smoke Tests

Verify at minimum:

- valid config startup
- invalid config rejection
- immediate task launch
- `settle_ms` coalescing
- successful and failed child status logging
- `INOTASK_LOG_LEVEL=info` and `debug`
- graceful `SIGINT` and `SIGTERM` exit status 0
- systemd stop does not restart the service
- fatal service failure is eligible for `Restart=on-failure`

Queue-overflow behavior is difficult to force reliably; confirm its handling
through focused tests or code review until the integration harness covers it.

## 7. Installation Smoke Test

Stage installation into a temporary root:

```sh
make install install-config install-systemd \
    DESTDIR=/tmp/inotask-stage CFG=inotaskd-sample.conf
```

Confirm expected paths and permissions:

```text
/usr/local/bin/inotask                  0755
/etc/inotask/inotaskd.cfg              0644
/etc/systemd/system/inotask.service    0644
```

On a systemd test host, follow `systemd.md` and verify status, journal output,
restart behavior, and configured watch permissions.

## 8. Publish

After all checks pass:

```sh
git switch main
git merge --ff-only development
git push origin main
git tag -a v0.1.0 -m "inotask v0.1.0"
git push origin v0.1.0
```

Adjust the version for later releases. Release notes should summarize user
behavior, configuration changes, operational changes, limitations, and the
exact checks completed.
