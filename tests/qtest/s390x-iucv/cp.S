/*
 * IUCV guest program: the CP core of QEMU with the echo service (iucv-echo,
 * name ECHO). z/Architecture, DAT off, prefix 0. Result word at 0x2000:
 * 0 = all steps passed, otherwise the number of the failing step.
 * 0x2008 interruption count, 0x2100 the 40-byte records in order,
 * 0x2010 program-interruption code, 0x2018/0x2020 R0/R1 of QUERY.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
	.macro IUCV fc
	lghi	%r0,\fc
	la	%r1,0x100(%r10)
	.long	0xb2f01000
	.endm

	.macro FAIL step
	lghi	%r0,\step
	st	%r0,0(%r11)
	j	fail
	.endm

	.macro CLEARP
	xc	0x100(40,%r10),0x100(%r10)
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

	/* record \i: type \t */
	.macro CHKTYPE i,t,step
	cli	0x100+40*\i+3(%r11),\t
	jne	f\step
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

	/* 1: QUERY */
	IUCV	0
	jnz	f1
	stg	%r0,0x18(%r11)
	stg	%r1,0x20(%r11)
	chi	%r0,40
	jne	f1

	/* 2: DECLARE BUFFER */
	CLEARP
	st	%r10,0x10c(%r10)
	IUCV	12
	jnz	f2
	/* 3: a second DCLBFR is refused with rc 0x13 */
	IUCV	12
	jnl	f3
	cli	0x103(%r10),0x13
	jne	f3

	lctlg	%c0,%c0,0xa90(%r11)
	stosm	0xaa0(%r11),0x01

	/* 4: CONNECT to a user ID that does not exist: rc 0x0b */
	CLEARP
	larl	%r2,nobody
	mvc	0x108(8,%r10),0(%r2)
	IUCV	11
	jnl	f4
	cli	0x103(%r10),0x0b
	jne	f4

	/* 5: CONNECT to ECHO, connection complete */
	CLEARP
	larl	%r2,echo
	mvc	0x108(8,%r10),0(%r2)
	mvc	0x110(16,%r10),8(%r2)		/* user data */
	IUCV	11
	jnz	f5
	lh	%r6,0x100(%r10)			/* path id */
	WAITN	1
	CHKTYPE	0,2,5
	ch	%r6,0x100(%r11)
	jne	f5
	clc	0x110(16,%r11),8(%r2)		/* user data comes back */
	jne	f5

	/* 6: two-way SEND, the reply is the data */
	larl	%r2,hello
	mvc	0x200(10,%r10),0(%r2)
	xc	0x300(64,%r10),0x300(%r10)
	CLEARP
	sth	%r6,0x100(%r10)
	mvhi	0x108(%r10),7			/* target class */
	la	%r3,0x200(%r10)
	st	%r3,0x10c(%r10)
	mvhi	0x110(%r10),10
	mvhi	0x114(%r10),0x11		/* source class */
	mvhi	0x118(%r10),0x22		/* tag */
	la	%r3,0x300(%r10)
	st	%r3,0x11c(%r10)
	mvhi	0x120(%r10),64
	IUCV	4
	jnz	f6
	l	%r7,0x104(%r10)			/* message id */
	WAITN	2
	CHKTYPE	1,7,6
	c	%r7,0x100+40+4(%r11)
	jne	f6
	clc	0x100+40+0x18(4,%r11),0x118(%r10)	/* tag */
	jne	f6
	clc	0x100+40+0x14(4,%r11),0x114(%r10)	/* source class */
	jne	f6
	cli	0x100+40+8(%r11),0		/* audit */
	jne	f6
	chsi	0x100+40+0x20(%r11),54		/* answer residual */
	jne	f6
	clc	0x300(10,%r10),0x200(%r10)
	jne	f6

	/* 7: one-way SEND class 1: complete, then the echo is pending */
	CLEARP
	sth	%r6,0x100(%r10)
	mvi	0x102(%r10),0x10
	mvhi	0x108(%r10),1
	la	%r3,0x200(%r10)
	st	%r3,0x10c(%r10)
	mvhi	0x110(%r10),10
	IUCV	4
	jnz	f7
	WAITN	4
	CHKTYPE	2,7,7
	tm	0x100+80+2(%r11),0x10		/* one-way */
	jno	f7
	CHKTYPE	3,9,7
	chsi	0x100+120+0x10(%r11),10		/* length */
	jne	f7
	/* 8: RECEIVE it */
	CLEARP
	mvc	0x100(2,%r10),0x100+120(%r11)
	mvi	0x102(%r10),0x07
	mvc	0x104(8,%r10),0x100+120+4(%r11)	/* msgid, class */
	la	%r3,0x400(%r10)
	st	%r3,0x10c(%r10)
	mvhi	0x110(%r10),64
	IUCV	5
	jnz	f8
	chsi	0x110(%r10),54			/* unused part */
	jne	f8
	clc	0x400(10,%r10),0x200(%r10)
	jne	f8

	/* 9: one-way SEND class 2: the service sends a two-way message */
	CLEARP
	sth	%r6,0x100(%r10)
	mvi	0x102(%r10),0x10
	mvhi	0x108(%r10),2
	la	%r3,0x200(%r10)
	st	%r3,0x10c(%r10)
	mvhi	0x110(%r10),10
	IUCV	4
	jnz	f9
	WAITN	6
	CHKTYPE	4,7,9
	CHKTYPE	5,9,9
	tm	0x100+200+2(%r11),0x10		/* two-way */
	jnz	f9
	/* RECEIVE with a short buffer: rc 5, then the rest */
	CLEARP
	mvc	0x100(2,%r10),0x100+200(%r11)
	mvi	0x102(%r10),0x07
	mvc	0x104(8,%r10),0x100+200+4(%r11)
	la	%r3,0x400(%r10)
	st	%r3,0x10c(%r10)
	mvhi	0x110(%r10),4
	IUCV	5
	jnl	f9
	cli	0x103(%r10),5
	jne	f9
	chsi	0x110(%r10),6			/* residual */
	jne	f9
	mvhi	0x110(%r10),64			/* buffer continues */
	IUCV	5
	jnz	f9
	clc	0x400(10,%r10),0x200(%r10)
	jne	f9

	/* 10: REPLY; the service sends the reply back as class 4 */
	larl	%r2,reply
	mvc	0x500(5,%r10),0(%r2)
	CLEARP
	mvc	0x100(2,%r10),0x100+200(%r11)
	mvc	0x104(8,%r10),0x100+200+4(%r11)
	la	%r3,0x500(%r10)
	st	%r3,0x11c(%r10)
	mvhi	0x120(%r10),5
	IUCV	6
	jnz	f10
	WAITN	7
	CHKTYPE	6,9,10
	chsi	0x100+240+0x10(%r11),5
	jne	f10
	CLEARP
	mvc	0x100(2,%r10),0x100+240(%r11)
	mvi	0x102(%r10),0x07
	mvc	0x104(8,%r10),0x100+240+4(%r11)
	la	%r3,0x600(%r10)
	st	%r3,0x10c(%r10)
	mvhi	0x110(%r10),64
	IUCV	5
	jnz	f10
	clc	0x600(5,%r10),0x500(%r10)
	jne	f10

	/* 11: SETMASK 0 holds messages back, SETMASK X'F8' lets them in */
	CLEARP
	IUCV	16
	CLEARP
	sth	%r6,0x100(%r10)
	mvi	0x102(%r10),0x10
	mvhi	0x108(%r10),1
	la	%r3,0x200(%r10)
	st	%r3,0x10c(%r10)
	mvhi	0x110(%r10),10
	IUCV	4
	jnz	f11
	lgfi	%r5,1000000
