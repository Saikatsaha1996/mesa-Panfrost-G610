# Copyright © 2018 Rob Clark
# Copyright © 2019 Collabora

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

inc_panfrost_hw = include_directories([
   'include', 'base'
])

inc_panfrost = include_directories([
   '.', 'include', 'shared', 'midgard', 'bifrost', 'lib'
])

compile_args_panfrost = [
  '-Wno-pointer-arith'
]

subdir('shared')
subdir('util')
subdir('midgard')
subdir('bifrost')

subdir('base')

if with_gallium_panfrost or with_panfrost_vk
   subdir('lib')
   subdir('perf')
endif

files_bifrost = files(
  'bifrost/cmdline.c',
)

bifrost_compiler = executable(
  'bifrost_compiler',
  [files_bifrost],
  include_directories : [
    inc_mapi,
    inc_mesa,
    inc_gallium,
    inc_gallium_aux,
    inc_include,
    inc_src,
    inc_panfrost,
    inc_panfrost_hw,
 ],
  dependencies : [
    idep_nir,
    idep_mesautil,
    idep_bi_opcodes_h,
    dep_libdrm,
  ],
  link_with : [
    libglsl_standalone,
    libpanfrost_bifrost,
  ],
  build_by_default : with_tools.contains('panfrost')
)

csf_test = executable(
  'csf_test',
  ['csf_test/test.c'],
  include_directories : [
    inc_mapi,
    inc_mesa,
    inc_gallium,
    inc_gallium_aux,
    inc_include,
    inc_src,
    inc_panfrost,
    inc_panfrost_hw,
  ],
  dependencies : [
    idep_nir,
    idep_mesautil,
    idep_bi_opcodes_h,
    dep_libdrm,
    libpanfrost_dep,
  ],
  build_by_default : true
)

custom_target(
  'panfrost_panloader',
  output: ['panfrost_panloader.txt'],
  depends : [
    libpanfrost_lib,
    libpanfrost_util,
    _libmesa_util,
    libpanfrost_decode,
    libpanfrost_decode_per_arch,
    libpanfrost_midgard_disasm,
    libpanfrost_bifrost_disasm,
    libpanfrost_valhall_disasm,
  ],
  command: ['touch', '@OUTPUT@'],
  build_by_default : false,
)

if with_panfrost_vk
  subdir('vulkan')
endif

if with_perfetto and (with_datasources.contains('panfrost') or with_datasources.contains('auto'))
  subdir('ds')
endif

if with_tools.contains('drm-shim')
  subdir('drm-shim')
endif

if with_tools.contains('panfrost')
   subdir('tools')
endif
