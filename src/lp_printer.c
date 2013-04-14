#include "lp_c.h"
#include "lp_font.h"
#include "lp_printer.h"
#include <rb/rbi.h>
#include <snlsys/snlsys.h>
#include <snlsys/math.h>
#include <snlsys/mem_allocator.h>
#include <float.h>
#include <string.h>

#define LP_SIZEOF_GLYPH_VERTEX ((3/*pos*/ + 2/*tex*/ + 3/*col*/)*sizeof(float))
#define LP_GLYPH_ATTRIB_POSITION_ID 0
#define LP_GLYPH_ATTRIB_TEXCOORD_ID 1
#define LP_GLYPH_ATTRIB_COLOR_ID 2
#define LP_GLYPH_ATTRIBS_COUNT 3
#define LP_GLYPH_VERTICES_COUNT 4
#define LP_GLYPH_INDICES_COUNT 6

/* Minimal scratch data structure */
struct scratch {
  struct mem_allocator* allocator;
  void* buffer;
  size_t size;
  size_t id;
};

struct lp_printer {
  struct ref ref;
  struct scratch scratch;
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
  uint32_t nb_glyphs; /* Number of glyphs currently drawn by the printer */
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
      const unsigned int indices[LP_GLYPH_INDICES_COUNT] = {
        0+i, 1+i, 3+i, 3+i, 1+i, 2+i
      };
      scratch_push_back(&printer->scratch, indices, sizeof(indices));
    }
    buffer_desc.size = vbufsiz;
    buffer_desc.target = RB_BIND_VERTEX_BUFFER;
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
}

static void
setup_font(struct lp_printer* printer)
{
  ASSERT(printer);
  printer_storage(printer, 4096);
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
    LP(font_ref_put(printer->font));
    LP(font_ref_get(font));
    LP(font_signal_connect
      (font, LP_FONT_SIGNAL_DATA_UPDATE, &printer->on_font_data_update));
    printer->font = font;

    setup_font(printer);
  }
  return LP_NO_ERROR;
}

