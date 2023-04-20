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

/**
 * Definitions for all of the ioctls for the original open source bifrost GPU
 * kernel driver, written by ARM.
 */

#ifndef __KBASE_IOCTL_H__
#define __KBASE_IOCTL_H__

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int32_t s32;
typedef int64_t s64;


typedef u8 mali_atom_id;

/**
 * Since these structs are passed to and from the kernel we need to make sure
 * that we get the size of each struct to match exactly what the kernel is
 * expecting. So, when editing this file make sure to add static asserts that
 * check each struct's size against the arg length you see in strace.
 */

enum kbase_ioctl_mem_flags {
	/* IN */
	BASE_MEM_PROT_CPU_RD = (1U << 0),      /**< Read access CPU side */
	BASE_MEM_PROT_CPU_WR = (1U << 1),      /**< Write access CPU side */
	BASE_MEM_PROT_GPU_RD = (1U << 2),      /**< Read access GPU side */
	BASE_MEM_PROT_GPU_WR = (1U << 3),      /**< Write access GPU side */
	BASE_MEM_PROT_GPU_EX = (1U << 4),      /**< Execute allowed on the GPU
						    side */

	BASE_MEM_GROW_ON_GPF = (1U << 9),      /**< Grow backing store on GPU
						    Page Fault */

	BASE_MEM_COHERENT_SYSTEM = (1U << 10), /**< Page coherence Outer
						    shareable, if available */
	BASE_MEM_COHERENT_LOCAL = (1U << 11),  /**< Page coherence Inner
						    shareable */
	BASE_MEM_CACHED_CPU = (1U << 12),      /**< Should be cached on the
						    CPU */

	/* IN/OUT */
	BASE_MEM_SAME_VA = (1U << 13), /**< Must have same VA on both the GPU
					    and the CPU */
	/* OUT */
	BASE_MEM_NEED_MMAP = (1U << 14), /**< Must call mmap to acquire a GPU
					     address for the alloc */
	/* IN */
	BASE_MEM_COHERENT_SYSTEM_REQUIRED = (1U << 15), /**< Page coherence
					     Outer shareable, required. */
	BASE_MEM_SECURE = (1U << 16),          /**< Secure memory */
	BASE_MEM_DONT_NEED = (1U << 17),       /**< Not needed physical
						    memory */
	BASE_MEM_IMPORT_SHARED = (1U << 18),   /**< Must use shared CPU/GPU zone
						    (SAME_VA zone) but doesn't
						    require the addresses to
						    be the same */
};

#define KBASE_IOCTL_MEM_FLAGS_IN_MASK                                          \
	(BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |                        \
	 BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_PROT_GPU_EX | \
	 BASE_MEM_GROW_ON_GPF |                                               \
	 BASE_MEM_COHERENT_SYSTEM | BASE_MEM_COHERENT_LOCAL |                 \
	 BASE_MEM_CACHED_CPU |                                                \
	 BASE_MEM_COHERENT_SYSTEM_REQUIRED | BASE_MEM_SECURE |                \
	 BASE_MEM_DONT_NEED | BASE_MEM_IMPORT_SHARED)
#define BASE_MEM_MAP_TRACKING_HANDLE (3ull << 12)

enum kbase_ioctl_coherency_mode {
	COHERENCY_ACE_LITE = 0,
	COHERENCY_ACE      = 1,
	COHERENCY_NONE     = 31
};

/*
 * Mali Atom priority
 *
 * Only certain priority levels are actually implemented, as specified by the
 * BASE_JD_PRIO_<...> definitions below. It is undefined to use a priority
 * level that is not one of those defined below.
 *
 * Priority levels only affect scheduling between atoms of the same type within
 * a mali context, and only after the atoms have had dependencies resolved.
 * Fragment atoms does not affect non-frament atoms with lower priorities, and
 * the other way around. For example, a low priority atom that has had its
 * dependencies resolved might run before a higher priority atom that has not
 * had its dependencies resolved.
 *
 * The scheduling between mali contexts/processes and between atoms from
 * different mali contexts/processes is unaffected by atom priority.
 *
 * The atoms are scheduled as follows with respect to their priorities:
 * - Let atoms 'X' and 'Y' be for the same job slot who have dependencies
 *   resolved, and atom 'X' has a higher priority than atom 'Y'
 * - If atom 'Y' is currently running on the HW, then it is interrupted to
 *   allow atom 'X' to run soon after
 * - If instead neither atom 'Y' nor atom 'X' are running, then when choosing
 *   the next atom to run, atom 'X' will always be chosen instead of atom 'Y'
 * - Any two atoms that have the same priority could run in any order with
 *   respect to each other. That is, there is no ordering constraint between
 *   atoms of the same priority.
 */
