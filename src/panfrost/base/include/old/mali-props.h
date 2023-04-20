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

#ifndef __MALI_PROPS_H__
#define __MALI_PROPS_H__

#include "mali-ioctl.h"

#define MALI_GPU_NUM_TEXTURE_FEATURES_REGISTERS 3
#define MALI_GPU_MAX_JOB_SLOTS 16
#define MALI_MAX_COHERENT_GROUPS 16

/* Capabilities of a job slot as reported by JS_FEATURES registers */

#define JS_FEATURE_NULL_JOB              (1u << 1)
#define JS_FEATURE_SET_VALUE_JOB         (1u << 2)
#define JS_FEATURE_CACHE_FLUSH_JOB       (1u << 3)
#define JS_FEATURE_COMPUTE_JOB           (1u << 4)
#define JS_FEATURE_VERTEX_JOB            (1u << 5)
#define JS_FEATURE_GEOMETRY_JOB          (1u << 6)
#define JS_FEATURE_TILER_JOB             (1u << 7)
#define JS_FEATURE_FUSED_JOB             (1u << 8)
#define JS_FEATURE_FRAGMENT_JOB          (1u << 9)

struct mali_gpu_core_props {
	/**
	 * Product specific value.
	 */
	u32 product_id;

	/**
	 * Status of the GPU release.
	 * No defined values, but starts at 0 and increases by one for each
	 * release status (alpha, beta, EAC, etc.).
	 * 4 bit values (0-15).
	 */
	u16 version_status;

	/**
	 * Minor release number of the GPU. "P" part of an "RnPn" release
	 * number.
	 * 8 bit values (0-255).
	 */
	u16 minor_revision;

	/**
	 * Major release number of the GPU. "R" part of an "RnPn" release
	 * number.
	 * 4 bit values (0-15).
	 */
	u16 major_revision;

	u16 :16;

	/**
	 * @usecase GPU clock speed is not specified in the Midgard
	 * Architecture, but is <b>necessary for OpenCL's clGetDeviceInfo()
	 * function</b>.
	 */
	u32 gpu_speed_mhz;

	/**
	 * @usecase GPU clock max/min speed is required for computing
	 * best/worst case in tasks as job scheduling ant irq_throttling. (It
	 * is not specified in the Midgard Architecture).
	 */
	u32 gpu_freq_khz_max;
	u32 gpu_freq_khz_min;

	/**
	 * Size of the shader program counter, in bits.
	 */
	u32 log2_program_counter_size;

	/**
	 * TEXTURE_FEATURES_x registers, as exposed by the GPU. This is a
	 * bitpattern where a set bit indicates that the format is supported.
	 *
	 * Before using a texture format, it is recommended that the
	 * corresponding bit be checked.
	 */
	u32 texture_features[MALI_GPU_NUM_TEXTURE_FEATURES_REGISTERS];

	/**
	 * Theoretical maximum memory available to the GPU. It is unlikely
	 * that a client will be able to allocate all of this memory for their
	 * own purposes, but this at least provides an upper bound on the
	 * memory available to the GPU.
	 *
	 * This is required for OpenCL's clGetDeviceInfo() call when
	 * CL_DEVICE_GLOBAL_MEM_SIZE is requested, for OpenCL GPU devices. The
	 * client will not be expecting to allocate anywhere near this value.
	 */
	u64 gpu_available_memory_size;
};

struct mali_gpu_l2_cache_props {
	u8 log2_line_size;
	u8 log2_cache_size;
	u8 num_l2_slices; /* Number of L2C slices. 1 or higher */
	u64 :40;
};

struct mali_gpu_tiler_props {
	u32 bin_size_bytes;	/* Max is 4*2^15 */
	u32 max_active_levels;	/* Max is 2^15 */
};

struct mali_gpu_thread_props {
	u32 max_threads;            /* Max. number of threads per core */
	u32 max_workgroup_size;     /* Max. number of threads per workgroup */
	u32 max_barrier_size;       /* Max. number of threads that can
				       synchronize on a simple barrier */
	u16 max_registers;          /* Total size [1..65535] of the register
				       file available per core. */
	u8  max_task_queue;         /* Max. tasks [1..255] which may be sent
				       to a core before it becomes blocked. */
	u8  max_thread_group_split; /* Max. allowed value [1..15] of the
				       Thread Group Split field. */
	enum {
		MALI_GPU_IMPLEMENTATION_UNKNOWN = 0,
		MALI_GPU_IMPLEMENTATION_SILICON = 1,
		MALI_GPU_IMPLEMENTATION_FPGA    = 2,
		MALI_GPU_IMPLEMENTATION_SW      = 3,
	} impl_tech :8;
	u64 :56;
};

