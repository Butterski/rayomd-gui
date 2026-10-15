/* SPDX-License-Identifier: Apache-2.0 */
/* The JPEG kernels, bit-exact with the scalar references (gfx/kernels.h has the definitions):
 *
 * void rgui_ycc_to_bgra_{sse2,avx2}(const uint8_t* const* planes, uint32_t* dst, size_t n)
 * void rgui_idct_islow_sse2(const int16_t* coefficients, uint8_t* out, size_t stride)
 *
 * ycc_to_bgra. The factors 91881, -46802 and 116130 do not fit a signed word, so each term is
 * split exactly (the multiple of 65536 leaves the shift unchanged):
 *   (91881 cr' + 32768) >> 16             = cr' + ((26345 cr' + 32768) >> 16)
 *   (116130 cb' + 32768) >> 16            = 2 cb' + ((-14942 cb' + 32768) >> 16)
 *   (-22554 cb' - 46802 cr' + 32768) >> 16 = -cr' + ((-22554 cb' + 18734 cr' + 32768) >> 16)
 * The G term is a pmaddwd of (cb', cr') pairs. A one-factor term uses
 * (k x + 32768) >> 16 = (pmulhw(2x, k) + 1) >> 1, since floor((floor(y) + 1) / 2) = floor((y + 1) / 2).
 * Y plus each term (words) is clamped by packuswb. SSE2 takes 8 pixels at a time, AVX2 16; the
 * rest go one at a time through the same SSE2 code (AVX2 after vzeroupper).
 *
 * idct_islow. Every step of the reference is int32 arithmetic modulo 2^32 (sums, differences,
 * products, left shifts) up to the descaling shift, so each 1-D output is exactly a linear form in
 * the eight int16 inputs, and the forms below (the reference's z1..z5 multiplied out) compute the
 * same bits for every input, overflow included. pmaddwd evaluates two terms at a time; it never
 * forms a 16-bit sum of inputs, which could wrap where the reference does not.
 *   even: tmp10/13 = 8192 (i0 + i4) +- (10703 i2 + 4433 i6)
 *         tmp11/12 = 8192 (i0 - i4) +- (4433 i2 - 10704 i6)
 *   odd:  tmp3 = 11363 i1 + 9633 i3 + 6437 i5 + 2260 i7     (outputs 0, 7)
 *         tmp2 = 9633 i1 - 2259 i3 - 11362 i5 - 6436 i7     (outputs 1, 6)
 *         tmp1 = 6437 i1 - 11362 i3 + 2261 i5 + 9633 i7     (outputs 2, 5)
 *         tmp0 = 2260 i1 - 6436 i3 + 9633 i5 - 11363 i7     (outputs 3, 4)
 * The block is done as four pairs of rows (r, 7 - r) without a workspace: pass 1 makes the
 * pass-1 rows r and 7 - r for all eight columns (vertical, columns in lanes, (x + 2^10) >> 11 and
 * packssdw's int16 saturation as in the reference); pass 2 then transforms each of them within
 * its register: the words are reordered to X0 X2 X1 X3 X4 X6 X5 X7, so one pmaddwd per output
 * pair c, 7 - c gives the partial even and odd sums of both, and a 4x4 transpose-add finishes
 * E0..E3 and O0..O3; outputs c = E + O and 7 - c = E - O, (x + 2^17) >> 18, + 128, packuswb. */
#include "asm.h"

    RODATA
    .balign 32
.Lk_1:
    .rept 16
    .short 1
    .endr
.Lk_128w:
    .rept 16
    .short 128
    .endr
.Lk_80b:
    .rept 32
    .byte 0x80
    .endr
.Lk_r:
    .rept 16
    .short 26345
    .endr
.Lk_b:
    .rept 16
    .short -14942
    .endr
.Lk_g:
    .rept 8
    .short -22554, 18734
    .endr
.Lk_32768:
    .rept 8
    .long 32768
    .endr
.Lk_1024:
    .rept 4
    .long 1024
    .endr
.Lk_2p17:
    .rept 4
    .long 131072
    .endr
