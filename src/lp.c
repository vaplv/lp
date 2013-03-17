#include "lp.h"
#include "lp_c.h"
#include <rb/rbi.h>
#include <snlsys/mem_allocator.h>

/*******************************************************************************
 *
 * Helper functions
 *
 ******************************************************************************/
static void
release_lp(struct ref* ref)
{
  struct lp* lp = NULL;
  ASSERT(ref);

  lp = CONTAINER_OF(ref, struct lp, ref);
  MEM_FREE(lp->allocator, lp);
}

/*******************************************************************************
 *
 * lp functions
 *
 ******************************************************************************/
enum lp_error
lp_create
  (struct rbi* rbi,
   struct rb_context* ctxt,
   struct mem_allocator* allocator,
   struct lp** out_lp)
{
  struct lp* lp = NULL;
  enum lp_error lp_err = LP_NO_ERROR;
  struct mem_allocator* alloc = allocator ? allocator : &mem_default_allocator;

  if(!rbi || !out_lp) {
    lp_err = LP_INVALID_ARGUMENT;
    goto error;
  }
  #define RB_FUNC(func_name, ...)                                              \
    if(!rbi->func_name) {                                                      \
      lp_err = LP_INVALID_ARGUMENT;                                            \
      goto error;                                                              \
    }
  #include <rb/rb_func.h>
  #undef RB_FUNC

  lp = MEM_CALLOC(alloc, 1, sizeof(struct lp));
  if(!lp) {
    lp_err = LP_MEMORY_ERROR;
    goto error;
  }
  ref_init(&lp->ref);
  lp->allocator = alloc;
  lp->rbi = rbi;
  lp->rb_ctxt = ctxt;
  RBI(lp->rbi, get_config(lp->rb_ctxt, &lp->rb_cfg));

exit:
  if(out_lp)
    *out_lp = lp;
  return lp_err;
error:
  if(lp) {
    LP(ref_put(lp));
    lp = NULL;
  }
  goto exit;
}

enum lp_error
lp_ref_get(struct lp* lp) 
{
  if(!lp)
    return LP_INVALID_ARGUMENT;
  ref_get(&lp->ref);
  return LP_NO_ERROR;
}

enum lp_error
lp_ref_put(struct lp* lp)
{
  if(!lp)
    return LP_INVALID_ARGUMENT;
  ref_put(&lp->ref, release_lp);
  return LP_NO_ERROR;
}

