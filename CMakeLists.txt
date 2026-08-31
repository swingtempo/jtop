cmake_minimum_required(VERSION 3.10)
project(jtop CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-O2 -Wall)

find_package(PkgConfig REQUIRED)
pkg_check_modules(X11 REQUIRED x11)
pkg_check_modules(CAIRO REQUIRED cairo)

add_executable(jtop src/main.cpp)
target_include_directories(jtop PRIVATE ${X11_INCLUDE_DIRS} ${CAIRO_INCLUDE_DIRS})
target_link_libraries(jtop ${X11_LIBRARIES} ${CAIRO_LIBRARIES})

# headless render test (no X server needed) -> writes widget_test.png
add_executable(jtop_render_test test/render_test.cc)
target_include_directories(jtop_render_test PRIVATE ${X11_INCLUDE_DIRS} ${CAIRO_INCLUDE_DIRS})
target_link_libraries(jtop_render_test ${X11_LIBRARIES} ${CAIRO_LIBRARIES})
