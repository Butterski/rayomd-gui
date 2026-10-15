/* SPDX-License-Identifier: Apache-2.0 */
/* rgui_accumulate_row_{sse2,avx2}: the rasterizer's row accumulation, bit-exact with
 * rgui_accumulate_row_scalar (gfx/kernels.h has the definition).
 *
 * uint32_t rgui_accumulate_row_<isa>(int32_t* cells, uint8_t* alpha, size_t width, uint32_t evenOdd)
 *
 * The vector loops take 8 cells at a time: covers and areas are separated, the covers turned into
 * their running sum (the carry lives broadcast in xmm5/ymm5), and m = |((acc << 9) - area) >> 9|
 * computed in dwords. NonZero clamps m to 255 by the signed then unsigned saturating packs.
 * EvenOdd folds t = m & 511 to min(t, 512 - t) (equal to the definition's "512 - t if t > 256")
 * before the packs. The alpha bytes are stored and ORed into T0 eight at a time; the last
 * width % 8 cells go through general-purpose registers. */
#include "asm.h"

/* Four cells at \off(A0): \r = their m as dwords. Advances the carry in xmm5; clobbers xmm0-1. */
.macro ACC4_SSE2 off, r
    movdqu \off(A0), %xmm0
    movdqu \off+16(A0), %xmm1
    movaps %xmm0, \r
    shufps $0x88, %xmm1, \r          /* covers */
    shufps $0xDD, %xmm1, %xmm0       /* areas */
    movdqa \r, %xmm1
    pslldq $4, %xmm1
    paddd %xmm1, \r
    movdqa \r, %xmm1
    pslldq $8, %xmm1
    paddd %xmm1, \r                  /* sums within the four */
    pshufd $0xFF, \r, %xmm1          /* their total, off the carry's dependency chain */
    paddd %xmm5, \r                  /* acc */
    paddd %xmm1, %xmm5
    pslld $9, \r
    psubd %xmm0, \r                  /* v */
    psrad $9, \r                     /* a */
    movdqa \r, %xmm1
    psrad $31, %xmm1
    pxor %xmm1, \r
    psubd %xmm1, \r                  /* m */
.endm

/* EvenOdd fold of the m dwords in \r into its low four words (xmm4 = 511); clobbers xmm0-1. */
.macro FOLD4_SSE2 r
    pand %xmm4, \r                   /* t */
    movdqa \r, %xmm0
    pxor %xmm4, %xmm0
    pcmpeqd %xmm1, %xmm1
    psubd %xmm1, %xmm0               /* 512 - t */
    packssdw %xmm0, \r
    pshufd $0x4E, \r, %xmm0
    pminsw %xmm0, \r
.endm

/* Stores the 8 alpha bytes in the low quadword of \r and ORs them into T0; clears the 8 cells
 * and advances the pointers and the count. */
.macro STORE8 r, zero
    movq \r, T1
    movq T1, (A1)
    orq T1, T0
    movdqu \zero, (A0)
    movdqu \zero, 16(A0)
    movdqu \zero, 32(A0)
    movdqu \zero, 48(A0)
    add $64, A0
    add $8, A1
    sub $8, A2
.endm

/* The remaining A2 cells, one at a time, the running sum in T2d. Then jumps to \done. */
.macro ACC_TAIL evenodd, done
    test A2, A2
    jz \done
1:
    addl (A0), T2d
    movl T2d, T1d
    shll $9, T1d
    subl 4(A0), T1d
    sarl $9, T1d
    movl T1d, A3d
    sarl $31, A3d
    xorl A3d, T1d
    subl A3d, T1d
.if \evenodd
    andl $511, T1d
    movl $512, A3d
    subl T1d, A3d
    cmpl $256, T1d
    cmovgl A3d, T1d
.endif
    movl $255, A3d
    cmpl A3d, T1d
    cmovgl A3d, T1d
    movq $0, (A0)
    movb T1b, (A1)
    orq T1, T0                       /* 64-bit: T0 holds the vector loop's 8 bytes */
    add $8, A0
    add $1, A1
    sub $1, A2
    jnz 1b
    jmp \done
.endm

/* T0 = the OR of the bytes of T0. */
.macro FOLD_OR
    movq T0, T1
    shrq $32, T1
    orl T1d, T0d
    movl T0d, T1d
    shrl $16, T1d
    orl T1d, T0d
    movl T0d, T1d
    shrl $8, T1d
    orl T1d, T0d
    movzbl T0b, T0d
.endm

FUNC rgui_accumulate_row_sse2
    xorl T0d, T0d
    pxor %xmm5, %xmm5
    testl A3d, A3d
    jnz .Lsse2_eo
    cmp $8, A2
    jb .Lsse2_nz_tail
.Lsse2_nz_loop:
    ACC4_SSE2 0, %xmm2
    ACC4_SSE2 32, %xmm3
    packssdw %xmm3, %xmm2
    packuswb %xmm2, %xmm2
    pxor %xmm0, %xmm0
    STORE8 %xmm2, %xmm0
    cmp $8, A2
    jae .Lsse2_nz_loop
