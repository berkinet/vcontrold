# Continuous optical capture: -t <trace-file>

This experimental mode investigates whether a KW/KW2 controller emits additional data or notifications outside requested replies. It is a separate capture feature, not verbose/debug logging. No notification semantics are inferred from bytes observed.

Use **-n -t /path/to/wire.log**. The foreground server handles one TCP client at a time; its single process owns one continuously open optical descriptor. Client sessions receive duplicate handles to that descriptor. When waiting for a TCP connection or a complete command line, select monitors optical input too. During protocol execution the ordinary receive functions log every buffer they read. There is no competing reader or capture thread. Commands/units/macros and setters otherwise retain their normal behavior. P300 and command-file capture are rejected in this first version.

The regular server and trace server must not read the same physical adapter. Stop the regular service before live capture. A separate TCP port alone does not isolate the physical link. Simulator tests may run alongside production because they use a different endpoint.

## Records and interpretation

Every record has a UTC host-read timestamp with nanosecond representation, a monotonic timestamp, event type, length and hex bytes. Resolution is not a claim of nanosecond accuracy or hardware arrival timing. Example:

```
2026-11-16T20:00:00.123456789Z mono=456.123456789 RX-IDLE len=1 05
```

- RX-IDLE: bytes observed while waiting for TCP/command input. Explicitly drained without decoding; not fed to a future command as a reply.
- RX-REPLY: bytes read by protocol routines. This label describes the read context, not proof that every byte really is a requested reply.
- RX-PRE-TX-DISCARD: queued bytes recorded before the normal transmit/raw-command buffer-clear point.
- TX-ATTEMPT: intended bytes; TX: write-reported bytes. TX-ERROR marks a failed write whose partial progress may be unknown.
- COMMAND: TCP command text encoded in hex, including newline. START, SESSION-OPEN/CLOSE, WAIT-COMMAND and CLIENT-EOF delimit host activity.

The physical port remains open without TCP clients, after quit/close and across reconnects. EOF/read failure terminates capture; write errors on the trace file terminate rather than silently produce an apparently complete capture. The file is append-only, newly created with mode0600; existing permissions are preserved. It contains raw controller data and commands. Use a dedicated file and existing log rotation/manual retention as appropriate; rotation/reopen is not implemented.

## Buffer-clear behavior

Normally vcontrold uses TCIOFLUSH before each send. Trace mode reads/logs the currently queued input instead, and flushes only pending output (TCOFLUSH). Thus RX bytes racing the drain remain available for the next protocol read rather than being silently flushed. This is a deliberate small behavioral difference: instrumentation cannot both erase unobserved incoming data and claim to have recorded it. The existing UART settings still apply: driver-discarded parity/framing-error bytes and hardware overruns are not observable here. Do not treat an empty capture as proof that no electrical/optical activity occurred.

## Build and tests

```
cmake -S . -B build-trace -DMANPAGES=OFF -DVCLIENT=ON -DVSIM=ON
cmake --build build-trace -j2
python3 -m unittest discover -s tests -v
```

Fixtures use only loopback sockets and a pseudo-terminal. They verify idle bytes before/after clients, fragmented command input, exact requested results, preflush capture, persistent ownership after close, trace-disabled compatibility and startup failure safeguards. The pseudo-terminal opener exists only in a fixture because PTYs do not implement USB adapter DTR ioctls; production serial configuration is unchanged.

On this project the Pi checkout is /home/rdp/viessmann/vcontrold-trace, beside /home/rdp/viessmann/vcontrold. The Mac checkout is /Users/rdp/Documents/Codex/vcontrold-trace. Production binary replacement is not required: a removable service override can point to a separately installed trace executable.

## Restore the normal service after capture

For the deployed removable override, run on the Pi:

```zsh
sudo systemctl stop vcontrol.service
sudo rm /etc/systemd/system/vcontrol.service.d/optical-trace.conf
sudo systemctl daemon-reload
sudo systemctl start vcontrol.service
```

Remove only this trace override, not other service configuration. The original /usr/local/sbin/vcontrold executable and base service unit are retained. Preserve the trace file for analysis; these commands do not delete it. The alternate executable is /usr/local/sbin/vcontrold-trace. Systemd supervises the foreground trace process as Type=simple.
