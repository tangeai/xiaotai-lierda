/*
 * SPDX-FileCopyrightText: 2025 Lierda Technology Co., Ltd.
 * SPDX-FileCopyrightText: 2026 Shenzhen Tange Intelligent Technology Co., Ltd. <https://tange.ai>
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file key_input.h
 * @brief Keypad demo task declaration
 *
 * Derived from Lierda's keypad demo declaration and modified for the TiRTC UI.
 * @date 2025-01-01
 * @version 1.0
 */

#ifndef TIRTC_APP_KEY_INPUT_H
#define TIRTC_APP_KEY_INPUT_H

#include <stdbool.h>

/**
 * @brief Keypad demo task entry
 * @param[in] argv  Task argument (unused)
 */
void demo_key_task(void *argv);

#ifdef HWDEMO_GROUP_ROOM_EN
/* Read only from task context. False also covers unavailable keypad hardware. */
bool demo_key_back_is_pressed(void);
#endif

#endif /* TIRTC_APP_KEY_INPUT_H */
