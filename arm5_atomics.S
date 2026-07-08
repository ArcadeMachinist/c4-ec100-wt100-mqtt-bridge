/* ARMv5 atomic stubs — single-core unsafe implementations */
.arch armv5te
.text

.global __sync_val_compare_and_swap_4
.type __sync_val_compare_and_swap_4, %function
__sync_val_compare_and_swap_4:   /* (uint32_t *ptr, uint32_t exp, uint32_t des) -> old */
    ldr  r3, [r0]
    cmp  r3, r1
    bne  .Lcas4_done
    str  r2, [r0]
.Lcas4_done:
    mov  r0, r3
    bx   lr

.global __sync_val_compare_and_swap_1
.type __sync_val_compare_and_swap_1, %function
__sync_val_compare_and_swap_1:   /* (uint8_t *ptr, uint8_t exp, uint8_t des) -> old */
    ldrb r3, [r0]
    cmp  r3, r1
    bne  .Lcas1_done
    strb r2, [r0]
.Lcas1_done:
    mov  r0, r3
    bx   lr

.global __sync_lock_test_and_set_1
.type __sync_lock_test_and_set_1, %function
__sync_lock_test_and_set_1:      /* (uint8_t *ptr, uint8_t val) -> old */
    ldrb r2, [r0]
    strb r1, [r0]
    mov  r0, r2
    bx   lr

.global __sync_lock_test_and_set_4
.type __sync_lock_test_and_set_4, %function
__sync_lock_test_and_set_4:      /* (uint32_t *ptr, uint32_t val) -> old */
    ldr  r2, [r0]
    str  r1, [r0]
    mov  r0, r2
    bx   lr

.global __sync_lock_release_4
.type __sync_lock_release_4, %function
__sync_lock_release_4:           /* (uint32_t *ptr) -> void */
    mov  r1, #0
    str  r1, [r0]
    bx   lr

.global __sync_lock_release_1
.type __sync_lock_release_1, %function
__sync_lock_release_1:           /* (uint8_t *ptr) -> void */
    mov  r1, #0
    strb r1, [r0]
    bx   lr