typedef u8 mali_jd_prio;
#define BASE_JD_PRIO_MEDIUM  ((mali_jd_prio)0)
#define BASE_JD_PRIO_HIGH    ((mali_jd_prio)1)
#define BASE_JD_PRIO_LOW     ((mali_jd_prio)2)

/**
 * @brief Job dependency type.
 *
 * A flags field will be inserted into the atom structure to specify whether a
 * dependency is a data or ordering dependency (by putting it before/after
 * 'core_req' in the structure it should be possible to add without changing
 * the structure size).  When the flag is set for a particular dependency to
 * signal that it is an ordering only dependency then errors will not be
 * propagated.
 */
typedef u8 mali_jd_dep_type;
#define BASE_JD_DEP_TYPE_INVALID  (0)       /**< Invalid dependency */
#define BASE_JD_DEP_TYPE_DATA     (1U << 0) /**< Data dependency */
#define BASE_JD_DEP_TYPE_ORDER    (1U << 1) /**< Order dependency */

/**
 * @brief Job chain hardware requirements.
 *
 * A job chain must specify what GPU features it needs to allow the
 * driver to schedule the job correctly.  By not specifying the
 * correct settings can/will cause an early job termination.  Multiple
 * values can be ORed together to specify multiple requirements.
 * Special case is ::BASE_JD_REQ_DEP, which is used to express complex
 * dependencies, and that doesn't execute anything on the hardware.
 */
typedef u32 mali_jd_core_req;

/* Requirements that come from the HW */

/**
 * No requirement, dependency only
 */
#define BASE_JD_REQ_DEP ((mali_jd_core_req)0)

/**
 * Requires fragment shaders
 */
#define BASE_JD_REQ_FS  ((mali_jd_core_req)1 << 0)

/**
 * Requires compute shaders
 * This covers any of the following Midgard Job types:
 * - Vertex Shader Job
 * - Geometry Shader Job
 * - An actual Compute Shader Job
 *
 * Compare this with @ref BASE_JD_REQ_ONLY_COMPUTE, which specifies that the
 * job is specifically just the "Compute Shader" job type, and not the "Vertex
 * Shader" nor the "Geometry Shader" job type.
 */
#define BASE_JD_REQ_CS  ((mali_jd_core_req)1 << 1)
#define BASE_JD_REQ_T   ((mali_jd_core_req)1 << 2)   /**< Requires tiling */
#define BASE_JD_REQ_CF  ((mali_jd_core_req)1 << 3)   /**< Requires cache flushes */
#define BASE_JD_REQ_V   ((mali_jd_core_req)1 << 4)   /**< Requires value writeback */

/* SW-only requirements - the HW does not expose these as part of the job slot
 * capabilities */

/* Requires fragment job with AFBC encoding */
#define BASE_JD_REQ_FS_AFBC  ((mali_jd_core_req)1 << 13)

/**
 * SW-only requirement: coalesce completion events.
 * If this bit is set then completion of this atom will not cause an event to
 * be sent to userspace, whether successful or not; completion events will be
 * deferred until an atom completes which does not have this bit set.
 *
 * This bit may not be used in combination with BASE_JD_REQ_EXTERNAL_RESOURCES.
 */
#define BASE_JD_REQ_EVENT_COALESCE ((mali_jd_core_req)1 << 5)

/**
 * SW Only requirement: the job chain requires a coherent core group. We don't
 * mind which coherent core group is used.
 */
