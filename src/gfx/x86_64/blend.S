/* SPDX-License-Identifier: Apache-2.0 */
/* rgui_blend_solid_{sse2,avx2} and rgui_blend_span_{sse2,avx2}: source-over compositing of
 * premultiplied pixels under 8-bit coverage, bit-exact with the scalar references
 * (gfx/kernels.h has the definition).
 *
 * void rgui_blend_solid_<isa>(uint32_t* dst, const uint8_t* coverage, size_t n, uint32_t color)
 * void rgui_blend_span_<isa>(uint32_t* dst, const uint32_t* src, const uint8_t* coverage, size_t n)
 *
 * Channels are widened to 16-bit words. mul255(a, b) = (t + (t >> 8)) >> 8 with t = a * b + 128
 * never leaves 16 bits (t <= 65153, t + (t >> 8) <= 65407), so pmullw, paddw and psrlw compute it
 * exactly. 255 - sa is sa ^ 255. s + d is kept to its low byte, as the reference does, before
 * the unsigned saturating pack. SSE2 takes 4 pixels at a time, AVX2 8; the rest go one at a time
 * (AVX2 hands its remainder to the SSE2 code after vzeroupper). */
#include "asm.h"

    RODATA
    .balign 32
.Lk_00ff:
    .rept 16
    .short 0x00FF
    .endr
.Lk_0080:
    .rept 16
    .short 0x0080
    .endr
/* vpshufb masks: coverage bytes c0..c7 (broadcast to every quadword) to words, each repeated
 * for the four channels: lo = c0 c1 | c4 c5, hi = c2 c3 | c6 c7, matching the in-lane unpacking
 * of the destination pixels 0 1 | 4 5 and 2 3 | 6 7. 0x80 makes a zero byte. */
.Lk_cov_lo:
    .byte 0, 0x80, 0, 0x80, 0, 0x80, 0, 0x80, 1, 0x80, 1, 0x80, 1, 0x80, 1, 0x80
    .byte 4, 0x80, 4, 0x80, 4, 0x80, 4, 0x80, 5, 0x80, 5, 0x80, 5, 0x80, 5, 0x80
.Lk_cov_hi:
    .byte 2, 0x80, 2, 0x80, 2, 0x80, 2, 0x80, 3, 0x80, 3, 0x80, 3, 0x80, 3, 0x80
    .byte 6, 0x80, 6, 0x80, 6, 0x80, 6, 0x80, 7, 0x80, 7, 0x80, 7, 0x80, 7, 0x80

/* ---- SSE2 ---- */

/* \r = mul255 of the products already in \r; clobbers \tmp. */
.macro MUL255 r, tmp
    paddw .Lk_0080(%rip), \r
    movdqa \r, \tmp
    psrlw $8, \tmp
    paddw \tmp, \r
    psrlw $8, \r
.endm

/* The low 8 bytes of \r as words. */
.macro WORDS r
    punpcklbw \r, \r
    psrlw $8, \r
.endm

/* \k = 255 - sa for each channel word of the s words in \s. */
.macro KEEP s, k
    pshuflw $0xFF, \s, \k
    pshufhw $0xFF, \k, \k
    pxor .Lk_00ff(%rip), \k
.endm

/* The coverage of 4 pixels at (\p) as words: xmm2 = pixels 0-1, xmm3 = pixels 2-3. */
.macro COV4 p
    movd (\p), %xmm2
    punpcklbw %xmm2, %xmm2
    punpcklwd %xmm2, %xmm2
    movdqa %xmm2, %xmm3
    WORDS %xmm2
    punpckhbw %xmm3, %xmm3
    psrlw $8, %xmm3
.endm

/* The coverage of 1 pixel at (\p) as words in xmm2. */
.macro COV1 p
    movzbl (\p), T1d
    movd T1d, %xmm2
    punpcklbw %xmm2, %xmm2
    punpcklwd %xmm2, %xmm2
    WORDS %xmm2
.endm

/* 4 destination pixels at (A0) blended with the s words in xmm2 (pixels 0-1) and xmm3 (2-3). */
.macro BLEND4
    movdqu (A0), %xmm0
    movdqa %xmm0, %xmm1
    WORDS %xmm0
    punpckhbw %xmm1, %xmm1
    psrlw $8, %xmm1
    KEEP %xmm2, %xmm4
    pmullw %xmm4, %xmm0
    MUL255 %xmm0, %xmm4
    paddw %xmm2, %xmm0
    KEEP %xmm3, %xmm4
    pmullw %xmm4, %xmm1
    MUL255 %xmm1, %xmm4
    paddw %xmm3, %xmm1
    pand .Lk_00ff(%rip), %xmm0
    pand .Lk_00ff(%rip), %xmm1
    packuswb %xmm1, %xmm0
    movdqu %xmm0, (A0)
.endm

/* 1 destination pixel at (A0) blended with the s words in xmm2. */
.macro BLEND1
    movd (A0), %xmm0
    WORDS %xmm0
    KEEP %xmm2, %xmm4
    pmullw %xmm4, %xmm0
    MUL255 %xmm0, %xmm4
    paddw %xmm2, %xmm0
    pand .Lk_00ff(%rip), %xmm0
    packuswb %xmm0, %xmm0
    movd %xmm0, (A0)
.endm

/* blend_solid from (A0, A1, A2) on, the colour words in xmm5 (both quadwords). */
.macro SOLID_SSE2 done
    cmp $4, A2
    jb 2f
1:
    COV4 A1
    pmullw %xmm5, %xmm2
    MUL255 %xmm2, %xmm4
    pmullw %xmm5, %xmm3
    MUL255 %xmm3, %xmm4
    BLEND4
    add $16, A0
    add $4, A1
    sub $4, A2
    cmp $4, A2
    jae 1b
