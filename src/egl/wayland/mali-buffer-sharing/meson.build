# Copyright © 2017 Intel Corporation

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

inc_mali_buffer_sharing = include_directories('.')

mali_buffer_sharing_protocol_c = custom_target(
  'mali-buffer-sharing-protocol.c',
  input : 'mali-buffer-sharing.xml',
  output : 'mali-buffer-sharing-protocol.c',
  command : [prog_wl_scanner, wl_scanner_arg, '@INPUT@', '@OUTPUT@'],
)

mali_buffer_sharing_client_protocol_h = custom_target(
  'mali-buffer-sharing-client-protocol.h',
  input : 'mali-buffer-sharing.xml',
  output : 'mali-buffer-sharing-client-protocol.h',
  command : [prog_wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

mali_buffer_sharing_server_protocol_h = custom_target(
  'mali-buffer-sharing-server-protocol.h',
  input : 'mali-buffer-sharing.xml',
  output : 'mali-buffer-sharing-server-protocol.h',
  command : [prog_wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

libmali_buffer_sharing = static_library(
  'mali-buffer-sharing',
  ['mali-buffer-sharing.c', mali_buffer_sharing_protocol_c, mali_buffer_sharing_server_protocol_h, wayland_drm_client_protocol_h],
  include_directories : inc_wayland_drm,
  gnu_symbol_visibility : 'hidden',
  dependencies : [dep_wayland_server],
  build_by_default : false,
)