#define BASE_JD_REQ_COHERENT_GROUP  ((mali_jd_core_req)1 << 6)

/**
 * SW Only requirement: The performance counters should be enabled only when
 * they are needed, to reduce power consumption.
 */

#define BASE_JD_REQ_PERMON               ((mali_jd_core_req)1 << 7)

/**
 * SW Only requirement: External resources are referenced by this atom.  When
 * external resources are referenced no syncsets can be bundled with the atom
 * but should instead be part of a NULL jobs inserted into the dependency
 * tree.  The first pre_dep object must be configured for the external
 * resouces to use, the second pre_dep object can be used to create other
 * dependencies.
 *
 * This bit may not be used in combination with BASE_JD_REQ_EVENT_COALESCE.
 */
#define BASE_JD_REQ_EXTERNAL_RESOURCES   ((mali_jd_core_req)1 << 8)

/**
 * SW Only requirement: Software defined job. Jobs with this bit set will not
 * be submitted to the hardware but will cause some action to happen within
 * the driver
 */
#define BASE_JD_REQ_SOFT_JOB        ((mali_jd_core_req)1 << 9)

#define BASE_JD_REQ_SOFT_DUMP_CPU_GPU_TIME      (BASE_JD_REQ_SOFT_JOB | 0x1)
#define BASE_JD_REQ_SOFT_FENCE_TRIGGER          (BASE_JD_REQ_SOFT_JOB | 0x2)
#define BASE_JD_REQ_SOFT_FENCE_WAIT             (BASE_JD_REQ_SOFT_JOB | 0x3)

/**
 * SW Only requirement : Replay job.
 *
 * If the preceding job fails, the replay job will cause the jobs specified in
 * the list of mali_jd_replay_payload pointed to by the jc pointer to be
 * replayed.
 *
 * A replay job will only cause jobs to be replayed up to MALIP_JD_REPLAY_LIMIT
 * times. If a job fails more than MALIP_JD_REPLAY_LIMIT times then the replay
 * job is failed, as well as any following dependencies.
 *
 * The replayed jobs will require a number of atom IDs. If there are not enough
 * free atom IDs then the replay job will fail.
 *
 * If the preceding job does not fail, then the replay job is returned as
 * completed.
 *
 * The replayed jobs will never be returned to userspace. The preceding failed
 * job will be returned to userspace as failed; the status of this job should
 * be ignored. Completion should be determined by the status of the replay soft
 * job.
 *
 * In order for the jobs to be replayed, the job headers will have to be
 * modified. The Status field will be reset to NOT_STARTED. If the Job Type
 * field indicates a Vertex Shader Job then it will be changed to Null Job.
 *
 * The replayed jobs have the following assumptions :
 *
 * - No external resources. Any required external resources will be held by the
 *   replay atom.
 * - Pre-dependencies are created based on job order.
 * - Atom numbers are automatically assigned.
 * - device_nr is set to 0. This is not relevant as
 *   BASE_JD_REQ_SPECIFIC_COHERENT_GROUP should not be set.
 * - Priority is inherited from the replay job.
 */
#define BASE_JD_REQ_SOFT_REPLAY                 (BASE_JD_REQ_SOFT_JOB | 0x4)
/**
 * SW only requirement: event wait/trigger job.
 *
 * - BASE_JD_REQ_SOFT_EVENT_WAIT: this job will block until the event is set.
 * - BASE_JD_REQ_SOFT_EVENT_SET: this job sets the event, thus unblocks the
 *   other waiting jobs. It completes immediately.
 * - BASE_JD_REQ_SOFT_EVENT_RESET: this job resets the event, making it
 *   possible for other jobs to wait upon. It completes immediately.
 */
#define BASE_JD_REQ_SOFT_EVENT_WAIT             (BASE_JD_REQ_SOFT_JOB | 0x5)
#define BASE_JD_REQ_SOFT_EVENT_SET              (BASE_JD_REQ_SOFT_JOB | 0x6)
#define BASE_JD_REQ_SOFT_EVENT_RESET            (BASE_JD_REQ_SOFT_JOB | 0x7)

