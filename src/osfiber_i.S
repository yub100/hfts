#define HFTS_REG_r0 0x00
#define HFTS_REG_r1 0x08
#define HFTS_REG_r16 0x10
#define HFTS_REG_r17 0x18
#define HFTS_REG_r18 0x20
#define HFTS_REG_r19 0x28
#define HFTS_REG_r20 0x30
#define HFTS_REG_r21 0x38
#define HFTS_REG_r22 0x40
#define HFTS_REG_r23 0x48
#define HFTS_REG_r24 0x50
#define HFTS_REG_r25 0x58
#define HFTS_REG_r26 0x60
#define HFTS_REG_r27 0x68
#define HFTS_REG_r28 0x70
#define HFTS_REG_r29 0x78
#define HFTS_REG_v8 0x80
#define HFTS_REG_v9 0x88
#define HFTS_REG_v10 0x90
#define HFTS_REG_v11 0x98
#define HFTS_REG_v12 0xa0
#define HFTS_REG_v13 0xa8
#define HFTS_REG_v14 0xb0
#define HFTS_REG_v15 0xb8
#define HFTS_REG_SP 0xc0
#define HFTS_REG_LR 0xc8

#ifndef HFTS_ASM_SYMBOL
#if defined(__APPLE__)
#define HFTS_ASM_SYMBOL(symbol) _##symbol
#else
#define HFTS_ASM_SYMBOL(symbol) symbol
#endif
#endif

#ifndef PAUTH_SIGN_SP
#define PAUTH_SIGN_SP
#endif

#ifndef PAUTH_AUTH_SP
#define PAUTH_AUTH_SP
#endif

// void hfts_fiber_swap(hfts_fiber_context* from, const hfts_fiber_context* to)
// x0: from
// x1: to
.text
.global HFTS_ASM_SYMBOL(hfts_fiber_swap)
.align 4
HFTS_ASM_SYMBOL(hfts_fiber_swap):
    
    PAUTH_SIGN_SP

    // Store x0's registers
    str x16, [x0, #HFTS_REG_r16]
    str x17, [x0, #HFTS_REG_r17]
    str x18, [x0, #HFTS_REG_r18]

    // Store from's registers
    str x19, [x0, #HFTS_REG_r19]
    str x20, [x0, #HFTS_REG_r20]
    str x21, [x0, #HFTS_REG_r21]
    str x22, [x0, #HFTS_REG_r22]
    str x23, [x0, #HFTS_REG_r23]
    str x24, [x0, #HFTS_REG_r24]
    str x25, [x0, #HFTS_REG_r25]
    str x26, [x0, #HFTS_REG_r26]
    str x27, [x0, #HFTS_REG_r27]
    str x28, [x0, #HFTS_REG_r28]
    str x29, [x0, #HFTS_REG_r29]

    // store float
    str d8,  [x0, #HFTS_REG_v8]
    str d9,  [x0, #HFTS_REG_v9]
    str d10, [x0, #HFTS_REG_v10]
    str d11, [x0, #HFTS_REG_v11]
    str d12, [x0, #HFTS_REG_v12]
    str d13, [x0, #HFTS_REG_v13]
    str d14, [x0, #HFTS_REG_v14]
    str d15, [x0, #HFTS_REG_v15]

    mov x2,  sp
    str x2,  [x0, #HFTS_REG_SP]

    mov x7,  x1

    // load x1's registers
    ldr x16, [x7, #HFTS_REG_r16]
    ldr x17, [x7, #HFTS_REG_r17]
    ldr x18, [x7, #HFTS_REG_r18]

    ldr x19, [x7, #HFTS_REG_r19]
    ldr x20, [x7, #HFTS_REG_r20]
    ldr x21, [x7, #HFTS_REG_r21]
    ldr x22, [x7, #HFTS_REG_r22]
    ldr x23, [x7, #HFTS_REG_r23]
    ldr x24, [x7, #HFTS_REG_r24]
    ldr x25, [x7, #HFTS_REG_r25]
    ldr x26, [x7, #HFTS_REG_r26]
    ldr x27, [x7, #HFTS_REG_r27]
    ldr x28, [x7, #HFTS_REG_r28]
    ldr x29, [x7, #HFTS_REG_r29]

    ldr d8,  [x7, #HFTS_REG_v8]
    ldr d9,  [x7, #HFTS_REG_v9]
    ldr d10, [x7, #HFTS_REG_v10]
    ldr d11, [x7, #HFTS_REG_v11]
    ldr d12, [x7, #HFTS_REG_v12]
    ldr d13, [x7, #HFTS_REG_v13]
    ldr d14, [x7, #HFTS_REG_v14]
    ldr d15, [x7, #HFTS_REG_v15]

    ldr x0,  [x7, #HFTS_REG_r0]
    ldr x1,  [x7, #HFTS_REG_r1]

    ldr x30, [x7, #HFTS_REG_LR]
    ldr x2,  [x7, #HFTS_REG_SP]
    mov sp, x2

    PAUTH_AUTH_SP

    ret