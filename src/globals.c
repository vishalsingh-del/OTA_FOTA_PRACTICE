#include "globals.h"

GlobalState gstate = {
    .fw_url = "",
    .can_normal_processing = true,
    .fw_update_start = false,
    .fw_update_in_progress = false,
    .total_blocks = 0,
    .entry_point = 0,
    .blocks = {{0}}};