#define BASE_JD_REQ_SOFT_DEBUG_COPY             (BASE_JD_REQ_SOFT_JOB | 0x8)

/**
 * SW only requirement: Just In Time allocation
 *
 * This job requests a JIT allocation based on the request in the
 * @base_jit_alloc_info structure which is passed via the jc element of
 * the atom.
 *
 * It should be noted that the id entry in @base_jit_alloc_info must not
 * be reused until it has been released via @BASE_JD_REQ_SOFT_JIT_FREE.
 *
 * Should this soft job fail it is expected that a @BASE_JD_REQ_SOFT_JIT_FREE
 * soft job to free the JIT allocation is still made.
 *
 * The job will complete immediately.
 */
#define BASE_JD_REQ_SOFT_JIT_ALLOC              (BASE_JD_REQ_SOFT_JOB | 0x9)
/**
 * SW only requirement: Just In Time free
 *
 * This job requests a JIT allocation created by @BASE_JD_REQ_SOFT_JIT_ALLOC
 * to be freed. The ID of the JIT allocation is passed via the jc element of
 * the atom.
 *
 * The job will complete immediately.
 */
#define BASE_JD_REQ_SOFT_JIT_FREE               (BASE_JD_REQ_SOFT_JOB | 0xa)

/**
 * SW only requirement: Map external resource
 *
 * This job requests external resource(s) are mapped once the dependencies
 * of the job have been satisfied. The list of external resources are
 * passed via the jc element of the atom which is a pointer to a
 * @base_external_resource_list.
 */
#define BASE_JD_REQ_SOFT_EXT_RES_MAP            (BASE_JD_REQ_SOFT_JOB | 0xb)
/**
 * SW only requirement: Unmap external resource
 *
 * This job requests external resource(s) are unmapped once the dependencies
 * of the job has been satisfied. The list of external resources are
 * passed via the jc element of the atom which is a pointer to a
 * @base_external_resource_list.
 */
#define BASE_JD_REQ_SOFT_EXT_RES_UNMAP          (BASE_JD_REQ_SOFT_JOB | 0xc)

/**
 * HW Requirement: Requires Compute shaders (but not Vertex or Geometry Shaders)
 *
 * This indicates that the Job Chain contains Midgard Jobs of the 'Compute
 * Shaders' type.
 *
 * In contrast to @ref BASE_JD_REQ_CS, this does \b not indicate that the Job
 * Chain contains 'Geometry Shader' or 'Vertex Shader' jobs.
 */
#define BASE_JD_REQ_ONLY_COMPUTE    ((mali_jd_core_req)1 << 10)

/**
 * HW Requirement: Use the mali_jd_atom::device_nr field to specify a
 * particular core group
 *
 * If both @ref BASE_JD_REQ_COHERENT_GROUP and this flag are set, this flag
 * takes priority
 *
 * This is only guaranteed to work for @ref BASE_JD_REQ_ONLY_COMPUTE atoms.
 *
 * If the core availability policy is keeping the required core group turned
 * off, then the job will fail with a @ref BASE_JD_EVENT_PM_EVENT error code.
 */
#define BASE_JD_REQ_SPECIFIC_COHERENT_GROUP ((mali_jd_core_req)1 << 11)

/**
 * SW Flag: If this bit is set then the successful completion of this atom
 * will not cause an event to be sent to userspace
 */
#define BASE_JD_REQ_EVENT_ONLY_ON_FAILURE   ((mali_jd_core_req)1 << 12)

/**
 * SW Flag: If this bit is set then completion of this atom will not cause an
 * event to be sent to userspace, whether successful or not.
 */
#define BASE_JD_REQ_EVENT_NEVER ((mali_jd_core_req)1 << 14)

/**
 * SW Flag: Skip GPU cache clean and invalidation before starting a GPU job.
 *
 * If this bit is set then the GPU's cache will not be cleaned and invalidated
 * until a GPU job starts which does not have this bit set or a job completes
 * which does not have the @ref BASE_JD_REQ_SKIP_CACHE_END bit set. Do not use if
 * the CPU may have written to memory addressed by the job since the last job
 * without this bit set was submitted.
 */
