/* collab_items=earn (src/runtime_collab.c): the shop sheet in a made-up two-stream archive gets
 * the parts on sale on floor 16 at 3 to 5 times the tab's 500 coins, with no window, and the other stream is unchanged. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/runtime_collab.c"

static const uint16_t list[] = {8965, 8984};
const uint16_t *runtime_game_golf_collab_list(size_t *count) { *count = 2; return list; }
int resogun_collab_items(void) { return 2; }
const char *runtime_file_user_dir(void) { return "."; }

static const char *const header =
    "\xef\xbb\xbfshop_id\tfloor_id\tsort_id\tchange_item\tannounce_notice\tannounce_report\tplayer_rank\titem_id\t"
    "ProductLabel\tdrawing_group\tdrawing_candidates\tdrawing_bias\tintl_group\tintl_bias\tprice\tsales_num\t"
    "attention_flug\trare_reward\tmonth_limited\tmonth_limited_weight\tsale_sun\tsale_mon\tsale_tue\tsale_wed\t"
    "sale_thu\tsale_fri\tsale_sat\tsale_open_hour\tsale_open_min\tsale_close_hour\tsale_close_min\t"
    "sale_open_year\tsale_open_month\tsale_open_month_shop\tsale_open_day\tsale_close_year\tsale_close_month\tsale_close_day\r\n";
/* Atelier's hair ornament in its 2018 window, and a ball on floor 17. */
static const char *const atelier = "0\t16\t61\t\t\t\t\t8965\t\t\t\t\t\t\t500\t1\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t2018\t8\t8\t31\t2018\t9\t28\r\n";
static const char *const ball = "0\t17\t2016\t\t\t>1\t\t7385\t\t\t\t\t\t\t180\t2\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\r\n";

static size_t pack(unsigned char *out, const char *text) {
    uLongf n = compressBound(strlen(text));
    assert(compress2(out, &n, (const unsigned char *)text, strlen(text), 9) == Z_OK);
    return n;
}

int main(void) {
    char sheet[4096];
    snprintf(sheet, sizeof sheet, "%s%s%s", header, atelier, ball);
    unsigned char a[4096], b[256], file[8192];
    const size_t na = pack(a, sheet), nb = pack(b, "other\tstream\r\n");
    /* Row 0 (lower hash): "other", data in stream 1; row 1: the sheet, data in stream 0. Row k's
     * sizes are stream k's. */
    const uint32_t sheet_hash = name_hash("data/param/serverFile/shop_price_sheet.txt");
    uint32_t rows[2][5] = {{sheet_hash - 1, 0, 1, (uint32_t)na, (uint32_t)strlen(sheet)},
                           {sheet_hash, 0, 0, (uint32_t)nb, 14}};
    const uint32_t head[4] = {0x4246, 2, 4, 0};
    memcpy(file, head, 16);
    memcpy(file + 16, rows, sizeof rows);
    size_t o = 56;
    memcpy(file + o, a, na); o += na; while (o & 3) file[o++] = 0;
    memcpy(file + o, b, nb); o += nb; while (o & 3) file[o++] = 0;

    unsigned char *patched = NULL;
    size_t size = 0;
    unsigned opened = 0, added = 0;
    assert(!runtime_collab_patch_archive(file, o, &patched, &size, &opened, &added));
    assert(opened == 1 && added == 1);
    uint32_t out_rows[2][5];
    memcpy(out_rows, patched + 16, sizeof out_rows);
    assert(out_rows[1][3] == nb && out_rows[1][4] == 14 && out_rows[0][2] == 1);
    char text[8192];
    uLongf n = sizeof text - 1;
    assert(uncompress((unsigned char *)text, &n, patched + 56, out_rows[0][3]) == Z_OK && n == out_rows[0][4]);
    text[n] = 0;
    const size_t second = (56 + out_rows[0][3] + 3) & ~(size_t)3;
    assert(!memcmp(patched + second, b, nb) && second + ((nb + 3) & ~(size_t)3) == size);
    assert(strstr(text, header) == text && strstr(text, ball));
    /* The Atelier row without its window, the Jibanyan suit added after the last sort id. */
    assert(strstr(text, "0\t16\t61\t\t\t\t\t8965\t\t\t\t\t\t\t1500\t1\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\r\n"));
    assert(strstr(text, "0\t16\t2017\t\t\t\t\t8984\t\t\t\t1\t\t\t2500\t1\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\r\n"));
    assert(!strstr(text, "2018"));
    /* Not an archive, or no shop sheet in it: nothing. */
    assert(runtime_collab_patch_archive((const unsigned char *)"FB", 2, &patched, &size, &opened, &added));
    rows[1][0] = sheet_hash + 1;
    memcpy(file + 16, rows, sizeof rows);
    unsigned char *none = NULL;
    assert(runtime_collab_patch_archive(file, o, &none, &size, &opened, &added) && !none);
    free(patched);
    puts("collab: ok");
    return 0;
}
