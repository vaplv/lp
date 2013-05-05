#include "lp_c.h"
#include "lp_font.h"
#include "lp_printer.h"
#include <rb/rbi.h>
#include <snlsys/snlsys.h>
#include <snlsys/math.h>
#include <snlsys/mem_allocator.h>
#include <float.h>
#include <limits.h>
#include <string.h>

#define LP_SIZEOF_GLYPH_VERTEX ((3/*pos*/ + 2/*tex*/ + 3/*col*/)*sizeof(float))
#define LP_GLYPH_ATTRIB_POSITION_ID 0
#define LP_GLYPH_ATTRIB_TEXCOORD_ID 1
#define LP_GLYPH_ATTRIB_COLOR_ID 2
#define LP_GLYPH_ATTRIBS_COUNT 3
#define LP_GLYPH_VERTICES_COUNT 4
#define LP_GLYPH_INDICES_COUNT 6
#define LP_GLYPH_COUNT_MAX 4096 /* Count of buffered glyphes */

#define LP_TAB_SPACES_COUNT 4 /* This may be a configurable parameter */

/* Minimal scratch data structure */
struct scratch {
  struct mem_allocator* allocator;
  void* buffer;
  size_t size;
  size_t id;
};

/* Window coordinate of the printable zone */
struct viewport {
  int x0, y0, x1, y1;
};

struct lp_printer {
  struct ref ref;
  struct scratch scratch;
  struct viewport viewport;
  struct lp* lp;

  struct lp_font* font;
  lp_font_callback_T on_font_data_update;

  struct rb_buffer_attrib glyph_attrib_list[LP_GLYPH_ATTRIBS_COUNT];
  struct rb_buffer* glyph_vertex_buffer;
  struct rb_buffer* glyph_index_buffer;

  struct rb_vertex_array* vertex_array;
  struct rb_shader* vertex_shader;
  struct rb_shader* fragment_shader;
  struct rb_program* shading_program;
  struct rb_sampler* sampler;
  struct rb_uniform* uniform_sampler;
  struct rb_uniform* uniform_scale;
  struct rb_uniform* uniform_bias;

  uint32_t max_nb_glyphs; /* Maximum number glyphs that the printer can draw */
  uint32_t nb_glyphs; /* Number of glyphs printed but not flushed */
};

/*******************************************************************************
 *
 * Embedded shader sources
 *
 ******************************************************************************/
static const char* print_vs_src =
  "#version 330\n"
  "layout(location =" STR(LP_GLYPH_ATTRIB_POSITION_ID) ") in vec3 pos;\n"
  "layout(location =" STR(LP_GLYPH_ATTRIB_TEXCOORD_ID) ") in vec2 tex;\n"
  "layout(location =" STR(LP_GLYPH_ATTRIB_COLOR_ID) ") in vec3 col;\n"
  "uniform vec3 scale;\n"
  "uniform vec3 bias;\n"
  "smooth out vec2 glyph_tex;\n"
  "flat   out vec3 glyph_col;\n"
  "void main()\n"
  "{\n"
  "  glyph_tex = tex;\n"
  "  glyph_col = col;\n"
  "  gl_Position = vec4(pos * scale + bias, 1.f);\n"
  "}\n";

static const char* print_fs_src =
  "#version 330\n"
  "uniform sampler2D glyph_cache;\n"
  "smooth in vec2 glyph_tex;\n"
  "flat   in vec3 glyph_col;\n"
  "out vec4 color;\n"
  "void main()\n"
  "{\n"
  "  float val = texture(glyph_cache, glyph_tex).r;\n"
  "  color = vec4(val * glyph_col, val);\n"
  "}\n";

/*******************************************************************************
 *
 * Minimal implementation of a growable scratch buffer
 *
 ******************************************************************************/
static void
scratch_init(struct mem_allocator* allocator, struct scratch* scratch)
{
  ASSERT(allocator && scratch);
  memset(scratch, 0, sizeof(struct scratch));
  scratch->allocator = allocator;
}

