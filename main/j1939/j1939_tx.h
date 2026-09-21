#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

void j1939_tx_init(void);
void j1939_tx_poll(void);
bool j1939_tx_claimed(void);
esp_err_t j1939_tx_claim(void);
void j1939_tsc1_hold(bool hold, uint16_t rpm);
void j1939_regen_force(bool on);
void j1939_regen_inhibit(bool on);
void j1939_regen_cancel(void);
bool j1939_tsc1_active(void);
bool j1939_regen_active(void);
const char *j1939_tx_gate_reason(void);
