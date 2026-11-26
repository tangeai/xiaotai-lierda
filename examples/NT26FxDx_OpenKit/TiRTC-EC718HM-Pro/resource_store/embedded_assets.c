/* Built-in counterpart of assets.c. Select exactly one backend in the build.
 * The existing UI sources contain the original immutable resources when
 * TIRTC_EXTERNAL_UI_ASSETS is absent. No filesystem, SPI, loader task or
 * duplicate resource array is needed for boards with enough application Flash.
 */
#include "assets.h"
#include "asset_io.h"
#include "asset_manifest.h"
#include "../ui/font/tirtc_font_14.h"
#include "../ui/font/tirtc_font_digits_48.h"
#include "../ui/assets/modern_icons.h"
#include "../ui/assets/ui_face_background.h"
#include "../ui/ui_face.h"
#include <stddef.h>

#ifdef TIRTC_EXTERNAL_UI_ASSETS
#error "embedded_assets.c requires the built-in UI resource branches"
#endif

typedef char embedded_background_size_check[
    sizeof(ui_face_background_pixels) == TIRTC_BACKGROUND_BYTES ? 1 : -1];

static const lv_font_fmt_txt_dsc_t *embedded_font(void)
{
    return (const lv_font_fmt_txt_dsc_t *)tirtc_font_14.dsc;
}

/* This checks linked descriptors, not an external-file load or CRC pass.
 * Resource byte equivalence is verified on the host against assets14 before
 * release; immutable application bytes share the firmware's verification.
 */
static bool embedded_resources_valid(void)
{
    const lv_font_fmt_txt_dsc_t *font = embedded_font();
    const lv_font_fmt_txt_dsc_t *digits =
        (const lv_font_fmt_txt_dsc_t *)tirtc_font_digits_48.dsc;
    return font && font->glyph_bitmap && font->glyph_dsc && font->cmaps &&
        font->cmap_num == 1U && font->bpp == 4U &&
        font->cmaps[0].list_length == 7010U &&
        digits && digits->glyph_bitmap && digits->glyph_dsc && digits->cmaps &&
        tirtc_font_14.get_glyph_bitmap && tirtc_font_digits_48.get_glyph_bitmap &&
        ui_face_background_image.data == (const uint8_t *)ui_face_background_pixels &&
        ui_face_background_image.data_size == TIRTC_BACKGROUND_BYTES &&
        ui_phone_image.data && ui_phone_image.data_size == TIRTC_ICON_PHONE_BYTES &&
        ui_wechat_qr_image.data && ui_wechat_qr_image.data_size == TIRTC_ICON_WECHAT_BYTES &&
        ui_face_key_count() != 0U && ui_face_key_at(0U) != NULL;
}

int tirtc_assets_start(void)
{
    return embedded_resources_valid() ? 0 : TIRTC_ASSET_E_MISMATCH;
}

bool tirtc_assets_ready(void)
{
    return embedded_resources_valid();
}

int tirtc_assets_error(void)
{
    return embedded_resources_valid() ? 0 : TIRTC_ASSET_E_MISMATCH;
}

const uint8_t *tirtc_assets_font(void)
{
    return embedded_resources_valid() ? embedded_font()->glyph_bitmap : NULL;
}

const uint8_t *tirtc_assets_glyph(uint32_t offset, uint32_t bytes)
{
    const uint8_t *font = tirtc_assets_font();
    if (!font || !bytes || offset > TIRTC_FONT_BYTES ||
        bytes > TIRTC_FONT_BYTES - offset) return NULL;
    return font + offset;
}

bool tirtc_assets_font_process(void)
{
    /* Immutable glyphs are available synchronously; no completion mailbox. */
    return false;
}

const uint16_t *tirtc_assets_background(void)
{
    return embedded_resources_valid() ? ui_face_background_pixels : NULL;
}

void tirtc_assets_get_snapshot(tirtc_assets_snapshot_t *out)
{
    bool valid;
    if (!out) return;
    valid = embedded_resources_valid();
    *out = (tirtc_assets_snapshot_t){0};
    out->state = valid ? TIRTC_ASSETS_READY : TIRTC_ASSETS_ERROR;
    out->error = valid ? 0 : TIRTC_ASSET_E_MISMATCH;
    out->total_bytes = TIRTC_UI_ASSET_TOTAL_BYTES;
    /* loaded_bytes/attempts stay zero: this backend never loads files. */
}
