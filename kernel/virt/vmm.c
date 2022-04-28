/*
 * Copyright (C) 2018 Min Le (lemin9538@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <minos/minos.h>
#include <virt/vm.h>
#include <virt/iommu.h>
#include <minos/arch.h>

#define VM_IPA_SIZE (1UL << 40)

struct block_section {
	unsigned long start;
	unsigned long size;
	unsigned long end;
	unsigned long free_blocks;
	unsigned long total_blocks;
	unsigned long current_index;
	unsigned long *bitmap;
	struct block_section *next;
};

static struct block_section *bs_head;
static DEFINE_SPIN_LOCK(bs_lock);
static unsigned long free_blocks;

#define mm_to_vm(__mm) container_of((__mm), struct vm, mm)
#define VMA_SIZE(vma) ((vma)->end - (vma)->start)

static int __create_guest_mapping(struct mm_struct *mm, virt_addr_t vir,
		phy_addr_t phy, size_t size, unsigned long flags)
{
	struct vm *vm = mm_to_vm(mm);
	unsigned long tmp;
	int ret;

	tmp = BALIGN(vir + size, PAGE_SIZE);
	vir = ALIGN(vir, PAGE_SIZE);
	phy = ALIGN(phy, PAGE_SIZE);
	size = tmp - vir;

	pr_debug("map [x%x 0x%x] to [0x%x 0x%x] vm-%d\n",
			vir, vir + size, phy, phy + size, vm->vmid);
	ret = arch_guest_map(mm, vir, vir + size, phy, flags);
	if (!ret)
		ret = iommu_iotlb_flush_all(vm);

	return ret;
}

int create_guest_mapping(struct mm_struct *mm, virt_addr_t vir,
		phy_addr_t phy, size_t size, unsigned long flags)
{
	int ret;

	spin_lock(&mm->lock);
	ret = __create_guest_mapping(mm, vir, phy, size, flags);
	spin_unlock(&mm->lock);

	return ret;
}

static int __destroy_guest_mapping(struct mm_struct *mm,
		unsigned long vir, size_t size)
{
	unsigned long end;
	int ret;

	if (!IS_PAGE_ALIGN(vir) || !IS_PAGE_ALIGN(size)) {
		pr_warn("WARN: destroy guest mapping [0x%x 0x%x]\n",
				vir, vir + size);
		end = PAGE_BALIGN(vir + size);
		vir = PAGE_ALIGN(vir);
		size = end - vir;
	}

	ret = arch_guest_unmap(mm, vir, vir + size);
	if (!ret)
		ret = iommu_iotlb_flush_all(mm_to_vm(mm));

	return ret;
}

int destroy_guest_mapping(struct mm_struct *mm, unsigned long vir, size_t size)
{
	int ret;

	spin_lock(&mm->lock);
	ret = __destroy_guest_mapping(mm, vir, size);
	spin_unlock(&mm->lock);

	return ret;
}

static struct vmm_area *__alloc_vmm_area_entry(unsigned long base, size_t size)
{
	struct vmm_area *va;

	va = zalloc(sizeof(struct vmm_area));
	if (!va)
		return NULL;

	va->start = base;
	va->pstart = BAD_ADDRESS;
	va->end = base + size;
	va->flags = 0;

	return va;
}

static int __add_free_vmm_area(struct mm_struct *mm, struct vmm_area *area)
{
	struct vmm_area *tmp, *next, *va = area;
	size_t size;

	/*
	 * indicate it not inserted to the free list
	 */
	va->list.next = NULL;
	size = va->end - va->start;
	va->flags = 0;
	va->vmid = 0;
	va->pstart = 0;
