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
#include <virt/vmodule.h>
#include <minos/timer.h>
#include <asm/io.h>
#include <asm/reg.h>
#include <asm/trap.h>
#include <minos/timer.h>
#include <minos/irq.h>
#include <minos/sched.h>
#include <virt/virq.h>
#include <virt/os.h>
#include <asm/aarch64_reg.h>

#define REG_CNTCR		0x000
#define REG_CNTSR		0x004
#define REG_CNTCV_L		0x008
#define REG_CNTCV_H		0x00c
#define REG_CNTFID0		0x020

#define REG_CNTVCT_LO		0x08
#define REG_CNTVCT_HI		0x0c
#define REG_CNTFRQ		0x10
#define REG_CNTP_CVAL		0x24
#define REG_CNTP_TVAL		0x28
#define REG_CNTP_CTL		0x2c
#define REG_CNTV_CVAL		0x30
#define REG_CNTV_TVAL		0x38
#define REG_CNTV_CTL		0x3c

#define ACCESS_REG		0x0
#define ACCESS_MEM		0x1

struct vtimer {
	struct vcpu *vcpu;
	struct timer timer;
	int virq;
	uint32_t cnt_ctl;
	uint64_t cnt_cval;
	uint64_t freq;
};

struct vtimer_context {
	struct vtimer phy_timer;
	struct vtimer virt_timer;
	unsigned long offset;
};

static int arm_phy_timer_trap(struct vcpu *vcpu,
		int reg, int read, unsigned long *value);

static int vtimer_vmodule_id = INVALID_MODULE_ID;

#define get_access_vtimer(vtimer, c, access)		\
	do {						\
		vtimer = &c->phy_timer;			\
	} while (0)

static void phys_timer_expire_function(unsigned long data)
{
	struct vtimer *vtimer = (struct vtimer *)data;

	vtimer->cnt_ctl |= CNT_CTL_ISTATUS;
	vtimer->cnt_cval = 0;

	if (!(vtimer->cnt_ctl & CNT_CTL_IMASK))
		send_virq_to_vcpu(vtimer->vcpu, vtimer->virq);
}

static void virt_timer_expire_function(unsigned long data)
{
	struct vtimer *vtimer = (struct vtimer *)data;

	/*
	 * just wake up the target vCPU. when switch to
	 * this vcpu, the value of vtimer will restore and
	 * if the irq is not mask, the vtimer will trigger
	 * the hardware irq again.
	 */
	wake(&vtimer->vcpu->vcpu_event);
}

static void vtimer_state_restore(struct vcpu *vcpu, void *context)
{
	struct vtimer_context *c = (struct vtimer_context *)context;
	struct vtimer *vtimer = &c->virt_timer;

	stop_timer(&vtimer->timer);

	write_sysreg64(c->offset, ARM64_CNTVOFF_EL2);
	write_sysreg64(vtimer->cnt_cval, ARM64_CNTV_CVAL_EL0);
	write_sysreg32(vtimer->cnt_ctl, ARM64_CNTV_CTL_EL0);
	isb();
}

static void vtimer_state_save(struct vcpu *vcpu, void *context)
{
	struct task *task = vcpu->task;
	struct vtimer_context *c = (struct vtimer_context *)context;
	struct vtimer *vtimer = &c->virt_timer;

	vtimer->cnt_cval = read_sysreg64(ARM64_CNTV_CVAL_EL0);
	vtimer->cnt_ctl = read_sysreg32(ARM64_CNTV_CTL_EL0);
	write_sysreg32(0, CNTV_CTL_EL0);
	isb();

	if ((task->state == TASK_STATE_STOP) ||
			(task->state == TASK_STATE_SUSPEND))
		return;

	if ((vtimer->cnt_ctl & CNT_CTL_ENABLE) &&
		!(vtimer->cnt_ctl & CNT_CTL_IMASK)) {
		mod_timer(&vtimer->timer, ticks_to_ns(vtimer->cnt_cval +
				c->offset - boot_tick));
	}
}

