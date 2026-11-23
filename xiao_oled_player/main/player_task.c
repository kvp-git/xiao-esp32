#include "player_task.h"
#include "display_ssd1351.h"
#include "startup_png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lz4.h"
#include "PNGdec.h"
#include "JPEGDEC.h"

#define MOUNT_POINT "/littlefs"
#define FRAME_BYTES (OLED_WIDTH * OLED_HEIGHT * 2) // 32,768 bytes
#define MAX_COMP_BUF 33000

// 12-byte video stream header (LZ4S or MJPS).
typedef struct __attribute__((packed)) {
    char magic[4];      // "LZ4S" or "MJPS"
    uint16_t width;     // 128
    uint16_t height;    // 128
    uint16_t fps;       // Target FPS
    uint16_t flags;     // Reserved
} video_header_t;

static_assert(sizeof(video_header_t) == 12, "Unexpected video header size");

typedef struct {
    int src_w;
    int src_h;
    int dst_w;
    int dst_h;
    int off_x;
    int off_y;
    int last_dst_y;
    uint16_t *line_buf;
} png_ctx_t;

static SemaphoreHandle_t fs_mutex;
static volatile bool pause_requested = false;
static uint16_t *frame_buf = NULL;   // DMA-capable 32 KB output buffer
static uint8_t  *comp_buf  = NULL;   // 33 KB compressed input buffer
static PNG png;
static JPEGDEC jpeg;
static bool mjpeg_playback_reported = false;

// JPEGDEC supplies MCU blocks; assemble a complete frame before displaying it.
static int jpeg_draw_cb(JPEGDRAW *draw) {
    if (pause_requested || !draw->pPixels || draw->iWidthUsed <= 0 ||
        draw->iWidthUsed > draw->iWidth || draw->iHeight <= 0 ||
        draw->x < 0 || draw->y < 0 ||
        draw->x + draw->iWidthUsed > OLED_WIDTH ||
        draw->y + draw->iHeight > OLED_HEIGHT) {
        return 0;
    }
    for (int y = 0; y < draw->iHeight; y++) {
        memcpy(&frame_buf[(draw->y + y) * OLED_WIDTH + draw->x],
               &draw->pPixels[y * draw->iWidth],
               draw->iWidthUsed * sizeof(uint16_t));
    }
    return 1;
}

static bool decode_jpeg_frame(int size) {
    if (!jpeg.openRAM(comp_buf, size, jpeg_draw_cb)) return false;
    bool decoded = false;
    if (jpeg.getWidth() == OLED_WIDTH && jpeg.getHeight() == OLED_HEIGHT &&
        jpeg.getJPEGType() == JPEG_MODE_BASELINE) {
        jpeg.setPixelType(RGB565_BIG_ENDIAN);
        decoded = jpeg.decode(0, 0, 0) == 1;
    }
    jpeg.close();
    return decoded && !pause_requested;
}

// PNGdec line callback: scales (aspect-preserving) and centers image into 128x128 frame_buf
static int png_draw_cb(PNGDRAW *pDraw) {
    png_ctx_t *ctx = (png_ctx_t *)pDraw->pUser;
    if (!ctx || !ctx->line_buf || ctx->src_h <= 0 || ctx->src_w <= 0) return 0;

    int y0 = (pDraw->y * ctx->dst_h) / ctx->src_h;
    int y1 = ((pDraw->y + 1) * ctx->dst_h) / ctx->src_h;
    if (y1 <= y0) y1 = y0 + 1;
    if (y0 >= ctx->dst_h) return 1;
    if (y1 > ctx->dst_h) y1 = ctx->dst_h;

    png.getLineAsRGB565(pDraw, ctx->line_buf, PNG_RGB565_BIG_ENDIAN, 0xffffffff);

    for (int dy = y0; dy < y1; dy++) {
        int out_y = ctx->off_y + dy;
        if (out_y < 0 || out_y >= OLED_HEIGHT) continue;
        uint16_t *dst_row = &frame_buf[out_y * OLED_WIDTH + ctx->off_x];

        if (ctx->dst_w == ctx->src_w) {
            memcpy(dst_row, ctx->line_buf, ctx->dst_w * sizeof(uint16_t));
        } else {
            for (int dx = 0; dx < ctx->dst_w; dx++) {
                int sx = (dx * ctx->src_w) / ctx->dst_w;
                if (sx >= ctx->src_w) sx = ctx->src_w - 1;
                dst_row[dx] = ctx->line_buf[sx];
            }
        }
    }
    return 1;
}

