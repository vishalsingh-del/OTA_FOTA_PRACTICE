#ifndef OTA_H
#define OTA_H

typedef struct
{
    char version_server[32];
    char url[256];
} ota_info_t;

void ota_task(void *pv);

#endif