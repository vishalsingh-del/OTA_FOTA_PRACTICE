#include "globals.h"
#include "cert.h"
#include "fota_can.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "driver/twai.h"
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define TAG "FW_UPDATE"
#include <stdbool.h>
#include "esp_crt_bundle.h"
#include "esp_partition.h"
#define CHUNK_SIZE 2048 // 2KB or adjust as needed
#define FIRMWARE_CHUNK_SIZE 4096
#define MAX_LINE_LENGTH 260
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"

#define OTA_BUFFER_SIZE 4096 // Optimal chunk size for ESP32
#define FOTA_TASK_STACK_SIZE 16384

// ---------------------------------------------------------------------------
// CAN IDs and handshake keys
// ---------------------------------------------------------------------------
#define BOOT_ID 0x12EF34AB
#define RESP_ID 0x12EF34AA
#define MAX_RETRIES 3
#define BLOCK_ACK_TIMEOUT_MS 20000
#define CAN_TEST_ID 0x123
#define DEFAULT_FIRMWARE_URL "https://192.168.31.237:8000/Tetra_Tower_RNLTonhe%20%281%29.hex"

// TI ROM/bootloader "8-bit wide" KeyValue (see F28004x TRM, "ROM Code and
// Peripheral Booting", Table 4-28: LSB = 0xAA, MSB = 0x08 for 8-bit memory width).
#define BOOT_TABLE_KEY_LSB 0xAA
#define BOOT_TABLE_KEY_MSB 0x08
#define BOOT_TABLE_REG_INIT_WORDS 8 // words 2-9 of the table (register init / reserved)

char *get_line_from_partition(void);
void reset_line_reader(void);

// ---------------------------------------------------------------------------
// Intel-HEX line reader backed by the flash partition (streams the .hex file
// a chunk at a time so we never need the whole firmware image in RAM).
// ---------------------------------------------------------------------------
typedef struct
{
    const esp_partition_t *partition;
    size_t offset;
    char buffer[CHUNK_SIZE];
    size_t buffer_len;
    size_t buffer_pos;
    int current_line_num; // Tracks which line to return next
} LineReaderState;

static LineReaderState reader_state = {
    .partition = NULL,
    .offset = 0,
    .buffer_len = 0,
    .buffer_pos = 0,
    .current_line_num = 1 // Start at line 1
};

void reset_line_reader(void)
{
    reader_state.offset = 0;
    reader_state.buffer_len = 0;
    reader_state.buffer_pos = 0;
    reader_state.current_line_num = 1;
    reader_state.partition = NULL;
}
static uint8_t KEY[] = {0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A};          // Boot id (i have to send)
static uint8_t RESPONSE_KEY[] = {0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5}; // sent by MCU when ready
static uint8_t MCU_ACK_KEY[] = {0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55};  // per-block complete ACK

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    return ESP_OK;
}

bool wait_for_can_response(const uint8_t *expected_data, uint32_t timeout_ms);
esp_err_t can_send_message_for_bootloading(uint32_t identifier, uint8_t *data, uint8_t data_length);
esp_err_t can_send_for_bootloading(uint32_t identifier, uint8_t *data, uint8_t data_length);
esp_err_t can_send_for_bootloading_with_ack(uint32_t identifier, uint8_t *data, uint8_t data_length);
esp_err_t can_receive_message(uint32_t *id, uint8_t *data, TickType_t timeout_ticks);
void analyze_memory(void);
void firmware_update(void);
void convert_can_frame_to_little_endian(uint8_t *can_frame);

// Parses one Intel HEX line into its raw encoded bytes.
// bytes[0]        = record byte count (data length)
// bytes[1],[2]    = 16-bit load address (big-endian, per HEX spec)
// bytes[3]        = record type
// bytes[4..]      = data payload (bytes[0] bytes long)
// bytes[4+len]    = checksum
static bool parse_hex_line(const char *line, uint8_t *bytes, int *byte_count)
{
    if (line[0] != ':')
        return false;

    int len = strlen(line);
    if (len < 11)
        return false;

    *byte_count = (len - 1) / 2;
    if (*byte_count > 256)
        return false;

    for (int i = 0; i < *byte_count; i++)
    {
        if (sscanf(&line[1 + i * 2], "%2hhx", &bytes[i]) != 1)
        {
            return false;
        }
    }

    return true;
}

static bool download_and_parse_firmware(void);
static bool send_boot_table_over_can(void);