/* Pass 1, per pair p (rows p and 7 - p): the (i0, i4), (i2, i6), (i1, i3), (i5, i7) factors. */
.Lk_idct1:
    .rept 4
    .short 8192, 8192
    .endr
    .rept 4
    .short 10703, 4433
    .endr
    .rept 4
    .short 11363, 9633
    .endr
    .rept 4
    .short 6437, 2260
    .endr

    .rept 4
    .short 8192, -8192
    .endr
    .rept 4
    .short 4433, -10704
    .endr
    .rept 4
    .short 9633, -2259
    .endr
    .rept 4
    .short -11362, -6436
    .endr

    .rept 4
    .short 8192, -8192
    .endr
    .rept 4
    .short -4433, 10704
    .endr
    .rept 4
    .short 6437, -11362
    .endr
    .rept 4
    .short 2261, 9633
    .endr

    .rept 4
    .short 8192, 8192
    .endr
    .rept 4
    .short -10703, -4433
    .endr
    .rept 4
    .short 2260, -6436
    .endr
    .rept 4
    .short 9633, -11363
    .endr
/* Pass 2, per output pair c, 7 - c, on the words X0 X2 X1 X3 X4 X6 X5 X7. */
.Lk_idct2:
    .short 8192, 10703, 11363, 9633, 8192, 4433, 6437, 2260
    .short 8192, 4433, 9633, -2259, -8192, -10704, -11362, -6436
    .short 8192, -4433, 6437, -11362, -8192, 10704, 2261, 9633
    .short 8192, -10703, 2260, -6436, 8192, -4433, 9633, -11363

/* ---- ycc_to_bgra ---- */

/* 8 samples (or 1 when \one) of the plane at \base, index A3, into the low quadword of \reg. */
.macro LOADB reg, base, one
.if \one
    movzbl (\base, A3), A0d
    movd A0d, \reg
.else
    movq (\base, A3), \reg
.endif
.endm

/* 8 pixels (or 1) at index A3: T0, T1, T2 = the Y, Cb, Cr planes, A1 = dst. */
.macro YCC_SSE2 one
    LOADB %xmm0, T1, \one
    pxor .Lk_80b(%rip), %xmm0
    punpcklbw %xmm0, %xmm0
    psraw $8, %xmm0                  /* cb' */
    LOADB %xmm1, T2, \one
    pxor .Lk_80b(%rip), %xmm1
    punpcklbw %xmm1, %xmm1
    psraw $8, %xmm1                  /* cr' */
    movdqa %xmm0, %xmm2
    punpcklwd %xmm1, %xmm2
    pmaddwd .Lk_g(%rip), %xmm2
    paddd .Lk_32768(%rip), %xmm2
    psrad $16, %xmm2
    movdqa %xmm0, %xmm3
    punpckhwd %xmm1, %xmm3
    pmaddwd .Lk_g(%rip), %xmm3
    paddd .Lk_32768(%rip), %xmm3
    psrad $16, %xmm3
    packssdw %xmm3, %xmm2
    psubw %xmm1, %xmm2               /* G - Y */
    movdqa %xmm1, %xmm3
    psllw $1, %xmm3
    pmulhw .Lk_r(%rip), %xmm3
    paddw .Lk_1(%rip), %xmm3
    psraw $1, %xmm3
    paddw %xmm1, %xmm3               /* R - Y */
    movdqa %xmm0, %xmm1
    psllw $1, %xmm1
    pmulhw .Lk_b(%rip), %xmm1
    paddw .Lk_1(%rip), %xmm1
    psraw $1, %xmm1
    paddw %xmm0, %xmm1
    paddw %xmm0, %xmm1               /* B - Y */
    LOADB %xmm0, T0, \one
    punpcklbw %xmm0, %xmm0
    psrlw $8, %xmm0                  /* Y */
    paddw %xmm0, %xmm2
    paddw %xmm0, %xmm3
    paddw %xmm0, %xmm1
    packuswb %xmm2, %xmm2
    packuswb %xmm3, %xmm3
    packuswb %xmm1, %xmm1
    punpcklbw %xmm2, %xmm1           /* B G */
    pcmpeqb %xmm0, %xmm0
    punpcklbw %xmm0, %xmm3           /* R A */
    movdqa %xmm1, %xmm0
    punpcklwd %xmm3, %xmm1
.if \one
    movd %xmm1, (A1, A3, 4)
