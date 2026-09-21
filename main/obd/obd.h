#pragma once

#include "twai_port.h"

void obd_init(void);
void obd_on_frame(const can_frame_t *msg);
void obd_poll(void);
