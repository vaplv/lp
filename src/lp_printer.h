#ifndef LP_PRINTER_H
#define LP_PRINTER_H

#include "lp.h"

struct lp_printer;
struct lp_font;

#ifdef __cplusplus
extern "C" {
#endif

LP_API enum lp_error
lp_printer_create
  (struct lp* lp,
   struct lp_printer** printer);

LP_API enum lp_error
lp_printer_ref_get
  (struct lp_printer* printer);
 
LP_API enum lp_error
lp_printer_ref_put
  (struct lp_printer* printer);

LP_API enum lp_error
lp_printer_set_font
  (struct lp_printer* printer,
   struct lp_font* font);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LP_PRINTER_H */

