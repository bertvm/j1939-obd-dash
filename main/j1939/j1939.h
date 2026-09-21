#pragma once

#include "twai_port.h"
#include <stdbool.h>
#include <stdint.h>

uint32_t j1939_pgn_from_id(uint32_t id);
uint8_t j1939_sa_from_id(uint32_t id);
uint8_t j1939_da_from_id(uint32_t id);
uint32_t j1939_make_id(uint8_t prio, uint32_t pgn, uint8_t da, uint8_t sa);
void j1939_on_frame(const can_frame_t *msg);
void j1939_init(void);
