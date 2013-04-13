#include "lp.h"
#include "lp_font.h"
#include <font_rsrc.h>
#include <rb/rbi.h>
#include <snlsys/image.h>
#include <snlsys/math.h>
#include <snlsys/mem_allocator.h>
#include <wm/wm_device.h>
#include <wm/wm_window.h>
#include <stdbool.h>
#include <string.h>

#define BAD_ARG LP_INVALID_ARGUMENT
#define OK LP_NO_ERROR

int
main(int argc, char** argv)
{
  /* Miscellaneous data */
  const char* driver_name = NULL;
  const char* font_name = NULL;
  FILE* file = NULL;
  const uint8_t nb_glyphs = 94;
  const uint8_t nb_duplicated_glyphs = 5;
  const uint8_t total_nb_glyphs = nb_glyphs + nb_duplicated_glyphs;
  const unsigned char* bmp_cache = NULL;
  uint32_t w = 0;
  uint32_t h = 0;
  uint8_t Bpp = 0;
  uint16_t min_width = 0;
  uint16_t line_space = 0;
  int i = 0;
  int err = 0;
  bool b = false;

  /* render backend data structure */
  struct rbi rbi;
  struct rb_context* rb_ctxt = NULL;

  /* Resources data */
  unsigned char* glyph_bitmap_list[total_nb_glyphs];
  struct font_system* font_sys = NULL;
  struct font_rsrc* font_rsrc = NULL;
  struct font_glyph* font_glyph = NULL;

  /* Window manager data structures */
  struct wm_device* device = NULL;
  struct wm_window* window = NULL;
  struct wm_window_desc win_desc = {
    .width = 1, .height = 1, .fullscreen = false
  };

  /* LP data */
  struct lp_font_metrics lp_font_metrics;
  struct lp_font_glyph_desc lp_font_glyph_desc_list[total_nb_glyphs];
  struct lp_font* lp_font = NULL;
  struct lp* lp = NULL;

  /* Check command arguments */
  if(argc != 3) {
    printf("usage: %s RB_DRIVER FONT\n", argv[0]);
    goto error;
  }
  driver_name = argv[1];
  font_name = argv[2];

  file = fopen(driver_name, "r");
  if(!file) {
    fprintf(stderr, "Invalid driver %s\n", driver_name);
    goto error;
  }
  fclose(file);

  file = fopen(font_name, "r");
  if(!file) {
    fprintf(stderr, "Invalid font name %s\n", font_name);
    goto error;
  }
  fclose(file);

  WM(create_device(NULL, &device));
  WM(create_window(device, &win_desc, &window));

  FONT(system_create(NULL, &font_sys));
  FONT(rsrc_create(font_sys, font_name, &font_rsrc));
  if(FONT(rsrc_is_scalable(font_rsrc, &b)), b) {
    FONT(rsrc_set_size(font_rsrc, 24, 24));
  }

  min_width = UINT16_MAX;
  for(i = 0; i < total_nb_glyphs; ++i) {
    struct font_glyph_desc font_glyph_desc;
    wchar_t character = 33 + (i % nb_glyphs);
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t Bpp = 0;

    FONT(rsrc_get_glyph(font_rsrc, character, &font_glyph));
    FONT(glyph_get_bitmap(font_glyph, true, &width, &height, &Bpp, NULL));

    glyph_bitmap_list[i]=MEM_CALLOC(&mem_default_allocator, width*height, Bpp);
    NCHECK(glyph_bitmap_list[i], NULL);
    FONT(glyph_get_bitmap
      (font_glyph, true, &width, &height, &Bpp, glyph_bitmap_list[i]));

    lp_font_glyph_desc_list[i].bitmap.width = width;
    lp_font_glyph_desc_list[i].bitmap.height = height;
    lp_font_glyph_desc_list[i].bitmap.bytes_per_pixel = Bpp;
    lp_font_glyph_desc_list[i].bitmap.buffer = glyph_bitmap_list[i];

    FONT(glyph_get_desc(font_glyph, &font_glyph_desc));
    min_width = MIN(font_glyph_desc.width, min_width);
    lp_font_glyph_desc_list[i].width = font_glyph_desc.width;
    lp_font_glyph_desc_list[i].character = font_glyph_desc.character;
    lp_font_glyph_desc_list[i].bitmap_left = font_glyph_desc.bbox.x_min;
    lp_font_glyph_desc_list[i].bitmap_top = font_glyph_desc.bbox.y_min;

    FONT(glyph_ref_put(font_glyph));
  }

  CHECK(rbi_init(driver_name, &rbi), 0);
  RBI(&rbi, create_context(NULL, &rb_ctxt));

  CHECK(lp_create(NULL, NULL, NULL, NULL), BAD_ARG);
  CHECK(lp_create(&rbi, NULL, NULL, NULL), BAD_ARG);
  CHECK(lp_create(NULL, rb_ctxt, NULL, NULL), BAD_ARG);
  CHECK(lp_create(&rbi, rb_ctxt, NULL, NULL), BAD_ARG);
  CHECK(lp_create(NULL, NULL, NULL, &lp), BAD_ARG);
  CHECK(lp_create(NULL, rb_ctxt, NULL, &lp), BAD_ARG);
  CHECK(lp_create(&rbi, rb_ctxt, NULL, &lp), OK);

  CHECK(lp_font_create(NULL, NULL), BAD_ARG);
  CHECK(lp_font_create(lp, NULL), BAD_ARG);
  CHECK(lp_font_create(NULL, &lp_font), BAD_ARG);
  CHECK(lp_font_create(lp, &lp_font), OK);

  CHECK(lp_font_get_bitmap_cache(NULL, NULL, NULL, NULL, NULL), BAD_ARG);
  CHECK(lp_font_get_bitmap_cache(NULL, &w, NULL, NULL, NULL), BAD_ARG);
  CHECK(lp_font_get_bitmap_cache(NULL, NULL, &h, NULL, NULL), BAD_ARG);
  CHECK(lp_font_get_bitmap_cache(NULL, &w, &h, NULL, NULL), BAD_ARG);
  CHECK(lp_font_get_bitmap_cache(NULL, NULL, NULL, &Bpp, NULL), BAD_ARG);
  CHECK(lp_font_get_bitmap_cache(NULL, &w, NULL, &Bpp, NULL), BAD_ARG);
  CHECK(lp_font_get_bitmap_cache(NULL, NULL, &h, &Bpp, NULL), BAD_ARG);
  CHECK(lp_font_get_bitmap_cache(NULL, &w, &h, &Bpp, NULL), BAD_ARG);
  CHECK(lp_font_get_bitmap_cache(NULL, NULL, NULL, NULL, &bmp_cache), BAD_ARG);
  CHECK(lp_font_get_bitmap_cache(NULL, &w, NULL,NULL, &bmp_cache), BAD_ARG);
  CHECK(lp_font_get_bitmap_cache(NULL, NULL, &h,NULL,&bmp_cache), BAD_ARG);
  CHECK(lp_font_get_bitmap_cache(NULL, &w, &h, NULL,&bmp_cache), BAD_ARG);
  CHECK(lp_font_get_bitmap_cache(NULL, NULL, NULL, &Bpp, &bmp_cache), BAD_ARG);
  CHECK(lp_font_get_bitmap_cache(NULL, &w, NULL, &Bpp, &bmp_cache), BAD_ARG);
  CHECK(lp_font_get_bitmap_cache(NULL, NULL, &h, &Bpp, &bmp_cache), BAD_ARG);
  CHECK(lp_font_get_bitmap_cache(NULL, &w, &h, &Bpp,&bmp_cache), BAD_ARG);
  CHECK(lp_font_get_bitmap_cache(lp_font, NULL, NULL, NULL, NULL), OK);
  CHECK(lp_font_get_bitmap_cache(lp_font, &w, NULL, NULL, NULL), OK);
  CHECK(lp_font_get_bitmap_cache(lp_font, NULL, &h, NULL, NULL), OK);
  CHECK(lp_font_get_bitmap_cache(lp_font, &w, &h, NULL, NULL), OK);
  CHECK(lp_font_get_bitmap_cache(lp_font, NULL, NULL, &Bpp, NULL), OK);
  CHECK(lp_font_get_bitmap_cache(lp_font, &w, NULL, &Bpp, NULL), OK);
  CHECK(lp_font_get_bitmap_cache(lp_font, NULL, &h, &Bpp, NULL), OK);
  CHECK(lp_font_get_bitmap_cache(lp_font, &w, &h, &Bpp, NULL), OK);
  CHECK(lp_font_get_bitmap_cache(lp_font, NULL, NULL, NULL, &bmp_cache), OK);
  CHECK(lp_font_get_bitmap_cache(lp_font, &w, NULL, NULL, &bmp_cache), OK);
  CHECK(lp_font_get_bitmap_cache(lp_font, NULL, &h, NULL, &bmp_cache), OK);
  CHECK(lp_font_get_bitmap_cache(lp_font, &w, &h, NULL, &bmp_cache), OK);
  CHECK(lp_font_get_bitmap_cache(lp_font, NULL, NULL, &Bpp, &bmp_cache), OK);
  CHECK(lp_font_get_bitmap_cache(lp_font, &w, NULL, &Bpp, &bmp_cache), OK);
  CHECK(lp_font_get_bitmap_cache(lp_font, NULL, &h, &Bpp, &bmp_cache), OK);
  CHECK(lp_font_get_bitmap_cache(lp_font, &w, &h, &Bpp, &bmp_cache), OK);

  w = h = Bpp = 1;
  bmp_cache = (void*)0xdeadbeef;
  CHECK(lp_font_get_bitmap_cache(lp_font, &w, &h, &Bpp, &bmp_cache), OK);
  CHECK(bmp_cache, NULL);
  CHECK(w, 0);
  CHECK(h, 0);
  CHECK(Bpp, 0);

  CHECK(lp_font_set_data(NULL, 0, 0, NULL), BAD_ARG);
  CHECK(lp_font_set_data(lp_font, 0, 0, NULL), OK);


  FONT(rsrc_get_line_space(font_rsrc, &line_space));

  CHECK(lp_font_set_data
    (lp_font, line_space, nb_glyphs, lp_font_glyph_desc_list), OK);
  CHECK(lp_font_get_bitmap_cache(lp_font, &w, &h, &Bpp, &bmp_cache), OK);
  NCHECK(bmp_cache, NULL);
  NCHECK(w, 0);
  NCHECK(h, 0);
  NCHECK(Bpp, 0);

  CHECK(image_ppm_write("/tmp/font_cache.ppm", w, h, Bpp, bmp_cache), 0);

  CHECK(lp_font_get_metrics(NULL, NULL), BAD_ARG);
  CHECK(lp_font_get_metrics(lp_font, NULL), BAD_ARG);
  CHECK(lp_font_get_metrics(NULL, &lp_font_metrics), BAD_ARG);
  CHECK(lp_font_get_metrics(lp_font, &lp_font_metrics), OK);
  CHECK(lp_font_get_metrics(lp_font, &lp_font_metrics), OK);
  CHECK(lp_font_metrics.line_space, line_space);
  CHECK(lp_font_metrics.min_glyph_width, min_width);

  CHECK(lp_font_ref_get(NULL), BAD_ARG);
  CHECK(lp_font_ref_get(lp_font), OK);
  CHECK(lp_font_ref_put(NULL), BAD_ARG);
  CHECK(lp_font_ref_put(lp_font), OK);
  CHECK(lp_font_ref_put(lp_font), OK);

  CHECK(lp_ref_get(NULL), BAD_ARG);
  CHECK(lp_ref_get(lp), OK);
  CHECK(lp_ref_put(NULL), BAD_ARG);
  CHECK(lp_ref_put(lp), OK);
  CHECK(lp_ref_put(lp), OK);

  for(i = 0; i < total_nb_glyphs; ++i)
    MEM_FREE(&mem_default_allocator, glyph_bitmap_list[i]);

  FONT(rsrc_ref_put(font_rsrc));
  FONT(system_ref_put(font_sys));
  RBI(&rbi, context_ref_put(rb_ctxt));
  CHECK(rbi_shutdown(&rbi), 0);
  WM(window_ref_put(window));
  WM(device_ref_put(device));

  CHECK(MEM_ALLOCATED_SIZE(&mem_default_allocator), 0);

exit:
  return err;

error:
  err = -1;
  goto exit;
}