// ---------------------------------------------------------------------------
// HTTP download of the .hex boot table into the flash "storage"/"mydownload"
// partition, then bootloader erase-wait handshake, then hand off to the
// boot-table CAN sender.
// ---------------------------------------------------------------------------
static bool download_and_parse_firmware(void)
{
    if (strlen(gstate.fw_url) == 0)
    {
        strncpy(gstate.fw_url, DEFAULT_FIRMWARE_URL, sizeof(gstate.fw_url) - 1);
        gstate.fw_url[sizeof(gstate.fw_url) - 1] = '\0';
    }

    ESP_LOGI(TAG, "Firmware URL: %s", gstate.fw_url);

    // ---------------- PARTITION ----------------------------------

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "mydownload");

    if (!partition)
    {
        partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    }

    if (!partition)
    {
        ESP_LOGE(TAG, "No writable data partition found. Add a 'mydownload' partition in partitions.csv.");
        return false;
    }

    ESP_LOGI(TAG, "Using partition '%s' (%u bytes)", partition->label, (unsigned int)partition->size);

    esp_err_t perr = esp_partition_erase_range(partition, 0, partition->size);
    if (perr != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to erase partition: %s", esp_err_to_name(perr));
        return false;
    }
    ESP_LOGI(TAG, "Partition erased and ready.");

    bool is_https = strncmp(gstate.fw_url, "https://", 8) == 0;

    esp_http_client_config_t config = {
        .url = gstate.fw_url,
        .event_handler = http_event_handler,
        .timeout_ms = 60000,
        .buffer_size = 16384,
        .buffer_size_tx = 1024,
        .transport_type = is_https ? HTTP_TRANSPORT_OVER_SSL : HTTP_TRANSPORT_OVER_TCP,
        .cert_pem = is_https ? cert_pem : NULL,
        .skip_cert_common_name_check = is_https,
        .disable_auto_redirect = false,
        .max_redirection_count = 5};

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return false;
    }

    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Cache-Control", "no-cache");

    esp_http_client_set_method(client, HTTP_METHOD_HEAD);
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "HEAD request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int status = esp_http_client_get_status_code(client);
    int content_length = esp_http_client_get_content_length(client);
    ESP_LOGI(TAG, "HTTP Status: %d, Content-Length: %d", status, content_length);

    if (status != 200 || content_length <= 0)
    {
        ESP_LOGE(TAG, "Invalid status or content length");
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_GET);
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "GET request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    ESP_LOGI("MEMORY", "Free heap: %" PRIu32 " bytes", (uint32_t)esp_get_free_heap_size());
    ESP_LOGI("MEMORY", "Minimum free heap: %" PRIu32 " bytes", (uint32_t)esp_get_minimum_free_heap_size());
    ESP_LOGI("MEMORY", "Current task watermark: %d bytes", uxTaskGetStackHighWaterMark(NULL));

    uint8_t *chunk_buffer = malloc(CHUNK_SIZE);
    if (!chunk_buffer)
    {
        ESP_LOGE(TAG, "Failed to allocate chunk buffer");
        esp_http_client_cleanup(client);
        return false;
    }

    int total_read = 0;
    int partition_offset = 0;

    while (1)
    {
        int read_len = esp_http_client_read(client, (char *)chunk_buffer, CHUNK_SIZE);
        if (read_len < 0)
        {
            ESP_LOGE(TAG, "Error reading data");
            free(chunk_buffer);
            esp_http_client_cleanup(client);
            return false;
        }
        else if (read_len == 0)
        {
            break; // finished
        }

        esp_err_t werr = esp_partition_write(partition, partition_offset, chunk_buffer, read_len);
        if (werr != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to write chunk to partition: %s", esp_err_to_name(werr));
            free(chunk_buffer);
            esp_http_client_cleanup(client);
            return false;
        }

        partition_offset += read_len;
        total_read += read_len;

        ESP_LOGI(TAG, "Written %d bytes to partition (Total downloaded: %d)", read_len, total_read);
        vTaskDelay(1);
    }

    free(chunk_buffer);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Firmware downloaded and written to partition (%d bytes)", total_read);

    // ---------------- Bootloader erase-wait handshake ----------------------
    ESP_LOGI(TAG, "=== Starting firmware update ===");
    can_send_message_for_bootloading(BOOT_ID, KEY, 8);
    uint8_t boot_start[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    wait_for_can_response(boot_start, 20000);
    ESP_LOGI(TAG, "Waiting for bootloader status...");
    bool got_erase_success = false;
    uint32_t start_tick = xTaskGetTickCount();
    uint32_t timeout_ticks = pdMS_TO_TICKS(20000);

    ESP_LOGI(TAG, "Waiting for enter  to bootloader mode...");
    uint8_t boot_mode_data[8] = {4, 0, 0, 0, 0, 0, 0, 0};
    if (!wait_for_can_response(boot_mode_data, 20000))
    {
        ESP_LOGE(TAG, "MCU did not enter bootloader mode \n");

        // i have to send last two messages for pooping out from boot loader and jump to application layer

        esp_http_client_cleanup(client);
        return false;
    }

    printf("MCU is in boot loader \n");

    while ((xTaskGetTickCount() - start_tick) < timeout_ticks)
    {
        uint32_t received_id;
        uint8_t received_data[8] = {0};
        esp_err_t ret = can_receive_message(&received_id, received_data, pdMS_TO_TICKS(100));
        if (ret == ESP_OK)
        {
            // if (received_id == RESP_ID)
            // {
            if (received_data[0] == 1)
            {
                ESP_LOGI(TAG, "Bootloader init response received");
                continue;
            }
            else if (received_data[0] == 2)
            {
                ESP_LOGE(TAG, "Bootloader reports no firmware available");
                return false;
            }
            else if (received_data[0] == 3)
            {
                ESP_LOGI(TAG, "Bootloader flash API initialized");
                continue;
            }
            else if (received_data[0] == 4)
            {
                ESP_LOGI(TAG, "Bootloader erase complete");
                continue;
            }
            else if (memcmp(received_data, RESPONSE_KEY, 8) == 0)
            {
                ESP_LOGI(TAG, "Bootloader ready ACK received");
                got_erase_success = true;
                break;
            }
            else if (received_data[0] == 8)
            {
                ESP_LOGE(TAG, "Bootloader aborted due to no valid CAN messages");
                return false;
            }
            else
            {
                ESP_LOGW(TAG, "Bootloader returned unexpected status: %02X", received_data[0]);
            }
            // }
        }
        else if (ret != ESP_ERR_TIMEOUT)
        {
            ESP_LOGE(TAG, "CAN receive error: %s", esp_err_to_name(ret));
            return false;
        }
    }

    if (!got_erase_success)
    {
        ESP_LOGE(TAG, "Did not receive bootloader erase-complete response");
        return false;
    }

    ESP_LOGI(TAG, "MCU is in bootloader and ready to receive frames");
    if (!wait_for_can_response(RESPONSE_KEY, 15000))
    {
        ESP_LOGE(TAG, "Bootloader did not send start ACK");
        return false;
    }

    // ---------------- Stream the boot table over CAN -----------------------
    reset_line_reader();
    (void)partition; // partition is picked up again lazily by get_line_from_partition()
    return send_boot_table_over_can();
}

