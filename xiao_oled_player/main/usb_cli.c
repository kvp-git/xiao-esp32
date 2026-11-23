#include "usb_cli.h"
#include "player_task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "esp_littlefs.h"
#include "esp_rom_crc.h"

#define MOUNT_POINT "/littlefs"
#define CHUNK_SIZE  1024

static void usb_write_str(const char *s) {
    usb_serial_jtag_write_bytes((const uint8_t *)s, strlen(s), portMAX_DELAY);
}

static int usb_read_line(char *buf, size_t max_len) {
    size_t idx = 0;
    while (idx < max_len - 1) {
        uint8_t c;
        int n = usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY);
        if (n > 0) {
            if (c == '\n') break;
            if (c == '\r') continue;
            buf[idx++] = (char)c;
        }
    }
    buf[idx] = '\0';
    return (int)idx;
}

static bool sanitize_filename(const char *in, char *out_path, size_t out_size) {
    // Single root directory only: reject slashes or parent traversal
    if (!in || strlen(in) == 0 || strlen(in) > 64) return false;
    if (strchr(in, '/') || strchr(in, '\\') || strstr(in, "..")) return false;
    snprintf(out_path, out_size, "%s/%s", MOUNT_POINT, in);
    return true;
}

static void cmd_list(void) {
    player_pause();

    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);

    // Allow a full 255-byte filename plus the FILE prefix and file size.
    char msg[320];
    snprintf(msg, sizeof(msg), "OK LIST total=%u used=%u free=%u\n",
             (unsigned)total, (unsigned)used, (unsigned)(total - used));
    usb_write_str(msg);

    DIR *dir = opendir(MOUNT_POINT);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_type != DT_REG) continue;
            char path[320];
            struct stat st;
            snprintf(path, sizeof(path), "%s/%s", MOUNT_POINT, ent->d_name);
            if (stat(path, &st) == 0) {
                snprintf(msg, sizeof(msg), "FILE %s %ld\n", ent->d_name, st.st_size);
                usb_write_str(msg);
            }
        }
        closedir(dir);
    }
    usb_write_str("END\n");

    player_resume();
}

static void cmd_upload(const char *fname, size_t expected_size, uint32_t expected_crc32, bool verify_crc) {
    char path[160];
    if (!sanitize_filename(fname, path, sizeof(path)) || expected_size == 0) {
        usb_write_str("ERR INVALID_ARGS\n");
        return;
    }

    player_pause();

    FILE *f = fopen(path, "wb");
    if (!f) {
        player_resume();
        usb_write_str("ERR OPEN_FAILED\n");
        return;
    }

    usb_write_str("READY\n");

    uint8_t *buf = malloc(CHUNK_SIZE);
    if (!buf) {
        fclose(f);
        unlink(path);
        player_resume();
        usb_write_str("ERR OOM\n");
        return;
    }

    size_t remaining = expected_size;
    uint32_t running_crc = 0;
    bool ok = true;

    while (remaining > 0) {
        size_t to_read = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;
        size_t got = 0;
        while (got < to_read) {
            int n = usb_serial_jtag_read_bytes(buf + got, to_read - got, pdMS_TO_TICKS(3000));
            if (n <= 0) {
                ok = false;
                break;
            }
            got += n;
        }
        if (!ok) break;

        running_crc = esp_rom_crc32_le(running_crc, buf, got);

        if (fwrite(buf, 1, got, f) != got) {
            ok = false;
            break;
        }
        remaining -= got;
    }

    free(buf);
    fclose(f);

    if (!ok) {
        unlink(path);
        usb_write_str("ERR TRANSFER_TIMEOUT_OR_WRITE\n");
    } else if (verify_crc && running_crc != expected_crc32) {
        unlink(path);
        char err_msg[96];
        snprintf(err_msg, sizeof(err_msg), "ERR CRC_MISMATCH expected=%08lX got=%08lX\n",
                 (unsigned long)expected_crc32, (unsigned long)running_crc);
        usb_write_str(err_msg);
    } else {
        char ok_msg[64];
        snprintf(ok_msg, sizeof(ok_msg), "OK UPLOAD_DONE crc32=%08lX\n", (unsigned long)running_crc);
        usb_write_str(ok_msg);
    }

    player_resume();
}

static void cmd_download(const char *fname) {
    char path[160];
    if (!sanitize_filename(fname, path, sizeof(path))) {
        usb_write_str("ERR INVALID_NAME\n");
        return;
    }

    player_pause();

    struct stat st;
    if (stat(path, &st) != 0) {
        player_resume();
        usb_write_str("ERR NOT_FOUND\n");
        return;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        player_resume();
        usb_write_str("ERR OPEN_FAILED\n");
        return;
    }

    // Pre-pass to compute CRC32 so client can verify download integrity too
    uint8_t *buf = malloc(CHUNK_SIZE);
    uint32_t file_crc = 0;
    size_t n;
    while ((n = fread(buf, 1, CHUNK_SIZE, f)) > 0) {
        file_crc = esp_rom_crc32_le(file_crc, buf, n);
    }
    fseek(f, 0, SEEK_SET);

    char hdr[96];
    snprintf(hdr, sizeof(hdr), "OK DATA %ld %08lX\n", st.st_size, (unsigned long)file_crc);
    usb_write_str(hdr);

    while ((n = fread(buf, 1, CHUNK_SIZE, f)) > 0) {
        usb_serial_jtag_write_bytes(buf, n, portMAX_DELAY);
    }

    free(buf);
    fclose(f);
    usb_write_str("\nEND\n");

    player_resume();
}

static void cmd_delete(const char *fname) {
    char path[160];
    if (!sanitize_filename(fname, path, sizeof(path))) {
        usb_write_str("ERR INVALID_NAME\n");
        return;
    }

    player_pause();
    if (unlink(path) == 0) {
        usb_write_str("OK DELETED\n");
    } else {
        usb_write_str("ERR NOT_FOUND\n");
    }
    player_resume();
}

static void usb_cli_task(void *arg) {
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = 4096,
        .tx_buffer_size = 4096,
    };
    usb_serial_jtag_driver_install(&cfg);

    char line[160];
    while (1) {
        int len = usb_read_line(line, sizeof(line));
        if (len == 0) continue;

        if (strcmp(line, "LIST") == 0) {
            cmd_list();
        } else if (strncmp(line, "UPLOAD ", 7) == 0) {
            char fname[65];
            unsigned long fsize = 0;
            unsigned long crc32_val = 0;
            int parsed = sscanf(line + 7, "%64s %lu %lx", fname, &fsize, &crc32_val);
            if (parsed == 3) {
                cmd_upload(fname, (size_t)fsize, (uint32_t)crc32_val, true);
            } else if (parsed == 2) {
                cmd_upload(fname, (size_t)fsize, 0, false);
            } else {
                usb_write_str("ERR USAGE: UPLOAD <filename> <bytes> [crc32_hex]\n");
            }
        } else if (strncmp(line, "DOWNLOAD ", 9) == 0) {
            char fname[65];
            if (sscanf(line + 9, "%64s", fname) == 1) {
                cmd_download(fname);
            }
        } else if (strncmp(line, "DELETE ", 7) == 0) {
            char fname[65];
            if (sscanf(line + 7, "%64s", fname) == 1) {
                cmd_delete(fname);
            }
        } else {
            usb_write_str("ERR UNKNOWN_CMD (Use: LIST, UPLOAD, DOWNLOAD, DELETE)\n");
        }
    }
}

void usb_cli_start(void) {
    xTaskCreate(usb_cli_task, "usb_cli", 4096, NULL, 6, NULL);
}