.Lsse2_nz_tail:
    movd %xmm5, T2d
    ACC_TAIL 0, .Lsse2_done
.Lsse2_eo:
    pcmpeqd %xmm4, %xmm4
    psrld $23, %xmm4                 /* 511 */
    cmp $8, A2
    jb .Lsse2_eo_tail
.Lsse2_eo_loop:
    ACC4_SSE2 0, %xmm2
    FOLD4_SSE2 %xmm2
    ACC4_SSE2 32, %xmm3
    FOLD4_SSE2 %xmm3
    punpcklqdq %xmm3, %xmm2
    packuswb %xmm2, %xmm2
    pxor %xmm0, %xmm0
    STORE8 %xmm2, %xmm0
    cmp $8, A2
    jae .Lsse2_eo_loop
.Lsse2_eo_tail:
    movd %xmm5, T2d
    ACC_TAIL 1, .Lsse2_done
.Lsse2_done:
    FOLD_OR
    ret
ENDFUNC rgui_accumulate_row_sse2

/* Eight cells at (A0): ymm2 = their m as dwords, cells cleared (ymm3 = 0). Advances the carry in
 * ymm5; clobbers ymm0-1. */
.macro ACC8_AVX2
    vmovdqu (A0), %ymm0              /* c0 a0 c1 a1 | c2 a2 c3 a3 */
    vmovdqu 32(A0), %ymm1            /* c4 a4 c5 a5 | c6 a6 c7 a7 */
    vshufps $0x88, %ymm1, %ymm0, %ymm2
    vshufps $0xDD, %ymm1, %ymm0, %ymm0
    vpermq $0xD8, %ymm2, %ymm2       /* c0 .. c7 */
    vpermq $0xD8, %ymm0, %ymm0       /* a0 .. a7 */
    vmovdqu %ymm3, (A0)
    vmovdqu %ymm3, 32(A0)
    vpslldq $4, %ymm2, %ymm1
    vpaddd %ymm1, %ymm2, %ymm2
    vpslldq $8, %ymm2, %ymm1
    vpaddd %ymm1, %ymm2, %ymm2       /* sums within each half */
    vpshufd $0xFF, %ymm2, %ymm1
    vperm2i128 $0x08, %ymm1, %ymm1, %ymm1
    vpaddd %ymm1, %ymm2, %ymm2       /* sums within the eight */
    vpshufd $0xFF, %ymm2, %ymm1
    vperm2i128 $0x11, %ymm1, %ymm1, %ymm1  /* their total, off the carry's dependency chain */
    vpaddd %ymm5, %ymm2, %ymm2       /* acc */
    vpaddd %ymm1, %ymm5, %ymm5
    vpslld $9, %ymm2, %ymm2
    vpsubd %ymm0, %ymm2, %ymm2
    vpsrad $9, %ymm2, %ymm2
    vpabsd %ymm2, %ymm2              /* m */
.endm

/* Packs the dwords of ymm2 (at most 2^22, or 256 after the fold) to bytes, stores them. */
.macro PACK8_AVX2
    vextracti128 $1, %ymm2, %xmm1
    vpackssdw %xmm1, %xmm2, %xmm2
    vpackuswb %xmm2, %xmm2, %xmm2
    vmovq %xmm2, T1
    movq T1, (A1)
    orq T1, T0
    add $64, A0
    add $8, A1
    sub $8, A2
.endm

FUNC rgui_accumulate_row_avx2
    xorl T0d, T0d
    vpxor %xmm5, %xmm5, %xmm5
    vpxor %xmm3, %xmm3, %xmm3
    testl A3d, A3d
    jnz .Lavx2_eo
    cmp $8, A2
    jb .Lavx2_nz_tail
.Lavx2_nz_loop:
    ACC8_AVX2
    PACK8_AVX2
    cmp $8, A2
    jae .Lavx2_nz_loop
.Lavx2_nz_tail:
    vmovd %xmm5, T2d
    vzeroupper
    ACC_TAIL 0, .Lavx2_done
.Lavx2_eo:
    vpcmpeqd %ymm4, %ymm4, %ymm4
    vpsrld $23, %ymm4, %ymm4         /* 511 */
    cmp $8, A2
    jb .Lavx2_eo_tail
.Lavx2_eo_loop:
    ACC8_AVX2
    vpand %ymm4, %ymm2, %ymm2        /* t */
    vpxor %ymm4, %ymm2, %ymm1
    vpcmpeqd %ymm0, %ymm0, %ymm0
    vpsubd %ymm0, %ymm1, %ymm1       /* 512 - t */
    vpminsd %ymm1, %ymm2, %ymm2
    PACK8_AVX2
    cmp $8, A2
    jae .Lavx2_eo_loop
.Lavx2_eo_tail:
    vmovd %xmm5, T2d
    vzeroupper
    ACC_TAIL 1, .Lavx2_done
.Lavx2_done:
    FOLD_OR
    ret
ENDFUNC rgui_accumulate_row_avx2

FILE_END