static enum lp_error
scratch_reserve(struct scratch* scratch, size_t size)
{
  enum lp_error lp_err = LP_NO_ERROR;
  ASSERT(scratch && scratch->allocator);

  if(size <= scratch->size)
    return LP_NO_ERROR;

  if(scratch->buffer) {
    scratch->buffer = MEM_REALLOC(scratch->allocator, scratch->buffer, size);
  } else {
    scratch->buffer = MEM_ALIGNED_ALLOC(scratch->allocator, size, 16);
    memset(scratch->buffer, 0, size);
  }
  if(!scratch->buffer) {
    lp_err = LP_MEMORY_ERROR;
    goto error;
  }
  scratch->size = size;

exit:
  return lp_err;
error:
  if(scratch->buffer) {
    MEM_FREE(scratch->allocator, scratch->buffer);
    scratch->buffer = NULL;
  }
  scratch->size = 0;
  scratch->id = 0;
  goto exit;
}

static void
scratch_release(struct scratch* scratch)
{
  ASSERT(scratch && scratch->allocator);
  MEM_FREE(scratch->allocator, scratch->buffer);
}

static FINLINE void
scratch_clear(struct scratch* scratch)
{
  ASSERT(scratch);
  scratch->id = 0;
}

static FINLINE void*
scratch_buffer(struct scratch* scratch)
{
  ASSERT(scratch);
  return scratch->buffer;
}

static enum lp_error
scratch_push_back(struct scratch* scratch, const void* data, size_t size)
{
  enum lp_error lp_err = LP_NO_ERROR;
  ASSERT(scratch && data);

  if(scratch->id + size > scratch->size) {
    lp_err = scratch_reserve(scratch, scratch->id + size);
    if(lp_err != LP_NO_ERROR)
      return lp_err;
  }
  memcpy((void*)((uintptr_t)scratch->buffer + scratch->id), data, size);
  scratch->id += size;
  return LP_NO_ERROR;
}

/*******************************************************************************
 *
 * Helper functions
 *
 ******************************************************************************/
static void
printer_rb_init(struct lp_printer* printer)
{
  ASSERT(printer);
  struct rb_sampler_desc sampler_desc;
  struct rbi* rbi = printer->lp->rbi;
  struct rb_context* ctxt = printer->lp->rb_ctxt;

  #define REGISTER_ATTRIB(Id, Index, Type, Offset )                            \
    {                                                                          \
      printer->glyph_attrib_list[Id].index = Index;                            \
      printer->glyph_attrib_list[Id].stride = LP_SIZEOF_GLYPH_VERTEX;          \
      printer->glyph_attrib_list[Id].offset = Offset;                          \
      printer->glyph_attrib_list[Id].type = Type;                              \
    } (void)0
  REGISTER_ATTRIB(0, LP_GLYPH_ATTRIB_POSITION_ID, RB_FLOAT3, 0 * sizeof(float));
  REGISTER_ATTRIB(1, LP_GLYPH_ATTRIB_TEXCOORD_ID, RB_FLOAT2, 3 * sizeof(float));
  REGISTER_ATTRIB(2, LP_GLYPH_ATTRIB_COLOR_ID,    RB_FLOAT3, 5 * sizeof(float));
  #undef REGISTER_ATTRIB

  RBI(rbi, create_vertex_array(ctxt, &printer->vertex_array));

  /* Sampler */
  sampler_desc.filter = RB_MIN_POINT_MAG_POINT_MIP_POINT;
  sampler_desc.address_u = RB_ADDRESS_CLAMP;
  sampler_desc.address_v = RB_ADDRESS_CLAMP;
  sampler_desc.address_w = RB_ADDRESS_CLAMP;
  sampler_desc.lod_bias = 0;
  sampler_desc.min_lod = -FLT_MAX;
  sampler_desc.max_lod = FLT_MAX;
  sampler_desc.max_anisotropy = 1;
  RBI(rbi, create_sampler(ctxt, &sampler_desc, &printer->sampler));

  /* Shaders */
  RBI(rbi, create_shader
    (ctxt, RB_VERTEX_SHADER, print_vs_src, strlen(print_vs_src),
     &printer->vertex_shader));
  RBI(rbi, create_shader
    (ctxt, RB_FRAGMENT_SHADER, print_fs_src, strlen(print_fs_src),
     &printer->fragment_shader));

  /* Shading program */
  RBI(rbi, create_program(ctxt, &printer->shading_program));
  RBI(rbi, attach_shader(printer->shading_program, printer->vertex_shader));
  RBI(rbi, attach_shader(printer->shading_program, printer->fragment_shader));
  RBI(rbi, link_program(printer->shading_program));

  /* Uniforms */
  RBI(rbi, get_named_uniform
    (ctxt, printer->shading_program, "glyph_cache", &printer->uniform_sampler));
  RBI(rbi, get_named_uniform
    (ctxt, printer->shading_program, "scale", &printer->uniform_scale));
  RBI(rbi, get_named_uniform
    (ctxt, printer->shading_program, "bias", &printer->uniform_bias));
}