repeat:
	/*
	 * check whether this two vmm_area can merged to one
	 * vmm_area and do the action
	 */
	list_for_each_entry_safe(tmp, next, &mm->vmm_area_free, list) {
		if (va->start == tmp->end) {
			va->start = tmp->start;
			list_del(&tmp->list);
			free(tmp);
			goto repeat;
		}

		if (va->end == tmp->start) {
			va->end = tmp->end;
			list_del(&tmp->list);
			free(tmp);
			goto repeat;
		}

		if (size <= (tmp->end - tmp->start)) {
			list_insert_before(&tmp->list, &va->list);
			break;
		}
	}

	if (va->list.next == NULL)
		list_add_tail(&mm->vmm_area_free, &va->list);

	return 0;
}

static void inline release_vmm_area_bk(struct vmm_area *va)
{
	struct mem_block *block = va->b_head, *tmp;

	while (block != NULL) {
		tmp = block->next;
		block->next = NULL;
		vmm_free_memblock(block);
		block = tmp;
	}

	va->b_head = NULL;
}

static void release_vmm_area_memory(struct vmm_area *va)
{
	/*
	 * can not free the physical memory when the memory
	 * is not belong to this vmm_area, this means the va
	 * is shareded with other vmm area, not the owner of
	 * it.
	 */
	if (va->flags & __VM_SHARED)
		return;

	switch (va->flags & VM_MAP_TYPE_MASK) {
	case VM_MAP_PT:
		break;
	case VM_MAP_BK:
		release_vmm_area_bk(va);
		break;
	default:
		if (va->pstart != BAD_ADDRESS) {
			if (va->flags & __VM_SHMEM)
				free_shmem((void *)va->pstart);
			else
				free_pages((void *)va->pstart);
			va->pstart = BAD_ADDRESS;
		}
		break;
	}
}

int release_vmm_area(struct mm_struct *mm, struct vmm_area *va)
{
	release_vmm_area_memory(va);
	spin_lock(&mm->lock);
	list_del(&va->list);
	__add_free_vmm_area(mm, va);
	spin_unlock(&mm->lock);

	return 0;
}

static int vmm_area_map_ln(struct mm_struct *mm, struct vmm_area *va)
{
	return __create_guest_mapping(mm, va->start,
			va->pstart, VMA_SIZE(va), va->flags);
}

static int vmm_area_map_bk(struct mm_struct *mm, struct vmm_area *va)
{
	struct mem_block *block = va->b_head;;
	unsigned long base = va->start;
	unsigned long size = VMA_SIZE(va);
	int ret;

	while (block) {
		ret = __create_guest_mapping(mm, base, BFN2PHY(block->bfn),
				MEM_BLOCK_SIZE, va->flags | VM_HUGE | VM_GUEST);
		if (ret)
			return ret;

		base += MEM_BLOCK_SIZE;
		size -= MEM_BLOCK_SIZE;
		block = block->next;
	}

	ASSERT(size == 0);

	return 0;
}

int map_vmm_area(struct mm_struct *mm,
		struct vmm_area *va, unsigned long pbase)
{
	int ret;

	switch (va->flags & VM_MAP_TYPE_MASK) {
	case VM_MAP_PT:
		va->pstart = va->start;
		ret = vmm_area_map_ln(mm, va);
		break;
	case VM_MAP_BK:
		ret = vmm_area_map_bk(mm, va);
		break;
	default:
		va->pstart = pbase;
		ret = vmm_area_map_ln(mm, va);
		break;
	}

	return ret;
}

static struct vmm_area *__split_vmm_area(struct mm_struct *mm,
		struct vmm_area *vma, unsigned long base,
		unsigned long end, int flags)
{
	struct vmm_area *left = NULL, *right = NULL;
	size_t left_size, right_size;

	left_size = base - vma->start;
	right_size = vma->end - end;

	if (left_size == 0 && right_size == 0)
		goto out;

	if (left_size > 0) {
		left = __alloc_vmm_area_entry(vma->start, left_size);
		if (!left)
			return NULL;
		list_add(&mm->vmm_area_free, &left->list);
	}

	if (right_size > 0) {
		right = __alloc_vmm_area_entry(end, right_size);
		if (!right)
			goto out_err_right;
		list_add(&mm->vmm_area_free, &right->list);
	}
out:
	vma->start = base;
	vma->end = end;
	vma->flags = flags;
	list_del(&vma->list);
	list_add_tail(&mm->vmm_area_used, &vma->list);

	return vma;

out_err_right:
	if (left) {
		list_del(&left->list);
		free(left);
	}
	return NULL;
}

