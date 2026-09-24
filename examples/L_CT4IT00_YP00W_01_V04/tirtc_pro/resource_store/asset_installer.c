#include "asset_installer.h"
#include "asset_io.h"
#include "asset_manifest.h"
#include "../storage/tirtc_storage.h"
#include "liot_log.h"
#include "liot_os.h"

static int write_asset(const char *path, const uint8_t *blob, uint32_t size, uint32_t crc)
{
    uint32_t offset = 0;
    int result = 0, closed;
    int fd = tirtc_storage_open(path, "w");
    if (fd < 0) return fd;
    while (offset < size) {
        uint32_t amount = size - offset;
        if (amount > 4096U) amount = 4096U;
        int count = tirtc_storage_write(fd, blob + offset, amount);
        /* A partial write is a failure; do not replay any failed operation. */
        if (count < 0 || (uint32_t)count != amount) {
            result = count < 0 ? count : TIRTC_STORAGE_E_IO;
            break;
        }
        offset += amount;
        liot_rtos_task_sleep_ms(1U);
    }
    if (!result) result = tirtc_storage_sync(fd);
    closed = tirtc_storage_close(fd);
    if (!result) result = closed;
    if (result) return result;
    return tirtc_asset_read_verified(path, NULL, size, crc, NULL);
}

int tirtc_assets_install(void)
{
    tirtc_storage_snapshot_t storage;
    int font, background, result = 0;
    uint32_t needed = 8192U; /* Conservative allowance for metadata/COW. */
    tirtc_storage_get_snapshot(&storage);
    if (!storage.filesystem_valid || (storage.state != TIRTC_STORAGE_STATE_READY &&
        storage.state != TIRTC_STORAGE_STATE_READY_LIMITED)) return TIRTC_STORAGE_E_NOT_READY;
    /* Validate embedded input before any mutation. */
    if ((tirtc_asset_crc_update(UINT32_MAX, tirtc_install_font, TIRTC_FONT_BYTES) ^ UINT32_MAX) != TIRTC_FONT_CRC32 ||
        (tirtc_asset_crc_update(UINT32_MAX, tirtc_install_background, TIRTC_BACKGROUND_BYTES) ^ UINT32_MAX) != TIRTC_BACKGROUND_CRC32)
        return TIRTC_ASSET_E_MISMATCH;
    font = tirtc_asset_read_verified(TIRTC_ASSET_FONT_PATH, NULL, TIRTC_FONT_BYTES, TIRTC_FONT_CRC32, NULL);
    if (font && font != TIRTC_ASSET_E_MISSING && font != TIRTC_ASSET_E_MISMATCH) return font;
    background = tirtc_asset_read_verified(TIRTC_ASSET_BACKGROUND_PATH, NULL,
                                          TIRTC_BACKGROUND_BYTES, TIRTC_BACKGROUND_CRC32, NULL);
    if (background && background != TIRTC_ASSET_E_MISSING && background != TIRTC_ASSET_E_MISMATCH) return background;
    if (font || background) {
        if (storage.read_only) return TIRTC_STORAGE_E_PROTECTED;
        /* Re-sample after verification, accounting for any concurrent state
         * change. The standalone installer is the only filesystem client. */
        tirtc_storage_poll();
        tirtc_storage_get_snapshot(&storage);
        if (!storage.filesystem_valid || storage.read_only) return TIRTC_STORAGE_E_NOT_READY;
        if (font) needed += (TIRTC_FONT_BYTES + 4095U) & ~4095U;
        if (background) needed += (TIRTC_BACKGROUND_BYTES + 4095U) & ~4095U;
        if (storage.filesystem_free_bytes < needed) return TIRTC_ASSET_E_SPACE;
    }
    if (font) result = write_asset(TIRTC_ASSET_FONT_PATH, tirtc_install_font, TIRTC_FONT_BYTES, TIRTC_FONT_CRC32);
    if (!result && background)
        result = write_asset(TIRTC_ASSET_BACKGROUND_PATH, tirtc_install_background,
                             TIRTC_BACKGROUND_BYTES, TIRTC_BACKGROUND_CRC32);
    if (result) return result;
    /* Final pair validation before the only success marker. */
    result = tirtc_asset_read_verified(TIRTC_ASSET_FONT_PATH, NULL, TIRTC_FONT_BYTES, TIRTC_FONT_CRC32, NULL);
    if (!result) result = tirtc_asset_read_verified(TIRTC_ASSET_BACKGROUND_PATH, NULL,
                                                  TIRTC_BACKGROUND_BYTES, TIRTC_BACKGROUND_CRC32, NULL);
    if (!result) liot_trace("[assets14] INSTALL_OK font=%lu bg=%lu font_skip=%d bg_skip=%d\r\n",
                            (unsigned long)TIRTC_FONT_BYTES, (unsigned long)TIRTC_BACKGROUND_BYTES,
                            font == 0, background == 0);
    return result;
}
