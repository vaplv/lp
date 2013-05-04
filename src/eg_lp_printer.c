#include "lp.h"
#include "lp_font.h"
#include "lp_printer.h"
#include <font_rsrc.h>
#include <rb/rbi.h>
#include <snlsys/math.h>
#include <snlsys/mem_allocator.h>
#include <wm/wm_device.h>
#include <wm/wm_window.h>
#include <stdbool.h>
#include <string.h>

int
main(int argc, char** argv)
{
  /* Check command arguments */
  if(argc != 3) {
    printf("usage: %s RB_DRIVER FONT\n", argv[0]);
    return -1;
  }
  const char* driver_name = argv[1];
  const char* font_name = argv[2];

  FILE* file = fopen(driver_name, "r");
  if(!file) {
    fprintf(stderr, "Invalid driver %s\n", driver_name);
    return -1;
  }
  fclose(file);

  file = fopen(font_name, "r");
  if(!file) {
    fprintf(stderr, "Invalid font name %s\n", font_name);
    return -1;
  }
  fclose(file);

  /* Spawn a drawable windows */ 
  struct wm_device* device = NULL;
  struct wm_window* window = NULL;
  const struct wm_window_desc win_desc = 
    { .width = 800, .height = 600, .fullscreen = false };
  WM(create_device(NULL, &device));
  WM(create_window(device, &win_desc, &window));

  /* Create a render backend */
  struct rbi rbi;
  struct rb_context* rb_ctxt = NULL;
  CHECK(rbi_init(driver_name, &rbi), 0);
  RBI(&rbi, create_context(NULL, &rb_ctxt));

  /* Load font resource */
  struct font_system* font_sys = NULL;
  struct font_rsrc* font_rsrc = NULL;
  bool is_font_scalable = false;
  uint16_t line_space = 0;
  FONT(system_create(NULL, &font_sys));
  FONT(rsrc_create(font_sys, font_name, &font_rsrc));
  FONT(rsrc_get_line_space(font_rsrc, &line_space));
  FONT(rsrc_is_scalable(font_rsrc, &is_font_scalable));
  if(is_font_scalable) {
    FONT(rsrc_set_size(font_rsrc, 24, 24));
  }

  /* Build x charset description */
  uint16_t glyph_min_width = UINT16_MAX;
  const wchar_t* charset = 
    L"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
    L" &~\"#'{([-|`_\\^@)]=}+$%*,?;.:/!<>";
  const size_t charset_len = wcslen(charset);
  size_t i = 0;
  struct lp_font_glyph_desc lp_font_glyph_desc_list[512];
  unsigned char* glyph_bitmap_list[512];
  ASSERT(charset_len <= 512);

  for(i = 0; i < charset_len; ++i) {
    struct font_glyph_desc font_glyph_desc;
    struct font_glyph* font_glyph = NULL;    
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t Bpp = 0;

    FONT(rsrc_get_glyph(font_rsrc, charset[i], &font_glyph));

    FONT(glyph_get_desc(font_glyph, &font_glyph_desc));
    glyph_min_width = MIN(font_glyph_desc.width, glyph_min_width);
    lp_font_glyph_desc_list[i].width = font_glyph_desc.width;
    lp_font_glyph_desc_list[i].character = font_glyph_desc.character;
    lp_font_glyph_desc_list[i].bitmap_left = font_glyph_desc.bbox.x_min;
    lp_font_glyph_desc_list[i].bitmap_top = font_glyph_desc.bbox.y_min;

    FONT(glyph_get_bitmap(font_glyph, true, &width, &height, &Bpp, NULL));
    glyph_bitmap_list[i] = MEM_CALLOC
      (&mem_default_allocator, (size_t)width*height, (size_t)Bpp);
    NCHECK(glyph_bitmap_list[i], NULL);
    FONT(glyph_get_bitmap
      (font_glyph, true, &width, &height, &Bpp, glyph_bitmap_list[i]));

    lp_font_glyph_desc_list[i].bitmap.width = width;
    lp_font_glyph_desc_list[i].bitmap.height = height;
    lp_font_glyph_desc_list[i].bitmap.bytes_per_pixel = Bpp;
    lp_font_glyph_desc_list[i].bitmap.buffer = glyph_bitmap_list[i];

    FONT(glyph_ref_put(font_glyph));
  }

  /* Create the lp system and font  */
  struct lp* lp = NULL;
  struct lp_font* lp_font = NULL;
  LP(create(&rbi, rb_ctxt, NULL, &lp));
  LP(font_create(lp, &lp_font));
  LP(font_set_data
    (lp_font, line_space, (uint32_t)charset_len, lp_font_glyph_desc_list));

  /* Create the printer */
  struct lp_printer* lp_printer = NULL;
  LP(printer_create(lp, &lp_printer));
  LP(printer_set_font(lp_printer, lp_font));
  LP(printer_set_viewport(lp_printer, 0, 0, win_desc.width, win_desc.height));
  for(;;) {
    LP(printer_print_wstring
      (lp_printer, 50, 50, L"Test", (float[]){1.f, 1.f, 1.f}));
    LP(printer_flush(lp_printer));
    WM(swap(window));
  }

  /* Release data */
  for(i = 0; i < charset_len; ++i) {
    MEM_FREE(&mem_default_allocator, glyph_bitmap_list[i]);
  }
  LP(printer_ref_put(lp_printer));
  LP(font_ref_put(lp_font));
  LP(ref_put(lp));
  FONT(rsrc_ref_put(font_rsrc));
  FONT(system_ref_put(font_sys));
  RBI(&rbi, context_ref_put(rb_ctxt));
  CHECK(rbi_shutdown(&rbi), 0);
  WM(window_ref_put(window));
  WM(device_ref_put(device));

  CHECK(MEM_ALLOCATED_SIZE(&mem_default_allocator), 0);

  return 0;
}