static struct vmm_area *__alloc_free_vmm_area(struct mm_struct *mm,
		struct vmm_area *vma, size_t size,
		unsigned long mask, int flags)
{
	unsigned long base, end;

	base = (vma->start + mask) & ~mask;
	end = base + size;
	if (!((base >= vma->start) && (end <= vma->end)))
		return NULL;

	return __split_vmm_area(mm, vma, base, end, flags);
}

struct vmm_area *alloc_free_vmm_area(struct mm_struct *mm,
		size_t size, unsigned long mask, int flags)
{
	struct vmm_area *va;
	struct vmm_area *new = NULL;

	mask = (mask == BLOCK_MASK) ? BLOCK_MASK : PAGE_MASK;
	size = BALIGN(size, PAGE_SIZE);

	spin_lock(&mm->lock);
	list_for_each_entry(va, &mm->vmm_area_free, list) {
		if ((va->end - va->start) < size)
			continue;

		new = __alloc_free_vmm_area(mm, va, size, mask, flags);
		if (new)
			break;
	}
	spin_unlock(&mm->lock);

	return new;
}

struct vmm_area *split_vmm_area(struct mm_struct *mm,
		unsigned long base, size_t size, int flags)
{
	unsigned long end = base + size;
	struct vmm_area *va, *out = NULL;

	if ((flags & VM_NORMAL) && (!IS_PAGE_ALIGN(base) || !IS_PAGE_ALIGN(size))) {
		pr_err("vm_area is not PAGE align 0x%p 0x%x\n",
				base, size);
		return NULL;
	}

	spin_lock(&mm->lock);
	list_for_each_entry(va, &mm->vmm_area_free, list) {
		if ((base >= va->start) && (end <= va->end)) {
			out = va;
			break;
		}
	}

	if (!out)
		goto exit;

	out = __split_vmm_area(mm, out, base, end, flags);
exit:
	spin_unlock(&mm->lock);

	if (!out)
		pr_err("split vma [0x%lx 0x%lx] failed\n", base, end);

	return out;
}

struct vmm_area *request_vmm_area(struct mm_struct *mm,
		unsigned long base, unsigned long pbase,
		size_t size, int flags)
{
	struct vmm_area *va;

	va = split_vmm_area(mm, base, size, flags);
	if (!va)
		return NULL;

	va->pstart = pbase;

	return va;
}

static void dump_vmm_areas(struct mm_struct *mm)
{
	struct vmm_area *va;

	pr_debug("***** free vmm areas *****\n");
	list_for_each_entry(va, &mm->vmm_area_free, list)
		pr_debug("[VA] 0x%p->0x%p\n", va->start, va->end);

	pr_debug("***** used vmm areas *****\n");
	list_for_each_entry(va, &mm->vmm_area_used, list)
		pr_debug("[VA] 0x%p->0x%p\n", va->start, va->end);
}

static void release_vmm_area_in_vm0(struct vm *vm)
{
	struct vm *vm0 = get_host_vm();
	struct mm_struct *mm = &vm0->mm;
	struct vmm_area *va, *n;

	spin_lock(&mm->lock);
	list_for_each_entry_safe(va, n, &mm->vmm_area_used, list) {
		if (va->vmid != vm->vmid)
			continue;

		__destroy_guest_mapping(mm, va->start, VMA_SIZE(va));

		if (!(va->flags & VM_SHARED))
			free_pages((void *)va->pstart);

		list_del(&va->list);
		__add_free_vmm_area(mm, va);
	}
	spin_unlock(&mm->lock);
}

