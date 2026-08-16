/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: MIT
 * Derived from Espressif usb_device_uac 1.3.1.
 */
#pragma once

#include "sdkconfig.h"

#define SPEAK_CHANNEL_NUM CONFIG_UAC_SPEAKER_CHANNEL_NUM
#define MIC_CHANNEL_NUM CONFIG_UAC_MIC_CHANNEL_NUM
#define DEFAULT_SAMPLE_RATE CONFIG_UAC_SAMPLE_RATE
#define SPK_INTERVAL_MS CONFIG_UAC_SPK_INTERVAL_MS
#define MIC_INTERVAL_MS CONFIG_UAC_MIC_INTERVAL_MS

#define VK_UAC_CONTROL_VALUE U32_TO_U8S_LE( \
    AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_MUTE_POS | \
    AUDIO_CTRL_RW << AUDIO_FEATURE_UNIT_CTRL_VOLUME_POS)

#if MIC_CHANNEL_NUM == 1
#define MIC_CTRL VK_UAC_CONTROL_VALUE, VK_UAC_CONTROL_VALUE
#else
#error "VibeBoard supports exactly one UAC microphone channel"
#endif

#define TUD_AUDIO_DESC_MIC_FEATURE_UNIT_N_CHANNEL_LEN \
    (6 + (MIC_CHANNEL_NUM + 1) * 4)
#define TUD_AUDIO_DESC_FEATURE_UNIT_N_CHANNEL(_length, _unitid, _srcid, \
                                               _stridx, ...) \
    _length, TUSB_DESC_CS_INTERFACE, AUDIO_CS_AC_INTERFACE_FEATURE_UNIT, \
    _unitid, _srcid, __VA_ARGS__, _stridx