.else
    punpckhwd %xmm3, %xmm0
    movdqu %xmm1, (A1, A3, 4)
    movdqu %xmm0, 16(A1, A3, 4)
.endif
.endm

/* The pixels from index A3 on, A2 of them. */
.macro YCC_REST_SSE2 done
    cmp $8, A2
    jb 2f
1:
    YCC_SSE2 0
    add $8, A3
    sub $8, A2
    cmp $8, A2
    jae 1b
2:
    test A2, A2
    jz \done
3:
    YCC_SSE2 1
    add $1, A3
    sub $1, A2
    jnz 3b
.endm

/* T0, T1, T2 = the planes; A3 = 0. */
.macro YCC_SETUP
    mov (A0), T0
    mov 8(A0), T1
    mov 16(A0), T2
    xorl A3d, A3d
.endm

FUNC rgui_ycc_to_bgra_sse2
    YCC_SETUP
    YCC_REST_SSE2 .Lycc_sse2_done
.Lycc_sse2_done:
    ret
ENDFUNC rgui_ycc_to_bgra_sse2

FUNC rgui_ycc_to_bgra_avx2
    YCC_SETUP
    cmp $16, A2
    jb 2f
1:
    vpmovzxbw (T1, A3), %ymm0
    vpsubw .Lk_128w(%rip), %ymm0, %ymm0       /* cb' */
    vpmovzxbw (T2, A3), %ymm1
    vpsubw .Lk_128w(%rip), %ymm1, %ymm1       /* cr' */
    vpunpcklwd %ymm1, %ymm0, %ymm2
    vpmaddwd .Lk_g(%rip), %ymm2, %ymm2
    vpaddd .Lk_32768(%rip), %ymm2, %ymm2
    vpsrad $16, %ymm2, %ymm2
    vpunpckhwd %ymm1, %ymm0, %ymm3
    vpmaddwd .Lk_g(%rip), %ymm3, %ymm3
    vpaddd .Lk_32768(%rip), %ymm3, %ymm3
    vpsrad $16, %ymm3, %ymm3
    vpackssdw %ymm3, %ymm2, %ymm2
    vpsubw %ymm1, %ymm2, %ymm2                /* G - Y */
    vpsllw $1, %ymm1, %ymm3
    vpmulhw .Lk_r(%rip), %ymm3, %ymm3
    vpaddw .Lk_1(%rip), %ymm3, %ymm3
    vpsraw $1, %ymm3, %ymm3
    vpaddw %ymm1, %ymm3, %ymm3                /* R - Y */
    vpsllw $1, %ymm0, %ymm1
    vpmulhw .Lk_b(%rip), %ymm1, %ymm1
    vpaddw .Lk_1(%rip), %ymm1, %ymm1
    vpsraw $1, %ymm1, %ymm1
    vpaddw %ymm0, %ymm1, %ymm1
    vpaddw %ymm0, %ymm1, %ymm1                /* B - Y */
    vpmovzxbw (T0, A3), %ymm0                 /* Y */
    vpaddw %ymm0, %ymm2, %ymm2
    vpaddw %ymm0, %ymm3, %ymm3
    vpaddw %ymm0, %ymm1, %ymm1
    vpackuswb %ymm2, %ymm2, %ymm2             /* G 0-7 | G 8-15, each twice */
    vpackuswb %ymm3, %ymm3, %ymm3
    vpackuswb %ymm1, %ymm1, %ymm1
    vpunpcklbw %ymm2, %ymm1, %ymm1            /* B G */
    vpcmpeqb %ymm0, %ymm0, %ymm0
    vpunpcklbw %ymm0, %ymm3, %ymm3            /* R A */
    vpunpckhwd %ymm3, %ymm1, %ymm0            /* pixels 4-7 | 12-15 */
    vpunpcklwd %ymm3, %ymm1, %ymm1            /* pixels 0-3 | 8-11 */
    vperm2i128 $0x20, %ymm0, %ymm1, %ymm2
    vperm2i128 $0x31, %ymm0, %ymm1, %ymm3
    vmovdqu %ymm2, (A1, A3, 4)
    vmovdqu %ymm3, 32(A1, A3, 4)
    add $16, A3
    sub $16, A2
    cmp $16, A2
    jae 1b
2:
    vzeroupper
    YCC_REST_SSE2 .Lycc_avx2_done