#define BASE_JD_REQ_SKIP_CACHE_START ((mali_jd_core_req)1 << 15)

/**
 * SW Flag: Skip GPU cache clean and invalidation after a GPU job completes.
 *
 * If this bit is set then the GPU's cache will not be cleaned and invalidated
 * until a GPU job completes which does not have this bit set or a job starts
 * which does not have the @ref BASE_JD_REQ_SKIP_CACHE_START bti set. Do not
 * use if the CPU may read from or partially overwrite memory addressed by the
 * job before the next job without this bit set completes.
 */
#define BASE_JD_REQ_SKIP_CACHE_END ((mali_jd_core_req)1 << 16)

/**
 * These requirement bits are currently unused in mali_jd_core_req
 */
#define MALIP_JD_REQ_RESERVED \
	(~(BASE_JD_REQ_ATOM_TYPE | BASE_JD_REQ_EXTERNAL_RESOURCES | \
	BASE_JD_REQ_EVENT_ONLY_ON_FAILURE | MALIP_JD_REQ_EVENT_NEVER | \
	BASE_JD_REQ_EVENT_COALESCE | \
	BASE_JD_REQ_COHERENT_GROUP | BASE_JD_REQ_SPECIFIC_COHERENT_GROUP | \
	BASE_JD_REQ_FS_AFBC | BASE_JD_REQ_PERMON | \
	BASE_JD_REQ_SKIP_CACHE_START | BASE_JD_REQ_SKIP_CACHE_END))

/**
 * Mask of all bits in mali_jd_core_req that control the type of the atom.
 *
 * This allows dependency only atoms to have flags set
 */
#define BASE_JD_REQ_ATOM_TYPE \
	(BASE_JD_REQ_FS | BASE_JD_REQ_CS | BASE_JD_REQ_T | BASE_JD_REQ_CF | \
	BASE_JD_REQ_V | BASE_JD_REQ_SOFT_JOB | BASE_JD_REQ_ONLY_COMPUTE)

/**
 * Mask of all bits in mali_jd_core_req that control the type of a soft job.
 */
#define BASE_JD_REQ_SOFT_JOB_TYPE (BASE_JD_REQ_SOFT_JOB | 0x1f)

/*
 * Returns non-zero value if core requirements passed define a soft job or
 * a dependency only job.
 */
#define BASE_JD_REQ_SOFT_JOB_OR_DEP(core_req) \
	((core_req & BASE_JD_REQ_SOFT_JOB) || \
	(core_req & BASE_JD_REQ_ATOM_TYPE) == BASE_JD_REQ_DEP)

/**
 * @brief The payload for a replay job. This must be in GPU memory.
 */
struct mali_jd_replay_payload {
	/**
	 * Pointer to the first entry in the mali_jd_replay_jc list.  These
	 * will be replayed in @b reverse order (so that extra ones can be added
	 * to the head in future soft jobs without affecting this soft job)
	 */
	u64 tiler_jc_list;

	/**
	 * Pointer to the fragment job chain.
	 */
	u64 fragment_jc;

	/**
	 * Pointer to the tiler heap free FBD field to be modified.
	 */
	u64 tiler_heap_free;

	/**
	 * Hierarchy mask for the replayed fragment jobs. May be zero.
	 */
	u16 fragment_hierarchy_mask;

	/**
	 * Hierarchy mask for the replayed tiler jobs. May be zero.
	 */
	u16 tiler_hierarchy_mask;

	/**
	 * Default weight to be used for hierarchy levels not in the original
	 * mask.
	 */
	u32 hierarchy_default_weight;

	/**
	 * Core requirements for the tiler job chain
	 */
	mali_jd_core_req tiler_core_req;

	/**
	 * Core requirements for the fragment job chain
	 */
	mali_jd_core_req fragment_core_req;
};

/**
 * @brief An entry in the linked list of job chains to be replayed. This must
 *        be in GPU memory.
 */
