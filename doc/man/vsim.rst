..
    SPDX-FileCopyrightText: 2012 Frank Nobis <frank.nobis@radio-do.de>

    SPDX-License-Identifier: CC-BY-SA-4.0

====
vsim
====

-----------------------------
simulation tool for vcontrold
-----------------------------

:Manual section: 1

DESCRIPTION
===========

``vsim`` is a simulation tool for the ``vcontrold`` daemon to simulate a connected Viessmann device.

It is a simple developer tool, and features only a limited commandset which is not runtime configurable.

LOCAL KO2B TEST EXTENSION
========================

This fork binds to 127.0.0.1:6578 and supplies synthetic KO2B fixtures.
It is not evidence of physical-controller support. Four synthetic setpoint
writes update readback values in memory until process restart.

``--enq-interval SECONDS`` sets the idle 0x05 synchronization interval
(default 2.3 seconds, based on one observed controller). Zero disables periodic
sending while retaining the normal 0x04 -> 0x05 handshake. No periodic bytes
are emitted while a request is incomplete. This is a simplified KW simulation,
not a complete timing model of every controller.

``--activity-only`` suppresses fixture reads and setter acknowledgements/updates
but retains synchronization activity. This permits testing an optically active
controller with no usable readings. Use interval zero together with this option
for no passive activity/no data; the explicit handshake still responds.

The timer starts on connection and resets on received bytes and emitted ENQs.
Unknown/incomplete commands remain pending rather than being silently decoded.
Only run this backend with isolated simulator configuration, never by attaching
it to a physical serial device.

The local clock-health extension answers 088E/8 with advancing BCD host wall
time, 7507/9 with synthetic no-error history and A152/2 with zero relay bytes.
These are test fixtures, not physical-controller behavior guarantees.

SEE ALSO
========

* man 1 vcontrold
* man 1 vclient
* vcontrold @GitHub: `https://github.com/openv/vcontrold <https://github.com/openv/vcontrold>`__