.Lycc_avx2_done:
    ret
ENDFUNC rgui_ycc_to_bgra_avx2

/* ---- idct_islow ---- */

/* Pass 1 for the four columns at byte offset \h of each input row (A0), the factors of the pair
 * at (T0): \e = rows r, \t1 = rows 7 - r, descaled dwords. Clobbers \o and \t2. */
.macro IDCT_PASS1 h, e, o, t1, t2
    movq \h(A0), \e
    movq \h+64(A0), \t1
    punpcklwd \t1, \e
    pmaddwd 0(T0), \e
    movq \h+32(A0), \t1
    movq \h+96(A0), \t2
    punpcklwd \t2, \t1
    pmaddwd 16(T0), \t1
    paddd \t1, \e                    /* even */
    movq \h+16(A0), \o
    movq \h+48(A0), \t1
    punpcklwd \t1, \o
    pmaddwd 32(T0), \o
    movq \h+80(A0), \t1
    movq \h+112(A0), \t2
    punpcklwd \t2, \t1
    pmaddwd 48(T0), \t1
    paddd \t1, \o                    /* odd */
    movdqa \e, \t1
    paddd \o, \e
    psubd \o, \t1
    paddd .Lk_1024(%rip), \e
    paddd .Lk_1024(%rip), \t1
    psrad $11, \e
    psrad $11, \t1
.endm

/* Pass 2 of the pass-1 row in xmm0, stored at (\out). Clobbers xmm0-4. */
.macro IDCT_PASS2 out
    pshuflw $0xD8, %xmm0, %xmm0
    pshufhw $0xD8, %xmm0, %xmm0      /* X0 X2 X1 X3 X4 X6 X5 X7 */
    movdqa %xmm0, %xmm1
    movdqa %xmm0, %xmm2
    movdqa %xmm0, %xmm3
    pmaddwd .Lk_idct2(%rip), %xmm0
    pmaddwd .Lk_idct2+16(%rip), %xmm1
    pmaddwd .Lk_idct2+32(%rip), %xmm2
    pmaddwd .Lk_idct2+48(%rip), %xmm3
    movdqa %xmm0, %xmm4
    punpckldq %xmm1, %xmm0
    punpckhdq %xmm1, %xmm4
    paddd %xmm4, %xmm0               /* E0 E1 O0 O1 */
    movdqa %xmm2, %xmm4
    punpckldq %xmm3, %xmm2
    punpckhdq %xmm3, %xmm4
    paddd %xmm4, %xmm2               /* E2 E3 O2 O3 */
    movdqa %xmm0, %xmm1
    punpcklqdq %xmm2, %xmm0          /* E */
    punpckhqdq %xmm2, %xmm1          /* O */
    movdqa %xmm0, %xmm2
    paddd %xmm1, %xmm0               /* outputs 0 1 2 3 */
    psubd %xmm1, %xmm2               /* outputs 7 6 5 4 */
    paddd .Lk_2p17(%rip), %xmm0
    paddd .Lk_2p17(%rip), %xmm2
    psrad $18, %xmm0
    psrad $18, %xmm2
    packssdw %xmm2, %xmm0
    pshufhw $0x1B, %xmm0, %xmm0
    paddw .Lk_128w(%rip), %xmm0
    packuswb %xmm0, %xmm0
    movq %xmm0, (\out)
.endm

FUNC rgui_idct_islow_sse2
    lea .Lk_idct1(%rip), T0
    lea (, A2, 8), T1
    sub A2, T1
    lea (A1, T1), A3                 /* row 7 */
    movl $4, T1d
1:
    IDCT_PASS1 0, %xmm0, %xmm2, %xmm1, %xmm3
    IDCT_PASS1 8, %xmm2, %xmm3, %xmm4, %xmm5
    packssdw %xmm2, %xmm0            /* pass-1 row r */
    packssdw %xmm4, %xmm1            /* pass-1 row 7 - r */
    movdqa %xmm1, %xmm5
    IDCT_PASS2 A1
    movdqa %xmm5, %xmm0
    IDCT_PASS2 A3
    add $64, T0
    add A2, A1
    sub A2, A3
    sub $1, T1d
    jnz 1b
    ret
ENDFUNC rgui_idct_islow_sse2

FILE_END
