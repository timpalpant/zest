<!--
Copyright (c) 2026 Timothy Palpant

SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Kconfig notes

Reference material, not a Zephyr snippet and not something to include.

Some of the most expensive knowledge in a Zephyr project is not expressible in
C++ at all: it is a Kconfig value whose correct setting is coupled to a *second*
value somewhere else — in the controller image, in the driver, in the
bootloader — where the two disagreeing produces a failure that names neither of
them. A host ACL buffer one byte smaller than the controller's makes `bt_enable()`
return `-EINVAL`. A transmit buffer count that does not match the network core's
corrupts connection accounting and makes an unrelated advertiser start fail
`-ENOMEM`. A connection pool one slot short makes a peripheral silently stop
being connectable after some *other* link comes up.

No library can wrap those. What a library can do is write them down.

Each file here is a commented fragment covering one such area. The comments are
the content; the values are examples from a build where they were measured or
debugged, and they are wrong for a different SoC, controller, or bootloader
mode. **Read them and adapt the numbers — do not `EXTRA_CONF_FILE` them.** They
are deliberately not Zephyr snippets for that reason: a snippet that can be
applied with `-S` would let the wrong numbers in silently, which is the exact
failure mode these notes exist to describe.

| File | Covers |
| --- | --- |
| [`bluetooth-le-audio.conf`](bluetooth-le-audio.conf) | LE Audio unicast on a two-core SoC with a separate controller image: connection pool sizing, buffer coupling across the HCI boundary, CAP/BAP service dependencies, bond table sizing |
| [`bluetooth-classic-a2dp.conf`](bluetooth-classic-a2dp.conf) | Classic A2DP sink: ACL buffer sizing against real peer behaviour, and the event-buffer allocation that costs a full ACL buffer each |
