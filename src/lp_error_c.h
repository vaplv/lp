#ifndef LP_ERROR_C_H
#define LP_ERROR_C_H

#include "lp_error.h"
#include <sl/sl_error.h>
#include <snlsys/snlsys.h>

static FINLINE enum lp_error
sl_to_lp_error(enum sl_error sl_err)
{
  enum lp_error lp_err = LP_NO_ERROR;
  switch(sl_err) {
    case SL_ALIGNMENT_ERROR:
    case SL_OVERFLOW_ERROR:
      lp_err = LP_UNKNOWN_ERROR;
      break;
    case SL_INVALID_ARGUMENT:
      lp_err = LP_INVALID_ARGUMENT;
      break;
    case SL_MEMORY_ERROR:
      lp_err = LP_MEMORY_ERROR;
      break;
    case SL_NO_ERROR:
      lp_err = LP_NO_ERROR;
      break;
    default: ASSERT(0);
  }
  return lp_err;
}

#endif /* LP_ERROR_C_H */

