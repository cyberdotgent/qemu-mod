/*
 * IUCV guest program: "IUCV not available". 0x2000: 0 pass / failing step;
 * 0x2010 program-interruption code, 0x2020 the program old PSW.
 * 1: QUERY in the supervisor state
 * ends with CC 3. 2: DCLBFR (with a parameter list) CC 3, list unchanged.
 * 3: problem state: operation exception.
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
	larl	%r2,pgmnew
	mvc	0x1d0(16,%r0),0(%r2)
	lghi	%r0,0
	la	%r1,0x100(%r10)
	.long	0xb2f01000
	ipm	%r3
	srl	%r3,28
	chi	%r3,3
	jne	f1
	mvi	0x103(%r10),0x55
	lghi	%r0,12
	.long	0xb2f01000
	ipm	%r3
	srl	%r3,28
	chi	%r3,3
	jne	f2
	cli	0x103(%r10),0x55
	jne	f2
	larl	%r2,ppsw
	lpswe	0(%r2)
prob:	.long	0xb2f01000
	j	f3
pgm_h:	lghi	%r11,0x2000
	mvc	0x12(2,%r11),0x8e
	mvc	0x20(16,%r11),0x150		/* program old PSW */
	chsi	0x10(%r11),1
	jne	f3
	mvhi	0(%r11),0
	j	done
f1:	mvhi	0(%r11),1
	j	done
f2:	mvhi	0(%r11),2
	j	done
f3:	mvhi	0(%r11),3
done:	larl	%r2,donepsw
	lpswe	0(%r2)
	.align	8
pgmnew:	.quad	0x0000000180000000, pgm_h
ppsw:	.quad	0x0001000180000000, prob
donepsw: .quad	0x0002000180000000, 0x600d
