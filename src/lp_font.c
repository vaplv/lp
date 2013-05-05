#include "lp_c.h"
#include "lp_error_c.h"
#include "lp_font.h"

#include <sl/sl.h>
#include <sl/sl_hash_table.h>

#include <snlsys/mem_allocator.h>
#include <snlsys/math.h>
#include <snlsys/snlsys.h>

#include <rb/rbi.h>

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_CHAR ((wchar_t)~0)

struct lp_font {
  /* Miscellaneous data */
  struct ref ref; /* Ref counting */
  struct lp* lp; /* Owner ship on the system frow which the font was created */
  SIGNALS_LIST(signals, lp_font_callback_T, LP_FONT_SIGNALS_COUNT);

  /* Image and its associated texture in which font glyphes are stored */
  struct {
    uint32_t width;
    uint32_t height;
    uint8_t Bpp;
    unsigned char* buffer;
  } cache_img;
  struct rb_tex2d* cache_tex;

  /* Information on registered glyphes */
  struct sl_hash_table* glyph_htbl;

  /* Global font metrics */
  uint16_t line_space;
  uint16_t min_glyph_width;
  int16_t  min_glyph_pos_y;
};

/*******************************************************************************
 *
 * Glyph hash table
 *
 ******************************************************************************/
static size_t
hash(const void* key)
{
  return sl_hash(key, sizeof(wchar_t));
}

static bool
eq_key(const void* p0, const void* p1)
{
  return *(const wchar_t*)p0 == *(const wchar_t*)p1;
}

/*******************************************************************************
 *
 * Helper functions and data structure.
 *
 ******************************************************************************/
enum extendable_flag {
  EXTENDABLE_X = BIT(0),
  EXTENDABLE_Y = BIT(1)
};

struct node {
  struct node* left;
  struct node* right;
  uint32_t x, y;
  uint32_t width, height;
  uint32_t id;
  int extendable_flag;
};

#define GLYPH_BORDER 1u
#define IS_LEAF(n) (!((n)->left || (n)->right))

static struct node*
insert_rect
  (struct mem_allocator* allocator,
   struct node* node,
   const uint16_t width,
   const uint16_t height)
{
  struct node* ret_node = NULL;

  if(!node) {
    ret_node = NULL;
  } else if(!IS_LEAF(node)) {
    ret_node = insert_rect(allocator, node->left, width, height);
    if(!ret_node && node->right)
      ret_node = insert_rect(allocator, node->right, width, height);
  } else {
    /* Adjust the width and height in order to take care of the glyph border */
    const uint16_t width_adjusted = (uint16_t)(width + GLYPH_BORDER);
    const uint16_t height_adjusted = (uint16_t)(height + GLYPH_BORDER);

    if(width_adjusted > node->width || height_adjusted > node->height) {
      /* The leaf is too small to store the rectangle. */
      ret_node = NULL;
    } else {
      const uint32_t w = node->width - width_adjusted;
      const uint32_t h = node->height - height_adjusted;

      node->left = MEM_CALLOC(allocator, 1, sizeof(struct node));
      ASSERT(node->left);
      node->right = MEM_CALLOC(allocator, 1, sizeof(struct node));
      ASSERT(node->right);

      if(w > h) {
        /* +-----+
         * |R |  | ##: current node
         * +--+L | L : left node
         * |##|  | R : right node
         * +--+--+
         */
        node->left->x = node->x + width_adjusted;
        node->left->y = node->y;
        node->left->width = w;
        node->left->height = node->height;
        node->left->extendable_flag = node->extendable_flag;

        node->right->x = node->x;
        node->right->y = node->y + height_adjusted;
        node->right->width = width_adjusted;
        node->right->height = h;
        node->right->extendable_flag = node->extendable_flag & (~EXTENDABLE_X);
     } else {
        /* +-------+
         * |   L   | ##: current node
         * +--+----+ L : left node
         * |##| R  | R : right node
         * +--+----+
         */
        node->left->x = node->x;
        node->left->y = node->y + height_adjusted;
        node->left->width = node->width;
        node->left->height = h;
        node->left->extendable_flag = node->extendable_flag;

        node->right->x = node->x + width_adjusted;
        node->right->y = node->y;
        node->right->width = w;
        node->right->height = height_adjusted;
        node->right->extendable_flag = node->extendable_flag & (~EXTENDABLE_Y);
      }
      node->width = width_adjusted;
      node->height = height_adjusted;
      node->extendable_flag = 0;
      ret_node = node;
    }
  }
  return ret_node;
}
static void
extend_width(struct node* node, const uint16_t size)
{
  ASSERT(node);
  if(!IS_LEAF(node)) {
    extend_width(node->left, size);
    extend_width(node->right, size);
  } else {
    if((node->extendable_flag & EXTENDABLE_X) != 0) {
      node->width += size;
    }
  }
}