struct mali_jd_replay_jc {
	/**
	 * Pointer to next entry in the list. A setting of NULL indicates the
	 * end of the list.
	 */
	u64 next;

	/**
	 * Pointer to the job chain.
	 */
	u64 jc;
};

typedef u64 mali_ptr;

#define MALI_PTR_FMT "0x%" PRIx64
#define MALI_SHORT_PTR_FMT "0x%" PRIxPTR

#ifdef __LP64__
#define PAD_CPU_PTR(p) p
#else
#define PAD_CPU_PTR(p) p; u32 :32;
#endif

/* FIXME: Again, they don't specify any of these as packed structs. However,
 * looking at these structs I'm worried that there is already spots where the
 * compiler is potentially sticking in padding...
 * Going to try something a little crazy, and just hope that our compiler
 * happens to add the same kind of offsets since we can't really compare sizes
 */

/*
 * Blob provided by the driver to store callback driver, not actually modified
 * by the driver itself
 */
struct mali_jd_udata {
	u64 blob[2];
};

struct mali_jd_dependency {
	mali_atom_id  atom_id;               /**< An atom number */
	mali_jd_dep_type dependency_type;    /**< Dependency type */
};

#define MALI_EXT_RES_MAX 10

/* The original header never explicitly defines any values for these. In C,
 * this -should- expand to SHARED == 0 and EXCLUSIVE == 1, so the only flag we
 * actually need to decode here is EXCLUSIVE
 */
enum mali_external_resource_access {
	MALI_EXT_RES_ACCESS_SHARED,
	MALI_EXT_RES_ACCESS_EXCLUSIVE,
};

/* An aligned address to the resource | mali_external_resource_access */
typedef u64 mali_external_resource;

struct base_jd_atom_v2 {
	mali_ptr jc;           /**< job-chain GPU address */
	struct mali_jd_udata udata;	    /**< user data */
	u64 extres_list; /**< list of external resources */
	u16 nr_extres;			    /**< nr of external resources */
	u16 compat_core_req;	            /**< core requirements which
					      correspond to the legacy support
					      for UK 10.2 */
	struct mali_jd_dependency pre_dep[2];  /**< pre-dependencies, one need to
					      use SETTER function to assign
					      this field, this is done in
					      order to reduce possibility of
					      improper assigment of a
					      dependency field */
	mali_atom_id atom_number;	    /**< unique number to identify the
					      atom */
	mali_jd_prio prio;                  /**< Atom priority. Refer to @ref
					      mali_jd_prio for more details */
	u8 device_nr;			    /**< coregroup when
					      BASE_JD_REQ_SPECIFIC_COHERENT_GROUP
					      specified */
	u8 :8;
	mali_jd_core_req core_req;          /**< core requirements */
} __attribute__((packed));

/**
 * enum mali_error - Mali error codes shared with userspace
 *
 * This is subset of those common Mali errors that can be returned to userspace.
 * Values of matching user and kernel space enumerators MUST be the same.
 * MALI_ERROR_NONE is guaranteed to be 0.
 *
 * @MALI_ERROR_NONE: Success
 * @MALI_ERROR_OUT_OF_GPU_MEMORY: Not used in the kernel driver
 * @MALI_ERROR_OUT_OF_MEMORY: Memory allocation failure
 * @MALI_ERROR_FUNCTION_FAILED: Generic error code
 */
enum mali_error {
	MALI_ERROR_NONE = 0,
	MALI_ERROR_OUT_OF_GPU_MEMORY,
	MALI_ERROR_OUT_OF_MEMORY,
	MALI_ERROR_FUNCTION_FAILED,
};

/**
 * Header used by all ioctls
 */
union kbase_ioctl_header {
#ifdef dvalin
	u32 pad[0];
#else
	/* [in] The ID of the UK function being called */
	u32 id :32;
	/* [out] The return value of the UK function that was called */
	enum mali_error rc :32;

	u64 :64;
#endif
} __attribute__((packed));

struct kbase_ioctl_get_version {
	union kbase_ioctl_header header;
	u16 major; /* [out] */
	u16 minor; /* [out] */
	u32 :32;
} __attribute__((packed));

