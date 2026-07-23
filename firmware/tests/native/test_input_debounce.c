#include <assert.h>
#include <stdint.h>

#include "vk_input.h"

int main(void)
{
    vk_input_debounce_t state;
    vk_input_transition_t out[8];
    vk_input_debounce_reset(&state);

    assert(vk_input_debounce_sample(&state, 0x0fU, 0U, out, 8U) == 0U);
    assert(vk_input_debounce_sample(&state, 0x0fU, 5U, out, 8U) == 0U);
    assert(vk_input_debounce_sample(&state, 0U, 10U, out, 8U) == 0U);
    assert(vk_input_debounce_sample(&state, 0U, 15U, out, 8U) == 0U);

    assert(vk_input_debounce_sample(&state, 0x0fU, UINT32_MAX - 2U, out, 8U) == 0U);
    size_t count = vk_input_debounce_sample(&state, 0x0fU, UINT32_MAX, out, 8U);
    assert(count == 4U);
    for (size_t i = 0; i < 4U; ++i) {
        assert(out[i].kind == VK_INPUT_TRANSITION_DOWN);
        assert(out[i].key == (vk_input_key_t)i);
    }
    assert(vk_input_debounce_sample(&state, 0U, 2U, out, 8U) == 0U);
    count = vk_input_debounce_sample(&state, 0U, 4U, out, 8U);
    assert(count == 8U);
    for (size_t i = 0; i < 4U; ++i) {
        assert(out[i * 2U].kind == VK_INPUT_TRANSITION_UP);
        assert(out[i * 2U + 1U].kind == VK_INPUT_TRANSITION_CLICK);
        assert(out[i * 2U].key == (vk_input_key_t)i);
        assert(out[i * 2U].duration_ms == 5U);
    }

    vk_input_debounce_reset(&state);
    assert(vk_input_debounce_sample(&state, 1U, 0U, out, 8U) == 0U);
    assert(vk_input_debounce_sample(&state, 0U, 5U, out, 8U) == 0U);
    assert(vk_input_debounce_sample(&state, 1U, 10U, out, 8U) == 0U);
    return 0;
}