static void
extend_height(struct node* node, const uint16_t size)
{
  ASSERT(node);
  if(!IS_LEAF(node)) {
    extend_height(node->left, size);
    extend_height(node->right, size);
  } else {
    if((node->extendable_flag & EXTENDABLE_Y) != 0) {
      node->height += size;
    }
  }
}

static void
free_binary_tree(struct mem_allocator* allocator, struct node* node)
{
  if(node->left)
    free_binary_tree(allocator, node->left);
  if(node->right)
    free_binary_tree(allocator, node->right);
  MEM_FREE(allocator, node);
}

static void
copy_bitmap
  (unsigned char* restrict dst,
   const uint32_t dst_pitch,
   const unsigned char* restrict src,
   const uint32_t src_pitch,
   const uint16_t width,
   const uint16_t height,
   const uint8_t Bpp)
{
  uint16_t i = 0;

  ASSERT(dst && dst_pitch && src && src_pitch && width && height && Bpp);
  ASSERT(!IS_MEMORY_OVERLAPPED(dst, height*dst_pitch, src, height*src_pitch));
  for(i = 0; i < height; ++i) {
    unsigned char* dst_row = dst + i * dst_pitch;
    const unsigned char* src_row = src + i * src_pitch;
    memcpy(dst_row, src_row, (size_t)(width * Bpp));
  }
}

static void
compute_initial_cache_size
  (const uint32_t nb_glyphs,
   const struct lp_font_glyph_desc* glyph_list,
   uint32_t* out_width,
   uint32_t* out_height)
{
  uint32_t i = 0;
  uint16_t width = 0;
  uint16_t height = 0;

  ASSERT(glyph_list && out_width && out_height);

  for(i = 0; i < nb_glyphs; ++i) {
    width = MAX(glyph_list[i].bitmap.width, width);
    height = MAX(glyph_list[i].bitmap.height, height);
  }
  /* We multiply the max size required by a glyph by 4 in each dimension in
   * order to store at least 16 glyphs in the texture. */
  *out_width = (width + GLYPH_BORDER) * 4;
  *out_height = (height + GLYPH_BORDER) * 4;
}

static void
fill_font_cache
  (const struct node* node,
   struct lp_font* font,
   const struct lp_font_glyph_desc* glyph_list)
{
  ASSERT(node && font);
  if(!IS_LEAF(node)) {
    struct lp_font_glyph* glyph = NULL;
    const struct lp_font_glyph_desc* glyph_desc = glyph_list + node->id;
    const uint8_t cache_Bpp = font->cache_img.Bpp;
    const uint32_t cache_pitch = font->cache_img.width * cache_Bpp;
    const float rcp_cache_width = 1.f / (float)font->cache_img.width;
    const float rcp_cache_height = 1.f / (float)font->cache_img.height;
    const uint32_t w = node->width - GLYPH_BORDER;
    const uint32_t h = node->height - GLYPH_BORDER;
    const uint32_t x = node->x == 0 ? GLYPH_BORDER : node->x;
    const uint32_t y = node->y == 0 ? GLYPH_BORDER : node->y;
    const uint32_t glyph_bmp_size = (uint32_t)
      glyph_desc->bitmap.width
    * glyph_desc->bitmap.height
    * glyph_desc->bitmap.bytes_per_pixel;

    SL(hash_table_find
      (font->glyph_htbl, &glyph_desc->character,(void**)&glyph));
    ASSERT(glyph);

    glyph->width = glyph_desc->width;
    glyph->tex[0].x = (float)x * rcp_cache_width;
    glyph->tex[0].y = (float)(y + h) * rcp_cache_height;
    glyph->tex[1].x = (float)(x + w) * rcp_cache_width;
    glyph->tex[1].y = (float)y * rcp_cache_height;

    glyph->pos[0].x = (float)glyph_desc->bitmap_left;
    glyph->pos[0].y = (float)glyph_desc->bitmap_top;
    glyph->pos[1].x = (float)(glyph_desc->bitmap_left + (int)w);
    glyph->pos[1].y = (float)(glyph_desc->bitmap_top + (int)h);

    /* The glyph bitmap size may be equal to zero (e.g.: the space char) */
    if(0 != glyph_bmp_size) {
      unsigned char* dst = NULL;
      ASSERT(glyph_desc->bitmap.bytes_per_pixel == cache_Bpp);
      dst = font->cache_img.buffer + y * cache_pitch + x * cache_Bpp;
      copy_bitmap
        (dst,
         cache_pitch,
         glyph_desc->bitmap.buffer,
         (uint32_t)(glyph_desc->bitmap.width * cache_Bpp),
         glyph_desc->bitmap.width,
         glyph_desc->bitmap.height,
         cache_Bpp);
    }
    fill_font_cache(node->left, font, glyph_list);
    fill_font_cache(node->right, font, glyph_list);
  }
}