static bool player_draw_png(const uint8_t *png_data, int size) {
    if (!png_data || size <= 0) return false;
    if (!frame_buf) {
        frame_buf = (uint16_t *)heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_DMA);
        if (!frame_buf) return false;
    }
    bool drawn = false;
    memset(frame_buf, 0, FRAME_BYTES);
    // PNGdec's RAM API accepts a mutable pointer but only reads PNG bytes.
    int rc = png.openRAM(const_cast<uint8_t *>(png_data), size, png_draw_cb);
    if (rc == PNG_SUCCESS) {
        int sw = png.getWidth();
        int sh = png.getHeight();
        if (sw > 0 && sh > 0 && sw <= 1024) {
            int dw = sw;
            int dh = sh;
            // Aspect-fit downscale if larger than 128x128; preserve native size & center if <=128x128
            if (sw > OLED_WIDTH || sh > OLED_HEIGHT) {
                if (sw * OLED_HEIGHT >= sh * OLED_WIDTH) {
                    dw = OLED_WIDTH;
                    dh = (sh * OLED_WIDTH) / sw;
                } else {
                    dh = OLED_HEIGHT;
                    dw = (sw * OLED_HEIGHT) / sh;
                }
            }
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;

            png_ctx_t ctx = {
                .src_w = sw,
                .src_h = sh,
                .dst_w = dw,
                .dst_h = dh,
                .off_x = (OLED_WIDTH - dw) / 2,
                .off_y = (OLED_HEIGHT - dh) / 2,
                .last_dst_y = -1,
                .line_buf = (uint16_t *)malloc(sw * sizeof(uint16_t)),
            };

            if (ctx.line_buf) {
                int decode_rc = png.decode(&ctx, 0);
                free(ctx.line_buf);
                if (decode_rc == PNG_SUCCESS) {
                    ssd1351_draw_frame(frame_buf);
                    drawn = true;
                }
            }
        }
        png.close();
    }
    return drawn;
}

