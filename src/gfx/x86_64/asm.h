/* SPDX-License-Identifier: Apache-2.0 */
/* Shared definitions of the x86-64 kernels (accumulate.S, blend.S, jpeg.S), for ELF (System V
 * ABI) and COFF (MinGW, Win64 ABI). Included by C-preprocessed GNU assembly only.
 *
 * FUNC name / ENDFUNC name open and close a global function (endbr64 at the entry with
 * -fcf-protection). Kernels are leaf functions without a stack frame (Win64 would need SEH
 * unwind data for one) and use only the registers named here and xmm/ymm 0-5, which are
 * volatile under both ABIs (Win64 keeps xmm6-15 for the caller). A0..A3 are the first four
 * integer arguments, T0..T2 scratch registers; the d and b suffixes name their 32-bit and 8-bit
 * parts. AVX2 functions end with vzeroupper.
 *
 * RODATA opens the read-only data section of the constants (use .balign 32: SSE memory operands
 * need 16-byte alignment). FILE_END closes every .S file: the non-executable stack note on ELF
 * and, with -fcf-protection (__CET__), the GNU property note that marks the object IBT and SHSTK
 * compatible, so the linked executable keeps both. */
#ifndef RGUI_GFX_X86_64_ASM_H
#define RGUI_GFX_X86_64_ASM_H

#if defined(_WIN32)
#define A0 %rcx
#define A1 %rdx
#define A2 %r8
#define A3 %r9
#define A0d %ecx
#define A1d %edx
#define A2d %r8d
#define A3d %r9d
#else
#define A0 %rdi
#define A1 %rsi
#define A2 %rdx
#define A3 %rcx
#define A0d %edi
#define A1d %esi
#define A2d %edx
#define A3d %ecx
#endif
#define T0 %rax
#define T1 %r10
#define T2 %r11
#define T0d %eax
#define T1d %r10d
#define T2d %r11d
#define T0b %al
#define T1b %r10b
#define T2b %r11b

#if !defined(__ELF__) && !defined(_WIN32)
#error "unsupported object format"
#endif

.macro FUNC name
    .text
    .p2align 5
    .globl \name
#if defined(__ELF__)
    .type \name, @function
#else
    .def \name; .scl 2; .type 32; .endef
#endif
\name:
#if defined(__CET__)
    endbr64
#endif
.endm

.macro ENDFUNC name
#if defined(__ELF__)
    .size \name, . - \name
#endif
.endm

.macro RODATA
#if defined(__ELF__)
    .section .rodata
#else
    .section .rdata, "dr"
#endif
.endm

.macro FILE_END
#if defined(__ELF__)
    .section .note.GNU-stack, "", @progbits
#if defined(__CET__)
    /* NT_GNU_PROPERTY_TYPE_0: GNU_PROPERTY_X86_FEATURE_1_AND = IBT | SHSTK. */
    .section .note.gnu.property, "a"
    .p2align 3
    .long 4
    .long 16
    .long 5
    .asciz "GNU"
    .long 0xc0000002
    .long 4
    .long 3
    .p2align 3
#endif
#endif
.endm

#endif