static void
printer_rb_shutdown(struct lp_printer* printer)
{
  ASSERT(printer);
  struct rbi* rbi = printer->lp->rbi;

  #define REF_PUT(Type, Data) if(Data) RBI(rbi, Type ## _ref_put(Data))
  REF_PUT(buffer, printer->glyph_vertex_buffer);
  REF_PUT(buffer, printer->glyph_index_buffer);
  REF_PUT(vertex_array, printer->vertex_array);
  REF_PUT(shader, printer->vertex_shader);
  REF_PUT(shader, printer->fragment_shader);
  REF_PUT(program, printer->shading_program);
  REF_PUT(sampler, printer->sampler);
  REF_PUT(uniform, printer->uniform_sampler);
  REF_PUT(uniform, printer->uniform_scale);
  REF_PUT(uniform, printer->uniform_bias);
  #undef REF_PUT
}

static void
printer_storage(struct lp_printer* printer, const uint32_t max_nb_glyphs)
{
  ASSERT(printer);

  if(printer->glyph_vertex_buffer) {
    RBI(printer->lp->rbi, buffer_ref_put(printer->glyph_vertex_buffer));
    printer->glyph_vertex_buffer = NULL;
  }
  if(printer->glyph_index_buffer) {
    RBI(printer->lp->rbi, buffer_ref_put(printer->glyph_index_buffer));
    printer->glyph_index_buffer = NULL;
  }

  printer->nb_glyphs = 0;
  printer->max_nb_glyphs = max_nb_glyphs;
  if(max_nb_glyphs == 0) {
    const int attr_id_list[LP_GLYPH_ATTRIBS_COUNT] = {
      [0] = LP_GLYPH_ATTRIB_POSITION_ID,
      [1] = LP_GLYPH_ATTRIB_TEXCOORD_ID,
      [2] = LP_GLYPH_ATTRIB_COLOR_ID
    };
    STATIC_ASSERT(LP_GLYPH_ATTRIBS_COUNT == 3, Unexpected_constant);
    RBI(printer->lp->rbi, remove_vertex_attrib
      (printer->vertex_array, LP_GLYPH_ATTRIBS_COUNT, attr_id_list));
  } else {
    struct rb_buffer_desc buffer_desc;
    const size_t vbufsiz =
      max_nb_glyphs * LP_GLYPH_VERTICES_COUNT * LP_SIZEOF_GLYPH_VERTEX;
    const size_t ibufsiz =
      max_nb_glyphs * LP_GLYPH_INDICES_COUNT * sizeof(unsigned int);
    unsigned int i = 0;

    /* Create the vertex buffer. The vertex buffer data are going to be filled
     * by the draw function with respect to the printed terminal lines. */
    buffer_desc.size = vbufsiz;
    buffer_desc.target = RB_BIND_VERTEX_BUFFER;
    buffer_desc.usage = RB_USAGE_DYNAMIC;
    RBI(printer->lp->rbi, create_buffer
      (printer->lp->rb_ctxt, &buffer_desc, NULL,
       &printer->glyph_vertex_buffer));

    /* Create the immutable index buffer. Its internal data are the indices of
     * the ordered glyphs of the vertex buffer. */
    LP_CALL(scratch_reserve(&printer->scratch, MAX(vbufsiz, ibufsiz)));
    scratch_clear(&printer->scratch);
    for(i = 0; i < max_nb_glyphs; ++i) {
      const unsigned id_first = i * LP_GLYPH_VERTICES_COUNT;
      const unsigned int indices[LP_GLYPH_INDICES_COUNT] = {
        0+id_first, 1+id_first, 3+id_first, 3+id_first, 1+id_first, 2+id_first
      };
      scratch_push_back(&printer->scratch, indices, sizeof(indices));
    }
    buffer_desc.size = vbufsiz;
    buffer_desc.target = RB_BIND_INDEX_BUFFER;
    buffer_desc.usage = RB_USAGE_IMMUTABLE;
    RBI(printer->lp->rbi, create_buffer
      (printer->lp->rb_ctxt, &buffer_desc, scratch_buffer(&printer->scratch),
       &printer->glyph_index_buffer));

    /* Setup the vertex array */
    RBI(printer->lp->rbi, vertex_attrib_array
      (printer->vertex_array, printer->glyph_vertex_buffer,
       LP_GLYPH_ATTRIBS_COUNT, printer->glyph_attrib_list));
    RBI(printer->lp->rbi, vertex_index_array
      (printer->vertex_array, printer->glyph_index_buffer));
  }
  /* Clear the scratch for subsecquent uses */
  scratch_clear(&printer->scratch);
}

