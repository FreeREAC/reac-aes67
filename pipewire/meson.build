# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# reac-pw — PipeWire-native REAC endpoint (RX source node; TX sink scaffold).
#
# Reuses, does not reinvent:
#   - libreac           RX validate / counter / rate-detect      (subproject wrap)
#   - reac-aes67 core   plain-LE decode + AF_PACKET + pcap reader (sibling source)
# and links against the system libpipewire-0.3 / libspa-0.2 (Fedora pipewire-devel).

project('reac-pw', 'c',
  version : '0.1.0',
  license : 'GPL-3.0-or-later',
  default_options : ['c_std=c11', 'warning_level=2', 'buildtype=debugoptimized'])

cc = meson.get_compiler('c')

# SPA's inline string.h uses newlocale()/uselocale()/locale_t (LC_ALL_MASK),
# which are GNU/XPG extensions — define _GNU_SOURCE project-wide so every TU that
# includes a PipeWire/SPA header gets them (PipeWire itself builds this way).
add_project_arguments('-D_GNU_SOURCE', language : 'c')

# --- system deps: PipeWire + SPA (the only external libs) -------------------
pipewire_dep = dependency('libpipewire-0.3', version : '>=0.3.65')
spa_dep      = dependency('libspa-0.2')
threads_dep  = dependency('threads')
# libm only needed by the decode core's siblings; harmless if unused.
m_dep        = cc.find_library('m', required : false)

# --- libreac: RX/measure core, pulled as a meson subproject -----------------
# subprojects/libreac.wrap clones FreeREAC/libreac. It builds a static lib and
# exposes `libreac_dep` (see subprojects/packagefiles/libreac/meson.build, which
# the wrap drops in via patch_directory since upstream libreac ships only a
# hand Makefile). The header is <reac/reac.h>.
libreac_proj = subproject('libreac')
libreac_dep  = libreac_proj.get_variable('libreac_dep')

# --- reac-aes67 decode/capture/pcap core: reuse the sibling checkout ---------
# These three files are dependency-free and already proven on-rig (plain-LE
# sample-major decode, coherence 0.999). We compile them straight in rather than
# fork them, exactly as reac-aes67's own Makefile compiles libreac's reac.c in.
# Override with -Dreac_aes67=/path/to/reac-aes67 if the checkout lives elsewhere.
reac_aes67 = get_option('reac_aes67')
reac_aes67_src = [
  reac_aes67 / 'src' / 'reac_decode.c',
  reac_aes67 / 'src' / 'reac_capture.c',
  reac_aes67 / 'src' / 'pcap_source.c',
]
foreach f : reac_aes67_src
  if not import('fs').is_file(f)
    error('reac-aes67 core not found at @0@ — clone github.com/FreeREAC/reac-aes67 as a sibling, or pass -Dreac_aes67=PATH'.format(reac_aes67))
  endif
endforeach
reac_aes67_inc = include_directories(reac_aes67 / 'src')

# --- the reac-pw client -----------------------------------------------------
reac_pw_src = [
  'src/main.c',
  'src/reac_ring.c',
  'src/reac_rx.c',
  'src/reac_tx.c',
  'src/reac_source_node.c',
  'src/reac_sink_node.c',
]

executable('reac-pw',
  reac_pw_src + reac_aes67_src,
  include_directories : [reac_aes67_inc],
  dependencies : [pipewire_dep, spa_dep, threads_dep, m_dep, libreac_dep],
  install : true)

# --- unit test: the lock-free ring (RT-safe SPSC), no PipeWire needed --------
ring_test = executable('test_reac_ring',
  ['tests/test_reac_ring.c', 'src/reac_ring.c'],
  dependencies : [threads_dep])
test('reac_ring', ring_test)

# --- unit test: TX encoder round-trips through the reac-aes67 decode core -----
# No PipeWire, no socket — pure encode->decode bit-compare. Compiles reac_tx.c +
# reac_decode.c with both include trees (<reac/reac.h> from libreac_dep, the
# reac_decode/reac_mode headers from reac_aes67_inc).
tx_test = executable('test_reac_tx',
  ['tests/test_reac_tx.c', 'src/reac_tx.c', reac_aes67 / 'src' / 'reac_decode.c'],
  include_directories : [reac_aes67_inc, include_directories('src')],
  dependencies : [libreac_dep, m_dep])
test('reac_tx', tx_test)


