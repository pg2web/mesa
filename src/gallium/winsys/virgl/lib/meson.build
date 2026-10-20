# SPDX-License-Identifier: MIT

deps_vgpuapi = []
if host_machine.system() == 'windows'
  c = meson.get_compiler('c')
  deps_vgpuapi += c.find_library('gdi32')
  deps_vgpuapi += c.find_library('user32')
  deps_vgpuapi += c.find_library('setupapi')
endif

libvgpuapi = static_library(
  'vgpu_api',
  'vgpu_api.c',
  include_directories : [
    inc_include,
    inc_src,
    inc_mesa,
    inc_gallium,
    inc_gallium_aux,
    inc_gallium_drivers,
    inc_virtio,
  ],
  dependencies : [dep_libdrm, idep_mesautil, idep_nir_headers, deps_vgpuapi],
  gnu_symbol_visibility : 'hidden',
)

dep_libvgpuapi = declare_dependency(
  link_with : libvgpuapi,
  include_directories : include_directories('.'),
)
