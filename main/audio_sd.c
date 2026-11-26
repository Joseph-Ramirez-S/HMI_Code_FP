#include "audio_sd.h"
#include "intro_audio.h"

#include "driver/sdmmc_host.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "audio_sd";
static const char *staging_path = AUDIO_SD_MOUNT_PATH "/AUDIOWR.TMP";
static bool card_mounted;
enum { UPLOAD_LIMIT = 256 * 1024, HINT_UPLOAD_LIMIT = 512 * 1024 };

esp_err_t audio_sd_mount(void) {
    // The intro mounts first. After the intro, the upload task can retry a failed
    // mount. Keep a successful mount alive while the music file is open.
    if (card_mounted) return ESP_OK;
    // CrowPanel Advanced ESP32-P4 uses slot 0 with only D0 wired.
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = 10000;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = GPIO_NUM_43;
    slot.cmd = GPIO_NUM_44;
    slot.d0 = GPIO_NUM_39;
    slot.width = 1;
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    const esp_vfs_fat_sdmmc_mount_config_t mount = {.max_files = 2, .format_if_mount_failed = false};
    sdmmc_card_t *card = NULL;
    const esp_err_t err = esp_vfs_fat_sdmmc_mount(AUDIO_SD_MOUNT_PATH, &host, &slot, &mount, &card);
    card_mounted = err == ESP_OK;
    if (err != ESP_OK)
        ESP_LOGW(TAG, "Montaje SDMMC: %s. Si FatFs indica (13), revise el volumen FAT16/FAT32.", esp_err_to_name(err));
    return err;
}

static void reply(const char *message) {
    printf("SDW1 %s\n", message);
    fflush(stdout);
}

static bool receive_file(FILE *file, size_t total) {
    uint8_t buffer[1024];
    while (total) {
        const size_t chunk = total < sizeof(buffer) ? total : sizeof(buffer);
        size_t received = 0;
        while (received < chunk) {
            const int read = uart_read_bytes(UART_NUM_0, buffer + received, chunk - received, pdMS_TO_TICKS(2000));
            if (read <= 0) return false;
            received += read;
        }
        if (fwrite(buffer, 1, chunk, file) != chunk) return false;
        total -= chunk;
        if (total) reply("NEXT");
    }
    return fflush(file) == 0 && fsync(fileno(file)) == 0;
}

static bool verify_file(size_t expected_length, uint32_t expected_crc) {
    FILE *file = fopen(staging_path, "rb");
    if (!file) return false;
    uint8_t buffer[1024];
    uint32_t crc = 0;
    size_t count, total = 0;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0) {
        crc = esp_rom_crc32_le(crc, buffer, count);
        total += count;
    }
    const bool valid = !ferror(file) && total == expected_length && crc == expected_crc;
    return fclose(file) == 0 && valid;
}

static bool publish_audio(const char *destination, const char *backup) {
    struct stat info;
    const bool has_old_file = stat(destination, &info) == 0;
    if (!has_old_file && errno != ENOENT) return false;
    if (has_old_file) {
        if (unlink(backup) != 0 && errno != ENOENT) return false;
        if (rename(destination, backup) != 0) return false;
    }
    if (rename(staging_path, destination) != 0) {
        if (has_old_file && rename(backup, destination) != 0)
            ESP_LOGW(TAG, "Se conserva el archivo anterior en %s", backup);
        return false;
    }
    if (unlink(backup) != 0 && errno != ENOENT)
        ESP_LOGW(TAG, "No se pudo retirar %s", backup);
    return true;
}

static void receive_audio(size_t size, uint32_t crc, const char *destination, const char *backup) {
    if (audio_sd_mount() != ESP_OK) {
        reply("ERR MOUNT");
        return;
    }
    const char *result = "ERR OPEN";
    FILE *file = fopen(staging_path, "wb");
    if (file) {
        reply("READY");
        const bool written = receive_file(file, size);
        if (fclose(file) != 0 || !written) result = "ERR WRITE";
        else if (!verify_file(size, crc)) result = "ERR CRC";
        else if (!publish_audio(destination, backup)) result = "ERR RENAME";
        else result = "OK";
        if (strcmp(result, "OK")) unlink(staging_path);
    }
    uart_flush_input(UART_NUM_0);
    reply(result);
}

