#include "lp.h"
#include "lp_font.h"
#include "lp_printer.h"
#include <rb/rbi.h>
#include <rb/rb_types.h>
#include <snlsys/mem_allocator.h>
#include <wm/wm_device.h>
#include <wm/wm_window.h>

#define BAD_ARG LP_INVALID_ARGUMENT
#define OK LP_NO_ERROR

int
main(int argc, char** argv)
{
  /* Miscellaneous data */
  FILE* file = NULL;
  const char* driver_name = NULL;
  const char* font_name = NULL;
  /* Window Manager */
  struct wm_device* wm_dev = NULL;
  struct wm_window* wm_win = NULL;
  const struct wm_window_desc wm_win_desc = {
    .width = 640, .height = 480, .fullscreen = false
  };
  /* Render backend */
  struct rbi rbi;
  struct rb_context* rb_ctxt = NULL;
  /* LP data structure */
  struct lp* lp = NULL;
  struct lp_font* lp_font0 = NULL;
  struct lp_font* lp_font1 = NULL;
  struct lp_printer* lp_printer = NULL;

  float color[3] = { 1.f, 1.f, 1.f };

  if(argc != 3) {
    printf("usage: %s RB_DRIVER FONT\n", argv[0]);
    return -1;
  }
  driver_name = argv[1];
  font_name = argv[2];

  file = fopen(driver_name, "r");
  if(!file) {
    fprintf(stderr, "Invalid driver %s\n", driver_name);
    return -1;
  }
  fclose(file);

  file = fopen(font_name, "r");
  if(!file) {
    fprintf(stderr, "Invalid font name %s\n", font_name);
    return -1;
  }
  fclose(file);

  WM(create_device(NULL, &wm_dev));
  WM(create_window(wm_dev, &wm_win_desc, &wm_win));
  CHECK(rbi_init(driver_name, &rbi), 0);
  RBI(&rbi, create_context(NULL, &rb_ctxt));

  LP(create(&rbi, rb_ctxt, NULL, &lp));
  LP(font_create(lp, &lp_font0));
  LP(font_create(lp, &lp_font1));

  CHECK(lp_printer_create(NULL, NULL), BAD_ARG);
  CHECK(lp_printer_create(lp, NULL), BAD_ARG);
  CHECK(lp_printer_create(NULL, &lp_printer), BAD_ARG);
  CHECK(lp_printer_create(lp, &lp_printer), OK);

  CHECK(lp_printer_set_font(NULL, NULL), BAD_ARG);
  CHECK(lp_printer_set_font(lp_printer, NULL), BAD_ARG);
  CHECK(lp_printer_set_font(NULL, lp_font0), BAD_ARG);
  CHECK(lp_printer_set_font(lp_printer, lp_font0), OK);

  CHECK(lp_printer_set_viewport(NULL, 1, 1,-1,-1), BAD_ARG);
  CHECK(lp_printer_set_viewport(NULL,-1,-1, 1, 1), BAD_ARG);
  CHECK(lp_printer_set_viewport(lp_printer, 1, 1,-1,-1), BAD_ARG);
  CHECK(lp_printer_set_viewport(lp_printer,-1, 1, 1,-1), BAD_ARG);
  CHECK(lp_printer_set_viewport(lp_printer, 1,-1,-1, 1), BAD_ARG);
  CHECK(lp_printer_set_viewport(lp_printer,-1,-1, 1, 1), OK);

  CHECK(lp_printer_print_wstring
    (NULL, 0, 0, NULL, NULL, NULL, NULL), BAD_ARG);
  CHECK(lp_printer_print_wstring
    (lp_printer, 0, 0, NULL, NULL, NULL, NULL), BAD_ARG);
  CHECK(lp_printer_print_wstring
    (NULL, 0, 0, L"Test", NULL, NULL, NULL), BAD_ARG);
  CHECK(lp_printer_print_wstring
    (lp_printer, 0, 0, L"Test", NULL, NULL, NULL), BAD_ARG);
  CHECK(lp_printer_print_wstring
    (NULL, 0, 0, NULL, color, NULL, NULL), BAD_ARG);
  CHECK(lp_printer_print_wstring
    (lp_printer, 0, 0, NULL, color, NULL, NULL), BAD_ARG);
  CHECK(lp_printer_print_wstring
    (NULL, 0, 0, L"Test", color, NULL, NULL), BAD_ARG);
  CHECK(lp_printer_print_wstring
    (lp_printer, 0, 0, L"Test", color, NULL, NULL), OK);

  CHECK(lp_printer_set_font(lp_printer, lp_font1), OK);

  CHECK(lp_printer_flush(NULL), BAD_ARG);
  CHECK(lp_printer_flush(lp_printer), OK);

  CHECK(lp_printer_ref_get(NULL), BAD_ARG);
  CHECK(lp_printer_ref_get(lp_printer), OK);
  CHECK(lp_printer_ref_put(NULL), BAD_ARG);
  CHECK(lp_printer_ref_put(lp_printer), OK);
  CHECK(lp_printer_ref_put(lp_printer), OK);

  LP(ref_put(lp));
  LP(font_ref_put(lp_font0));
  LP(font_ref_put(lp_font1));
  RBI(&rbi, context_ref_put(rb_ctxt));
  CHECK(rbi_shutdown(&rbi), 0);
  WM(device_ref_put(wm_dev));
  WM(window_ref_put(wm_win));

  CHECK(MEM_ALLOCATED_SIZE(&mem_default_allocator), 0);
  return 0;
}

