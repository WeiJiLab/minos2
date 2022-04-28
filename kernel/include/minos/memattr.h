#ifndef __MINOS_MEM_ATTR_H__
#define __MINOS_MEM_ATTR_H__

#define VM_NONE			(0x00000000)

#define __VM_IO			(0x00000001)	/* IO memory */
#define __VM_NORMAL		(0x00000002)	/* Normal memory */
#define __VM_NORMAL_NC		(0x00000004)
#define __VM_WT			(0x00000008)	/* Write thought */

#define __VM_PFNMAP		(0x00000100)	/* map to the physical normal memory directly */
#define __VM_HUGE_2M		(0x00000200)
#define __VM_HUGE_1G		(0x00000400)
#define __VM_DEVMAP		(0x00000800)
#define __VM_SHARED		(0x00001000)	/* do not release the memory, kobject will release it */
#define __VM_HOST		(0x00002000)
#define __VM_GUEST		(0x00004000)
#define __VM_SHMEM		(0x00008000)	/* prviate memory, will not be shared */
#define __VM_PMA		(0x00010000)

#define __VM_RW_NON		(0x00000000)
#define __VM_READ		(0x00100000)
#define __VM_WRITE		(0x00200000)
#define __VM_EXEC		(0x00400000)
#define __VM_RO			(__VM_READ)
#define __VM_WO			(__VM_WRITE)
#define __VM_RW			(__VM_READ | __VM_WRITE)

#define VM_TYPE_MASK		(__VM_IO | __VM_NORMAL | __VM_NORMAL_NC | __VM_WT)

#define VM_HOST			(__VM_HOST)
#define VM_GUEST		(__VM_GUEST)

#define VM_RW_NON		(__VM_RW_NON)
#define VM_RO			(__VM_RO)
#define VM_WO			(__VM_WO)
#define VM_RW			(__VM_READ | __VM_WRITE)
#define VM_RWX			(__VM_READ | __VM_WRITE | __VM_EXEC)
#define VM_RW_MASK		(__VM_READ | __VM_WRITE)

#define VM_IO			(__VM_IO | __VM_DEVMAP | __VM_PFNMAP)
#define VM_NORMAL		(__VM_NORMAL)
#define VM_NORMAL_NC		(__VM_NORMAL_NC)
#define VM_NORMAL_WT		(__VM_WT)
#define VM_DMA			(__VM_NORMAL_NC)
#define VM_HUGE			(__VM_HUGE_2M)
#define VM_SHARED		(__VM_SHARED)
#define VM_SHMEM		(__VM_SHMEM)
#define VM_PFNMAP		(__VM_PFNMAP)
#define VM_DEVMAP		(__VM_DEVMAP)
#define VM_PMA			(__VM_PMA)

#define VM_MAP_BK		(0X01000000)	/* mapped as block */
#define VM_MAP_PT		(0x02000000)	/* mapped as pass though, PFN_MAP */
#define VM_MAP_TYPE_MASK	(0x0f000000)

#define VM_HOST_NORMAL		(VM_NORMAL | VM_PFNMAP | VM_HOST)
#define VM_HOST_NORMAL_NC	(__VM_NORMAL_NC | VM_PFNMAP | VM_HOST)
#define VM_HOST_IO		(VM_IO | VM_HOST)

#define VM_GUEST_NORMAL		(VM_NORMAL | VM_RWX | VM_MAP_BK | VM_GUEST)
#define VM_GUEST_IO		(VM_IO | VM_MAP_PT | VM_DEVMAP | VM_GUEST)	/* passthough device for guest VM */
#define VM_GUEST_VDEV		(VM_GUEST)					/* virtual device created by host for guest VM, memory R/W will trapped */
#define VM_GUEST_SHMEM		(VM_NORMAL_NC | VM_SHMEM | VM_GUEST)		/* shared memory between guests, memory will managemented by host */
#define VM_NATIVE_NORMAL	(VM_NORMAL | VM_RWX | VM_PFNMAP | VM_HUGE | VM_GUEST) /* native VM using the memory config from device tree */

#define MEM_BLOCK_SIZE		(0x200000)
#define MEM_BLOCK_SHIFT		(21)
#define BFN2PHY(bfn)		((unsigned long)(bfn) << MEM_BLOCK_SHIFT)
#define PHY2BFN(phy)		((unsigned long)(phy) >> MEM_BLOCK_SHIFT)
#define PAGES_IN_BLOCK		(MEM_BLOCK_SIZE >> PAGE_SHIFT)

#define __PAGE_MASK		(~((1UL << PAGE_SHIFT) - 1))
#define __BLOCK_MASK		(~((1UL << MEM_BLOCK_SHIFT) - 1))
#define BLOCK_MASK		((1 << MEM_BLOCK_SHIFT) - 1)

#define GFB_SLAB               (1 << 0)
#define GFB_PAGE               (1 << 1)
#define GPF_PAGE_META          (1 << 2)
#define GFB_VM                 (1 << 3)
#define GFB_FIXED              (1 << 5)

#define GFB_SLAB_BIT           (0)
#define GFB_PAGE_BIT           (1)
#define GFB_PAGE_META_BIT      (2)
#define GFB_VM_BIT             (3)
#define GFB_IO_BIT             (4)
#define GFB_FIXED_BIT          (5)

#define GFB_MASK               (0xffff)

#endif
