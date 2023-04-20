# Copyright © 2018 Rob Clark
# Copyright © 2019 Collabora
# Copyright © 2022 Icecream95

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

libpanfrost_base_versions = ['0', '1', '2', '258']
libpanfrost_base_per_arch = []

foreach ver : libpanfrost_base_versions
  libpanfrost_base_per_arch += static_library(
    'pan-base-v' + ver,
    'pan_vX_base.c',
    include_directories : [
      inc_src, inc_include, inc_gallium, inc_mesa, inc_gallium_aux,
      include_directories('include'),
    ],
    c_args : ['-DPAN_BASE_VER=' + ver],
    gnu_symbol_visibility : 'hidden',
    dependencies: [dep_valgrind],
)
endforeach

libpanfrost_base = static_library(
  'panfrost_base',
  'pan_base.c',
  include_directories : [
    inc_include, inc_src, inc_mapi, inc_mesa, inc_gallium, inc_gallium_aux, inc_panfrost_hw,
    include_directories('include'),
  ],
  gnu_symbol_visibility : 'hidden',
  build_by_default : false,
  link_with: [libpanfrost_base_per_arch],
)

libpanfrost_base_dep = declare_dependency(
  link_with: [libpanfrost_base_per_arch, libpanfrost_base],
  include_directories: [include_directories('.')],
)
