cmake_minimum_required(VERSION 3.19)
project(hyprmagic VERSION "0.1.0")
set(CMAKE_CXX_STANDARD 23)
add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-unused-value -Wno-missing-field-initializers -Wno-narrowing)

# nix workaround
if(CMAKE_EXPORT_COMPILE_COMMANDS)
  set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES
      ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED hyprland pixman-1 libdrm wayland-server xkbcommon pango pangocairo)

add_library(hyprmagic SHARED src/main.cpp src/floaters.cpp src/lua_bindings.cpp)
target_include_directories(hyprmagic PRIVATE ${DEPS_INCLUDE_DIRS})

# -fno-gnu-unique: see hyprwsg_plugin/CMakeLists.txt — required for live
# unload+load swaps to pick up a new build (GNU-unique symbols pin the old
# image on dlclose).
target_compile_options(hyprmagic PRIVATE -fno-gnu-unique)
# out/: one-time path change — the original build/libhyprmagic.so path string
# is dlopen-pinned in any session that ever loaded a pre--fno-gnu-unique build.
set_target_properties(hyprmagic PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out)

option(HYPRMAGIC_NO_VERSION_CHECK "Disable hyprland version check" FALSE)
if(HYPRMAGIC_NO_VERSION_CHECK)
    target_compile_definitions(hyprmagic PRIVATE HYPRMAGIC_NO_VERSION_CHECK=TRUE)
endif()

# ── waybar CFFI module (in-process waybar widget; see waybar/) ──────────
# Renders the floater indicator inside waybar itself; state comes from the
# compositor plugin's magic:waybar-state hyprctl command. No hyprland headers.
pkg_check_modules(GTK3 REQUIRED gtk+-3.0 gtk-layer-shell-0)
pkg_check_modules(WAYLAND REQUIRED wayland-client)

# xdg-output-unstable-v1: GDK reports each monitor's logical position but not its
# Hyprland connector name, and Hyprland hardcodes wl_output.geometry to (0,0) —
# so the module binds xdg-output itself for logical positions and maps the bar's
# GdkMonitor to a connector by position (robust even for identical-model twins).
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
pkg_get_variable(WL_PROTO_DIR wayland-protocols pkgdatadir)
set(XDG_OUTPUT_XML ${WL_PROTO_DIR}/unstable/xdg-output/xdg-output-unstable-v1.xml)
set(WL_GEN ${CMAKE_BINARY_DIR}/wl-gen)
file(MAKE_DIRECTORY ${WL_GEN})
add_custom_command(OUTPUT ${WL_GEN}/xdg-output-client-protocol.h
  COMMAND ${WAYLAND_SCANNER} client-header ${XDG_OUTPUT_XML} ${WL_GEN}/xdg-output-client-protocol.h
  DEPENDS ${XDG_OUTPUT_XML})
add_custom_command(OUTPUT ${WL_GEN}/xdg-output-protocol.c
  COMMAND ${WAYLAND_SCANNER} private-code ${XDG_OUTPUT_XML} ${WL_GEN}/xdg-output-protocol.c
  DEPENDS ${XDG_OUTPUT_XML})

add_library(hyprmagic_waybar SHARED
  waybar/module.cpp waybar/render.cpp
  ${WL_GEN}/xdg-output-protocol.c ${WL_GEN}/xdg-output-client-protocol.h)
target_include_directories(hyprmagic_waybar PRIVATE ${GTK3_INCLUDE_DIRS} ${WAYLAND_INCLUDE_DIRS} ${WL_GEN})
target_link_libraries(hyprmagic_waybar PRIVATE ${GTK3_LIBRARIES} ${WAYLAND_LIBRARIES})
target_compile_options(hyprmagic_waybar PRIVATE -fno-gnu-unique) # waybar dlcloses modules on reload
set_target_properties(hyprmagic_waybar PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out)

# Diff harness: render a magic:waybar-state reply from stdin (compare against
# the Rust binary's waybar JSON). See waybar/render_diff.sh.
add_executable(render_test waybar/render_test.cpp waybar/render.cpp)

include(GNUInstallDirs)
install(TARGETS hyprmagic hyprmagic_waybar LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
