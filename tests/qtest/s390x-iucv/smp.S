/*
 * IUCV guest program: two vCPUs. IUCV interruptions float to a vCPU that has
 * declared a buffer and is enabled for them (SETMASK is per vCPU).
 * CPU 1 runs with prefix 0x40000, buffer 0x5000, log at 0x6100.
 * CPU 0: buffer 0x3000, log at 0x2100, result at 0x2000.
 *  1  CPU 1 started and declared its buffer
 *  2  with CPU 0 masked off, connection complete + message records go to CPU 1
 *  3  CPU 0 enabled, CPU 1 masked off: the next records go to CPU 0
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

	/* wait until the word at \cnt is \n, enabled */
	.macro WAITC cnt,n
	lghi	%r8,\n
	larl	%r9,0f
	stg	%r9,0xa88(%r11)
0:	cg	%r8,\cnt
	jnh	1f
	lpswe	0xa80(%r11)
1:
	.endm

	/* spin until the word at \cnt is \n (set by the other CPU) */
	.macro SPINC cnt,n
	lghi	%r8,\n
0:	cg	%r8,\cnt
	jh	0b
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
	mvc	0x1d0(16,%r0),16(%r2)		/* program: fail */
	mvc	0xa80(8,%r11),32(%r2)		/* enabled wait mask */
	mvc	0xa90(8,%r11),40(%r2)		/* CR0 */
	/* CPU 1's prefix area and data */
	llilf	%r3,0x40000
	mvc	0x1a0(16,%r3),48(%r2)		/* restart new PSW: cpu1 */
	mvc	0x1b0(16,%r3),0(%r2)
	mvc	0x1d0(16,%r3),16(%r2)
	lghi	%r4,0x4000			/* CPU 1's data base */
	xc	0(256,%r4),0(%r4)
	mvc	0xa80(8,%r4),32(%r2)
	mvc	0xa90(8,%r4),40(%r2)
	/* SIGP set prefix (0x0d), restart (0x06) to CPU 1 */
	lghi	%r5,0
	llilf	%r5,0x40000
	lghi	%r6,1
	sigp	%r4,%r6,0x0d
	jnz	f1
	sigp	%r4,%r6,0x06
	jnz	f1

	lctlg	%c0,%c0,0xa90(%r11)
	stosm	0xaa0(%r11),0x01
	/* DECLARE BUFFER on CPU 0, then SETMASK 0 */
	CLEARP
	st	%r10,0x10c(%r10)
	IUCV	12
	jnz	f1
	CLEARP
	IUCV	16
	/* 1: wait for CPU 1 to declare */
	SPINC	0x4000+8,1
	/* 2: CONNECT; complete, then two messages for CPU 1 */
	CLEARP
	larl	%r2,echo
	mvc	0x108(8,%r10),0(%r2)
	IUCV	11
	jnz	f2
	lh	%r6,0x100(%r10)
	larl	%r2,hello
	mvc	0x200(10,%r10),0(%r2)
	CLEARP
	sth	%r6,0x100(%r10)
	mvi	0x102(%r10),0x10
	mvhi	0x108(%r10),1
	la	%r3,0x200(%r10)
	st	%r3,0x10c(%r10)
	mvhi	0x110(%r10),10
	IUCV	4
	jnz	f2
	lghi	%r4,0x4000
	SPINC	0x10(%r4),3			/* CPU 1 counts 3 */
	chsi	0xc(%r11),0			/* CPU 0 got none */
	jne	f2
	cli	0x100+40*1+3(%r4),2
	jne	f2
	cli	0x100+40*2+3(%r4),7
	jne	f2
	cli	0x100+40*3+3(%r4),9
	jne	f2
	/* 3: CPU 1 masks itself off (flag), CPU 0 enables */
	mvi	0x20(%r4),1
	SPINC	0x10(%r4),4			/* CPU 1 confirms */
	CLEARP
	mvi	0x100(%r10),0xf8
	IUCV	16
	/* RECEIVE the pending echo; then send again */
	CLEARP
	mvc	0x100(2,%r10),0x100+120(%r4)
	mvi	0x102(%r10),0x07
	mvc	0x104(8,%r10),0x100+120+4(%r4)
	la	%r3,0x400(%r10)
	st	%r3,0x10c(%r10)
	mvhi	0x110(%r10),64
	IUCV	5
	jnz	f3
	CLEARP
	sth	%r6,0x100(%r10)
	mvi	0x102(%r10),0x10
	mvhi	0x108(%r10),1
	la	%r3,0x200(%r10)
	st	%r3,0x10c(%r10)
	mvhi	0x110(%r10),10
	IUCV	4
	jnz	f3
	WAITC	8(%r11),2
	cli	0x100+40+3(%r11),7
	jne	f3
	cli	0x100+80+3(%r11),9
	jne	f3
	lghi	%r4,0x4000
	chsi	0x14(%r4),4			/* CPU 1 got no more */
	jne	f3

	mvhi	0(%r11),0
fail:	larl	%r2,donepsw
	lpswe	0(%r2)

	.irp	n,1,2,3
f\n:	FAIL	\n
	.endr

/* CPU 1: data base 0x4000 (count at 0x10, log at 0x100), buffer 0x5000 */
cpu1:	lghi	%r11,0x4000
	lghi	%r10,0x5000
	lctlg	%c0,%c0,0xa90(%r11)
	CLEARP
	st	%r10,0x10c(%r10)
	IUCV	12
	jnz	c1bad
	mvhi	0xc(%r11),0
	mvhi	0x8(%r11),1			/* declared: CPU 0 waits for this */
	stosm	0xaa0(%r11),0x01
c1loop:	cli	0x20(%r11),1
	jne	c1wait
	mvi	0x20(%r11),2
	CLEARP
	IUCV	16				/* SETMASK 0 */
	asi	0x14(%r11),1			/* confirm: count 4 */
c1wait:	j	c1loop
c1bad:	mvhi	0x8(%r11),-1
	j	c1bad

/* external interruption handler, both CPUs: data base by prefix */
ext_h:	stmg	%r10,%r13,0xb00(%r0)
	stap	0xb40(%r0)
	lghi	%r11,0x2000
	lghi	%r10,0x3000
	cli	0xb41,0
	je	1f
	lghi	%r11,0x4000
	lghi	%r10,0x5000
1:	l	%r12,0x14(%r11)			/* count, CPU 1 */
	cli	0xb41,0
	jne	2f
	l	%r12,0xc(%r11)			/* count, CPU 0 */
2:	ahi	%r12,1
	lgr	%r13,%r12
	mghi	%r13,40
	la	%r13,0x100(%r13,%r11)
	mvc	0(40,%r13),0(%r10)
	cli	0xb41,0
	jne	3f
	st	%r12,0xc(%r11)
	j	4f
3:	st	%r12,0x14(%r11)
4:	ni	0x131,0xfd
	lmg	%r10,%r13,0xb00(%r0)
	lpswe	0x130

pgm_h:	lghi	%r11,0x2000
	mvhi	0(%r11),99
	larl	%r2,donepsw
	lpswe	0(%r2)

	.align	8
newpsws:
	.quad	0x0000000180000000, ext_h
	.quad	0x0000000180000000, pgm_h
	.quad	0x0102000180000000		/* enabled wait */
	.quad	0x00000000000000e2		/* CR0: IUCV subclass */
	.quad	0x0000000180000000, cpu1
donepsw: .quad	0x0002000180000000, 0x600d
echo:	.byte	0xc5,0xc3,0xc8,0xd6,0x40,0x40,0x40,0x40		/* ECHO */
hello:	.ascii	"hello iucv"
