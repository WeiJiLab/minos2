/*
 * Copyright (C) 2019 Min Le (lemin9538@gmail.com)
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
#include <minos/of.h>
#include <minos/mm.h>
#include <minos/memory.h>

#define MAX_MEMORY_REGION	32

LIST_HEAD(mem_list);
static struct memory_region memory_regions[MAX_MEMORY_REGION];
static int current_region_id;

static struct memory_region *alloc_memory_region(void)
{
	ASSERT(current_region_id < MAX_MEMORY_REGION);
	return &memory_regions[current_region_id++];
}

int add_memory_region(uint64_t base, uint64_t size, uint32_t flags)
{
	phy_addr_t r_base, r_end;
	size_t r_size;
	struct memory_region *region;

	if (size == 0)
		return -EINVAL;

	/*
	 * need to check whether this region is confilct with
	 * other region
	 */
	list_for_each_entry(region, &mem_list, list) {
		r_size = region->size;
		r_base = region->phy_base;
		r_end = r_base + r_size - 1;

		if (!((base > r_end) || ((base + size) < r_base))) {
			pr_err("memory region invalid 0x%p ---> [0x%p]\n",
					r_base, r_end, r_size);
			return -EINVAL;
		}
	}

	region = alloc_memory_region();
	region->phy_base = base;
	region->size = size;
	region->flags = flags;

	pr_info("ADD   MEM: 0x%p [0x%p] 0x%x\n", region->phy_base,
			region->size, region->flags);

	init_list(&region->list);
	list_add_tail(&mem_list, &region->list);

	return 0;
}

int split_memory_region(phy_addr_t base, size_t size, uint32_t flags)
{
	phy_addr_t start, end;
	phy_addr_t new_end = base + size;
	struct memory_region *region, *n, *tmp;

	pr_info("SPLIT MEM: 0x%p [0x%p] 0x%x\n", base, size, flags);

	if ((size == 0))
		return -EINVAL;

	/*
	 * delete the memory for host, these region
	 * usually for vms
	 */
	list_for_each_entry_safe(region, n, &mem_list, list) {
		start = region->phy_base;
		end = start + region->size;

		if ((base > end) || (base < start) || (new_end > end))
			continue;

		/* just delete this region from the list */
		if ((base == start) && (new_end == end)) {
			region->flags = flags;
			return 0;
		} else if ((base == start) && (new_end < end)) {
			region->phy_base = new_end;
			region->size -= size;
		} else if ((base > start) && (new_end < end)) {
			/* create a new region for the tail space */
			n = alloc_memory_region();
			n->phy_base = new_end;
			n->size = end - new_end;
			n->flags = region->flags;
			list_add_tail(&mem_list, &n->list);
			region->size = base - start;
		} else if ((base > start) && (end == new_end)) {
			region->size = region->size - size;
		} else {
			pr_warn("incorrect memory region 0x%x 0x%x\n",
					base, size);
			return -EINVAL;
		}

		/* alloc a new memory region for vm memory */
		tmp = alloc_memory_region();
		tmp->phy_base = base;
		tmp->size = size;
		tmp->flags = flags;
		list_add_tail(&mem_list, &tmp->list);

		return 0;
	}

	panic("Found Invalid memory config 0x%p [0x%p]\n", base, size);

	return 0;
}

int memory_region_type(struct memory_region *region)
{
	return ffs_one_table[region->flags & 0xff];
}

void dump_memory_info(void)
{
	int type;
	char *name;
	struct memory_region *region;
	char *mem_attr[MEMORY_REGION_TYPE_MAX + 1] = {
		"Normal",
		"DMA",
		"RSV",
		"VM",
		"DTB",
		"Kernel",
		"RamDisk",
		"Unknown"
	};

	list_for_each_entry(region, &mem_list, list) {
		type = memory_region_type(region);
		if (type >= MEMORY_REGION_TYPE_MAX)
			name = mem_attr[MEMORY_REGION_TYPE_MAX];
		else
			name = mem_attr[type];

		pr_notice("MEM: 0x%p ---> 0x%p [0x%p] %s\n", region->phy_base,
				region->phy_base + region->size,
				region->size, name);
	}
}

void mem_init(void)
{
#ifdef CONFIG_DEVICE_TREE
	of_parse_memory_info();
#endif

	/*
	 * if there is no memory information founded then panic
	 * the system.
	 */
	BUG_ON(is_list_empty(&mem_list), "no memory information found\n");

	dump_memory_info();
}