2:
    test A2, A2
    jz \done
3:
    COV1 A1
    pmullw %xmm5, %xmm2
    MUL255 %xmm2, %xmm4
    BLEND1
    add $4, A0
    add $1, A1
    sub $1, A2
    jnz 3b
.endm

/* blend_span from (A0, A1, A2, A3) on. */
.macro SPAN_SSE2 done
    cmp $4, A3
    jb 2f
1:
    COV4 A2
    movdqu (A1), %xmm5
    movdqa %xmm5, %xmm4
    WORDS %xmm4
    pmullw %xmm4, %xmm2
    punpckhbw %xmm5, %xmm5
    psrlw $8, %xmm5
    pmullw %xmm5, %xmm3
    MUL255 %xmm2, %xmm4
    MUL255 %xmm3, %xmm4
    BLEND4
    add $16, A0
    add $16, A1
    add $4, A2
    sub $4, A3
    cmp $4, A3
    jae 1b
2:
    test A3, A3
    jz \done
3:
    COV1 A2
    movd (A1), %xmm5
    WORDS %xmm5
    pmullw %xmm5, %xmm2
    MUL255 %xmm2, %xmm4
    BLEND1
    add $4, A0
    add $4, A1
    add $1, A2
    sub $1, A3
    jnz 3b
.endm

FUNC rgui_blend_solid_sse2
    movd A3d, %xmm5
    WORDS %xmm5
    punpcklqdq %xmm5, %xmm5
    SOLID_SSE2 .Lsolid_sse2_done
.Lsolid_sse2_done:
    ret
ENDFUNC rgui_blend_solid_sse2

FUNC rgui_blend_span_sse2
    SPAN_SSE2 .Lspan_sse2_done
.Lspan_sse2_done:
    ret
ENDFUNC rgui_blend_span_sse2

/* ---- AVX2 ---- */

.macro VMUL255 r, tmp
    vpaddw .Lk_0080(%rip), \r, \r
    vpsrlw $8, \r, \tmp
    vpaddw \tmp, \r, \r
    vpsrlw $8, \r, \r
.endm

.macro VKEEP s, k
    vpshuflw $0xFF, \s, \k
    vpshufhw $0xFF, \k, \k
    vpxor .Lk_00ff(%rip), \k, \k
.endm

/* The coverage of 8 pixels at (\p) as words: ymm2 = pixels 0 1 | 4 5, ymm3 = 2 3 | 6 7. */
.macro VCOV8 p
    vpbroadcastq (\p), %ymm2
    vpshufb .Lk_cov_hi(%rip), %ymm2, %ymm3
    vpshufb .Lk_cov_lo(%rip), %ymm2, %ymm2
.endm

/* 8 destination pixels at (A0) blended with the s words in ymm2 and ymm3. */
.macro VBLEND8
    vmovdqu (A0), %ymm0
    vpunpckhbw %ymm0, %ymm0, %ymm1
    vpsrlw $8, %ymm1, %ymm1
    vpunpcklbw %ymm0, %ymm0, %ymm0
    vpsrlw $8, %ymm0, %ymm0
    VKEEP %ymm2, %ymm4
    vpmullw %ymm4, %ymm0, %ymm0
    VMUL255 %ymm0, %ymm4
    vpaddw %ymm2, %ymm0, %ymm0
    VKEEP %ymm3, %ymm4
    vpmullw %ymm4, %ymm1, %ymm1
    VMUL255 %ymm1, %ymm4
    vpaddw %ymm3, %ymm1, %ymm1
    vpand .Lk_00ff(%rip), %ymm0, %ymm0
    vpand .Lk_00ff(%rip), %ymm1, %ymm1
    vpackuswb %ymm1, %ymm0, %ymm0
    vmovdqu %ymm0, (A0)
.endm

FUNC rgui_blend_solid_avx2
    vmovd A3d, %xmm5
    vpunpcklbw %xmm5, %xmm5, %xmm5
    vpsrlw $8, %xmm5, %xmm5
    vpbroadcastq %xmm5, %ymm5
    cmp $8, A2
    jb 2f
1:
    VCOV8 A1
    vpmullw %ymm5, %ymm2, %ymm2
    VMUL255 %ymm2, %ymm4
    vpmullw %ymm5, %ymm3, %ymm3
    VMUL255 %ymm3, %ymm4
    VBLEND8
    add $32, A0
    add $8, A1
    sub $8, A2
    cmp $8, A2
    jae 1b
2:
    vzeroupper
    SOLID_SSE2 .Lsolid_avx2_done
.Lsolid_avx2_done:
    ret
ENDFUNC rgui_blend_solid_avx2

FUNC rgui_blend_span_avx2
    cmp $8, A3
    jb 2f
1:
    VCOV8 A2
    vmovdqu (A1), %ymm4
    vpunpcklbw %ymm4, %ymm4, %ymm5
    vpsrlw $8, %ymm5, %ymm5
    vpmullw %ymm5, %ymm2, %ymm2
    vpunpckhbw %ymm4, %ymm4, %ymm5
    vpsrlw $8, %ymm5, %ymm5
    vpmullw %ymm5, %ymm3, %ymm3
    VMUL255 %ymm2, %ymm4
    VMUL255 %ymm3, %ymm4
    VBLEND8
    add $32, A0
    add $32, A1
    add $8, A2
    sub $8, A3
    cmp $8, A3
    jae 1b
2:
    vzeroupper
    SPAN_SSE2 .Lspan_avx2_done
.Lspan_avx2_done:
    ret
ENDFUNC rgui_blend_span_avx2

FILE_END
