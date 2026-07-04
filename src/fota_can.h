#ifndef FIRMWARE_UPDATE_H
#define FIRMWARE_UPDATE_H

// // void firmware_update_task(void *pvParameters);
// void send_block_via_can(int idx);
// void firmware_update();
// void fota_can_run(void);
// Blocking: downloads the .hex boot table and streams it to the F28004x
// bootloader over CAN. Restarts the ESP32 on both success and failure.
void firmware_update(void);

// Non-blocking: spawns the FOTA task which calls firmware_update().
void fota_can_run(void);

#endif // FIRMWARE_UPDATE_H