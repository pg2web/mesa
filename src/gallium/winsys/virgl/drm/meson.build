# Copyright © 2017 Intel Corporation
# SPDX-License-Identifier: MIT

virgldrm_deps = [dep_libdrm, dep_libvirglcommon, idep_mesautil, idep_nir_headers]
if with_platform_windows
  virgldrm_deps += dep_libvgpuapi
endif

libvirgldrm = static_library(
  'virgldrm',
  'virgl_drm_winsys.c',
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux, inc_gallium_drivers, inc_virtio],
  dependencies : virgldrm_deps,
  gnu_symbol_visibility : 'hidden',
)