struct mali_mem_import_user_buffer {
	u64 ptr;
	u64 length;
};

union kbase_ioctl_mem_import {
        struct {
                union kbase_ioctl_header header;
                u64 phandle;
                enum {
                        BASE_MEM_IMPORT_TYPE_INVALID = 0,
                        BASE_MEM_IMPORT_TYPE_UMP = 1,
                        BASE_MEM_IMPORT_TYPE_UMM = 2,
                        BASE_MEM_IMPORT_TYPE_USER_BUFFER = 3,
                } type :32;
                u32 :32;
                u64 flags;
        } in;
        struct {
                union kbase_ioctl_header header;
                u64 pad[2];
                u64 flags;
                u64 gpu_va;
                u64 va_pages;
        } out;
} __attribute__((packed));

struct kbase_ioctl_mem_commit {
	union kbase_ioctl_header header;
	/* [in] */
	mali_ptr gpu_addr;
	u64 pages;
	/* [out] */
	u32 result_subcode;
	u32 :32;
} __attribute__((packed));

enum kbase_ioctl_mem_query_type {
	BASE_MEM_QUERY_COMMIT_SIZE = 1,
	BASE_MEM_QUERY_VA_SIZE     = 2,
	BASE_MEM_QUERY_FLAGS       = 3
};

struct kbase_ioctl_mem_query {
	union kbase_ioctl_header header;
	/* [in] */
	mali_ptr gpu_addr;
	enum kbase_ioctl_mem_query_type query : 32;
	u32 :32;
	/* [out] */
	u64 value;
} __attribute__((packed));

struct kbase_ioctl_mem_free {
	union kbase_ioctl_header header;
	mali_ptr gpu_addr; /* [in] */
} __attribute__((packed));
/* FIXME: Size unconfirmed (haven't seen in a trace yet) */

struct kbase_ioctl_mem_flags_change {
	union kbase_ioctl_header header;
	/* [in] */
	mali_ptr gpu_va;
	u64 flags;
	u64 mask;
} __attribute__((packed));
/* FIXME: Size unconfirmed (haven't seen in a trace yet) */

struct kbase_ioctl_mem_alias {
	union kbase_ioctl_header header;
	/* [in/out] */
	u64 flags;
	/* [in] */
	u64 stride;
	u64 nents;
	u64 ai;
	/* [out] */
	mali_ptr gpu_va;
	u64 va_pages;
} __attribute__((packed));

struct kbase_ioctl_mem_sync {
	union kbase_ioctl_header header;
	mali_ptr handle;
	u64 user_addr;
	u64 size;
	enum {
		MALI_SYNC_TO_DEVICE = 1,
		MALI_SYNC_TO_CPU = 2,
	} type :8;
	u64 :56;
} __attribute__((packed));

struct kbase_ioctl_set_flags {
	union kbase_ioctl_header header;
	u32 create_flags; /* [in] */
	u32 :32;
} __attribute__((packed));

struct kbase_ioctl_stream_create {
	union kbase_ioctl_header header;
	/* [in] */
	char name[32];
	/* [out] */
	s32 fd;
	u32 :32;
} __attribute__((packed));

struct kbase_ioctl_job_submit {
	union kbase_ioctl_header header;
	/* [in] */
	u64 addr;
	u32 nr_atoms;
	u32 stride;
} __attribute__((packed));

struct kbase_ioctl_get_context_id {
	union kbase_ioctl_header header;
	/* [out] */
	s64 id;
} __attribute__((packed));

#undef PAD_CPU_PTR

enum base_jd_event_code {
        BASE_JD_EVENT_DONE = 1,
};

struct base_jd_event_v2 {
	enum base_jd_event_code event_code;
	mali_atom_id atom_number;
	struct mali_jd_udata udata;
};

/* Defined in mali-props.h */
struct kbase_ioctl_gpu_props_reg_dump;

/* For ioctl's we haven't written decoding stuff for yet */
typedef struct {
	union kbase_ioctl_header header;
} __ioctl_placeholder;

#endif /* __KBASE_IOCTL_H__ */