static void
setup_font(struct lp_printer* printer)
{
  ASSERT(printer);
  printer_storage(printer, LP_GLYPH_COUNT_MAX);
}

static void
on_font_data_update(struct lp_font* font, void* data)
{
  struct lp_printer* printer = data;
  ASSERT(font && data && printer->font == font);
  setup_font(printer);
}

static void
release_printer(struct ref* ref)
{
  struct lp* lp = NULL;
  struct lp_printer* printer = NULL;
  ASSERT(NULL != ref);

  printer = CONTAINER_OF(ref, struct lp_printer, ref);

  printer_rb_shutdown(printer);
  CALLBACK_DISCONNECT(&printer->on_font_data_update);
  scratch_release(&printer->scratch);
  lp = printer->lp;
  MEM_FREE(lp->allocator, printer);
  LP(ref_put(lp));
}

/*******************************************************************************
 *
 * lp_printer functions
 *
 ******************************************************************************/
enum lp_error
lp_printer_create(struct lp* lp, struct lp_printer** out_printer)
{
  struct lp_printer* printer = NULL;

  if(UNLIKELY(!lp || !out_printer))
    return LP_INVALID_ARGUMENT;

  printer = MEM_CALLOC(lp->allocator, 1, sizeof(struct lp_printer));
  if(UNLIKELY(!printer))
    return LP_MEMORY_ERROR;

  ref_init(&printer->ref);
  printer->lp = lp;
  LP(ref_get(lp));
  printer_rb_init(printer);
  CALLBACK_INIT(&printer->on_font_data_update);
  CALLBACK_SETUP(&printer->on_font_data_update, on_font_data_update, printer);
  scratch_init(lp->allocator, &printer->scratch);
  *out_printer = printer;

  return LP_NO_ERROR;
}

enum lp_error
lp_printer_ref_get(struct lp_printer* printer)
{
  if(UNLIKELY(!printer))
    return LP_INVALID_ARGUMENT;
  ref_get(&printer->ref);
  return LP_NO_ERROR;
}

enum lp_error
lp_printer_ref_put(struct lp_printer* printer)
{
  if(UNLIKELY(!printer))
    return LP_INVALID_ARGUMENT;
  ref_put(&printer->ref, release_printer);
  return LP_NO_ERROR;
}

enum lp_error
lp_printer_set_font(struct lp_printer* printer, struct lp_font* font)
{
  if(UNLIKELY(!printer || !font))
    return LP_INVALID_ARGUMENT;

  if(font != printer->font) {
    if(printer->font) {
      LP(font_ref_put(printer->font));
    }
    LP(font_ref_get(font));
    LP(font_signal_connect
      (font, LP_FONT_SIGNAL_DATA_UPDATE, &printer->on_font_data_update));
    printer->font = font;

    setup_font(printer);
  }
  return LP_NO_ERROR;
}

enum lp_error
lp_printer_set_viewport
  (struct lp_printer* printer,
   const int x,
   const int y,
   const int width,
   const int height)
{
  if(!printer || width < 0 || height < 0)
    return LP_INVALID_ARGUMENT;

  printer->viewport.x0 = x;
  printer->viewport.y0 = y;
  printer->viewport.x1 = x + width;
  printer->viewport.y1 = y + height;
  return LP_NO_ERROR;
}

