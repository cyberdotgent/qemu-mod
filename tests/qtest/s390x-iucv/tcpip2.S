/*
 * IUCV guest program: the TCP/IP service with two paths (iucv-tcpip, name
 * TCPIP), apitype 3: path A (INETD) accepts a connection on guest port
 * 21 and gives the socket to path B (SERVER), which takes it, reads
 * "data" and writes "ok"; then CANCEL and PURGE of a pending READ. Needs a
 * client that connects to the port forwarded to guest port 21, sends
 * "data" and expects "ok". 0x2000: 0 = pass, else the failing step; 0x2010
 * last rc, 0x2014 last errno.
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

	/* two-way SEND: target class, buffer (or PRMMSG hi), length (or lo),
	   answer, answer length; wait for the completion */
	.macro CALL trg,buf,blen,ans,alen,flags=0
	CLEARP
	sth	%r6,0x100(%r10)
	mvi	0x102(%r10),\flags
	llilf	%r3,\trg
	st	%r3,0x108(%r10)
	llilf	%r3,\buf
	st	%r3,0x10c(%r10)
	llilf	%r3,\blen
	st	%r3,0x110(%r10)
	llilf	%r3,\ans
	st	%r3,0x11c(%r10)
	llilf	%r3,\alen
	st	%r3,0x120(%r10)
	xc	0(64,%r13),0(%r13)
	IUCV	4
	jnz	fail
	aghi	%r7,1
	WAITR
	mvc	0x10(8,%r11),0(%r13)		/* rc, errno */
	.endm

	.macro WAITR
	lgr	%r8,%r7
	larl	%r9,0f
	stg	%r9,0xa88(%r11)
0:	cg	%r8,8(%r11)
	jnh	1f
	lpswe	0xa80(%r11)
1:
	.endm

	/* rc must be \v */
	.macro RC v
	chsi	0(%r13),\v
	jne	fail
	.endm

	.macro SENDNW trg,buf,blen,ans,alen,flags=0
	CLEARP
	sth	%r6,0x100(%r10)
	mvi	0x102(%r10),\flags
	llilf	%r3,\trg
	st	%r3,0x108(%r10)
	llilf	%r3,\buf
	st	%r3,0x10c(%r10)
	llilf	%r3,\blen
	st	%r3,0x110(%r10)
	llilf	%r3,\ans
	st	%r3,0x11c(%r10)
	llilf	%r3,\alen
	st	%r3,0x120(%r10)
	IUCV	4
	jnz	fail
	l	%r4,0x104(%r10)
	.endm

	.text
	.globl	_start
_start:
	lghi	%r11,0x2000
	lghi	%r10,0x3000
	llilf	%r13,0x5000
	xc	0(256,%r11),0(%r11)
	mvhi	0(%r11),-1
	lghi	%r7,0
	larl	%r2,newpsws
	mvc	0x1b0(16,%r0),0(%r2)
	mvc	0x1d0(16,%r0),16(%r2)
	mvc	0xa80(8,%r11),32(%r2)
	mvc	0xa90(8,%r11),40(%r2)
	larl	%r2,reqs
	llilf	%r3,0x4000
	mvc	0(256,%r3),0(%r2)
	mvc	256(256,%r3),256(%r2)

	STEP	1
	CLEARP
	st	%r10,0x10c(%r10)
	IUCV	12
	jnz	fail
	lctlg	%c0,%c0,0xa90(%r11)
	stosm	0xaa0(%r11),0x01
	CLEARP					/* path A */
	larl	%r2,tcpip
	mvc	0x108(8,%r10),0(%r2)
	mvi	0x102(%r10),0x80
	IUCV	11
	jnz	fail
	lh	%r6,0x100(%r10)
	stg	%r6,0x40(%r11)
	aghi	%r7,1
	WAITR
	CLEARP					/* path B */
	mvc	0x108(8,%r10),0(%r2)
	mvi	0x102(%r10),0x80
	IUCV	11
	jnz	fail
	lh	%r6,0x100(%r10)
	stg	%r6,0x48(%r11)
	aghi	%r7,1
	WAITR
	CALL	0,0x4020,20,0x5000,8		/* init B: SERVER */
	chsi	4(%r13),49
	jne	fail
	lg	%r6,0x40(%r11)
	CALL	0,0x4000,20,0x5000,8		/* init A: INETD */
	chsi	4(%r13),49
	jne	fail

	STEP	2				/* A: listener on 21, ACCEPT 4 */
	CALL	25<<16,0x4040,16,0x5000,8
	RC	3
	CALL	(2<<16)|3,0x4050,16,0x5000,8
	RC	0
	CALL	(13<<16)|3,0,5,0x5000,8,0x80
	RC	0
	CALL	(1<<16)|3,0,4,0x5000,24,0x80
	RC	4

	STEP	3				/* A: GETCLIENTID, GIVESOCKET 4 */
	CALL	30<<16,0,0,0x5000,48,0x80
	RC	0
	llilf	%r3,0x4100
	mvc	0(40,%r3),8(%r13)		/* A's client ID */
	CALL	(31<<16)|4,0x4060,40,0x5000,8
	RC	0

	STEP	4				/* B: TAKESOCKET as 9 */
	lg	%r6,0x48(%r11)
	CALL	32<<16,0x4100,48,0x5000,8
	RC	9

	STEP	5				/* A: SELECT exception {4} */
	lg	%r6,0x40(%r11)
	CALL	(19<<16)|4,0x40b0,40,0x5000,28
	RC	1
	chsi	16+8(%r13),0x10
	jne	fail
	CALL	(3<<16)|4,0,0,0x5000,8,0x80	/* CLOSE 4 */
	RC	0

	STEP	6				/* B: READ 9 "data", WRITE "ok" */
	lg	%r6,0x48(%r11)
	CALL	(14<<16)|9,0,0,0x5000,88,0x80
	RC	4
	llilf	%r3,0x40d8
	clc	24(4,%r13),0(%r3)
	jne	fail
	CALL	(26<<16)|9,0x40e0,22,0x5000,8
	RC	2
	CALL	(3<<16)|9,0,0,0x5000,8,0x80
	RC	0

	STEP	7				/* B: UDP socket 10, READ, CANCEL */
	CALL	25<<16,0x4090,16,0x5000,8
	RC	10
	xc	0(16,%r13),0(%r13)
	SENDNW	(14<<16)|10,0,0,0x5100,88,0x80
	stg	%r4,0x50(%r11)
	CLEARP					/* CANCEL's PRMMSG lo = the msgid */
	sth	%r6,0x100(%r10)
	mvi	0x102(%r10),0x80
	llilf	%r3,(42<<16)|10
	st	%r3,0x108(%r10)
	mvhi	0x10c(%r10),14
	st	%r4,0x110(%r10)
	llilf	%r3,0x5000
	st	%r3,0x11c(%r10)
	mvhi	0x120(%r10),8
	IUCV	4
	jnz	fail
	aghi	%r7,1
	WAITR
	llilf	%r3,0x5100
	chsi	4(%r3),1009			/* the READ: canceled */
	jne	fail

	STEP	8				/* PURGE a pending READ: no reply */
	SENDNW	(14<<16)|10,0,0,0x5100,88,0x80
	CLEARP
	sth	%r6,0x100(%r10)
	mvi	0x102(%r10),0x07
	st	%r4,0x104(%r10)
	IUCV	9
	jnz	fail
	CALL	7<<16,0,0,0x5000,8,0x80		/* the path still works */
	llilf	%r3,0x0a00020f
	c	%r3,0(%r13)
	jne	fail

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
	ni	0x131,0xfd
	lmg	%r10,%r13,0xb00(%r0)
	lpswe	0x130

