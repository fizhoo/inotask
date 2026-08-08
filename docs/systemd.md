# systemd service

The supplied unit runs `inotask` as a system service. systemd captures both the
startup summary on stdout and runtime diagnostics on stderr in the journal; the
program does not need a separate journal logging mode.

## Install

Build and validate the configuration before installing it:

```sh
make
make check CFG=inotaskd.cfg
sudo make install install-config install-systemd CFG=inotaskd.cfg
sudo systemctl daemon-reload
sudo systemctl enable --now inotask.service
```

The default locations are:

- executable: `/usr/local/bin/inotask`
- configuration: `/etc/inotask/inotaskd.cfg`
- unit: `/etc/systemd/system/inotask.service`

`make install-config` replaces the installed configuration with `CFG`. It is an
explicit target so a normal binary update does not overwrite service config.

## Operate

```sh
systemctl status inotask.service
journalctl -u inotask.service
journalctl -u inotask.service -f
sudo systemctl restart inotask.service
sudo systemctl stop inotask.service
```

The unit uses `Restart=on-failure`. A normal `SIGTERM` shutdown stays stopped;
an unexpected nonzero exit is restarted after two seconds. `KillMode=control-group`
also stops task processes still associated with the service when the unit stops.

## Configure logging

The unit defaults to info-level output. Create an override to enable debug logs
without editing the installed unit:

```sh
sudo systemctl edit inotask.service
```

Add:

```ini
[Service]
Environment=INOTASK_LOG_LEVEL=debug
```

Then apply it:

```sh
sudo systemctl daemon-reload
sudo systemctl restart inotask.service
```

Accepted levels are `error`, `warn`, `info`, and `debug`. Invalid values make
startup fail rather than silently using an unintended logging configuration.

For a direct terminal run, diagnostics use readable prefixes such as
`INFO: watching for filesystem events`:

```sh
INOTASK_LOG_LEVEL=debug ./inotask inotaskd.cfg
```

Those same stderr lines appear in `journalctl -u inotask.service`. The textual
prefix remains part of the message; the unit does not add structured journal
priority metadata.

At `debug`, the journal also includes installed watch descriptors, raw inotify
masks and cookies, normalized event paths, rule matching decisions, and
settle-timer updates. Fatal event-loop failures return nonzero, allowing the
unit's `Restart=on-failure` policy to restart the daemon.

## Paths and permissions

The service runs as `root` by default because watched paths and task permission
requirements are deployment-specific. For a least-privilege deployment, add a
`User=` and `Group=` override and ensure that account can traverse every watched
directory and execute every configured task.

If the executable or config is installed elsewhere, copy the unit and update
`ExecStart=` to match. After any unit edit, run `systemctl daemon-reload`.
