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
#include <minos/poll.h>
#include <minos/task.h>
#include <minos/string.h>
#include <minos/sched.h>
#include <minos/mm.h>
#include <minos/event.h>
#include <minos/kobject.h>
#include <minos/task.h>
#include <minos/vspace.h>
#include <minos/uaccess.h>

#define to_poll_hub(kobj)	\
	(struct poll_hub *)kobj->data

struct poll_event *alloc_poll_event(void)
{
	struct poll_event_kernel *p;

	p = zalloc(sizeof(struct poll_event_kernel));
	if (!p)
		return NULL;
	p->release = 1;

	return &p->event;
}

int poll_event_send_static(struct kobject *kobj,
			struct poll_event_kernel *evk)
{
	struct poll_hub *peh = to_poll_hub(kobj);
	struct task *task;
	unsigned long flags;

	spin_lock_irqsave(&peh->lock, flags);

	list_add_tail(&peh->event_list, &evk->list);
	task = peh->task;

	/*
	 * wake up the waitter, if has.
	 */
	if (task && (task->stat == TASK_STAT_WAIT_EVENT) &&
			(task->wait_event == (unsigned long)peh))
		wake_up(task, 0);

	spin_unlock_irqrestore(&peh->lock, flags);

	return 0;
}

int poll_event_send_with_data(struct kobject *kobj, int ev,
		handle_t handle, void *data, int len)
{
	struct poll_event_kernel *p;

	p = (struct poll_event_kernel *)alloc_poll_event();
	if (!p)
		return -ENOMEM;

	p->event.event = ev;
	p->event.handle = handle;
	p->release = 1;

	if (data && (len > 0) && (len < POLL_EVENT_DATA_SIZE))
		memcpy(p->event.data, data, len);

	return poll_event_send_static(kobj, p);
}

int poll_event_send(struct kobject *kobj, int ev, handle_t handle)
{
	return poll_event_send_with_data(kobj, ev, handle, NULL, 0);
}

static int copy_poll_event_to_user(struct poll_event __user *events,
		struct list_head *head, int cnt)
{
	struct poll_event_kernel *pevent, *tmp;
	int ret;

	list_for_each_entry_safe(pevent, tmp, head, list) {
		list_del(&pevent->list);
		ret = copy_to_user(events, &pevent->event,
				sizeof(struct poll_event));
		/*
		 * free the epoll event's memory
		 */
		if (pevent->release)
			free(pevent);

		/*
		 * what should to do if copy is fail ?
		 * the event will lost.
		 */
		if (ret <= 0)
			return ret;
	}

	return cnt;
}

static int __poll_hub_read(struct poll_hub *peh,
		struct poll_event __user *events,
		int max_event, uint32_t timeout)
{
	unsigned long flags;
	LIST_HEAD(event_list);
	int ret = 0, cnt = 0;
	struct poll_event_kernel *pevent, *tmp;

	if (max_event <= 0)
		return -EINVAL;

	if (!user_ranges_ok((void *)events,
				max_event * sizeof(struct poll_event)))
		return -EFAULT;

	while (ret == 0) {
		spin_lock_irqsave(&peh->lock, flags);
		if (peh->task != NULL)
			goto out;

		if (is_list_empty(&peh->event_list)) {
			if (timeout == 0)
				goto out;

			peh->task = current;
			__event_task_wait((unsigned long)peh, TASK_EVENT_POLL, timeout);
			spin_unlock_irqrestore(&peh->lock, flags);

			/*
			 * the poll_hub kobject is only read/write by itself
			 * other process will not see it, so do not need to
			 * consider the case of EABORT
			 */
			ret = wait_event();
			if (ret)
				return ret;
			continue;
		}

		list_for_each_entry_safe(pevent, tmp, &peh->event_list, list) {
			list_del(&pevent->list);
			list_add_tail(&event_list, &pevent->list);

			cnt++;
			if (cnt == max_event)
				break;
		}
		spin_unlock_irqrestore(&peh->lock, flags);

		if (!is_list_empty(&event_list)) {
			ret = copy_poll_event_to_user(events, &event_list, cnt);
			peh->task = NULL;
			return ret;
		}
	}

out:
	spin_unlock_irqrestore(&peh->lock, flags);
	return -EAGAIN;
}

static ssize_t poll_hub_read(struct kobject *kobj,
		void __user *data, size_t data_size,
		void __user *extra, size_t extra_size,
		uint32_t timeout)
{
	struct poll_hub *peh = to_poll_hub(kobj);

	return __poll_hub_read(peh, data,
			data_size / sizeof(struct poll_event), timeout);
}

static void poll_hub_release(struct kobject *kobj)
{

}

static int poll_hub_close(struct kobject *kobj, right_t right)
{
	return 0;
}

static struct kobject_ops poll_hub_ops = {
	.recv		= poll_hub_read,
	.release	= poll_hub_release,
	.close		= poll_hub_close,
};

static struct kobject *poll_hub_create(char *str, right_t right,
		right_t right_req, unsigned long data)
{
	struct poll_hub *peh;

	if ((right & KOBJ_RIGHT_RWX) != KOBJ_RIGHT_RO)
		return ERROR_PTR(EPERM);

	if (right != right_req)
		return ERROR_PTR(EPERM);

	peh = zalloc(sizeof(struct poll_hub));
	if (!peh)
		return ERROR_PTR(ENOMEM);

	init_list(&peh->event_list);
	spin_lock_init(&peh->lock);
	kobject_init(&peh->kobj, current_pid, KOBJ_TYPE_POLL_HUB,
			0, right, (unsigned long)peh);
	peh->kobj.ops = &poll_hub_ops;

	return &peh->kobj;
}
DEFINE_KOBJECT(poll_hub, KOBJ_TYPE_POLL_HUB, poll_hub_create);