#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define MAX_FW_BLOCKS 64

    typedef struct
    {
        uint32_t address;
        uint32_t length;
        uint8_t data[8];
    } fw_block_t;

    typedef struct
    {
        char fw_url[2048];
        bool can_normal_processing;
        bool fw_update_start;
        bool fw_update_in_progress;
        int total_blocks;
        uint32_t entry_point;
        fw_block_t blocks[MAX_FW_BLOCKS];
    } GlobalState;

    extern GlobalState gstate;

#ifdef __cplusplus
}
#endif
