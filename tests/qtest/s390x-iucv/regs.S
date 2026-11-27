/*
 * IUCV guest program: register integrity across the instruction. SETMASK X'F8'
 * many times with the parameter list addressed by R2; R1 and R3-R9 hold
 * patterns that must survive. Result at 0x2000: 0 pass, else the
 * iteration count when a register changed (0x2008: which register).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
	.text
	.globl	_start
_start:
	lghi	%r11,0x2000
	lghi	%r10,0x3000
	xc	0(256,%r11),0(%r11)
	mvhi	0(%r11),-1
	/* DECLARE BUFFER */
	xc	0x100(40,%r10),0x100(%r10)
	st	%r10,0x10c(%r10)
	lghi	%r0,12
	la	%r2,0x100(%r10)
	.long	0xb2f02000
	lgfi	%r12,20000
loop:
	xc	0x100(40,%r10),0x100(%r10)
	mvi	0x100(%r10),0xf8
	llihf	%r1,0x11111111
	oilf	%r1,0x11111111
	lgr	%r3,%r1
	aghi	%r3,2
	lgr	%r4,%r1
	aghi	%r4,3
	lgr	%r5,%r12
	lgr	%r6,%r1
	aghi	%r6,5
	lghi	%r0,16
	la	%r2,0x100(%r10)
	.long	0xb2f02000
	/* check */
	llihf	%r7,0x11111111
	oilf	%r7,0x11111111
	cgr	%r1,%r7
	jne	bad1
	aghi	%r7,2
	cgr	%r3,%r7
	jne	bad3
	aghi	%r7,1
	cgr	%r4,%r7
	jne	bad4
	cgr	%r5,%r12
	jne	bad5
	aghi	%r7,2
	cgr	%r6,%r7
	jne	bad6
	la	%r7,0x100(%r10)
	cgr	%r2,%r7
	jne	bad2
	/* some time in the guest, varying */
	lgr	%r8,%r12
	nill	%r8,0x3ff
	sllg	%r8,%r8,6
	aghi	%r8,1
0:	brctg	%r8,0b
	brctg	%r12,loop
	mvhi	0(%r11),0
	j	done
bad1:	mvhi	8(%r11),1
	j	bad
bad2:	mvhi	8(%r11),2
	j	bad
bad3:	mvhi	8(%r11),3
	j	bad
bad4:	mvhi	8(%r11),4
	j	bad
bad5:	mvhi	8(%r11),5
	j	bad
bad6:	mvhi	8(%r11),6
bad:	st	%r12,0(%r11)
	stmg	%r0,%r7,0x40(%r11)
done:	larl	%r2,donepsw
	lpswe	0(%r2)
	.align	8
donepsw: .quad	0x0002000180000000, 0x600d