static void play_png_file(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 200 * 1024) {
        fclose(f);
        return;
    }

    uint8_t *png_data = (uint8_t *)malloc(fsize);
    if (!png_data) {
        fclose(f);
        return;
    }

    if (fread(png_data, 1, fsize, f) != (size_t)fsize) {
        free(png_data);
        fclose(f);
        return;
    }
    fclose(f);

    player_draw_png(png_data, (int)fsize);
    free(png_data);

    // 4-second static hold (interruptible in 50ms slices if USB command arrives)
    for (int i = 0; i < 80 && !pause_requested; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void play_video_file(const char *filepath, bool motion_jpeg) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return;

    video_header_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr.magic, motion_jpeg ? "MJPS" : "LZ4S", 4) != 0 ||
        hdr.width != OLED_WIDTH || hdr.height != OLED_HEIGHT ||
        hdr.flags != 0 || (motion_jpeg && (hdr.fps == 0 || hdr.fps > 60))) {
        fclose(f);
        return;
    }

    uint32_t fps = (hdr.fps > 0 && hdr.fps <= 60) ? hdr.fps : 25;
    int64_t frame_interval_us = 1000000LL / fps;
    int64_t next_frame_time = esp_timer_get_time();
    uint32_t frames_drawn = 0;
    uint32_t max_decode_us = 0;
    bool completed = false;

    while (!pause_requested) {
        uint16_t comp_size = 0;
        if (fread(&comp_size, 1, sizeof(uint16_t), f) != sizeof(uint16_t)) {
            break; // End of stream
        }
        if (comp_size == 0) {
            completed = true;
            break;
        }
        if (comp_size > MAX_COMP_BUF) break;

        if (fread(comp_buf, 1, comp_size, f) != comp_size) {
            break;
        }

        if (motion_jpeg) {
            int64_t decode_start = esp_timer_get_time();
            if (!decode_jpeg_frame(comp_size)) {
                if (!pause_requested) {
                    ESP_LOGE("PLAYER", "JPEG decode failed in %s at frame %lu",
                             filepath, (unsigned long)frames_drawn);
                }
                break;
            }
            uint32_t decode_us = (uint32_t)(esp_timer_get_time() - decode_start);
            if (decode_us > max_decode_us) max_decode_us = decode_us;
        } else if (comp_size == FRAME_BYTES) {
            memcpy(frame_buf, comp_buf, FRAME_BYTES);
        } else {
            int dec = LZ4_decompress_safe((const char *)comp_buf, (char *)frame_buf, comp_size, FRAME_BYTES);
            if (dec != FRAME_BYTES) {
                break;
            }
        }

        int64_t now = esp_timer_get_time();
        if (next_frame_time > now) {
            int64_t sleep_us = next_frame_time - now;
            if (sleep_us > 1000) {
                vTaskDelay(pdMS_TO_TICKS(sleep_us / 1000));
            }
        }
        next_frame_time += frame_interval_us;
        if (next_frame_time < esp_timer_get_time()) {
            next_frame_time = esp_timer_get_time();
        }

        ssd1351_draw_frame(frame_buf);
        frames_drawn++;
    }

    fclose(f);
    // Report the first complete MJPEG pass once per boot without flooding USB.
    if (motion_jpeg && completed && frames_drawn && !mjpeg_playback_reported) {
        ESP_LOGI("PLAYER", "MJPEG complete: %s, %lu frames, target %lu fps, max decode %lu us",
                 filepath, (unsigned long)frames_drawn, (unsigned long)fps,
                 (unsigned long)max_decode_us);
        mjpeg_playback_reported = true;
    }
}

static bool has_ext(const char *name, const char *ext) {
    size_t nl = strlen(name);
    size_t el = strlen(ext);
    return (nl > el) && (strcasecmp(name + nl - el, ext) == 0);
}

static void player_loop(void *arg) {
    // Show the built-in splash once, before entering directory playback.
    if (player_draw_png(startup_png, sizeof(startup_png))) {
        vTaskDelay(pdMS_TO_TICKS(4000));
    }

    // Repeat the entire directory indefinitely, reopening it on every pass
    // so files uploaded or deleted through USB are picked up automatically.
    while (1) {
        if (pause_requested) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        xSemaphoreTake(fs_mutex, portMAX_DELAY);

        DIR *dir = opendir(MOUNT_POINT);
        bool played_any = false;

        if (dir) {
            struct dirent *ent;
            while (!pause_requested && (ent = readdir(dir)) != NULL) {
                if (ent->d_type != DT_REG) continue;

                char path[320];
                snprintf(path, sizeof(path), "%s/%s", MOUNT_POINT, ent->d_name);

                if (has_ext(ent->d_name, ".lz4s")) {
                    play_video_file(path, false);
                    played_any = true;
                } else if (has_ext(ent->d_name, ".mjps")) {
                    play_video_file(path, true);
                    played_any = true;
                } else if (has_ext(ent->d_name, ".png")) {
                    play_png_file(path);
                    played_any = true;
                }
            }
            closedir(dir);
        }

        xSemaphoreGive(fs_mutex);

        // Yield even when all candidate files are corrupt or empty.
        vTaskDelay(played_any ? 1 : pdMS_TO_TICKS(200));
    }
}

void player_start(void) {
    fs_mutex = xSemaphoreCreateMutex();
    if (!frame_buf) {
        frame_buf = (uint16_t *)heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_DMA);
    }
    comp_buf  = (uint8_t *)malloc(MAX_COMP_BUF);
    configASSERT(fs_mutex && frame_buf && comp_buf);
    xTaskCreate(player_loop, "player_task", 8192, NULL, 5, NULL);
}

void player_pause(void) {
    pause_requested = true;
    xSemaphoreTake(fs_mutex, portMAX_DELAY);
}

void player_resume(void) {
    pause_requested = false;
    xSemaphoreGive(fs_mutex);
}
