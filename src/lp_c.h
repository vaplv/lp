#ifndef LP_C_H
#define LP_C_H

#include <rb/rb_types.h>
#include <snlsys/ref_count.h>

struct rb_context;
struct mem_allocator;

struct lp {
  struct ref ref;
  struct rbi* rbi;
  struct rb_config rb_cfg;
  struct rb_context* rb_ctxt;
  struct mem_allocator* allocator;
};

#endif /* LP_C_H */

