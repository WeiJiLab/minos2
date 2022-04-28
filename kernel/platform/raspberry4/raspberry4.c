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
#include <asm/arch.h>
#include <asm/io.h>
#include <libfdt/libfdt.h>
#include <minos/of.h>
#include <device/bcm_irq.h>
#include <asm/power.h>
#include <minos/platform.h>

#ifdef CONFIG_VIRT
#include <virt/virq.h>
#include <virt/vm.h>
#include <virt/vmm.h>
#include <virt/vdev.h>

#define BCM2838_RELEASE_ADDR	0xff800000

static int bcm2838_fake_scu_read(struct vdev *vdev, gp_regs *regs,
		int idx, unsigned long address, unsigned long *value)
{
	return 0;
}

static int inline bcm2838_bootup_secondary(struct vm *vm,
		int cpu, unsigned long entry)
{
	if (cpu >= vm->vcpu_nr) {
		pr_err("no such vcpu vcpu-id:%d\n", cpu);
		return -EINVAL;
	}

	if (cpu == 0)
		return 0;

	return vcpu_power_on(get_current_vcpu(),
			cpuid_to_affinity(cpu), entry, 0);
}

static int bcm2838_fake_64bit_scu_write(struct vdev *vdev, gp_regs *regs,
		int idx, unsigned long offset, unsigned long *value)
{
	int cpu;
	struct vm *vm = vdev->vm;

	if (offset % sizeof(uint64_t)) {
		pr_err("unsupport reg offset 0x%x\n", offset);
		return -EINVAL;
	}

	cpu = offset / sizeof(uint64_t);

	return bcm2838_bootup_secondary(vm, cpu, *value);
}

static int bcm2838_fake_32bit_scu_write(struct vdev *vdev, gp_regs *regs,
		int idx, unsigned long offset, unsigned long *value)
{
	int cpu;

	cpu = (offset - LOCAL_MAILBOX3_SET0) >> 4;

	return bcm2838_bootup_secondary(vdev->vm, cpu, *value);
}

static int raspberry4_setup_hvm(struct vm *vm, void *dtb)
{
	int i, offset, node;
	char name[16];
	uint64_t addr;
	uint64_t dtb_addr = 0;
	uint32_t *tmp = (uint32_t *)&dtb_addr;
	struct vdev *vdev;

	offset = of_get_node_by_name(dtb, 0, "cpus");
	if (offset < 0) {
		pr_err("can not find vcpus node for hvm\n");
		return -ENOENT;
	}

	/*
	 * using spin table boot methold redirect the
	 * relase addr to the interrupt controller space
	 */
	for (i = 0; i < vm->vcpu_nr; i++) {
		memset(name, 0, 16);
		sprintf(name, "cpu@%d", i);
		node = fdt_subnode_offset(dtb, offset, name);
		if (node <= 0)
			continue;

		addr = BCM2838_RELEASE_ADDR + i * sizeof(uint64_t);
		pr_notice("vcpu-%d release addr redirect to 0x%p\n", i, addr);
		tmp[0] = cpu_to_fdt32(addr >> 32);
		tmp[1] = cpu_to_fdt32(addr & 0xffffffff);

		fdt_setprop(dtb, node, "cpu-release-addr", (void *)tmp,
				2 * sizeof(uint32_t));
	}

	/* register a fake system controller for smp up handler */
	if (vm->vcpu_nr > 1) {
		vdev = zalloc(sizeof(struct vdev));
		if (!vdev)
			panic("no more memory for spi-table\n");

		host_vdev_init(vm, vdev, "smp-fake-con");
		vdev_add_iomem_range(vdev, BCM2838_RELEASE_ADDR, 0x1000);

		/*
		 * for raspberry4, currently kernel will use the local
		 * interrupt IC base address to wake up other cpu in 32
		 * bit mode, which is different with 64bit
		 */
		vdev->read = bcm2838_fake_scu_read;
		if (vm->flags & VM_FLAGS_32BIT)
			vdev->write = bcm2838_fake_32bit_scu_write;
		else
			vdev->write = bcm2838_fake_64bit_scu_write;
		vdev_add(vdev);
	}

	/* create pcie address mapping for VM0 */
	create_guest_mapping(&vm->mm, 0x600000000, 0x600000000,
			0x4000000, VM_GUEST_IO | VM_RW);

	pr_notice("raspberry4 setup vm done\n");

	return 0;
}
#endif