int unmap_vmm_area(struct mm_struct *mm, struct vmm_area *va)
{
	int ret;

	spin_lock(&mm->lock);
	ret = __destroy_guest_mapping(mm, va->start, VMA_SIZE(va));
	spin_unlock(&mm->lock);	

	return ret;
}

void release_vm_memory(struct vm *vm)
{
	struct mm_struct *mm = &vm->mm;
	struct vmm_area *va, *n;

	/*
	 * first unmap all the memory which maped to
	 * this VM. this will free the pages which used
	 * as the PAGE_TABLE, then free to the host.
	 */
	destroy_guest_mapping(mm, 0, VM_IPA_SIZE);

	/*
	 * - release all the vmm_area and its memory
	 * - release the page table
	 * - set all the mm_struct to 0
	 * this function will not be called when vm is
	 * running, do not to require the lock
	 */
	list_for_each_entry_safe(va, n, &mm->vmm_area_used, list) {
		release_vmm_area_memory(va);
		list_del(&va->list);
		free(va);
	}

	list_for_each_entry_safe(va, n, &mm->vmm_area_free, list) {
		list_del(&va->list);
		free(va);
	}

	/* release the vm0's memory belong to this vm */
	release_vmm_area_in_vm0(vm);

	free_pages((void *)mm->pgdp);
}

unsigned long create_hvm_shmem_map(struct vm *vm,
			unsigned long phy, uint32_t size)
{
	struct vm *vm0 = get_host_vm();
	struct vmm_area *va;

	va = alloc_free_vmm_area(&vm0->mm, size, PAGE_MASK, VM_GUEST_SHMEM |
			VM_SHARED | VM_RW);
	if (!va)
		return BAD_ADDRESS;

	va->vmid = vm->vmid;
	map_vmm_area(&vm0->mm, va, phy);

	return va->start;
}

int copy_from_guest(void *target, void __guest *src, size_t size)
{
	unsigned long start = (unsigned long)src;
	size_t copy_size, left = size;
	unsigned long pa;
	int ret;

	while (left > 0) {
		copy_size = PAGE_BALIGN(start) - PAGE_ALIGN(start);
		if (copy_size == 0)
			copy_size = PAGE_SIZE;
		if (copy_size > left)
			copy_size = left;

		pa = guest_va_to_pa(start, 1);
		ret = create_host_mapping(PAGE_ALIGN(ptov(pa)),
				PAGE_ALIGN(pa), PAGE_SIZE, VM_RO);
		if (ret)
			return ret;

		memcpy(target, (void *)vtop(pa), copy_size);
		destroy_host_mapping(PAGE_ALIGN(ptov(pa)), PAGE_SIZE);

		target += copy_size;
		start += copy_size;
		left -= copy_size;
	}

	return 0;
}

int translate_guest_ipa(struct mm_struct *mm,
		unsigned long offset, unsigned long *pa)
{
	int ret;

	spin_lock(&mm->lock);
	ret = arch_translate_guest_ipa(mm, offset, pa);
	spin_unlock(&mm->lock);

	return ret;
}

static int do_vm_mmap(struct mm_struct *mm, unsigned long hvm_mmap_base,
		unsigned long offset, unsigned long size)
{
	struct vm *vm0 = get_host_vm();
	struct mm_struct *mm0 = &vm0->mm;
	unsigned long pa;
	int ret;

	if (!IS_BLOCK_ALIGN(offset) || !IS_BLOCK_ALIGN(hvm_mmap_base) ||
			!IS_BLOCK_ALIGN(size)) {
		pr_err("__vm_mmap fail not PMD align 0x%p 0x%p 0x%x\n",
				hvm_mmap_base, offset, size);
		return -EINVAL;
	}

	while (size > 0) {
		ret = translate_guest_ipa(mm, offset, &pa);
		if (ret) {
			pr_err("addr 0x%x has not mapped in vm-%d\n", offset, vm0->vmid);
			return -EPERM;
		}

		ret = create_guest_mapping(mm0, hvm_mmap_base,
				pa, MEM_BLOCK_SIZE, VM_NORMAL | VM_RW);
		if (ret) {
			pr_err("%s failed\n", __func__);
			return ret;
		}

		hvm_mmap_base += MEM_BLOCK_SIZE;
		offset += MEM_BLOCK_SIZE;
		size -= MEM_BLOCK_SIZE;
	}

	return 0;
}

