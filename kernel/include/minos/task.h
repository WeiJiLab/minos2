#ifndef __MINOS_TASK_H__
#define __MINOS_TASK_H__

#include <minos/minos.h>
#include <minos/flag.h>
#include <config/config.h>
#include <minos/task_def.h>
#include <minos/proc.h>

#define to_task_info(task)	(&(task)->ti)

#ifdef CONFIG_TASK_RUN_TIME
#define TASK_RUN_TIME CONFIG_TASK_RUN_TIME
#else
#define TASK_RUN_TIME 50
#endif

struct process;

static int inline task_is_idle(struct task *task)
{
	return (task->flags & TASK_FLAGS_IDLE);
}

static inline int get_task_tid(struct task *task)
{
	return task->tid;
}

static inline uint8_t get_task_prio(struct task *task)
{
	return task->prio;
}

static inline int task_is_suspend(struct task *task)
{
	return !!(task->state & TASK_STATE_WAIT_EVENT);
}

static inline int task_is_running(struct task *task)
{
	return (task->state == TASK_STATE_RUNNING);
}

static inline int task_is_vcpu(struct task *task)
{
	return (task->flags & TASK_FLAGS_VCPU);
}

static inline int task_is_32bit(struct task *task)
{
	return (task->flags & TASK_FLAGS_32BIT);
}

static inline void task_set_resched(struct task *task)
{
	task->ti.flags |= TIF_NEED_RESCHED;
}

static inline void task_clear_resched(struct task *task)
{
	task->ti.flags &= ~TIF_NEED_RESCHED;
}

static inline int task_need_resched(struct task *task)
{
	return (task->ti.flags & TIF_NEED_RESCHED);
}

static inline void task_need_stop(struct task *task)
{
	set_bit(TIF_NEED_STOP, &task->ti.flags);
	smp_wmb();
}

static inline void task_need_freeze(struct task *task)
{
	set_bit(TIF_NEED_FREEZE, &task->ti.flags);
	smp_wmb();
}

static inline int is_task_need_stop(struct task *task)
{
	return !!(task->ti.flags & (__TIF_NEED_FREEZE | __TIF_NEED_STOP));
}

#define task_state_pend_ok(status)	\
	((status) == TASK_STATE_PEND_OK)
#define task_state_pend_timeout(status)	\
	((status) == TASK_STATE_PEND_TO)
#define task_state_pend_abort(status)	\
	((status) == TASK_STATE_PEND_ABORT)

/*
 * set current running task's state do not need to obtain
 * a lock, when need to wakeup the task, below state the state
 * can be changed:
 * 1 - running -> wait_event
 * 2 - wait_event -> running (waked up by event)
 * 3 - new -> running
 * 4 - running -> stopped
 */
#define set_current_state(_state, to) 		\
	do {			 		\
		current->state = (_state); 	\
		current->delay = (to);		\
		smp_mb();			\
	} while (0)

void do_release_task(struct task *task);

struct task *create_task(char *name,
		task_func_t func,
		size_t stk_size,
		void *usp,
		int prio,
		int aff,
		unsigned long opt,
		void *arg);

struct task *create_vcpu_task(char *name, task_func_t func, int aff,
		unsigned long flags, void *vcpu);

struct task *create_kthread(char *name, task_func_t func, int prio,
		int aff, unsigned long opt, void *arg);

void os_for_all_task(void (*hdl)(struct task *task));

void task_die(void);
void task_suspend(void);

#endif