static int
cmp_glyph_desc(const void* a, const void* b)
{
  const struct lp_font_glyph_desc* glyph0 = a;
  const struct lp_font_glyph_desc* glyph1 = b;
  const int area0 = glyph0->bitmap.width * glyph0->bitmap.height;
  const int area1 = glyph1->bitmap.width * glyph1->bitmap.height;
  return area1 - area0;
}

static enum rb_tex_format
Bpp_to_rb_tex_format(size_t Bpp)
{
  enum rb_tex_format tex_format = RB_R;
  switch(Bpp) {
    case 1:
      tex_format = RB_R;
      break;
    case 3:
      tex_format = RB_RGB;
      break;
    default:
      ASSERT(false);
      break;
  }
  return tex_format;
}

static void
reset_font(struct lp_font* font)
{
  ASSERT(font);

  SL(hash_table_clear(font->glyph_htbl));

  if(font->cache_tex) {
    RBI(font->lp->rbi, tex2d_ref_put(font->cache_tex));
    font->cache_tex = NULL;
  }

  if(font->cache_img.buffer)
    MEM_FREE(font->lp->allocator, font->cache_img.buffer);
  memset(&font->cache_img, 0, sizeof(font->cache_img));

  font->line_space = 0;
  SIGNAL_INVOKE(&font->signals, LP_FONT_SIGNAL_DATA_UPDATE, font);
}

static enum lp_error
create_default_glyph
  (struct mem_allocator* allocator,
   const uint16_t width,
   const uint16_t height,
   const uint8_t Bpp,
   struct lp_font_glyph_desc* glyph)
{
  unsigned char* buffer = NULL;
  const int pitch = width * Bpp;
  const int size = pitch * height;
  enum lp_error lp_err = LP_NO_ERROR;

  ASSERT(glyph && allocator);
  glyph->character = DEFAULT_CHAR;
  glyph->width = width;
  glyph->bitmap_left = 0;
  glyph->bitmap_top = 0;
  glyph->bitmap.width = width;
  glyph->bitmap.height = height;
  glyph->bitmap.bytes_per_pixel = Bpp;
  if(size) {
    buffer = MEM_CALLOC(allocator, 1, (size_t)size);
    if(NULL == buffer) {
      lp_err =  LP_MEMORY_ERROR;
      goto error;
    } else {
      uint16_t y = 0;
      memset(buffer, 0xFF, (size_t)pitch);
      memset(buffer + (height - 1) * pitch, 0xFF, (size_t)pitch);
      for(y = 1; y < height - 1; ++y) {
        memset(buffer + y * pitch, 0xFF, Bpp);
        memset(buffer + y * pitch + (width - 1) * Bpp, 0xFF, Bpp);
      }
    }
  }
exit:
  if(glyph)
    glyph->bitmap.buffer = buffer;
  return lp_err;
error:
  if(buffer)
    MEM_FREE(allocator, buffer);
  goto exit;
}

static void
free_default_glyph
  (struct mem_allocator* allocator,
   struct lp_font_glyph_desc* glyph)
{
  ASSERT(allocator && glyph);
  if(glyph->bitmap.buffer)
    MEM_FREE(allocator, glyph->bitmap.buffer);
  memset(glyph, 0, sizeof(struct lp_font_glyph_desc));
}