enum lp_error
lp_printer_print_wstring
  (struct lp_printer* printer,
   const int x,
   const int y,
   const wchar_t* wstr,
   const float color[3],
   int* cur_x,
   int* cur_y)
{
  if(!printer || !wstr || !color || !printer->font)
    return LP_INVALID_ARGUMENT;
  if(printer->viewport.x1 <= printer->viewport.x0 
  || printer->viewport.y1 <= printer->viewport.y0)  /* No printable zone */
    return LP_INVALID_ARGUMENT;

  struct lp_font_metrics font_metrics;
  LP(font_get_metrics(printer->font, &font_metrics));

  const int line_width = printer->viewport.x1 - printer->viewport.x0;
  int line_width_remaining = MAX(printer->viewport.x1 - x, 0);
  int line_x = x;
  int line_y = y;

  size_t i = 0;
  for(i = 0; wstr[i] != L'\0'; ++i) {
    struct lp_font_glyph glyph;
    int glyph_width_adjusted = 0;

    switch(wstr[i]) {
      case L'\t': /* Tabulation */
        LP(font_get_glyph(printer->font, L' ', &glyph));
        glyph_width_adjusted = glyph.width * LP_TAB_SPACES_COUNT;
        break;
      case L'\n': /* New line */
        glyph_width_adjusted = INT_MAX;
        break;
      default: /* Common characters */
        LP(font_get_glyph(printer->font, wstr[i], &glyph));
        glyph_width_adjusted = glyph.width;
        break;
    }

    /* Update remaining width */
    if(line_width_remaining >= glyph_width_adjusted) {
      line_width_remaining -= glyph_width_adjusted;
    } else { /* Wrap the line */
      line_width_remaining = line_width;
      line_x = printer->viewport.x0;
      line_y = line_y - font_metrics.line_space;
      if(glyph_width_adjusted > INT_MAX) { /* <=> New line */
        continue;
      } else if(line_width_remaining >= glyph_width_adjusted) {
        line_width_remaining = MAX(line_width_remaining-glyph_width_adjusted,0);
      }
    }

    /* The char lies inside the printable viewport */
    if(line_x >= printer->viewport.x0
    && line_y >= printer->viewport.y0
    && line_x + glyph_width_adjusted <= printer->viewport.x1
    && line_y + font_metrics.line_space <= printer->viewport.y1) {
      const struct { float x, y; } glyph_pos_adjusted[2] = {
        [0]={ glyph.pos[0].x + (float)line_x, glyph.pos[0].y + (float)line_y },
        [1]={ glyph.pos[1].x + (float)line_x, glyph.pos[1].y + (float)line_y }
      };

      float vertex[LP_SIZEOF_GLYPH_VERTEX / sizeof(float)];
      #define SET_POS(Dst, X, Y, Z) Dst[0] = (X), Dst[1] = (Y), Dst[2] = (Z)
      #define SET_TEX(Dst, U, V)    Dst[3] = (U), Dst[4] = (V)
      #define SET_COL(Dst, R, G, B) Dst[5] = (R), Dst[6] = (G), Dst[7] = (B)

      /* It is sufficient to set the color only of the first vertex */
      SET_COL(vertex, color[0], color[1], color[2]);

      /* Bottom left */
      SET_POS(vertex, glyph_pos_adjusted[0].x, glyph_pos_adjusted[1].y, 0.f);
      SET_TEX(vertex, glyph.tex[0].x, glyph.tex[1].y);
      scratch_push_back(&printer->scratch, vertex, sizeof(vertex));
      /* Top left */
      SET_POS(vertex, glyph_pos_adjusted[0].x, glyph_pos_adjusted[0].y, 0.f);
      SET_TEX(vertex, glyph.tex[0].x, glyph.tex[0].y);
      scratch_push_back(&printer->scratch, vertex, sizeof(vertex));
      /* Top right */
      SET_POS(vertex, glyph_pos_adjusted[1].x, glyph_pos_adjusted[0].y, 0.f);
      SET_TEX(vertex, glyph.tex[1].x, glyph.tex[0].y);
      scratch_push_back(&printer->scratch, vertex, sizeof(vertex));
      /* Bottom right */
      SET_POS(vertex, glyph_pos_adjusted[1].x, glyph_pos_adjusted[1].y, 0.f);
      SET_TEX(vertex, glyph.tex[1].x, glyph.tex[1].y);
      scratch_push_back(&printer->scratch, vertex, sizeof(vertex));

      #undef SET_POS
      #undef SET_TEX
      #undef SET_COL

      ++printer->nb_glyphs;
    }

    line_x += glyph_width_adjusted;

    ASSERT(printer->nb_glyphs <= printer->max_nb_glyphs);
    if(printer->nb_glyphs == printer->max_nb_glyphs) {
      LP(printer_flush(printer));
    }
  }

  if(cur_x)
    *cur_x = line_x;
  if(cur_y)
    *cur_y = line_y;

  return LP_NO_ERROR;
}