/*
 * map the guest vm memory space to vm0 to let vm0 can access
 * the memory space of the guest VM, this function can only
 * map the normal memory for the guest VM, will not map IO
 * memory
 *
 * offset - the base address need to be mapped
 * size - the size need to mapped
 */
struct vmm_area *vm_mmap(struct vm *vm, unsigned long offset, size_t size)
{
	struct vm *vm0 = get_host_vm();
	struct vmm_area *va;
	int ret;

	/*
	 * allocate all the memory the GVM request but will not
	 * map all the memory, only map the memory which mvm request
	 * for linux, if it need use virtio then need to map all
	 * the memory, but for other os, may not require to map
	 * all the memory.
	 */
	va = alloc_free_vmm_area(&vm0->mm, size,
			BLOCK_MASK, VM_GUEST_NORMAL | VM_SHARED | VM_RW);
	if (!va)
		return 0;

	pr_info("%s start:0x%x size:0x%x\n", __func__, va->start, size);
	ret = do_vm_mmap(&vm->mm, va->start, offset, size);
	if (ret) {
		pr_err("map guest vm memory to vm0 failed\n");
		release_vmm_area(&vm0->mm, va);
		return NULL;
	}

	/* mark this vmm_area is for guest vm map */
	va->vmid = vm->vmid;

	return va;
}

static int __alloc_vm_memory(struct mm_struct *mm, struct vmm_area *va)
{
	int i, count;
	unsigned long base;
	struct mem_block *block;

	base = ALIGN(va->start, MEM_BLOCK_SIZE);
	if (base != va->start) {
		pr_err("memory base is not mem_block align\n");
		return -EINVAL;
	}

	va->b_head = NULL;
	va->flags |= VM_MAP_BK;
	count = VMA_SIZE(va) >> MEM_BLOCK_SHIFT;

	/*
	 * here get all the memory block for the vm
	 * TBD: get contiueous memory or not contiueous ?
	 */
	for (i = 0; i < count; i++) {
		block = vmm_alloc_memblock();
		if (!block)
			return -ENOMEM;

		block->next = va->b_head;
		va->b_head = block;
	}

	return 0;
}

int alloc_vm_memory(struct vm *vm)
{
	struct mm_struct *mm = &vm->mm;
	struct vmm_area *va;

	list_for_each_entry(va, &mm->vmm_area_used, list) {
		if (!(va->flags & VM_NORMAL))
			continue;

		if (__alloc_vm_memory(mm, va)) {
			pr_err("alloc memory for vm-%d failed\n", vm->vmid);
			goto out;
		}

		if (map_vmm_area(mm, va, 0)) {
			pr_err("map memory for vm-%d failed\n", vm->vmid);
			goto out;
		}
	}

	return 0;
out:
	release_vm_memory(vm);
	return -ENOMEM;
}

static void vmm_area_init(struct mm_struct *mm, int bit64)
{
	unsigned long base, size;
	struct vmm_area *va;

	/*
	 * the virtual memory space for a virtual machine:
	 * 64bit - 40bit (1TB) IPA address space.
	 * 32bit - 32bit (4GB) IPA address space. (Without LPAE)
	 * 32bit - TBD (with LPAE)
	 */
	if (bit64) {
		base = 0x0;
		size = (1UL << 40);
	} else {
#ifdef CONFIG_VM_LPAE
		base = 0x0;
		size = 0x100000000;
#else
		base = 0x0;
		size = 0x100000000;
#endif
	}

	va = __alloc_vmm_area_entry(base, size);
	if (!va)
		pr_err("failed to alloc free vmm_area\n");
	else
		list_add_tail(&mm->vmm_area_free, &va->list);
}