/**
 * @brief descriptor for a coherent group
 *
 * \c core_mask exposes all cores in that coherent group, and \c num_cores
 * provides a cached population-count for that mask.
 *
 * @note Whilst all cores are exposed in the mask, not all may be available to
 * the application, depending on the Kernel Power policy.
 *
 * @note if u64s must be 8-byte aligned, then this structure has 32-bits of
 * wastage.
 */
struct mali_ioctl_gpu_coherent_group {
	u64 core_mask;	       /**< Core restriction mask required for the
				 group */
	u16 num_cores;	       /**< Number of cores in the group */
	u64 :48;
};

/**
 * @brief Coherency group information
 *
 * Note that the sizes of the members could be reduced. However, the \c group
 * member might be 8-byte aligned to ensure the u64 core_mask is 8-byte
 * aligned, thus leading to wastage if the other members sizes were reduced.
 *
 * The groups are sorted by core mask. The core masks are non-repeating and do
 * not intersect.
 */
struct mali_gpu_coherent_group_info {
	u32 num_groups;

	/**
	 * Number of core groups (coherent or not) in the GPU. Equivalent to
	 * the number of L2 Caches.
	 *
	 * The GPU Counter dumping writes 2048 bytes per core group,
	 * regardless of whether the core groups are coherent or not. Hence
	 * this member is needed to calculate how much memory is required for
	 * dumping.
	 *
	 * @note Do not use it to work out how many valid elements are in the
	 * group[] member. Use num_groups instead.
	 */
	u32 num_core_groups;

	/**
	 * Coherency features of the memory, accessed by @ref gpu_mem_features
	 * methods
	 */
	u32 coherency;

	u32 :32;

	/**
	 * Descriptors of coherent groups
	 */
	struct mali_ioctl_gpu_coherent_group group[MALI_MAX_COHERENT_GROUPS];
};

/**
 * A complete description of the GPU's Hardware Configuration Discovery
 * registers.
 *
 * The information is presented inefficiently for access. For frequent access,
 * the values should be better expressed in an unpacked form in the
 * base_gpu_props structure.
 *
 * @usecase The raw properties in @ref gpu_raw_gpu_props are necessary to
 * allow a user of the Mali Tools (e.g. PAT) to determine "Why is this device
 * behaving differently?". In this case, all information about the
 * configuration is potentially useful, but it <b>does not need to be processed
 * by the driver</b>. Instead, the raw registers can be processed by the Mali
 * Tools software on the host PC.
 *
 */
struct mali_gpu_raw_props {
	u64 shader_present;
	u64 tiler_present;
	u64 l2_present;
	u64 stack_present;

	u32 l2_features;
	u32 suspend_size; /* API 8.2+ */
	u32 mem_features;
	u32 mmu_features;

	u32 as_present;

	u32 js_present;
	u32 js_features[MALI_GPU_MAX_JOB_SLOTS];
	u32 tiler_features;
	u32 texture_features[3];

	u32 gpu_id;

	u32 thread_max_threads;
	u32 thread_max_workgroup_size;
	u32 thread_max_barrier_size;
	u32 thread_features;

	/*
	 * Note: This is the _selected_ coherency mode rather than the
	 * available modes as exposed in the coherency_features register.
	 */
	u32 coherency_mode;
};

struct kbase_ioctl_gpu_props_reg_dump {
	union kbase_ioctl_header header;
	struct mali_gpu_core_props core;
	struct mali_gpu_l2_cache_props l2;
	u64 :64;
	struct mali_gpu_tiler_props tiler;
	struct mali_gpu_thread_props thread;

	struct mali_gpu_raw_props raw;

	/** This must be last member of the structure */
	struct mali_gpu_coherent_group_info coherency_info;
} __attribute__((packed));

#endif
