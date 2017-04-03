#define XADDL(s,d) BYTE $0x0F; BYTE $0xC1; BYTE $((0<<6)|(s<<3)|(d))

TEXT	xaddu(SB),$0	/* ulong xadd(ulong *, long); */
TEXT	xadd(SB),$0	/* long xadd(long *, long); */

	MOVL	l+0(FP),BX
	MOVL	i+4(FP),AX
	LOCK
	XADDL(0,3)
	RET

TEXT	xincu(SB),$0	/* void _xinc(ulong *); */
TEXT	_xinc(SB),$0	/* void _xinc(long *); */

	MOVL	l+0(FP),AX
	LOCK
	INCL	0(AX)
	RET

TEXT	xdecu(SB),$0	/* ulong xdecu(ulong *); */
TEXT	_xdec(SB),$0	/* long _xdec(long *); */

	MOVL	l+0(FP),AX
	LOCK
	DECL	0(AX)
	JZ	iszero
	MOVL	$1, AX
	RET
iszero:
	MOVL	$0, AX
	RET

TEXT	xchgu(SB),$0	/* ulong xchg(ulong *, ulong); */
TEXT	xchg(SB),$0	/* long xchg(long *, long); */

	MOVL	l+0(FP),BX
	MOVL	i+4(FP),AX
	XCHGL	AX,(BX)         /* note: xchg has implicit LOCK */
	RET