static int raspberry4_iomem_valid(unsigned long addr)
{
	/*
	 * 0xff800000 ---> 0xff800fff will used for local_intc
	 * in 32bit mode, kernel will use this address to wake
	 * up other cpus
	 */
	if ((addr >= 0xff800000) && (addr < 0xff800fff))
		return 0;

	/*
	 * 0x3b400000 ---> 0x3ebfffff will used for the framebuffer
	 * in raspberry4
	 */
	if ((addr >= 0x3b400000) && (addr < 0x3ebfffff))
		return 1;

	if ((addr >= 0xf3000000) && (addr < 0xffffffff))
		return 1;

	pr_err("memory region:0x%p not register\n", addr, addr);

	return 0;
}

static void raspberry4_system_reboot(int mode, const char *cmd)
{

}

static void raspberry4_system_shutdown(void)
{

}

/*
 * VC FW passed below memory region to Linux kernel
 * 0x0		- 0x3b3fffff
 * 0x40000000	- 0xfc000000
 *
 * memory map get from vc FW and the cmdline is:
 * 0x0		- 0x3b3fffff		[Linux Memory]
 * 0x3b400000   - 0x3ebfffff		[Unknown may be VC]
 * 0x3ec00000	- 0x7ebfffff		[VC memory]
 * 0x7ec00000   - 0xf2ffffff		[Linux]
 *
 * need parse the right information to Linux kernel, from
 * the dtb just can get the first memory information, the
 * below is the memory map when using Minos Hypervisor
 *
 * 0x0		- 0x373fffff		[Linux Memeory]
 * 0x37400000	- 0x3b3fffff@64M	[Minos Memory]
 * 0x3b400000	- 0x3ebfffff		[Unknown Memory do not passed to Linux]
 * 0x3ec00000	- 0x7ebfffff		[Need to passed to Linux]
 */
static void raspberry4_parse_mem_info(void)
{
	int node, len;
	fdt32_t *v;
	unsigned long base, size;

	/*
	 * need to parse other memory region to support 2G or 4G
	 * rpi-4 version, currently, use a simple way, the memory
	 * information will store in bootargs/extra-memory field in
	 * minos device tree
	 */
	if (!dtb_address)
		return;

	node = fdt_path_offset(dtb_address, "/chosen");
	if (node <= 0)
		return;

	v = (fdt32_t *)fdt_getprop(dtb_address, node, "extra-memory", &len);
	if (!v || (len < 8))
		return;

	len = len / 4;
	if (len % 2 != 0) {
		pr_err("wrong memory config in extra-memory\n");
		return;
	}

	while (len > 0) {
		base = fdt32_to_cpu(v[0]);
		size = fdt32_to_cpu(v[1]);

		pr_notice("register extra memory region 0x%x 0x%x\n", base, size);
		add_memory_region(base, size, MEMORY_REGION_TYPE_NORMAL, 0);
		len -= 2;
		v += 2;
	}
}

static struct platform platform_raspberry4 = {
	.name 		 = "raspberrypi,4-model-b",
	.cpu_on		 = spin_table_cpu_on,
	.system_reboot	 = raspberry4_system_reboot,
	.system_shutdown = raspberry4_system_shutdown,
	.iomem_valid	 = raspberry4_iomem_valid,
#ifdef CONFIG_VIRT
	.setup_hvm	 = raspberry4_setup_hvm,
#endif
	.parse_mem_info  = raspberry4_parse_mem_info,
};
DEFINE_PLATFORM(platform_raspberry4);
