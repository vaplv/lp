cmake_minimum_required(VERSION 2.6)
project(lp C)
enable_testing()

################################################################################
# Setup compiler
################################################################################
if(NOT CMAKE_COMPILER_IS_GNUCC)
  message(FATAL_ERROR "Unsupported compiler")
endif(NOT CMAKE_COMPILER_IS_GNUCC)

set(CMAKE_DEBUG_POSTFIX "-dbg")

set(CMAKE_C_FLAGS "-pedantic -std=c99 -Wall -Wextra -Wcast-align -Wmissing-declarations -Wmissing-prototypes -fvisibility=hidden -fstrict-aliasing -Wl,-z,defs -Wconversion")
set(CMAKE_C_FLAGS_DEBUG "-g")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")

################################################################################
# Check dependencies
################################################################################
macro(check_library lib)
  find_library(${lib}_LIBRARY ${lib})
  if(NOT ${lib}_LIBRARY)
    message(STATUS "Can't find ${lib} library")
  else()
    message(STATUS "${lib} found: ${${lib}_LIBRARY}")
  endif()
endmacro()

macro(check_dependency lib header)
  check_library(${lib})

  find_path(${lib}_INCLUDE_DIR ${header})
  if(NOT ${lib}_INCLUDE_DIR)
    message(FATAL_ERROR "Can't find ${lib} header")
  endif()
  include_directories(${${lib}_INCLUDE_DIR})
endmacro()

check_dependency(font-rsrc font_rsrc.h)
check_dependency(rbi rb/rbi.h)
check_dependency(sl sl/sl.h)
check_dependency(snlsys snlsys/snlsys.h)
check_dependency(wm-glfw wm/wm.h)

check_dependency(font-rsrc-dbg font_rsrc.h)
check_dependency(rbi-dbg rb/rbi.h)
check_dependency(sl-dbg sl/sl.h)
check_dependency(snlsys-dbg snlsys/snlsys.h)
check_dependency(wm-glfw-dbg wm/wm.h)

################################################################################
# Target
################################################################################
set(LP_FILES_INC lp.h lp_error.h lp_font.h lp_printer.h)
set(LP_FILES_SRC lp.c lp_c.h lp_error_c.h lp_font.c lp_printer.c)
add_library(lp SHARED ${LP_FILES_SRC} ${LP_FILES_INC})
set_target_properties(lp PROPERTIES DEFINE_SYMBOL LP_SHARED_BUILD)
target_link_libraries(lp ${snlsys_LIBRARY} ${sl_LIBRARY} ${rbi_LIBRARY})

################################################################################
# Add tests
################################################################################
macro(check_font font suffix)
  find_file(${font}_FONT ${font}.${suffix} PATH_SUFFIXES etc/font/ )
  if(NOT ${font}_FONT)
    message(STATUS "Can't find ${font}.${suffix} font")
  else()
    message(STATUS "${font} found: ${${font}_FONT}")
  endif()
endmacro()

check_font(Tower_Print ttf)
check_font(6x12-iso8859-1 fon)
check_font(8x13-iso8859-1 fon)
check_library(rb-ogl3)
check_library(rb-null)

# Test font
add_executable(test_lp_font test_lp_font.c)
target_link_libraries(test_lp_font
  lp ${snlsys-dbg_LIBRARY} ${font-rsrc-dbg_LIBRARY} ${wm-glfw-dbg_LIBRARY})
target_link_libraries(test_lp_font
  lp ${snlsys_LIBRARY} ${font-rsrc_LIBRARY} ${wm-glfw_LIBRARY})

if(Tower_Print_FONT AND rb-null_LIBRARY)
  add_test(test_lp_font_null_TowerPrint
    test_lp_font ${rb-null_LIBRARY} ${Tower_Print_FONT})
endif()
if(Tower_Print_FONT AND rb-ogl3_LIBRARY)
  add_test(test_lp_font_ogl3_TowerPrint
    test_lp_font ${rb-ogl3_LIBRARY} ${Tower_Print_FONT})
endif()
if(6x12-iso8859-1_FONT AND rb-ogl3_LIBRARY)
  add_test(test_lp_font_ogl3_6x12-iso8859-1
    test_lp_font ${rb-ogl3_LIBRARY} ${6x12-iso8859-1_FONT})
endif()
if(8x13-iso8859-1_FONT AND rb-ogl3_LIBRARY)
  add_test(test_lp_font_ogl3_8x13-iso8859-1
    test_lp_font ${rb-ogl3_LIBRARY} ${8x13-iso8859-1_FONT})
endif()

# Test printer
add_executable(test_lp_printer test_lp_printer.c)
target_link_libraries(test_lp_printer debug
  lp ${snlsys-dbg_LIBRARY} ${font-rsrc-dbg_LIBRARY} ${wm-glfw-dbg_LIBRARY})
target_link_libraries(test_lp_printer optimized
  lp ${snlsys_LIBRARY} ${font-rsrc_LIBRARY} ${wm-glfw_LIBRARY})

add_test(test_lp_printer_ogl3_8x13-iso8859-1
  test_lp_printer ${rb-ogl3_LIBRARY} ${8x13-iso8859-1_FONT})

################################################################################
# Output files
################################################################################
install(FILES ${LP_FILES_INC} DESTINATION include/lp)
install(TARGETS lp LIBRARY DESTINATION lib)