static inline int check_vm_address(struct vm *vm, unsigned long addr)
{
	struct vmm_area *va;

	list_for_each_entry(va, &vm->mm.vmm_area_used, list) {
		if ((addr >= va->start) && (addr < va->end))
			return 0;
	}

	return 1;
}

static int vm_memory_init(struct vm *vm)
{
	struct memory_region *region;
	struct vmm_area *va;
	int ret = 0;

	if (!vm_is_native(vm))
		return 0;

	/*
	 * find the memory region which belongs to this
	 * VM and register to this VM.
	 */
	for_each_memory_region(region) {
		if (region->vmid != vm->vmid)
			continue;

		va = split_vmm_area(&vm->mm, region->phy_base,
				region->size, VM_NATIVE_NORMAL);
		if (!va)
			return -EINVAL;
	}

	/*
	 * check whether the entry address, setup_data address and load
	 * address are in the valid memory region.
	 */
	ret = check_vm_address(vm, (unsigned long)vm->load_address);
	ret += check_vm_address(vm, (unsigned long)vm->entry_point);
	ret += check_vm_address(vm, (unsigned long)vm->setup_data);

	return ret;
}

int vm_mm_struct_init(struct vm *vm)
{
	struct mm_struct *mm = &vm->mm;

	mm->pgdp = NULL;
	spin_lock_init(&mm->lock);
	init_list(&mm->vmm_area_free);
	init_list(&mm->vmm_area_used);

	mm->pgdp = arch_alloc_guest_pgd();
	if (mm->pgdp == NULL) {
		pr_err("No memory for vm page table\n");
		return -ENOMEM;
	}

	vmm_area_init(mm, !vm_is_32bit(vm));

	/*
	 * attch the memory region to the native vm.
	 */
	return vm_memory_init(vm);
}

int vm_mm_init(struct vm *vm)
{
	int ret;
	unsigned long base, end, size;
	struct vmm_area *va, *n;
	struct mm_struct *mm = &vm->mm;

	if (test_and_set_bit(VM_FLAGS_BIT_SKIP_MM_INIT, &vm->flags))
		return 0;

	dump_vmm_areas(&vm->mm);

	/* just mapping the physical memory for native VM */
	list_for_each_entry(va, &mm->vmm_area_used, list) {
		if (!(va->flags & __VM_NORMAL))
			continue;

		ret = map_vmm_area(mm, va, va->start);
		if (ret) {
			pr_err("map mem failed for vm-%d [0x%lx 0x%lx]\n",
				vm->vmid, va->start, va->end);
			return ret;
		}
	}

	/*
	 * make sure that all the free vmm_area are PAGE aligned
	 * when caculated the end address need to plus 1.
	 */
	list_for_each_entry_safe(va, n, &mm->vmm_area_free, list) {
		base = BALIGN(va->start, PAGE_SIZE);
		end = ALIGN(va->end, PAGE_SIZE);
		size = end - base;

		if (size < PAGE_SIZE) {
			pr_debug("drop unused vmm_area [0x%lx 0x%lx]\n",
					va->start, va->end);
			list_del(&va->list);
			free(va);
			continue;
		}

		if (size != (va->end - va->start)) {
			pr_debug("adjust vma [0x%lx 0x%lx] to [0x%lx->0x%lx]\n",
					va->start, va->end, base, end);
			va->start = base;
			va->end = end;
		}
	}

	return 0;
}

int vmm_has_enough_memory(size_t size)
{
	return ((size >> MEM_BLOCK_SHIFT) <= free_blocks);
}

