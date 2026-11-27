/*
 * IUCV guest program: nothing but an enabled wait, so that the machine
 * runs (and its network backends deliver frames) while a test talks to a
 * service from the network side. 0x2000: 0 once it runs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
	.text
	.globl	_start
_start:
	lghi	%r11,0x2000
	mvhi	0(%r11),0
	larl	%r2,waitpsw
	lpswe	0(%r2)

	.align	8
waitpsw: .quad	0x0102000180000000, 0