enum lp_error
lp_printer_flush(struct lp_printer* printer)
{
  if(!printer)
    return LP_INVALID_ARGUMENT;

  if(printer->nb_glyphs == 0)
    return LP_NO_ERROR;

  /* No printable zone => Draw nothing */
  if(printer->viewport.x1 <= printer->viewport.x0 
  || printer->viewport.y1 <= printer->viewport.y0) { 
    scratch_clear(&printer->scratch);
    return LP_NO_ERROR;
  }

  struct rbi* rbi = printer->lp->rbi;
  struct rb_context* rb_ctxt = printer->lp->rb_ctxt;

  void* data = scratch_buffer(&printer->scratch);
  const size_t size = 
    4 /* vertices per glyph */ * printer->nb_glyphs * LP_SIZEOF_GLYPH_VERTEX;
  RBI(rbi, buffer_data(printer->glyph_vertex_buffer, 0, (int)size, data));

  const struct rb_depth_stencil_desc depth_stencil_desc = {
    .enable_depth_test = 0,
    .enable_depth_write = 0,
    .enable_stencil_test = 0,
    .front_face_op.write_mask = 0,
    .back_face_op.write_mask = 0
  };
  const struct rb_viewport_desc viewport_desc = {
    .x = printer->viewport.x0,
    .y = printer->viewport.y0,
    .width = printer->viewport.x1 - printer->viewport.x0,
    .height = printer->viewport.y1 - printer->viewport.y0
  };
  struct rb_blend_desc blend_desc = {
    .enable = 1,
    .src_blend_RGB = RB_BLEND_SRC_ALPHA,
    .src_blend_Alpha = RB_BLEND_ONE,
    .dst_blend_RGB = RB_BLEND_ONE_MINUS_SRC_ALPHA,
    .dst_blend_Alpha = RB_BLEND_ZERO,
    .blend_op_RGB = RB_BLEND_OP_ADD,
    .blend_op_Alpha = RB_BLEND_OP_ADD
  };
  const float scale[3] = { 
    2.f/(float)viewport_desc.width,
    2.f/(float)viewport_desc.height,
    1.f
  };
  const float bias[3] = { -1.f, -1.f, 0.f };
  struct rb_tex2d* font_tex = NULL;
  const unsigned int font_tex_unit = 0;

  LP(font_get_texture(printer->font, &font_tex));

  RBI(rbi, depth_stencil(rb_ctxt, &depth_stencil_desc));
  RBI(rbi, viewport(rb_ctxt, &viewport_desc));
  RBI(rbi, blend(rb_ctxt, &blend_desc));

  RBI(rbi, bind_tex2d(rb_ctxt, font_tex, font_tex_unit));
  RBI(rbi, bind_sampler(rb_ctxt, printer->sampler, font_tex_unit));

  RBI(rbi, bind_program(rb_ctxt, printer->shading_program));
  RBI(rbi, uniform_data(printer->uniform_sampler, 1, &font_tex_unit));
  RBI(rbi, uniform_data(printer->uniform_scale, 1, scale));
  RBI(rbi, uniform_data(printer->uniform_bias, 1, bias));

  RBI(rbi, bind_vertex_array(rb_ctxt, printer->vertex_array));
  RBI(rbi, draw_indexed
    (rb_ctxt, RB_TRIANGLE_LIST, printer->nb_glyphs * LP_GLYPH_INDICES_COUNT));

  blend_desc.enable = 0;
  RBI(rbi, blend(rb_ctxt, &blend_desc));
  RBI(rbi, bind_program(rb_ctxt, NULL));
  RBI(rbi, bind_vertex_array(rb_ctxt, NULL));
  RBI(rbi, bind_tex2d(rb_ctxt, NULL, font_tex_unit));
  RBI(rbi, bind_sampler(rb_ctxt, NULL, font_tex_unit));

  printer->nb_glyphs = 0;
  scratch_clear(&printer->scratch);

  return LP_NO_ERROR;
}

