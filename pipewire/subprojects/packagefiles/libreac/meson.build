# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# meson shim for libreac (FreeREAC/libreac ships only a hand Makefile). Dropped
# into the libreac checkout by reac-pw's subprojects/libreac.wrap patch_directory.
# Builds the static RX/measure core and exposes libreac_dep with <reac/reac.h>.

project('libreac', 'c',
  version : '0.1.0',
  license : 'GPL-3.0-or-later',
  default_options : ['c_std=c11', 'warning_level=2'])

libreac_inc = include_directories('include')

libreac_lib = static_library('reac',
  ['src/reac.c'],
  include_directories : libreac_inc,
  install : false)

libreac_dep = declare_dependency(
  link_with : libreac_lib,
  include_directories : libreac_inc)