0:	brctg	%r5,0b
	chsi	0xc(%r11),7			/* low word of the count */
	jne	f11
	CLEARP
	mvi	0x100(%r10),0xf8
	IUCV	16
	WAITN	9
	CHKTYPE	7,7,11
	CHKTYPE	8,9,11
	/* 12: REJECT the echo */
	CLEARP
	mvc	0x100(2,%r10),0x100+320(%r11)
	mvi	0x102(%r10),0x07
	mvc	0x104(8,%r10),0x100+320+4(%r11)
	IUCV	8
	jnz	f12
	/* now nothing is left to receive: CC 2 */
	CLEARP
	la	%r3,0x400(%r10)
	st	%r3,0x10c(%r10)
	mvhi	0x110(%r10),64
	IUCV	5
	ipm	%r4
	srl	%r4,28
	chi	%r4,2
	jne	f12

	/* 13: SEVER, then SEND on it: rc 1 */
	CLEARP
	sth	%r6,0x100(%r10)
	IUCV	15
	jnz	f13
	CLEARP
	sth	%r6,0x100(%r10)
	mvi	0x102(%r10),0x10
	IUCV	4
	jnl	f13
	cli	0x103(%r10),1
	jne	f13

	/* 14: the instruction in the problem state: operation exception */
	mvi	0x30(%r11),1			/* expected */
	larl	%r2,ppsw
	lpswe	0(%r2)
prob:	.long	0xb2f01000
	j	f14
back:	cli	0x30(%r11),2
	jne	f14
	chsi	0x10(%r11),1
	jne	f14

	mvhi	0(%r11),0
fail:	larl	%r2,donepsw
	lpswe	0(%r2)

	.irp	n,1,2,3,4,5,6,7,8,9,10,11,12,13,14
f\n:	FAIL	\n
	.endr

ext_h:	stmg	%r10,%r13,0xb00(%r0)
	lghi	%r11,0x2000
	lghi	%r10,0x3000
	lg	%r12,8(%r11)
	lgr	%r13,%r12
	mghi	%r13,40
	la	%r13,0x100(%r13,%r11)
	mvc	0(40,%r13),0(%r10)
	aghi	%r12,1
	stg	%r12,8(%r11)
	ni	0x131,0xfd			/* leave the wait */
	lmg	%r10,%r13,0xb00(%r0)
	lpswe	0x130

pgm_h:	lghi	%r11,0x2000
	mvc	0x12(2,%r11),0x8e
	cli	0x30(%r11),1
	jne	f14
	mvi	0x30(%r11),2
	larl	%r2,back
	stg	%r2,0x158			/* program old PSW: supervisor, go on */
	ni	0x151,0xfe
	lpswe	0x150

	.align	8
newpsws:
	.quad	0x0000000180000000, ext_h
	.quad	0x0000000180000000, pgm_h
	.quad	0x0102000180000000		/* enabled wait */
	.quad	0x00000000000000e2		/* CR0: IUCV subclass */
ppsw:	.quad	0x0001000180000000, prob	/* problem state */
donepsw: .quad	0x0002000180000000, 0x600d
	/* EBCDIC */
nobody:	.byte	0xd5,0xd6,0xc2,0xd6,0xc4,0xe8,0x40,0x40		/* NOBODY */
echo:	.byte	0xc5,0xc3,0xc8,0xd6,0x40,0x40,0x40,0x40		/* ECHO */
	.ascii	"user-data-16byte"
hello:	.ascii	"hello iucv"
reply:	.ascii	"reply"