static void vtimer_state_init(struct vcpu *vcpu, void *context)
{
	struct vtimer *vtimer;
	struct arm_virt_data *arm_data = vcpu->vm->arch_data;
	struct vtimer_context *c = (struct vtimer_context *)context;

	if (get_vcpu_id(vcpu) == 0) {
		vcpu->vm->time_offset = get_sys_ticks();
		arm_data->phy_timer_trap = arm_phy_timer_trap;
	}

	c->offset = vcpu->vm->time_offset;

	vtimer = &c->virt_timer;
	vtimer->vcpu = vcpu;
	vtimer->virq = vcpu->vm->vtimer_virq;
	vtimer->cnt_ctl = 0;
	vtimer->cnt_cval = 0;
	init_timer(&vtimer->timer, virt_timer_expire_function,
			(unsigned long)vtimer);

	vtimer = &c->phy_timer;
	vtimer->vcpu = vcpu;
	vtimer->virq = 26;
	vtimer->cnt_ctl = 0;
	vtimer->cnt_cval = 0;
	init_timer(&vtimer->timer, phys_timer_expire_function,
			(unsigned long)vtimer);
}

static void vtimer_state_stop(struct vcpu *vcpu, void *context)
{
	struct vtimer_context *c = (struct vtimer_context *)context;

	stop_timer(&c->virt_timer.timer);
	stop_timer(&c->phy_timer.timer);
}

static inline void
asoc_handle_cntp_ctl(struct vcpu *vcpu, struct vtimer *vtimer)
{
	/*
	 * apple xnu use physical timer's interrupt as a fiq
	 * and read the ctl register to check wheter the timer
	 * is triggered, if the read access is happened in the
	 * fiq handler, need to clear the interrupt
	 */
	if ((vtimer->cnt_ctl & CNT_CTL_ISTATUS) &&
			(read_sysreg(HCR_EL2) & HCR_EL2_VF)) {
		vtimer->cnt_ctl &= ~CNT_CTL_ISTATUS;
		clear_pending_virq(vcpu, vtimer->virq);
	}
}

static void vtimer_handle_cntp_ctl(struct vcpu *vcpu, int access,
		int read, unsigned long *value)
{
	uint32_t v;
	struct vtimer *vtimer;
	struct vtimer_context *c;
	unsigned long ns;

	c = get_vmodule_data_by_id(vcpu, vtimer_vmodule_id);
	get_access_vtimer(vtimer, c, access);

	if (read) {
		*value = vtimer->cnt_ctl;
		if (vcpu->vm->os->type == OS_TYPE_XNU)
			asoc_handle_cntp_ctl(vcpu, vtimer);
	} else {
		v = (uint32_t)(*value);
		v &= ~CNT_CTL_ISTATUS;

		if (v & CNT_CTL_ENABLE)
			v |= vtimer->cnt_ctl & CNT_CTL_ISTATUS;
		vtimer->cnt_ctl = v;

		if ((vtimer->cnt_ctl & CNT_CTL_ENABLE) &&
				(vtimer->cnt_cval != 0)) {
			ns = ticks_to_ns(vtimer->cnt_cval + c->offset);
			mod_timer(&vtimer->timer, ns);
		} else {
			stop_timer(&vtimer->timer);
		}
	}
}

static void vtimer_handle_cntp_tval(struct vcpu *vcpu,
		int access, int read, unsigned long *value)
{
	struct vtimer *vtimer;
	unsigned long now;
	unsigned long ticks;
	struct vtimer_context *c;

	c = get_vmodule_data_by_id(vcpu, vtimer_vmodule_id);
	get_access_vtimer(vtimer, c, access);
	now = get_sys_ticks() - c->offset;

	if (read) {
		ticks = (vtimer->cnt_cval - now - c->offset) & 0xffffffff;
		*value = ticks;
	} else {
		unsigned long v = *value;

		vtimer->cnt_cval = get_sys_ticks() + v;
		if (vtimer->cnt_ctl & CNT_CTL_ENABLE) {
			vtimer->cnt_ctl &= ~CNT_CTL_ISTATUS;
			ticks = ticks_to_ns(vtimer->cnt_cval);
			mod_timer(&vtimer->timer, ticks);
		}
	}
}

