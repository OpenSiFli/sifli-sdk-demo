/*
 * SPDX-FileCopyrightText: 2024-2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Public interface of the iPhone lockdown Pair module.
 *
 */
#ifndef IPH_PAIR_H
#define IPH_PAIR_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start one round of lockdown Pair (call it once the iPhone is plugged
 *        in and the ipheth interface is ready)
 *
 * Returns immediately; the actual pairing runs in an internal thread. Repeated
 * calls are ignored while a pairing is already in progress or after it has
 * succeeded.
 */
void iph_pair_auto_start(void);

/**
 * @brief Reset pairing state (call on USB disconnect) so the next plug starts
 *        a fresh Pair
 *
 * The phone remembers a pairing record, not an "already opened interface".
 * A fresh handshake is required after every re-plug, otherwise the phone never
 * opens the ipheth data path.
 */
void iph_pair_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* IPH_PAIR_H */
