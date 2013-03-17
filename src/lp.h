#ifndef LP_H
#define LP_H

#include "lp_error.h"
#include <snlsys/snlsys.h>

#if defined(LP_SHARED_BUILD)
  #define LP_API EXPORT_SYM
#else
  #define LP_API IMPORT_SYM
#endif

#ifndef NDEBUG
  #define LP(func) ASSERT(LP_NO_ERROR == lp_##func)
#else
  #define LP(func) lp_##func
#endif

struct lp;
struct mem_allocator;
struct rbi;
struct rb_context;

#ifdef __cplusplus
extern "C" {
#endif

LP_API enum lp_error
lp_create
  (struct rbi* rbi,
   struct rb_context* ctxt,
   struct mem_allocator* allocator, /* May be NULL */
   struct lp** lp);

LP_API enum lp_error
lp_ref_get
  (struct lp* lp);

LP_API enum lp_error
lp_ref_put
  (struct lp* lp);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LP_H */

