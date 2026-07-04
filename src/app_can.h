#ifndef CAN_H
#define CAN_H

#define CAN_RX_GPIO GPIO_NUM_15
#define CAN_TX_GPIO GPIO_NUM_16
#define CAN_BAUD_RATE TWAI_TIMING_CONFIG_250KBITS
// // #define CAN_BAUD_RATE TWAI_TIMING_CONFIG_250KBITS
void can_app_start(void);

#endif // CAN_H