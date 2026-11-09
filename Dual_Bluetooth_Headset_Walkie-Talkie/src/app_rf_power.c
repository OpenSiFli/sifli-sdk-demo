/* SPDX-License-Identifier: Apache-2.0 */
#include <stdint.h>
#include "rtconfig.h"

#if defined(SF32LB57X)

/* DS5702: BR/BLE support 17 dBm; EDR2/EDR3 support 13 dBm.
 * A single entry makes the controller's minimum and maximum indices equal. */
static uint8_t app_rf_max_power(int8_t *power, uint8_t capacity, int8_t dbm)
{
    if (power == 0 || capacity == 0)
    {
        return 0;
    }
    power[0] = dbm;
    return 1;
}

uint8_t bt_rf_get_br_tx_pwr(int8_t *power, uint8_t capacity)
{
    return app_rf_max_power(power, capacity, 17);
}

uint8_t bt_rf_get_edr_tx_pwr(int8_t *power, uint8_t capacity)
{
    return app_rf_max_power(power, capacity, 13);
}

uint8_t bt_rf_get_ble_tx_pwr(int8_t *power, uint8_t capacity)
{
    return app_rf_max_power(power, capacity, 17);
}

#endif