static void
release_font(struct ref* ref)
{
  struct lp* lp = NULL;
  struct lp_font* font = NULL;
  ASSERT(NULL != ref);

  font = CONTAINER_OF(ref, struct lp_font, ref);

  if(font->glyph_htbl)
    SL(free_hash_table(font->glyph_htbl));
  if(font->cache_tex)
    RBI(font->lp->rbi, tex2d_ref_put(font->cache_tex));
  if(font->cache_img.buffer)
    MEM_FREE(font->lp->allocator, font->cache_img.buffer);
  lp = font->lp;
  MEM_FREE(lp->allocator, font);
  LP(ref_put(lp));
}

#undef IS_LEAF

/*******************************************************************************
 *
 * Font functions.
 *
 ******************************************************************************/
enum lp_error
lp_font_create(struct lp* lp, struct lp_font** out_font)
{
  struct lp_font* font = NULL;
  enum lp_error lp_err = LP_NO_ERROR;
  enum sl_error sl_err = SL_NO_ERROR;

  if(!lp || !out_font) {
    lp_err = LP_INVALID_ARGUMENT;
    goto error;
  }
  font = MEM_CALLOC(lp->allocator, 1, sizeof(struct lp_font));
  if(!font) {
    lp_err = LP_MEMORY_ERROR;
    goto error;
  }
  ref_init(&font->ref);
  font->lp = lp;
  LP(ref_get(lp));
  SIGNALS_LIST_INIT(&font->signals);

  sl_err = sl_create_hash_table
    (sizeof(wchar_t),
     ALIGNOF(wchar_t),
     sizeof(struct lp_font_glyph),
     ALIGNOF(struct lp_font_glyph),
     hash,
     eq_key,
     font->lp->allocator,
     &font->glyph_htbl);
  if(sl_err != SL_NO_ERROR) {
    lp_err = sl_to_lp_error(sl_err);
    goto error;
  }

exit:
  if(out_font)
    *out_font = font;
  return lp_err;
error:
  if(font) {
    LP(font_ref_put(font));
    font = NULL;
  }
  goto exit;
}

enum lp_error
lp_font_ref_get(struct lp_font* font)
{
  if(!font)
    return LP_INVALID_ARGUMENT;
  ref_get(&font->ref);
  return LP_NO_ERROR;
}

enum lp_error
lp_font_ref_put(struct lp_font* font)
{
  if(!font)
    return LP_INVALID_ARGUMENT;
  ref_put(&font->ref, release_font);
  return LP_NO_ERROR;
}

