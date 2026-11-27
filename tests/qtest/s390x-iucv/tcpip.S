/*
 * IUCV guest program: the TCP/IP service (iucv-tcpip, name TCPIP, address
 * 10.0.2.15/24), apitype 3. Needs a host 10.0.2.2 on the network of the
 * service with a TCP echo server and a UDP echo server, whose ports the
 * test stores at 0x2f00 and 0x2f02 before the guest starts, nothing
 * listening on its port 1, and a client that connects to 10.0.2.15 port
 * 23, sends "ping" and expects "pong". 0x2000: 0 = pass, else the failing
 * step; 0x2010 last rc, 0x2014 last errno.
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

	.text
	.globl	_start
_start:
	lghi	%r11,0x2000
	lghi	%r10,0x3000
	llilf	%r13,0x5000			/* answers */
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
	mvc	0(256,%r3),0(%r2)		/* request templates */
	mvc	0x100(256,%r3),0x100(%r2)
	mvc	0x32(2,%r3),0xf00(%r11)		/* port of the echo server */
	mvc	0x116(2,%r3),0xf02(%r11)	/* port of the UDP echo server */

	STEP	1
	CLEARP
	st	%r10,0x10c(%r10)
	IUCV	12
	jnz	fail
	lctlg	%c0,%c0,0xa90(%r11)
	stosm	0xaa0(%r11),0x01
	CLEARP
	larl	%r2,tcpip
	mvc	0x108(8,%r10),0(%r2)
	mvi	0x102(%r10),0x80		/* takes PRMDATA */
	IUCV	11
	jnz	fail
	lh	%r6,0x100(%r10)
	aghi	%r7,1
	WAITR

	STEP	2				/* init: maxsock 50, apitype 3 */
	CALL	0,0x4000,20,0x5000,8
	chsi	4(%r13),49
	jne	fail

	STEP	3				/* SOCKET 5, stream */
	CALL	25<<16,0x4020,16,0x5000,8
	RC	5
	STEP	4				/* CONNECT 10.0.2.2, echo */
	CALL	(4<<16)|5,0x4030,16,0x5000,8
	RC	0
	STEP	5				/* WRITE "hello" */
	CALL	(26<<16)|5,0x4040,25,0x5000,8
	RC	5
	STEP	6				/* READ */
	CALL	(14<<16)|5,0,0,0x5000,88,0x80
	RC	5
	llilf	%r3,0x4040+20
	clc	24(5,%r13),0(%r3)
	jne	fail
	STEP	7				/* GETHOSTID: 10.0.2.15 */
	CALL	7<<16,0,0,0x5000,8,0x80
	llilf	%r3,0x0a00020f
	c	%r3,0(%r13)
	jne	fail
	STEP	8				/* GETSOCKNAME 5 */
	CALL	(10<<16)|5,0,0,0x5000,24,0x80
	RC	0
	llilf	%r3,0x0a00020f
	c	%r3,12(%r13)
	jne	fail

	STEP	9				/* SELECT socket 5, 100 ms: none */
	CALL	(19<<16)|4,0x4080,40,0x5000,28
	RC	0

	STEP	10				/* unknown call 6: errno 1000 */
	CALL	(6<<16)|5,0,0,0x5000,8,0x80
	RC	-1
	chsi	4(%r13),1000
	jne	fail

	STEP	11				/* CONNECT 10.0.2.2:1: refused */
	CALL	25<<16,0x40b0,16,0x5000,8	/* SOCKET 8 */
	RC	8
	CALL	(4<<16)|8,0x40c0,16,0x5000,8
	RC	-1
	chsi	4(%r13),61
	jne	fail
	CALL	(3<<16)|8,0,0,0x5000,8,0x80	/* CLOSE 8 */
	RC	0

	STEP	16				/* UDP: SENDTO the echo, RECV */
	CALL	25<<16,0x4100,16,0x5000,8	/* SOCKET 9, datagram */
	RC	9
	CALL	(22<<16)|9,0x4110,25,0x5000,8
	RC	5
	CALL	(16<<16)|9,0,0,0x5000,88,0x80
	RC	5
	llilf	%r3,0x4110+20
	clc	24(5,%r13),0(%r3)
	jne	fail
	llilf	%r3,0x0a000202			/* from 10.0.2.2 */
	c	%r3,12(%r13)
	jne	fail
	CALL	(3<<16)|9,0,0,0x5000,8,0x80	/* CLOSE 9 */
	RC	0

	STEP	12				/* listener on forwarded port 23 */
	CALL	25<<16,0x40d0,16,0x5000,8	/* SOCKET 6 */
	RC	6
	CALL	(2<<16)|6,0x40e0,16,0x5000,8	/* BIND :23 */
	RC	0
	CALL	(13<<16)|6,0,5,0x5000,8,0x80	/* LISTEN 5 */
	RC	0
	STEP	13				/* ACCEPT as 7: waits for the client */
	CALL	(1<<16)|6,0,7,0x5000,24,0x80
	RC	7
	STEP	14				/* READ 7: "ping" */
	CALL	(14<<16)|7,0,0,0x5000,88,0x80
	RC	4
	llilf	%r3,0x4040+20+5
	clc	24(4,%r13),0(%r3)
	jne	fail
	STEP	15				/* WRITE 7 "pong", CLOSE 7, 5 */
	CALL	(26<<16)|7,0x4060,24,0x5000,8
	RC	4
	CALL	(3<<16)|7,0,0,0x5000,8,0x80
	RC	0
	CALL	(3<<16)|5,0,0,0x5000,8,0x80
	RC	0

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
/* 0x4000 init */
	.byte	0xc9,0xe4,0xc3,0xe5,0xc1,0xd7,0xc9,0x40
	.short	50, 3
	.byte	0xd7,0xd9,0xd6,0xc2,0xc5,0x40,0x40,0x40		/* PROBE */
	.fill	12,1,0
/* 0x4020 SOCKET 5 stream */
	.long	2, 1, 0, 5
/* 0x4030 sockaddr 10.0.2.2, the port from 0x2f00 */
	.short	2, 0
	.byte	10,0,2,2
	.fill	8,1,0
/* 0x4040 WRITE: 20 zero bytes, "hello", "ping" (expected) */
	.fill	20,1,0
	.ascii	"hello"
	.ascii	"ping"
	.fill	3,1,0
/* 0x4060 WRITE "pong" */
	.fill	20,1,0
	.ascii	"pong"
	.fill	8,1,0
/* 0x4080 SELECT: nfds 6, timeout, read set, timeval 0.1 s, read set {5} */
	.long	6, 1, 1, 0, 0, 0, 100000
	.long	0x20, 0, 0
	.fill	8,1,0
/* 0x40b0 SOCKET 8 */
	.long	2, 1, 0, 8
/* 0x40c0 sockaddr 10.0.2.2:1 */
	.short	2, 1
	.byte	10,0,2,2
	.fill	8,1,0
/* 0x40d0 SOCKET 6 */
	.long	2, 1, 0, 6
/* 0x40e0 sockaddr 0.0.0.0:23 */
	.short	2, 23
	.long	0
	.fill	8,1,0
/* 0x40f0 unused */
	.fill	16,1,0
/* 0x4100 SOCKET 9 datagram */
	.long	2, 2, 0, 9
/* 0x4110 SENDTO: flags, sockaddr 10.0.2.2 (port from 0x2f02), "hello" */
	.long	0
	.short	2, 0
	.byte	10,0,2,2
	.fill	8,1,0
	.ascii	"hello"
	.fill	11,1,0
