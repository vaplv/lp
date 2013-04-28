#ifndef LP_PRINTER_H
#define LP_PRINTER_H

#include "lp.h"
#include <wchar.h>

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

LP_API enum lp_error
lp_printer_set_viewport
  (struct lp_printer* printer,
   const int x,
   const int y,
   const int width,
   const int height);

LP_API enum lp_error
lp_printer_print_wstring
  (struct lp_printer* printer,
   const int x,
   const int y,
   const wchar_t* wstr,
   const float color[3]);

LP_API enum lp_error
lp_printer_flush
  (struct lp_printer* printer);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LP_PRINTER_H */