enum lp_error
lp_font_set_data
  (struct lp_font* font,
   const uint16_t line_space,
   const uint32_t nb_glyphs,
   const struct lp_font_glyph_desc* glyph_lst)
{
  struct rb_tex2d_desc tex2d_desc;
  struct lp_font_glyph_desc default_glyph;
  struct lp_font_glyph_desc* sorted_glyphs = NULL;
  struct node* root = NULL;
  uint32_t cache_width = 0;
  uint32_t cache_height = 0;
  uint32_t i = 0;
  uint32_t nb_glyphs_adjusted = nb_glyphs + 1; /* +1 <=> default glyph. */
  uint16_t max_bmp_width = 0;
  uint16_t max_bmp_height = 0;
  uint8_t Bpp = 0;
  enum lp_error lp_err = LP_NO_ERROR;
  memset(&tex2d_desc, 0, sizeof(tex2d_desc));
  memset(&default_glyph, 0, sizeof(default_glyph));

  #define CALLOC(Dst, Nb, Size)                                                \
    {                                                                          \
      Dst = MEM_CALLOC(font->lp->allocator, Nb, Size);                         \
      if(!Dst) {                                                               \
        lp_err = LP_MEMORY_ERROR;                                              \
        goto error;                                                            \
      }                                                                        \
    } (void)0

  if(!font || (nb_glyphs && !glyph_lst)) {
    lp_err = LP_INVALID_ARGUMENT;
    goto error;
  }
  if(0 == nb_glyphs)
    goto exit;

  reset_font(font);

  /* Retrieve global font metrics. */
  font->line_space = line_space;
  font->min_glyph_width = UINT16_MAX;
  font->min_glyph_pos_y = INT16_MAX;
  for(i = 0; i < nb_glyphs; ++i) {
    const int16_t bmp_top = (int16_t)glyph_lst[i].bitmap_top;
    ASSERT
      (  glyph_lst[i].bitmap_top <= INT16_MAX
      && glyph_lst[i].bitmap_top >= INT16_MIN );

    font->min_glyph_width = MIN(font->min_glyph_width, glyph_lst[i].width);
    font->min_glyph_pos_y = MIN(font->min_glyph_pos_y, bmp_top);
    max_bmp_width = MAX(max_bmp_width, glyph_lst[i].bitmap.width);
    max_bmp_height = MAX(max_bmp_height, glyph_lst[i].bitmap.height);
  }
  Bpp = glyph_lst[0].bitmap.bytes_per_pixel;
  if(Bpp != 1 && Bpp != 3) {
    lp_err = LP_INVALID_ARGUMENT;
    goto error;
  }
  lp_err = create_default_glyph
    (font->lp->allocator, max_bmp_width, max_bmp_height, Bpp, &default_glyph);
  if(LP_NO_ERROR != lp_err)
    goto error;

  /* Sort the input glyphs in descending order with respect to their
   * bitmap size. */
  #define SIZEOF_GLYPH sizeof(struct lp_font_glyph_desc)
  CALLOC(sorted_glyphs, nb_glyphs_adjusted, SIZEOF_GLYPH);
  memcpy(sorted_glyphs, &default_glyph, SIZEOF_GLYPH);
  memcpy(sorted_glyphs+1 ,glyph_lst, SIZEOF_GLYPH * nb_glyphs);
  qsort(sorted_glyphs, nb_glyphs_adjusted, SIZEOF_GLYPH, cmp_glyph_desc);
  #undef SIZEOF_GLYPH

  /* Create the binary tree data structure used to pack the glyphs into the
   * cache texture. */
  compute_initial_cache_size
    (nb_glyphs_adjusted, sorted_glyphs, &cache_width, &cache_height);

  CALLOC(root, 1, sizeof(struct node));
  root->x = 0;
  root->y = 0;
  root->width = cache_width;
  root->height = cache_height;
  root->extendable_flag = EXTENDABLE_X | EXTENDABLE_Y;
  root->id = UINT32_MAX;

  for(i = 0; i < nb_glyphs_adjusted; ++i) {
    void* data = NULL;
    struct node* node = NULL;
    uint16_t width = 0;
    uint16_t height = 0;

    /* Check the conformity of the glyph bitmap format. */
    if(sorted_glyphs[i].bitmap.bytes_per_pixel != Bpp) {
      lp_err = LP_INVALID_ARGUMENT;
      goto error;
    }
    width = sorted_glyphs[i].bitmap.width;
    height = sorted_glyphs[i].bitmap.height;
    /* Check whether the glyph character is already registered or not. */
    SL(hash_table_find(font->glyph_htbl, &sorted_glyphs[i].character, &data));
    if(data != NULL) {
      continue;
    } else {
      wchar_t character = 0;
      struct lp_font_glyph glyph;
      memset(&glyph, 0, sizeof(glyph));
      character = sorted_glyphs[i].character;
      SL(hash_table_insert(font->glyph_htbl, &character, &glyph));
    }
    /* Pack the glyph bitmap. */
    node = insert_rect(font->lp->allocator, root, width, height);
    while(!node) {
      const size_t max_tex_size = font->lp->rb_cfg.max_tex_size;
      const uint16_t extend_x = (uint16_t)MAX(width / 2, 1);
      const uint16_t extend_y = (uint16_t)MAX(height / 2, 1);
      const bool can_extend_w = (cache_width + extend_x) <= max_tex_size;
      const bool can_extend_h = (cache_height + extend_y) <= max_tex_size;
      const bool extend_w = can_extend_w && cache_width < cache_height;
      const bool extend_h = can_extend_h && !extend_w;

      if(extend_w) {
        extend_width(root, extend_x);
        cache_width += extend_x;
      } else if(extend_h) {
        extend_height(root, extend_y);
        cache_height += extend_y;
      } else if(can_extend_w) {
        extend_width(root, extend_x);
        cache_width += extend_x;
      } else if(can_extend_h) {
        extend_height(root, extend_y);
        cache_height += extend_y;
      } else {
        lp_err = LP_MEMORY_ERROR;
        goto error;
      }
      node = insert_rect(font->lp->allocator, root, width, height);
    }
    if(!node) {
      lp_err = LP_MEMORY_ERROR;
      goto error;
    }
    node->id = i;
  }
  /* Use the pack information to fill the font glyph cache. */
  font->cache_img.Bpp = Bpp;
  font->cache_img.width = cache_width;
  font->cache_img.height = cache_height;
  CALLOC(font->cache_img.buffer, cache_width * cache_height, Bpp);
  fill_font_cache(root, font, sorted_glyphs);
  /* Setup the cache texture. */
  tex2d_desc.width = cache_width;
  tex2d_desc.height = cache_height;
  tex2d_desc.mip_count = 1;
  tex2d_desc.format = Bpp_to_rb_tex_format(Bpp);
  tex2d_desc.usage = RB_USAGE_IMMUTABLE;
  tex2d_desc.compress = 0;
  RBI(font->lp->rbi, create_tex2d
    (font->lp->rb_ctxt,
     &tex2d_desc,
     (const void**)&font->cache_img.buffer,
     &font->cache_tex));

  SIGNAL_INVOKE(&font->signals, LP_FONT_SIGNAL_DATA_UPDATE, font);

  #undef CALLOC

exit:
  if(font)
    free_default_glyph(font->lp->allocator, &default_glyph);
  if(root)
    free_binary_tree(font->lp->allocator, root);
  if(sorted_glyphs)
    MEM_FREE(font->lp->allocator, sorted_glyphs);
  return lp_err;
error:
  if(font)
    reset_font(font);
  goto exit;
}

