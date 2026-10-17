@ RDPServer: NEON versions of the pixel work (the module only; measured
@ against the plain ARM ways at start-up, 0.2-test28).
@
@ The NEON registers belong to whichever program has a VFP context
@ active: these may only be called between plat_neon_enter and
@ plat_neon_leave (src/mod/vfpctx.c), which switch to the module's own
@ context. They use D0-D7 and D16-D23 only.
@
@ void rdps_neon_tbgr_to_32 (uint32_t *dst, const uint8_t *src, size_t pixels)
@   RISC OS 32 bpp &TTBBGGRR (bytes R, G, B, T) to the client's 32 bpp
@   &00RRGGBB (bytes B, G, R, 0), as pixels.c's pack does.
@ void rdps_neon_trgb_to_32 (uint32_t *dst, const uint8_t *src, size_t pixels)
@   RISC OS 32 bpp &TTRRGGBB (bytes B, G, R, T): T cleared only.
@ void rdps_neon_tbgr_to_565 (uint16_t *dst, const uint8_t *src, size_t pixels)
@ void rdps_neon_trgb_to_565 (uint16_t *dst, const uint8_t *src, size_t pixels)
@   the same to RGB 5:6:5 (red in the top bits), as pack (.., 16) does.
@   pixels: a multiple of 8; 0 does nothing. dst halfword aligned.
@ void rdps_neon_copy (void *dst, const void *src, size_t bytes)
@   bytes: a multiple of 64; 0 does nothing.
@ Ordinary AAPCS calls: R0-R3 and R12 corrupted, R4-R11 preserved.
@
@ Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE).

        .syntax unified
        .arm
        .fpu    neon
        .text

        .global rdps_neon_tbgr_to_32
        .type   rdps_neon_tbgr_to_32, %function
rdps_neon_tbgr_to_32:
        lsrs    r2, r2, #3
        bxeq    lr
        vmov.i8 d7, #0
1:      vld4.8  {d0, d1, d2, d3}, [r1]!   @ d0 R, d1 G, d2 B, d3 T
        vmov    d4, d2                    @ B
        vmov    d5, d1                    @ G
        vmov    d6, d0                    @ R
        vst4.8  {d4, d5, d6, d7}, [r0]!   @ B, G, R, 0
        subs    r2, r2, #1
        bne     1b
        bx      lr
        .size   rdps_neon_tbgr_to_32, . - rdps_neon_tbgr_to_32

        .global rdps_neon_trgb_to_32
        .type   rdps_neon_trgb_to_32, %function
rdps_neon_trgb_to_32:
        lsrs    r2, r2, #3
        bxeq    lr
1:      vld4.8  {d0, d1, d2, d3}, [r1]!   @ d0 B, d1 G, d2 R, d3 T
        vmov.i8 d3, #0
        vst4.8  {d0, d1, d2, d3}, [r0]!
        subs    r2, r2, #1
        bne     1b
        bx      lr
        .size   rdps_neon_trgb_to_32, . - rdps_neon_trgb_to_32

@ 5:6:5 from r, g, b in d6, d5, d4: red's top 5 bits to 15-11, green's
@ top 6 to 10-5, blue's top 5 to 4-0.
        .macro  pack565
        vshll.u8 q8, d6, #8               @ r << 8
        vshll.u8 q9, d5, #8               @ g << 8
        vshll.u8 q10, d4, #8              @ b << 8
        vsri.16 q8, q9, #5
        vsri.16 q8, q10, #11
        vst1.16 {d16, d17}, [r0]!
        .endm

        .global rdps_neon_tbgr_to_565
        .type   rdps_neon_tbgr_to_565, %function
rdps_neon_tbgr_to_565:
        lsrs    r2, r2, #3
        bxeq    lr
1:      vld4.8  {d0, d1, d2, d3}, [r1]!   @ R, G, B, T
        vmov    d6, d0
        vmov    d5, d1
        vmov    d4, d2
        pack565
        subs    r2, r2, #1
        bne     1b
        bx      lr
        .size   rdps_neon_tbgr_to_565, . - rdps_neon_tbgr_to_565

        .global rdps_neon_trgb_to_565
        .type   rdps_neon_trgb_to_565, %function
rdps_neon_trgb_to_565:
        lsrs    r2, r2, #3
        bxeq    lr
1:      vld4.8  {d0, d1, d2, d3}, [r1]!   @ B, G, R, T
        vmov    d6, d2
        vmov    d5, d1
        vmov    d4, d0
        pack565
        subs    r2, r2, #1
        bne     1b
        bx      lr
        .size   rdps_neon_trgb_to_565, . - rdps_neon_trgb_to_565

        .global rdps_neon_copy
        .type   rdps_neon_copy, %function
rdps_neon_copy:
        lsrs    r2, r2, #6
        bxeq    lr
1:      vld1.8  {d0, d1, d2, d3}, [r1]!
        vld1.8  {d4, d5, d6, d7}, [r1]!
        vst1.8  {d0, d1, d2, d3}, [r0]!
        vst1.8  {d4, d5, d6, d7}, [r0]!
        subs    r2, r2, #1
        bne     1b
        bx      lr
        .size   rdps_neon_copy, . - rdps_neon_copy

        .section .note.GNU-stack, "", %progbits