// ---------------------------------------------------------------------------
// Boot-table byte stream: pulls bytes out of the Intel HEX file in order,
// transparently skipping non-data records and honouring Extended Linear
// Address (type 0x04) records so the byte offsets always line up with
// Table 4-28 regardless of how the linker split records across 64KB pages.
// ---------------------------------------------------------------------------
typedef struct
{
    uint8_t linebuf[256];
    int linebuf_len;
    int linebuf_pos;
    bool eof;
    uint32_t ext_linear_addr; // informational: current upper 16 bits of address
} hex_stream_t;

static void hex_stream_init(hex_stream_t *hs)
{
    hs->linebuf_len = 0;
    hs->linebuf_pos = 0;
    hs->eof = false;
    hs->ext_linear_addr = 0;
}

static bool hex_stream_fill(hex_stream_t *hs)
{
    while (!hs->eof)
    {
        char *line = get_line_from_partition();
        if (!line)
        {
            hs->eof = true;
            return false;
        }

        uint8_t rec[256];
        int rec_len;
        bool ok = parse_hex_line(line, rec, &rec_len);
        free(line);

        if (!ok || rec_len < 5)
        {
            continue; // malformed / blank line, skip
        }

        int byte_count = rec[0];
        uint8_t rec_type = rec[3];

        if (rec_type == 0x01)
        {
            // End Of File record
            hs->eof = true;
            return false;
        }
        else if (rec_type == 0x04)
        {
            // Extended Linear Address - updates the upper 16 bits of the
            // address; carries no boot-table payload bytes of its own.
            if (byte_count >= 2)
            {
                hs->ext_linear_addr = ((uint32_t)rec[4] << 8) | rec[5];
            }
            continue;
        }
        else if (rec_type == 0x00)
        {
            if (byte_count == 0)
            {
                continue;
            }
            if (byte_count > sizeof(hs->linebuf))
            {
                ESP_LOGE(TAG, "HEX data record too long (%u bytes)", byte_count);
                continue;
            }
            memcpy(hs->linebuf, &rec[4], byte_count);
            hs->linebuf_len = byte_count;
            hs->linebuf_pos = 0;
            return true;
        }
        else
        {
            // Extended Segment Address (02), Start Segment Address (03),
            // Start Linear Address (05), or anything else: not part of the
            // boot-table payload.
            continue;
        }
    }
    return false;
}

