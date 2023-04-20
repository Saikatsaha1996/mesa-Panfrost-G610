/*
 * Copyright (C) 2022 Icecream95 <ixn@disroot.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef PAN_BASE_NOOP_H
#define PAN_BASE_NOOP_H

/* For Mali-G610 as used in RK3588 */
#define PROP(name, value) ((name << 2) | 2), value
static const uint32_t gpu_props[] = {
   PROP(KBASE_GPUPROP_RAW_GPU_ID,             0xa8670000),
   PROP(KBASE_GPUPROP_PRODUCT_ID,                 0xa867),
   PROP(KBASE_GPUPROP_RAW_SHADER_PRESENT,        0x50005),
   PROP(KBASE_GPUPROP_RAW_TEXTURE_FEATURES_0, 0xc1ffff9e),
   PROP(KBASE_GPUPROP_TLS_ALLOC,                   0x800),
   PROP(KBASE_GPUPROP_RAW_TILER_FEATURES,          0x809),
};
#undef PROP

#define NOOP_COOKIE_ALLOC     0x41000
#define NOOP_COOKIE_USER_IO   0x42000
#define NOOP_COOKIE_MEM_ALLOC 0x43000

static int
kbase_ioctl(int fd, unsigned long request, ...)
{
   int ret = 0;

   va_list args;

   va_start(args, request);
   void *ptr = va_arg(args, void *);
   va_end(args);

   switch (request) {
   case KBASE_IOCTL_GET_GPUPROPS: {
      struct kbase_ioctl_get_gpuprops *props = ptr;

      if (props->size)
         memcpy((void *)(uintptr_t) props->buffer,
                gpu_props, MIN2(props->size, sizeof(gpu_props)));

      ret = sizeof(gpu_props);
      break;
   }

   case KBASE_IOCTL_MEM_ALLOC: {
      union kbase_ioctl_mem_alloc *alloc = ptr;

      alloc->out.gpu_va = NOOP_COOKIE_ALLOC;
      alloc->out.flags = BASE_MEM_SAME_VA;
      break;
   }

   case KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6: {
      union kbase_ioctl_cs_queue_group_create_1_6 *create = ptr;

      // TODO: Don't return duplicates?
      create->out.group_handle = 0;
      create->out.group_uid = 1;
      break;
   }

   case KBASE_IOCTL_CS_TILER_HEAP_INIT: {
      union kbase_ioctl_cs_tiler_heap_init *init = ptr;

      /* The values don't really matter, the CPU has no business in accessing
       * these. */
      init->out.gpu_heap_va = 0x60000;
      init->out.first_chunk_va = 0x61000;
      break;
   }

   case KBASE_IOCTL_CS_QUEUE_BIND: {
      union kbase_ioctl_cs_queue_bind *bind = ptr;
      bind->out.mmap_handle = NOOP_COOKIE_USER_IO;
      break;
   }

   case KBASE_IOCTL_MEM_IMPORT: {
      union kbase_ioctl_mem_import *import = ptr;

      if (import->in.type != BASE_MEM_IMPORT_TYPE_UMM) {
         ret = -1;
         errno = EINVAL;
         break;
      }

      int *fd = (int *)(uintptr_t) import->in.phandle;

      off_t size = lseek(*fd, 0, SEEK_END);

      import->out.flags = BASE_MEM_NEED_MMAP;
      import->out.gpu_va = NOOP_COOKIE_MEM_ALLOC;
      import->out.va_pages = DIV_ROUND_UP(size, 4096);
   }

   case KBASE_IOCTL_SET_FLAGS:
   case KBASE_IOCTL_MEM_EXEC_INIT:
   case KBASE_IOCTL_MEM_JIT_INIT:
   case KBASE_IOCTL_CS_QUEUE_REGISTER:
   case KBASE_IOCTL_CS_QUEUE_KICK:
   case KBASE_IOCTL_CS_TILER_HEAP_TERM:
   case KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE:
   case KBASE_IOCTL_MEM_SYNC:
      break;

   default:
      ret = -1;
      errno = ENOSYS;
   }

   return ret;
}

static void *
kbase_mmap(void *addr, size_t length, int prot, int flags,
           int fd, off_t offset)
{
   switch (offset) {
   case BASE_MEM_MAP_TRACKING_HANDLE:
   case BASEP_MEM_CSF_USER_REG_PAGE_HANDLE:
   case NOOP_COOKIE_ALLOC:
   case NOOP_COOKIE_USER_IO:
   case NOOP_COOKIE_MEM_ALLOC:
      return mmap(NULL, length, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

   default:
      errno = ENOSYS;
      return MAP_FAILED;
   }
}
#endif