static void vtimer_handle_cntp_cval(struct vcpu *vcpu,
		int access, int read, unsigned long *value)
{
	unsigned long ns;
	struct vtimer *vtimer;
	struct vtimer_context *c;

	c = get_vmodule_data_by_id(vcpu, vtimer_vmodule_id);
	get_access_vtimer(vtimer, c, access);

	if (read) {
		*value = vtimer->cnt_cval - c->offset;
	} else {
		vtimer->cnt_cval = *value + c->offset;
		if (vtimer->cnt_ctl & CNT_CTL_ENABLE) {
			vtimer->cnt_ctl &= ~CNT_CTL_ISTATUS;
			ns = ticks_to_ns(vtimer->cnt_cval);
			mod_timer(&vtimer->timer, ns);
		}
	}
}

static int arm_phy_timer_trap(struct vcpu *vcpu,
		int reg, int read, unsigned long *value)
{
	switch (reg) {
	case ESR_SYSREG_CNTP_CTL_EL0:
		vtimer_handle_cntp_ctl(vcpu, ACCESS_REG, read, value);
		break;
	case ESR_SYSREG_CNTP_CVAL_EL0:
		vtimer_handle_cntp_cval(vcpu, ACCESS_REG, read, value);
		break;
	case ESR_SYSREG_CNTP_TVAL_EL0:
		vtimer_handle_cntp_tval(vcpu, ACCESS_REG, read, value);
		break;
	default:
		break;
	}

	return 0;
}

static int vtimer_vmodule_init(struct vmodule *vmodule)
{
	vmodule->context_size = sizeof(struct vtimer_context);
	vmodule->state_init = vtimer_state_init;
	vmodule->state_save = vtimer_state_save;
	vmodule->state_restore = vtimer_state_restore;
	vmodule->state_stop = vtimer_state_stop;
	vmodule->state_reset = vtimer_state_stop;
	vtimer_vmodule_id = vmodule->id;

	return 0;
}

int arch_vtimer_init(uint32_t virtual_irq, uint32_t phy_irq)
{
	return register_vcpu_vmodule("vtimer_module", vtimer_vmodule_init);
}

int virtual_timer_irq_handler(uint32_t irq, void *data)
{
	uint32_t value;
	struct vcpu *vcpu = get_current_vcpu();

	/*
	 * if the current task is not a vcpu, disable the vtimer
	 * since the pending request vtimer irq is set to
	 * the timer
	 */
	if (!task_is_vcpu(current)) {
		write_sysreg32(0, ARM64_CNTV_CTL_EL0);
		return 0;
	}

	/*
	 * this case ususally happened when the vcpu called
	 * WFI to enter idle idle mode, but the vtimer irq is
	 * triggered when context switch. then when in idle task
	 * this IRQ is responsed. two case need consider:
	 *
	 * 1 - the vtimer interrup will send to wrong vcpu ?
	 * 2 - Here the logic of idle can be optimized to avoid
	 *     this situation
	 */
	value = read_sysreg32(ARM64_CNTV_CTL_EL0);
	if (!(value & CNT_CTL_ISTATUS)) {
		pr_debug("vtimer is not trigger\n");
		return 0;
	}

	value = value | CNT_CTL_IMASK;
	write_sysreg32(value, ARM64_CNTV_CTL_EL0);

	return send_virq_to_vcpu(vcpu, vcpu->vm->vtimer_virq);
}