static bool hex_stream_next_byte(hex_stream_t *hs, uint8_t *out)
{
    if (hs->linebuf_pos >= hs->linebuf_len)
    {
        if (!hex_stream_fill(hs))
        {
            return false;
        }
    }
    *out = hs->linebuf[hs->linebuf_pos++];
    return true;
}

// Table 4-28 words are encoded LSB-first (first byte), then MSB (second byte).
static bool hex_stream_next_word(hex_stream_t *hs, uint16_t *out)
{
    uint8_t lo, hi;
    if (!hex_stream_next_byte(hs, &lo))
        return false;
    if (!hex_stream_next_byte(hs, &hi))
        return false;
    *out = ((uint16_t)hi << 8) | lo;
    return true;
}

// ---------------------------------------------------------------------------
// CAN frame builder: accumulates boot-table bytes and flushes them as 8-byte
// TWAI frames. Each logical section (header, and each block) is flushed on
// its own frame boundary; a short trailing frame is padded with 0xFF.
// ---------------------------------------------------------------------------
typedef struct
{
    uint8_t buf[8];
    int len;
} can_frame_builder_t;

static void frame_reset(can_frame_builder_t *f)
{
    memset(f->buf, 0xFF, sizeof(f->buf));
    f->len = 0;
}

static bool frame_push_byte(can_frame_builder_t *f, uint8_t b)
{
    f->buf[f->len++] = b;
    if (f->len == 8)
    {
        uint8_t frame[8];
        memcpy(frame, f->buf, 8);
        convert_can_frame_to_little_endian(frame);
        esp_err_t err = can_send_for_bootloading_with_ack(BOOT_ID, frame, 8);
        frame_reset(f);
        return err == ESP_OK;
    }
    return true;
}

static bool frame_push_word(can_frame_builder_t *f, uint16_t w)
{
    // Preserve the table's LSB-first, MSB-second byte order for each word.
    return frame_push_byte(f, w & 0xFF) && frame_push_byte(f, (w >> 8) & 0xFF);
}

static bool frame_flush(can_frame_builder_t *f)
{
    if (f->len == 0)
    {
        return true;
    }
    uint8_t frame[8];
    memcpy(frame, f->buf, 8); // remaining bytes already 0xFF-padded from reset
    convert_can_frame_to_little_endian(frame);
    esp_err_t err = can_send_for_bootloading_with_ack(BOOT_ID, frame, 8);
    frame_reset(f);
    return err == ESP_OK;
}