static void upload_service(void *unused) {
    (void)unused;
    // The intro's graphics are gone. Start music and load effects without blocking rendering.
    animation_audio_prepare();
    char line[64];
    size_t used = 0;
    bool overflow = false;
    for (;;) {
        uint8_t byte;
        if (uart_read_bytes(UART_NUM_0, &byte, 1, portMAX_DELAY) != 1) continue;
        if (byte != '\n') {
            if (byte == '\r') continue;
            if (used < sizeof(line) - 1) line[used++] = (char)byte;
            else overflow = true;
            continue;
        }
        line[used] = '\0';
        if (!overflow) {
            if (!strcmp(line, "SDW1 HELLO")) {
                reply("HERE");
            } else if (!strcmp(line, "SDW1 REBOOT")) {
                reply("REBOOTING");
                uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(500));
                esp_restart();
            } else if (!strncmp(line, "SDW1 PUT ", 9)) {
                uint32_t bytes, crc;
                char trailing;
                const char *header = line + 9;
                const char *destination = AUDIO_SD_BUTTONS, *backup = AUDIO_SD_BUTTONS_BACKUP;
                unsigned limit = UPLOAD_LIMIT;
                if (!strncmp(header, "ARRIVAL ", 8)) {
                    header += 8;
                    destination = AUDIO_SD_ARRIVAL;
                    backup = AUDIO_SD_ARRIVAL_BACKUP;
                } else if (!strncmp(header, "S1_BF ", 6)) {
                    header += 6;
                    destination = AUDIO_SD_HINT;
                    backup = AUDIO_SD_HINT_BACKUP;
                    limit = HINT_UPLOAD_LIMIT;
                } else if (!strncmp(header, "SCENE ", 6)) {
                    header += 6;
                    destination = AUDIO_SD_SCENE;
                    backup = AUDIO_SD_SCENE_BACKUP;
                } else if (!strncmp(header, "CALL ", 5)) {
                    header += 5;
                    destination = AUDIO_SD_HALL_CALL;
                    backup = AUDIO_SD_HALL_CALL_BACKUP;
                } else if (!strncmp(header, "ALERT ", 6)) {
                    header += 6;
                    destination = AUDIO_SD_EMERGENCY;
                    backup = AUDIO_SD_EMERGENCY_BACKUP;
                } else if (!strncmp(header, "MOTION ", 7)) {
                    header += 7;
                    destination = AUDIO_SD_CABIN_MOTION;
                    backup = AUDIO_SD_CABIN_MOTION_BACKUP;
                } else if (!strncmp(header, "BACK ", 5)) {
                    header += 5;
                    destination = AUDIO_SD_STAIRS_BACK;
                    backup = AUDIO_SD_STAIRS_BACK_BACKUP;
                } else if (!strncmp(header, "OPTIONS ", 8)) {
                    header += 8;
                    destination = AUDIO_SD_OPTIONS;
                    backup = AUDIO_SD_OPTIONS_BACKUP;
                } else if (!strncmp(header, "SLIDER ", 7)) {
                    header += 7;
                    destination = AUDIO_SD_SLIDER;
                    backup = AUDIO_SD_SLIDER_BACKUP;
                }
                if (sscanf(header, "%" SCNu32 " %" SCNx32 " %c", &bytes, &crc, &trailing) == 2 &&
                    bytes >= 44 && bytes <= limit)
                    receive_audio(bytes, crc, destination, backup);
                else reply("ERR HEADER");
            }
        }
        used = 0;
        overflow = false;
    }
}

esp_err_t audio_sd_start_upload_service(void) {
    // UART0 is already at 115200 through the CrowPanel's USB-to-UART bridge.
    esp_err_t err = uart_driver_install(UART_NUM_0, 4096, 0, 0, NULL, 0);
    if (err != ESP_OK) return err;
    if (xTaskCreatePinnedToCore(upload_service, "AudioUpload", 5120, NULL, 2, NULL, 0) != pdPASS) {
        uart_driver_delete(UART_NUM_0);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
