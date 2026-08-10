# systemd Operation

The supplied unit runs `inotask` as a system service. systemd captures the
startup summary from stdout and runtime diagnostics from stderr in the journal.
No journal-specific logging mode is required.

## Install

Build and validate before installing:

```sh
make
make check CFG=inotaskd.cfg
sudo make install install-systemd
sudo make install-config CFG=inotaskd.cfg
sudo systemctl daemon-reload
sudo systemctl enable --now inotask.service
```

Default locations:

| Item | Path |
|---|---|
| Executable | `/usr/local/bin/inotask` |
| Configuration | `/etc/inotask/inotaskd.cfg` |
| Unit | `/etc/systemd/system/inotask.service` |

`install-config` is separate because it overwrites the installed configuration.
A normal `make install` updates only the executable.

Before enabling the unit, confirm that every configured watch and executable is
appropriate for the service host.

## Operate

```sh
systemctl status inotask.service
journalctl -u inotask.service
journalctl -u inotask.service -f
sudo systemctl restart inotask.service
sudo systemctl stop inotask.service
```

The unit uses `Restart=on-failure` with a two-second delay. Requested `SIGTERM`
shutdown returns zero and remains stopped. Fatal event-source failures return
nonzero and are eligible for restart.

`KillMode=control-group` terminates task processes that remain in the service's
control group when the unit stops.

## Logging

The unit defaults to `INOTASK_LOG_LEVEL=info`. To enable event-level debugging:

```sh
sudo systemctl edit inotask.service
```

```ini
[Service]
Environment=INOTASK_LOG_LEVEL=debug
```

Then restart:

```sh
sudo systemctl restart inotask.service
journalctl -u inotask.service -f
```

Accepted levels are `error`, `warn`, `info`, and `debug`. Invalid values fail
startup. Log prefixes remain part of each journal message; the unit does not add
structured journal-priority metadata.

## Permissions

The supplied unit has no `User=` or `Group=`, so a system manager starts it as
root. Configured tasks therefore also start as root. Review task executables and
arguments carefully.

For least privilege, create an override containing a dedicated account:

```ini
[Service]
User=inotask
Group=inotask
```

That account must be able to traverse each watched directory and execute each
configured task. Tasks also need whatever read or write permissions their work
requires.

## Custom Paths

If the executable or config is installed elsewhere, copy or override the unit's
`ExecStart=`. A systemd override must first clear the original command:

```ini
[Service]
ExecStart=
ExecStart=/custom/bin/inotask /custom/etc/inotask.cfg
```

Run `systemctl daemon-reload` after editing unit files directly. `systemctl edit`
handles the override location for you.
