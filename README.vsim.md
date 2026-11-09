# KO2B synthetic vsim fork

Branch `viessmann-vsim` is based on upstream v0.98.36, matching our Pi simulator. Production vcontrold is unchanged. This branch preserves our synthetic KO2B reads, four in-memory setpoint writes, loopback-only binding and bounded per-connection input.

New options: `--enq-interval SECONDS` (default 2.3; 0 disables periodic activity) and `--activity-only` (synchronization without usable readings or writes). Timer uses monotonic time and never injects periodic bytes into a partially received request. This simplified KW simulator does not prove real-controller support or model heating physics.

Build with `cmake -S . -B build -DVSIM=ON -DMANPAGES=OFF` then `cmake --build build --target vsim`. Read `doc/man/vsim.rst` for behavior. Run `python3 tests/vsim_idle.py build/vsim` with exclusive access to loopback port 6578; do not run alongside a simulator service.

Validation on the Pi: periodic ENQs, handshake, fragmented-request silence, fixtures, setpoint write/readback, reconnect, activity-only, disabled timer and invalid arguments. Plugin-client smoke polls are checked separately by the Indigo plugin project.
