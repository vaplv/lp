#include "lp.h"
#include "lp_printer.h"
#include <rb/rbi.h>
#include <rb/rb_types.h>
#include <snlsys/mem_allocator.h>
#include <wm/wm_device.h>
#include <wm/wm_window.h>

int
main(int argc, char** argv)
{
  /* Miscellaneous data */
  FILE* file = NULL;
  const char* driver_name = NULL;
  const char* font_name = NULL;
  int err = 0;
  /* Window Manager */
  struct wm_device* wm_dev = NULL;
  struct wm_window* wm_win = NULL;
  const struct wm_window_desc wm_win_desc = {
    .width = 640, .height = 480, .fullscreen = false
  };
  /* Render backend */
  struct rbi rbi;
  struct rb_context* rb_ctxt = NULL;

  if(argc != 3) {
    printf("usage: %s RB_DRIVER FONT\n", argv[0]);
    goto error;
  }
  driver_name = argv[1];
  font_name = argv[2];

  file = fopen(driver_name, "r");
  if(!file) {
    fprintf(stderr, "Invalid driver %s\n", driver_name);
    goto error;
  }
  fclose(file);

  file = fopen(font_name, "r");
  if(!file) {
    fprintf(stderr, "Invalid font name %s\n", font_name);
    goto error;
  }
  fclose(file);

  WM(create_device(NULL, &wm_dev));
  WM(create_window(wm_dev, &wm_win_desc, &wm_win));
  CHECK(rbi_init(driver_name, &rbi), 0);
  RBI(&rbi, create_context(NULL, &rb_ctxt));

  /* TODO */

  RBI(&rbi, context_ref_put(rb_ctxt));
  CHECK(rbi_shutdown(&rbi), 0);
  WM(device_ref_put(wm_dev));
  WM(window_ref_put(wm_win));

  CHECK(MEM_ALLOCATED_SIZE(&mem_default_allocator), 0);

exit:
  return err;
error:
  err = -1;
  goto exit;
}
