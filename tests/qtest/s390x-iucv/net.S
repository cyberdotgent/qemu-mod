/*
 * IUCV guest program: a netiucv link (iucv-net, name NET). Before the
 * guest starts, the test stores two bundles: at 0x80000 (length at
 * 0x2f00) one with a packet larger than the MTU of the link and Don't
 * Fragment off, at 0x90000 (length at 0x2f04) one with Don't Fragment on.
 * The guest connects with the user data of Linux netiucv, sends both as
 * one-way messages and receives the message the link sends back (the
 * ICMP error for the second packet) at 0xa0000, its length at 0x2f08,
 * and ends when the test sets the byte at 0x2f10.
 * 0x2000: 0 = pass, else the failing step; 0x2100 the interruption
 * records.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
	.macro IUCV fc
	lghi	%r0,\fc
	la	%r1,0x100(%r10)
	.long	0xb2f01000
	.endm

	.macro CLEARP
	xc	0x100(40,%r10),0x100(%r10)
	.endm

	.macro STEP n
	lghi	%r12,\n
	.endm

	/* wait until \n interruptions have been taken */
	.macro WAITN n
	lghi	%r8,\n
	larl	%r9,0f
	stg	%r9,0xa88(%r11)
0:	cg	%r8,8(%r11)
	jnh	1f
	lpswe	0xa80(%r11)
1:
	.endm

	/* one-way SEND of the bundle at \buf, its length at \len(%r11) */
	.macro SENDB buf,len
	CLEARP
	sth	%r6,0x100(%r10)
	mvi	0x102(%r10),0x10
	llilf	%r3,\buf
	st	%r3,0x10c(%r10)
	mvc	0x110(4,%r10),\len(%r11)
	IUCV	4
	jnz	fail
	.endm

	.text
	.globl	_start
_start:
	lghi	%r11,0x2000
	lghi	%r10,0x3000
	xc	0(256,%r11),0(%r11)
	mvhi	0(%r11),-1
	larl	%r2,newpsws
	mvc	0x1b0(16,%r0),0(%r2)		/* external */
	mvc	0x1d0(16,%r0),16(%r2)		/* program */
	mvc	0xa80(8,%r11),32(%r2)		/* enabled wait mask */
	mvc	0xa90(8,%r11),40(%r2)		/* CR0 */

	STEP	1				/* DECLARE BUFFER */
	CLEARP
	st	%r10,0x10c(%r10)
	IUCV	12
	jnz	fail
	lctlg	%c0,%c0,0xa90(%r11)
	stosm	0xaa0(%r11),0x01

	STEP	2				/* CONNECT to NET */
	CLEARP
	larl	%r2,net
	mvc	0x108(8,%r10),0(%r2)
	mvc	0x110(16,%r10),8(%r2)		/* netiucv user data */
	IUCV	11
	jnz	fail
	lh	%r6,0x100(%r10)
	WAITN	1
	cli	0x100+3(%r11),2			/* connection complete */
	jne	fail

	STEP	3				/* the packet to fragment */
	SENDB	0x80000,0xf00
	WAITN	2
	cli	0x100+40+3(%r11),7		/* message complete */
	jne	fail

	STEP	4				/* the packet with DF */
	SENDB	0x90000,0xf04
	WAITN	4				/* complete + ICMP pending */
	la	%r4,0x100+80(%r11)
	cli	3(%r4),9
	je	2f
	la	%r4,0x100+120(%r11)
	cli	3(%r4),9
	jne	fail
2:
	STEP	5				/* RECEIVE the ICMP bundle */
	CLEARP
	mvc	0x100(2,%r10),0(%r4)
	mvi	0x102(%r10),0x07
	mvc	0x104(8,%r10),4(%r4)		/* message id, class */
	llilf	%r3,0xa0000
	st	%r3,0x10c(%r10)
	mvhi	0x110(%r10),4096
	IUCV	5
	jnz	fail
	lghi	%r3,4096
	s	%r3,0x110(%r10)
	st	%r3,0xf08(%r11)
	/* the network backend stops with the guest: wait for the test */
3:	cli	0xf10(%r11),0
	je	3b

	lghi	%r12,0
fail:	st	%r12,0(%r11)
	larl	%r2,donepsw
	lpswe	0(%r2)

ext_h:	stmg	%r10,%r13,0xb00(%r0)
	lghi	%r11,0x2000
	lghi	%r10,0x3000
	lg	%r12,8(%r11)
	lgr	%r13,%r12
	nill	%r13,7
	mghi	%r13,40
	la	%r13,0x100(%r13,%r11)
	mvc	0(40,%r13),0(%r10)
	aghi	%r12,1
	stg	%r12,8(%r11)
	ni	0x131,0xfd			/* leave the wait */
	lmg	%r10,%r13,0xb00(%r0)
	lpswe	0x130

pgm_h:	lghi	%r11,0x2000
	mvc	0x10(2,%r11),0x8e
	mvhi	0(%r11),99
	larl	%r2,donepsw
	lpswe	0(%r2)

	.align	8
newpsws:
	.quad	0x0000000180000000, ext_h
	.quad	0x0000000180000000, pgm_h
	.quad	0x0102000180000000		/* enabled wait */
	.quad	0x00000000000000e2		/* CR0: IUCV subclass */
donepsw: .quad	0x0002000180000000, 0x600d
	/* EBCDIC */
net:	.byte	0xd5,0xc5,0xe3,0x40,0x40,0x40,0x40,0x40		/* NET */
	.byte	0xf0,0x40,0x40,0x40,0x40,0x40,0x40,0x40		/* netiucv */
	.byte	0xf0,0x40,0x40,0x40,0x40,0x40,0x40,0x40
