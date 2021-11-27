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
#include <minos/sched.h>
#include <minos/platform.h>
#include <minos/irq.h>
#include <minos/mm.h>
#include <minos/of.h>
#include <minos/task.h>
#include <minos/flag.h>
#include <minos/proc.h>

void system_reboot(void)
{
	if (platform->system_reboot)
		platform->system_reboot(0, NULL);

	panic("can not reboot system now\n");
}

void system_shutdown(void)
{
	if (platform->system_shutdown)
		platform->system_shutdown();

	panic("cant not shutdown system now\n");
}

int system_suspend(void)
{
	if (platform->system_suspend)
		platform->system_suspend();

	wfi();

	return 0;
}

static inline bool pcpu_can_idle(struct pcpu *pcpu)
{
	return (pcpu->local_rdy_grp == (1 << OS_PRIO_IDLE)) &&
			(is_list_empty(&pcpu->stop_list) &&
			(is_list_empty(&pcpu->die_process)));
}

static void do_pcpu_cleanup_work(struct pcpu *pcpu)
{
	struct task *task;
	unsigned long flags;

	for (; ;) {
		task = NULL;
		local_irq_save(flags);
		if (!is_list_empty(&pcpu->stop_list)) {
			task = list_first_entry(&pcpu->stop_list,
					struct task, stat_list);
			list_del(&task->stat_list);
		}
		local_irq_restore(flags);

		if (!task)
			break;

		release_task(task);
	}

	clean_process_on_pcpu(pcpu);
}

void cpu_idle(void)
{
	struct pcpu *pcpu = get_pcpu();

	set_os_running();

	local_irq_enable();

	while (1) {
		do_pcpu_cleanup_work(pcpu);

		/*
		 * need to check whether the pcpu can go to idle
		 * state to avoid the interrupt happend before wfi
		 */
		while (!need_resched() && pcpu_can_idle(pcpu)) {
			local_irq_disable();
			if (pcpu_can_idle(pcpu)) {
				pcpu->state = PCPU_STATE_IDLE;
				wfi();
				nop();
				pcpu->state = PCPU_STATE_RUNNING;
			}
			local_irq_enable();
		}

		set_need_resched();
		sched();
	}
}
