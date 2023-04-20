/*
 * © Copyright 2017-2018 The Panfrost Community
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU license.
 *
 * A copy of the licence is included with the program, and can also be obtained
 * from Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 */

#ifndef __KBASE_IOCTL_MIDGARD_H__
#define __KBASE_IOCTL_MIDGARD_H__

#define KBASE_IOCTL_TYPE_BASE  0x80
#define KBASE_IOCTL_TYPE_MAX   0x82

union kbase_ioctl_mem_alloc {
        struct {
                union kbase_ioctl_header header;
                u64 va_pages;
                u64 commit_pages;
                u64 extension;
                u64 flags;
        } in;
        struct {
                union kbase_ioctl_header header;
                u64 pad[3];
                u64 flags;
                mali_ptr gpu_va;
                u16 va_alignment;
        } out;
        u64 pad[7];
} __attribute__((packed));

#define KBASE_IOCTL_TYPE_COUNT (KBASE_IOCTL_TYPE_MAX - KBASE_IOCTL_TYPE_BASE + 1)

#define KBASE_IOCTL_GET_VERSION             (_IOWR(0x80,  0, struct kbase_ioctl_get_version))
#define KBASE_IOCTL_MEM_ALLOC               (_IOWR(0x82,  0, union kbase_ioctl_mem_alloc))
#define KBASE_IOCTL_MEM_IMPORT              (_IOWR(0x82,  1, union kbase_ioctl_mem_import))
#define KBASE_IOCTL_MEM_COMMIT              (_IOWR(0x82,  2, struct kbase_ioctl_mem_commit))
#define KBASE_IOCTL_MEM_QUERY               (_IOWR(0x82,  3, struct kbase_ioctl_mem_query))
#define KBASE_IOCTL_MEM_FREE                (_IOWR(0x82,  4, struct kbase_ioctl_mem_free))
#define KBASE_IOCTL_MEM_FLAGS_CHANGE        (_IOWR(0x82,  5, struct kbase_ioctl_mem_flags_change))
#define KBASE_IOCTL_MEM_ALIAS               (_IOWR(0x82,  6, struct kbase_ioctl_mem_alias))
#define KBASE_IOCTL_MEM_SYNC                (_IOWR(0x82,  8, struct kbase_ioctl_mem_sync))
#define KBASE_IOCTL_POST_TERM               (_IOWR(0x82,  9, __ioctl_placeholder))
#define KBASE_IOCTL_HWCNT_SETUP             (_IOWR(0x82, 10, __ioctl_placeholder))
#define KBASE_IOCTL_HWCNT_DUMP              (_IOWR(0x82, 11, __ioctl_placeholder))
#define KBASE_IOCTL_HWCNT_CLEAR             (_IOWR(0x82, 12, __ioctl_placeholder))
#define KBASE_IOCTL_GPU_PROPS_REG_DUMP      (_IOWR(0x82, 14, struct kbase_ioctl_gpu_props_reg_dump))
#define KBASE_IOCTL_FIND_CPU_OFFSET         (_IOWR(0x82, 15, __ioctl_placeholder))
#define KBASE_IOCTL_GET_VERSION_NEW         (_IOWR(0x82, 16, struct kbase_ioctl_get_version))
#define KBASE_IOCTL_SET_FLAGS               (_IOWR(0x82, 18, struct kbase_ioctl_set_flags))
#define KBASE_IOCTL_SET_TEST_DATA           (_IOWR(0x82, 19, __ioctl_placeholder))
#define KBASE_IOCTL_INJECT_ERROR            (_IOWR(0x82, 20, __ioctl_placeholder))
#define KBASE_IOCTL_MODEL_CONTROL           (_IOWR(0x82, 21, __ioctl_placeholder))
#define KBASE_IOCTL_KEEP_GPU_POWERED        (_IOWR(0x82, 22, __ioctl_placeholder))
#define KBASE_IOCTL_FENCE_VALIDATE          (_IOWR(0x82, 23, __ioctl_placeholder))
#define KBASE_IOCTL_STREAM_CREATE           (_IOWR(0x82, 24, struct kbase_ioctl_stream_create))
#define KBASE_IOCTL_GET_PROFILING_CONTROLS  (_IOWR(0x82, 25, __ioctl_placeholder))
#define KBASE_IOCTL_SET_PROFILING_CONTROLS  (_IOWR(0x82, 26, __ioctl_placeholder))
#define KBASE_IOCTL_DEBUGFS_MEM_PROFILE_ADD (_IOWR(0x82, 27, __ioctl_placeholder))
#define KBASE_IOCTL_JOB_SUBMIT              (_IOWR(0x82, 28, struct kbase_ioctl_job_submit))
#define KBASE_IOCTL_DISJOINT_QUERY          (_IOWR(0x82, 29, __ioctl_placeholder))
#define KBASE_IOCTL_GET_CONTEXT_ID          (_IOWR(0x82, 31, struct kbase_ioctl_get_context_id))
#define KBASE_IOCTL_TLSTREAM_ACQUIRE_V10_4  (_IOWR(0x82, 32, __ioctl_placeholder))
#define KBASE_IOCTL_TLSTREAM_TEST           (_IOWR(0x82, 33, __ioctl_placeholder))
#define KBASE_IOCTL_TLSTREAM_STATS          (_IOWR(0x82, 34, __ioctl_placeholder))
#define KBASE_IOCTL_TLSTREAM_FLUSH          (_IOWR(0x82, 35, __ioctl_placeholder))
#define KBASE_IOCTL_HWCNT_READER_SETUP      (_IOWR(0x82, 36, __ioctl_placeholder))
#define KBASE_IOCTL_SET_PRFCNT_VALUES       (_IOWR(0x82, 37, __ioctl_placeholder))
#define KBASE_IOCTL_SOFT_EVENT_UPDATE       (_IOWR(0x82, 38, __ioctl_placeholder))
#define KBASE_IOCTL_MEM_JIT_INIT            (_IOWR(0x82, 39, __ioctl_placeholder))
#define KBASE_IOCTL_TLSTREAM_ACQUIRE        (_IOWR(0x82, 40, __ioctl_placeholder))

#endif /* __KBASE_IOCTL_MIDGARD_H__ */