pgm_h:	lghi	%r11,0x2000
	mvc	0x18(2,%r11),0x8e
	mvhi	0(%r11),99
	larl	%r2,donepsw
	lpswe	0(%r2)

	.align	8
newpsws:
	.quad	0x0000000180000000, ext_h
	.quad	0x0000000180000000, pgm_h
	.quad	0x0102000180000000
	.quad	0x00000000000000e2
donepsw: .quad	0x0002000180000000, 0x600d
tcpip:	.byte	0xe3,0xc3,0xd7,0xc9,0xd7,0x40,0x40,0x40		/* TCPIP */

	.align	8
reqs:
/* 0x4000 init INETD */
	.byte	0xc9,0xe4,0xc3,0xe5,0xc1,0xd7,0xc9,0x40
	.short	50, 3
	.byte	0xc9,0xd5,0xc5,0xe3,0xc4,0x40,0x40,0x40
	.fill	12,1,0
	. = reqs + 0x20
/* 0x4020 init SERVER */
	.byte	0xc9,0xe4,0xc3,0xe5,0xc1,0xd7,0xc9,0x40
	.short	50, 3
	.byte	0xe2,0xc5,0xd9,0xe5,0xc5,0xd9,0x40,0x40
	.fill	12,1,0
	. = reqs + 0x40
/* 0x4040 SOCKET 3 stream */
	.long	2, 1, 0, 3
	. = reqs + 0x50
/* 0x4050 sockaddr 0:21 */
	.short	2, 21
	.long	0
	.fill	8,1,0
	. = reqs + 0x60
/* 0x4060 GIVESOCKET target: anyone, subtask SERVER */
	.long	2
	.byte	0x40,0x40,0x40,0x40,0x40,0x40,0x40,0x40
	.byte	0xe2,0xc5,0xd9,0xe5,0xc5,0xd9,0x40,0x40
	.fill	20,1,0
	.fill	8,1,0
	. = reqs + 0x90
/* 0x4070 SOCKET 10 datagram */
	.long	2, 2, 0, 10
	. = reqs + 0xb0
/* 0x40b0 SELECT: nfds 5, timeout 1 s, except set {4} */
	.long	5, 1, 0, 0, 1, 1, 0
	.long	0, 0, 0x10
	. = reqs + 0xd8
/* 0x40d8 expected data */
	.ascii	"data"
	.fill	4,1,0
	. = reqs + 0xe0
/* 0x40e0 WRITE "ok" */
	.fill	20,1,0
	.ascii	"ok"
	.fill	6,1,0
	. = reqs + 0x100
/* 0x4100: client ID of A (filled), giver's socket 4, new socket 9 */
	.fill	40,1,0
	.long	4, 9
