#include "lp_c.h"
#include "lp_printer.h"
#include <rb/rbi.h>
#include <snlsys/snlsys.h>
#include <snlsys/mem_allocator.h>
#include <float.h>
#include <string.h>

#define LP_SIZEOF_GLYPH_VERTEX ((3/*pos*/ + 2/*tex*/ + 3/*col*/)*sizeof(float))
#define LP_GLYPH_ATTRIB_POSITION_ID 0
#define LP_GLYPH_ATTRIB_TEXCOORD_ID 1
#define LP_GLYPH_ATTRIB_COLOR_ID 2
#define LP_GLYPH_ATTRIBS_COUNT 3

struct lp_printer {
  struct ref ref;
  struct lp* lp;

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
release_printer(struct ref* ref)
{
  struct lp* lp = NULL;
  struct lp_printer* printer = NULL;
  ASSERT(NULL != ref);

  printer = CONTAINER_OF(ref, struct lp_printer, ref);

  printer_rb_shutdown(printer);
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