// ---------------------------------------------------------------------------
// Streams the whole boot table (KeyValue -> register-init words -> entry
// point -> N blocks -> 0x0000 terminator) over CAN, per Table 4-28.
// Blocks are ACKed individually (MCU_ACK_KEY on RESP_ID) after each block's
// data has been fully sent; the header is sent straight through with no ACK
// expected until the first block completes.
// ---------------------------------------------------------------------------
static bool send_boot_table_over_can(void)
{
    hex_stream_t hs;
    hex_stream_init(&hs);

    can_frame_builder_t fb;
    frame_reset(&fb);

    // ---- 1. KeyValue word ----
    uint16_t key_word;
    if (!hex_stream_next_word(&hs, &key_word))
    {
        ESP_LOGE(TAG, "Unexpected EOF reading KeyValue word");
        return false;
    }
    if ((key_word & 0xFF) != BOOT_TABLE_KEY_LSB || ((key_word >> 8) & 0xFF) != BOOT_TABLE_KEY_MSB)
    {
        ESP_LOGW(TAG, "Unexpected KeyValue word 0x%04X (expected 0x%02X%02X)",
                 key_word, BOOT_TABLE_KEY_MSB, BOOT_TABLE_KEY_LSB);
    }
    if (!frame_push_word(&fb, key_word))
    {
        ESP_LOGE(TAG, "CAN send failed on KeyValue word");
        return false;
    }

    // ---- 2. Register-init words ----
    for (int i = 0; i < BOOT_TABLE_REG_INIT_WORDS; i++)
    {
        uint16_t w;
        if (!hex_stream_next_word(&hs, &w))
        {
            ESP_LOGE(TAG, "Unexpected EOF reading register-init word %d", i);
            return false;
        }
        if (!frame_push_word(&fb, w))
        {
            ESP_LOGE(TAG, "CAN send failed on register-init word %d", i);
            return false;
        }
    }

    // ---- 3. Entry point (2 words) ----
    uint16_t entry_hi_word, entry_lo_word;
    if (!hex_stream_next_word(&hs, &entry_hi_word) || !hex_stream_next_word(&hs, &entry_lo_word))
    {
        ESP_LOGE(TAG, "Unexpected EOF reading entry point words");
        return false;
    }

    uint8_t pc_31_24 = (entry_hi_word >> 8) & 0xFF; // always 0x00 per table
    uint8_t pc_23_16 = entry_hi_word & 0xFF;
    uint8_t pc_15_8 = (entry_lo_word >> 8) & 0xFF;
    uint8_t pc_7_0 = entry_lo_word & 0xFF;
    gstate.entry_point = ((uint32_t)pc_31_24 << 24) | ((uint32_t)pc_23_16 << 16) |
                         ((uint32_t)pc_15_8 << 8) | pc_7_0;
    ESP_LOGI(TAG, "Entry point: 0x%08" PRIX32, (uint32_t)gstate.entry_point);

    if (!frame_push_word(&fb, entry_hi_word) || !frame_push_word(&fb, entry_lo_word))
    {
        ESP_LOGE(TAG, "CAN send failed on entry point words");
        return false;
    }

    // Header is a fixed 22 bytes (11 words); pad + flush so every block
    // always starts cleanly on its own frame boundary.
    if (!frame_flush(&fb))
    {
        ESP_LOGE(TAG, "CAN send failed flushing header frame");
        return false;
    }

    // ---- 4. Block loop ----
    int block_num = 0;
    while (1)
    {
        esp_task_wdt_reset();

        uint16_t block_size_words;
        if (!hex_stream_next_word(&hs, &block_size_words))
        {
            ESP_LOGE(TAG, "Unexpected EOF while expecting block-size word (after %d block(s))", block_num);
            return false;
        }

        if (block_size_words == 0)
        {
            // 0x0000 terminator: end of source program.
            if (!frame_push_word(&fb, 0x0000) || !frame_flush(&fb))
            {
                ESP_LOGE(TAG, "CAN send failed on end-of-source marker");
                return false;
            }
            ESP_LOGI(TAG, "Sent end-of-source marker after %d block(s)", block_num);
            break;
        }

        block_num++;

        uint16_t addr_msw, addr_lsw;
        if (!hex_stream_next_word(&hs, &addr_msw) || !hex_stream_next_word(&hs, &addr_lsw))
        {
            ESP_LOGE(TAG, "Unexpected EOF reading block %d destination address", block_num);
            return false;
        }
        uint32_t dest_addr = ((uint32_t)addr_msw << 16) | addr_lsw;

        ESP_LOGI(TAG, "Block %d: %u word(s) -> addr 0x%08" PRIX32, block_num, block_size_words, dest_addr);

        if (!frame_push_word(&fb, block_size_words) ||
            !frame_push_word(&fb, addr_msw) ||
            !frame_push_word(&fb, addr_lsw))
        {
            ESP_LOGE(TAG, "CAN send failed on block %d header", block_num);
            return false;
        }

        for (uint16_t w = 0; w < block_size_words; w++)
        {
            uint16_t data_word;
            if (!hex_stream_next_word(&hs, &data_word))
            {
                ESP_LOGE(TAG, "Unexpected EOF mid-block (word %u/%u of block %d)",
                         w, block_size_words, block_num);
                return false;
            }
            if (!frame_push_word(&fb, data_word))
            {
                ESP_LOGE(TAG, "CAN send failed mid-block (word %u/%u of block %d)",
                         w, block_size_words, block_num);
                return false;
            }
            if ((w & 0x3F) == 0)
            {
                vTaskDelay(pdMS_TO_TICKS(1)); // yield periodically on long blocks
            }
        }

        // Pad + send the block's final (possibly partial) frame.
        if (!frame_flush(&fb))
        {
            ESP_LOGE(TAG, "CAN send failed flushing block %d", block_num);
            return false;
        }

        // Per-block ACK, once per block.
        if (!wait_for_can_response(MCU_ACK_KEY, BLOCK_ACK_TIMEOUT_MS))
        {
            ESP_LOGE(TAG, "No block-complete ACK after block %d", block_num);
            return false;
        }
    }

    ESP_LOGI(TAG, "Boot table fully transmitted (%d block(s))", block_num);
    return block_num > 0;
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------
static void firmware_update_task(void *arg)
{
    (void)arg;
    firmware_update();
    vTaskDelete(NULL);
}

void fota_can_run(void)
{
    BaseType_t task_created = xTaskCreate(
        firmware_update_task,
        "fota_can_task",
        FOTA_TASK_STACK_SIZE,
        NULL,
        5,
        NULL);

    if (task_created != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create FOTA task");
    }
}

void firmware_update(void)
{
    gstate.fw_update_in_progress = true;
    gstate.can_normal_processing = false; // Disable normal handling

    ESP_LOGI(TAG, "Downloading firmware...");
    if (!download_and_parse_firmware())
    {
        ESP_LOGE(TAG, "Firmware update failed");
        gstate.fw_update_start = false;
        gstate.fw_update_in_progress = false;
        gstate.can_normal_processing = true;
        esp_restart();
        return;
    }

    ESP_LOGI(TAG, "=== Firmware update successful ===");

    gstate.fw_update_start = false;
    gstate.fw_update_in_progress = false;
    gstate.can_normal_processing = true;
    esp_restart();
}

// esp_err_t can_receive_message(uint32_t *id, uint8_t *data, TickType_t timeout_ticks)
// {
//     twai_message_t msg;
//     esp_err_t ret = twai_receive(&msg, timeout_ticks);

//     if (ret == ESP_OK)
//     {
//         *id = msg.identifier;
//         memcpy(data, msg.data, msg.data_length_code);
//         return ESP_OK;
//     }
//     else if (ret == ESP_ERR_TIMEOUT)
//     {
//         ESP_LOGD(TAG, "CAN receive timeout");
//         return ESP_ERR_TIMEOUT;
//     }
//     else
//     {
//         ESP_LOGE(TAG, "CAN receive error: %s", esp_err_to_name(ret));
//         return ret;
//     }
// }

esp_err_t can_receive_message(uint32_t *id, uint8_t *data, TickType_t timeout_ticks)
{
    twai_message_t msg;
    TickType_t start_tick = xTaskGetTickCount();

    while ((xTaskGetTickCount() - start_tick) < timeout_ticks)
    {
        TickType_t remaining = timeout_ticks - (xTaskGetTickCount() - start_tick);
        esp_err_t ret = twai_receive(&msg, remaining);

        if (ret == ESP_OK)
        {
            ESP_LOGD(TAG, "Received CAN message - ID: 0x%08" PRIX32 ", extd: %d, Data: %02X %02X %02X %02X %02X %02X %02X %02X",
                     msg.identifier, msg.extd,
                     msg.data[0], msg.data[1], msg.data[2], msg.data[3],
                     msg.data[4], msg.data[5], msg.data[6], msg.data[7]);

            if (msg.identifier == RESP_ID)
            {
                *id = msg.identifier;
                memcpy(data, msg.data, msg.data_length_code);
                return ESP_OK;
            }

            ESP_LOGD(TAG, "Ignoring CAN ID: 0x%08" PRIX32, msg.identifier);
        }
        else if (ret == ESP_ERR_TIMEOUT)
        {
            return ESP_ERR_TIMEOUT;
        }
        else
        {
            ESP_LOGE(TAG, "CAN receive error: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    return ESP_ERR_TIMEOUT;
}

void convert_can_frame_to_little_endian(uint8_t *can_frame)
{
    // Swap bytes within each 16-bit word (every 2 bytes).
    for (int i = 0; i < 8; i += 2)
    {
        uint8_t temp = can_frame[i];
        can_frame[i] = can_frame[i + 1];
        can_frame[i + 1] = temp;
    }
}

bool wait_for_block_response(uint32_t expected_id, const uint8_t *expected_data, uint32_t timeout_ms)
{
    uint32_t received_id;
    uint8_t received_data[8];

    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while ((xTaskGetTickCount() - start_tick) < timeout_ticks)
    {
        if (can_receive_message(&received_id, received_data, pdMS_TO_TICKS(100)) == ESP_OK)
        {
            if (received_id == expected_id && memcmp(received_data, expected_data, 8) == 0)
            {
                return true; // Expected frame received
            }
        }
    }

    esp_restart();
}

esp_err_t can_send_message_for_bootloading(uint32_t identifier, uint8_t *data, uint8_t data_length)
{
    if (data == NULL || data_length == 0 || data_length > 8)
    {
        return ESP_ERR_INVALID_ARG;
    }

    twai_message_t msg = {
        .identifier = identifier,
        .extd = 1,
        .rtr = 0,
        .data_length_code = data_length,
    };
    memcpy(msg.data, data, data_length);
    printf("Sending CAN message - ID: 0x%08X, Data: ", (unsigned int)identifier);
    for (int i = 0; i < data_length; i++)
    {
        printf("%02X ", data[i]);
    }
    return twai_transmit(&msg, pdMS_TO_TICKS(10));
}

esp_err_t can_send_for_bootloading(uint32_t identifier, uint8_t *data, uint8_t data_length)
{
    return can_send_message_for_bootloading(identifier, data, data_length);
}

esp_err_t can_send_for_bootloading_with_ack(uint32_t identifier, uint8_t *data, uint8_t data_length)
{
    // Per-frame ACKs are not expected from the bootloader; only per-block
    // ACKs (see wait_for_can_response(RESP_ID, MCU_ACK_KEY, ...) in
    // send_boot_table_over_can()). This wrapper just transmits the frame.
    return can_send_message_for_bootloading(identifier, data, data_length);
}

void analyze_memory(void)
{
    printf("\n===== MEMORY ANALYSIS =====\n");
    printf("Current free heap: %u bytes\n", (unsigned int)esp_get_free_heap_size());
    printf("Minimum ever free heap: %u bytes\n", (unsigned int)esp_get_minimum_free_heap_size());

    printf("\n[Heap Capabilities]\n");
    printf("8-bit accessible DRAM: %u bytes\n", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    printf("32-bit accessible DRAM: %u bytes\n", heap_caps_get_free_size(MALLOC_CAP_32BIT));
    printf("DMA capable memory: %u bytes\n", heap_caps_get_free_size(MALLOC_CAP_DMA));
    printf("SPI RAM (if available): %u bytes\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_8BIT);
    printf("\n[Fragmentation Analysis]\n");
    printf("Total free bytes: %u\n", info.total_free_bytes);
    printf("Largest free block: %u\n", info.largest_free_block);
    printf("Allocated blocks: %u\n", info.allocated_blocks);
    printf("Free blocks: %u\n", info.free_blocks);
    printf("Estimated fragmentation: %.1f%%\n",
           100.0 - (info.largest_free_block * 100.0 / info.total_free_bytes));

    printf("\n[Task Stack Usage]\n");
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    printf("Current task '%s' high watermark: %u bytes\n",
           pcTaskGetName(current_task),
           uxTaskGetStackHighWaterMark(current_task));

    printf("\n[Detailed Heap Info]\n");
    heap_caps_print_heap_info(MALLOC_CAP_8BIT);
}

bool wait_for_can_response(const uint8_t *expected_data, uint32_t timeout_ms)
{
    TickType_t start_time = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while ((xTaskGetTickCount() - start_time) < timeout_ticks)
    {
        uint32_t received_id;
        uint8_t received_data[8] = {0};

        esp_err_t ret = can_receive_message(&received_id,
                                            received_data,
                                            pdMS_TO_TICKS(100));

        if (ret == ESP_OK)
        {
            ESP_LOGI(TAG,
                     "Received CAN message - ID: 0x%08X, Data: "
                     "%02X %02X %02X %02X %02X %02X %02X %02X",
                     (unsigned int)received_id,
                     received_data[0], received_data[1], received_data[2], received_data[3],
                     received_data[4], received_data[5], received_data[6], received_data[7]);

            // If only waiting for the CAN frame, no data check
            if (expected_data == NULL)
            {
                return true;
            }

            // Verify the received payload
            if (memcmp(received_data, expected_data, 8) == 0)
            {
                ESP_LOGI(TAG, "Received expected CAN response");
                return true;
            }

            ESP_LOGW(TAG, "Received frame but data did not match");
        }
        else if (ret != ESP_ERR_TIMEOUT)
        {
            ESP_LOGE(TAG, "CAN receive error: %s", esp_err_to_name(ret));
            return false;
        }
    }

    ESP_LOGE(TAG, "Timeout waiting for CAN response");
    return false;
}

// bool wait_for_can_response(uint32_t expected_id, const uint8_t *expected_data, uint32_t timeout_ms)
// {
//     uint32_t start_time = xTaskGetTickCount();
//     uint32_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

//     while ((xTaskGetTickCount() - start_time) < timeout_ticks)
//     {
//         uint32_t received_id;
//         uint8_t received_data[8] = {0};

//         esp_err_t ret = can_receive_message(&received_id, received_data, pdMS_TO_TICKS(100));

//         if (ret == ESP_OK)
//         {
//             ESP_LOGI(TAG, "Received CAN message - ID: 0x%08X, Data: %02X %02X %02X %02X %02X %02X %02X %02X",
//                      (unsigned int)received_id,
//                      received_data[0], received_data[1], received_data[2], received_data[3],
//                      received_data[4], received_data[5], received_data[6], received_data[7]);

//             if (received_id == expected_id)
//             {
//                 if (expected_data == NULL)
//                 {
//                     return true;
//                 }
//                 else if (memcmp(received_data, expected_data, 8) == 0)
//                 {
//                     ESP_LOGI(TAG, "Got expected response on ID 0x%08X", (unsigned int)expected_id);
//                     return true;
//                 }
//                 else
//                 {
//                     ESP_LOGW(TAG, "ID matched but data didn't match expected response");
//                 }
//             }
//         }
//         else if (ret != ESP_ERR_TIMEOUT)
//         {
//             ESP_LOGE(TAG, "CAN receive error: %s", esp_err_to_name(ret));
//             return false;
//         }
//     }

//     ESP_LOGE(TAG, "Timeout waiting for CAN response on ID 0x%08X", (unsigned int)expected_id);
//     return false;
// }

// ---------------------------------------------------------------------------
// Raw line reader over the flash partition holding the downloaded .hex file.
// Returns a heap-allocated, NUL-terminated line starting at ':' each call,
// or NULL at end of partition data. Caller must free() the returned string.
// ---------------------------------------------------------------------------
#define MAX_HEX_LINE_LEN 512

char *get_line_from_partition(void)
{
    if (!reader_state.partition)
    {
        reader_state.partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA,
            ESP_PARTITION_SUBTYPE_ANY,
            "mydownload");
        if (!reader_state.partition)
        {
            reader_state.partition = esp_partition_find_first(
                ESP_PARTITION_TYPE_DATA,
                ESP_PARTITION_SUBTYPE_ANY,
                "storage");
        }
        if (!reader_state.partition)
        {
            ESP_LOGE("HEX_READER", "Partition not found for firmware download");
            return NULL;
        }
    }

    char line[MAX_HEX_LINE_LEN];
    size_t line_pos = 0;
    int target_line = reader_state.current_line_num;

    while (1)
    {
        if (reader_state.buffer_pos >= reader_state.buffer_len)
        {
            if (reader_state.offset >= reader_state.partition->size)
            {
                return NULL; // EOF
            }

            size_t to_read = MIN(CHUNK_SIZE, reader_state.partition->size - reader_state.offset);
            esp_err_t err = esp_partition_read(reader_state.partition,
                                               reader_state.offset,
                                               reader_state.buffer,
                                               to_read);
            if (err != ESP_OK)
            {
                ESP_LOGE("HEX_READER", "Read error at 0x%x", reader_state.offset);
                return NULL;
            }
            reader_state.buffer_len = to_read;
            reader_state.buffer_pos = 0;
            reader_state.offset += to_read;
        }

        for (; reader_state.buffer_pos < reader_state.buffer_len; reader_state.buffer_pos++)
        {
            char c = reader_state.buffer[reader_state.buffer_pos];

            if (c == '\r')
            {
                continue; // ignore stray CR from CRLF-terminated files
            }

            if (c == '\n' || c == ':')
            {
                if (line_pos > 0 && line[0] == ':')
                {
                    line[line_pos] = '\0';
                    int current_line = reader_state.current_line_num++;

                    if (current_line == target_line)
                    {
                        return strdup(line);
                    }
                }

                line_pos = 0;
                if (c == ':')
                {
                    line[line_pos++] = c;
                }
                continue;
            }

            if (line_pos < MAX_HEX_LINE_LEN - 1)
            {
                line[line_pos++] = c;
            }
        }
    }
}