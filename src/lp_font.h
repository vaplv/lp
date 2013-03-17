#ifndef LP_FONT_H
#define LP_FONT_H

#include "lp.h"
#include <snlsys/signal.h>
#include <snlsys/snlsys.h>
#include <wchar.h>

/*******************************************************************************
 *
 * Font types
 *
 ******************************************************************************/
/* List of available font signal */
enum lp_font_signal {
  LP_FONT_SIGNAL_DATA_UPDATE,
  LP_FONT_SIGNALS_COUNT
};

struct lp_font;
struct rb_tex2d; /* Forward declaration of the rb texture type */

/* Declare the callback type lp_font_callback_T */
CALLBACK(lp_font_callback_T, struct lp_font*);

/* Descriptor of a glyph to registered against the font */
struct lp_font_glyph_desc {
   wchar_t character;
   uint16_t width;
   int bitmap_left;
   int bitmap_top;
   struct {
     uint16_t width;
     uint16_t height;
     uint8_t bytes_per_pixel;
     unsigned char* buffer;
   } bitmap;
};

/* Information on registered glyph */
struct lp_font_glyph {
  uint16_t width;
  struct { float x; float y; } tex[2], pos[2];
};

/* Global font metrics */
struct lp_font_metrics {
  uint16_t line_space;
  uint16_t min_glyph_width;
  int min_glyph_pos_y;
};

/*******************************************************************************
 *
 * Font API
 *
 ******************************************************************************/
#ifdef __cplusplus
extern "C" {
#endif

LP_API enum lp_error
lp_font_create
  (struct lp* lp,
   struct lp_font** font);

LP_API enum lp_error
lp_font_ref_get
  (struct lp_font* font);

LP_API enum lp_error
lp_font_ref_put
  (struct lp_font* font);

LP_API enum lp_error
lp_font_set_data
  (struct lp_font* font,
   const uint16_t line_space,
   const uint32_t nb_glyphs,
   const struct lp_font_glyph_desc* glyph_list);

LP_API enum lp_error
lp_font_get_metrics
  (struct lp_font* font,
   struct lp_font_metrics* metrics);

/* Retrieve glyph information for a given character. If the character does not
 * exist, use the default character to fill the glyph information */
LP_API enum lp_error
lp_font_get_glyph
  (struct lp_font* font,
   const wchar_t character,
   struct lp_font_glyph* glyph);

LP_API enum lp_error
lp_font_get_texture
  (struct lp_font* font,
   struct rb_tex2d** tex);

LP_API enum lp_error
lp_font_get_bitmap_cache
  (const struct lp_font* font,
   uint32_t* width, /* May be NULL */
   uint32_t* height, /* May be NULL */
   uint8_t* bytes_per_pixel, /* May be NULL */
   const unsigned char** bitmap_cache); /* May be NULL */

LP_API enum lp_error
lp_font_signal_connect
  (struct lp_font* font,
   const enum lp_font_signal signal,
   lp_font_callback_T* clbk);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LP_FONT_H */

