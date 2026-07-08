/* Single-core ARMv5 atomic stubs.
 * ARM926EJ-S has no LDREX/STREX. Since the EC-100 is uniprocessor,
 * non-atomic implementations are safe (no concurrent access). */
#include <stdint.h>

uint32_t __sync_val_compare_and_swap_4(uint32_t *ptr, uint32_t expected, uint32_t desired) {
    uint32_t old = *ptr;
    if (old == expected) *ptr = desired;
    return old;
}

uint8_t __sync_val_compare_and_swap_1(uint8_t *ptr, uint8_t expected, uint8_t desired) {
    uint8_t old = *ptr;
    if (old == expected) *ptr = desired;
    return old;
}

uint8_t __sync_lock_test_and_set_1(uint8_t *ptr, uint8_t val) {
    uint8_t old = *ptr;
    *ptr = val;
    return old;
}

/* Also provide the 4-byte version just in case */
uint32_t __sync_lock_test_and_set_4(uint32_t *ptr, uint32_t val) {
    uint32_t old = *ptr;
    *ptr = val;
    return old;
}

void __sync_lock_release_4(uint32_t *ptr) { *ptr = 0; }
void __sync_lock_release_1(uint8_t *ptr)  { *ptr = 0; }
