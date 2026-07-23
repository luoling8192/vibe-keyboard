#include "vk_input.h"

#include <string.h>

void vk_input_debounce_reset(vk_input_debounce_t *state)
{
    if (state != NULL) memset(state, 0, sizeof(*state));
}

static bool append(vk_input_transition_t *output, size_t capacity, size_t *count,
                   vk_input_transition_kind_t kind, size_t key, uint32_t duration)
{
    if (*count >= capacity) return false;
    output[*count] = (vk_input_transition_t){
        .kind = kind, .key = (vk_input_key_t)key, .duration_ms = duration,
    };
    ++*count;
    return true;
}

size_t vk_input_debounce_sample(vk_input_debounce_t *state,
                                uint8_t pressed_mask,
                                uint32_t now_ms,
                                vk_input_transition_t *output,
                                size_t capacity)
{
    if (state == NULL || (output == NULL && capacity != 0U)) return 0U;
    size_t count = 0U;
    for (size_t index = 0; index < 4U; ++index) {
        vk_input_debounce_key_t *key = &state->keys[index];
        bool pressed = (pressed_mask & (uint8_t)(1U << index)) != 0U;
        if (pressed != key->candidate_pressed) {
            key->candidate_pressed = pressed;
            key->candidate_count = 1U;
        } else if (key->candidate_count < 2U) {
            ++key->candidate_count;
        }
        if (key->candidate_count < 2U || pressed == key->stable_pressed) continue;
        key->stable_pressed = pressed;
        if (!pressed) {
            if (!key->armed) {
                key->armed = true;
                continue;
            }
            uint32_t duration = now_ms - key->down_ms;
            if (!append(output, capacity, &count, VK_INPUT_TRANSITION_UP, index, duration) ||
                !append(output, capacity, &count, VK_INPUT_TRANSITION_CLICK, index, duration)) {
                return capacity + 1U;
            }
        } else if (key->armed) {
            key->down_ms = now_ms;
            if (!append(output, capacity, &count, VK_INPUT_TRANSITION_DOWN, index, 0U)) {
                return capacity + 1U;
            }
        }
    }
    return count;
}