enum lp_error
lp_font_get_metrics
  (struct lp_font* font,
   struct lp_font_metrics* metrics)
{
  if(!font || !metrics)
    return LP_INVALID_ARGUMENT;
  metrics->line_space = font->line_space;
  metrics->min_glyph_width = font->min_glyph_width;
  metrics->min_glyph_pos_y = font->min_glyph_pos_y;
  return LP_NO_ERROR;
}

enum lp_error
lp_font_get_glyph
  (struct lp_font* font,
   const wchar_t character,
   struct lp_font_glyph* dst_glyph)
{
  struct lp_font_glyph* glyph = NULL;
  struct lp_font_glyph font_glyph_default;
  memset(&font_glyph_default, 0, sizeof(struct lp_font_glyph));

  if(!font || !dst_glyph)
    return LP_INVALID_ARGUMENT;

  SL(hash_table_find(font->glyph_htbl, &character,(void**)&glyph));

  if(glyph == NULL) {
    SL(hash_table_find
      (font->glyph_htbl, (wchar_t[]){DEFAULT_CHAR}, (void**)&glyph));
    if( glyph == NULL ) {
      glyph = &font_glyph_default;
    }
  }
  memcpy(dst_glyph, glyph, sizeof(struct lp_font_glyph));

  return LP_NO_ERROR;
}

enum lp_error
lp_font_get_texture(struct lp_font* font, struct rb_tex2d** tex)
{
  if(!font || !tex)
    return LP_INVALID_ARGUMENT;
  *tex = font->cache_tex;
  return LP_NO_ERROR;
}

enum lp_error
lp_font_get_bitmap_cache
  (const struct lp_font* font,
   uint32_t* width,
   uint32_t* height,
   uint8_t* bytes_per_pixel,
   const unsigned char** bitmap_cache)
{
  if(!font)
    return LP_INVALID_ARGUMENT;

  if(width)
    *width = font->cache_img.width;
  if(height)
    *height = font->cache_img.height;
  if(bytes_per_pixel)
    *bytes_per_pixel = font->cache_img.Bpp;
  if(bitmap_cache)
    *bitmap_cache = font->cache_img.buffer;

  return LP_NO_ERROR;
}

enum lp_error
lp_font_signal_connect
  (struct lp_font* font,
   const enum lp_font_signal signal,
   lp_font_callback_T* clbk)
{
  if(!font || !clbk || signal >= LP_FONT_SIGNALS_COUNT)
    return LP_INVALID_ARGUMENT;
  SIGNAL_CONNECT_CALLBACK(&font->signals, signal, clbk);
  return LP_NO_ERROR;
}

#undef GLYPH_BORDER