static int __vmm_free_memblock(uint32_t bfn)
{
	unsigned long base = bfn << MEM_BLOCK_SHIFT;
	struct block_section *bs = bs_head;

	while (bs) {
		if ((base >= bs->start) && (base < bs->end)) {
			bfn = (base - bs->start) >> MEM_BLOCK_SHIFT;
			clear_bit(bfn, bs->bitmap);
			bs->free_blocks += 1;
			free_blocks += 1;
			return 0;
		}

		bs = bs->next;
	}

	pr_err("wrong memory block 0x%x\n", bfn);

	return -EINVAL;
}

int vmm_free_memblock(struct mem_block *mb)
{
	uint32_t bfn = mb->bfn;
	int ret;

	free(mb);
	spin_lock(&bs_lock);
	ret = __vmm_free_memblock(bfn);
	spin_unlock(&bs_lock);

	return ret;
}

static int get_memblock_from_section(struct block_section *bs, uint32_t *bfn)
{
	uint32_t id;

	id = find_next_zero_bit_loop(bs->bitmap,
			bs->total_blocks, bs->current_index);
	if (id >= bs->total_blocks)
		return -ENOSPC;

	set_bit(id, bs->bitmap);
	bs->current_index = id + 1;
	bs->free_blocks -= 1;
	free_blocks -= 1;
	*bfn = (bs->start >> MEM_BLOCK_SHIFT) + id;

	return 0;
}

struct mem_block *vmm_alloc_memblock(void)
{
	struct block_section *bs;
	struct mem_block *mb;
	int success = 0, ret;
	uint32_t bfn = 0;

	spin_lock(&bs_lock);
	bs = bs_head;
	while (bs) {
		if (bs->free_blocks != 0) {
			ret = get_memblock_from_section(bs, &bfn);
			if (ret == 0) {
				success = 1;
				break;
			} else {
				pr_err("memory block content wrong\n");
			}
		}
		bs = bs->next;
	}
	spin_unlock(&bs_lock);

	if (!success)
		return NULL;

	mb = malloc(sizeof(struct mem_block));
	if (!mb) {
		spin_lock(&bs_lock);
		__vmm_free_memblock(bfn);
		spin_unlock(&bs_lock);
		return NULL;
	}

	mb->bfn = bfn;
	mb->next = NULL;

	return mb;
}

void vmm_init(void)
{
	struct memory_region *region;
	struct block_section *bs;
	unsigned long start, end;
	int size;

	ASSERT(!is_list_empty(&mem_list));

	/*
	 * all the free memory will used as the guest VM
	 * memory. The guest memory will allocated as block.
	 */
	list_for_each_entry(region, &mem_list, list) {
		if (region->type != MEMORY_REGION_TYPE_NORMAL)
			continue;

		/*
		 * block section need BLOCK align.
		 */
		start = BALIGN(region->phy_base, BLOCK_SIZE);
		end = ALIGN(region->phy_base + region->size, BLOCK_SIZE);
		if (end - start <= 0) {
			pr_warn("VMM drop memory region [0x%lx 0x%lx]\n",
					region->phy_base,
					region->phy_base + region->size);
			continue;
		}

		pr_notice("VMM add memory region [0x%lx 0x%lx]\n", start, end);
		bs = malloc(sizeof(struct block_section));
		ASSERT(bs != NULL);
		bs->start = start;
		bs->end = end;
		bs->size = bs->end - bs->start;
		bs->total_blocks = bs->free_blocks = bs->size >> BLOCK_SHIFT;
		bs->current_index = 0;
		free_blocks += bs->total_blocks;

		/*
		 * allocate the memory for block bitmap.
		 */
		size = BITS_TO_LONGS(bs->free_blocks) * sizeof(long);
		bs->bitmap = malloc(size);
		ASSERT(bs->bitmap != NULL);
		memset(bs->bitmap, 0, size);

		bs->next = bs_head;
		bs_head = bs;
	}
}
