/*
* Copyright (C) 2016 MediaTek Inc.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
* See http://www.gnu.org/licenses/gpl-2.0.html for more details.
*/
#include <mt-plat/upmu_common.h>
#include <mt-plat/mtk_battery.h>
#include <linux/list.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/wakelock.h>

#include "include/mtk_gauge_class.h"


static struct list_head coulomb_head_plus = LIST_HEAD_INIT(coulomb_head_plus);
static struct list_head coulomb_head_minus = LIST_HEAD_INIT(coulomb_head_minus);
static struct mutex coulomb_lock;
static unsigned long reset_coulomb;
static spinlock_t slock;
static struct wake_lock wlock;
static wait_queue_head_t wait_que;
static bool coulomb_thread_timeout;
static int ftlog_level = 2;
static int pre_coulomb;

#define FTLOG_ERROR_LEVEL   1
#define FTLOG_DEBUG_LEVEL   2
#define FTLOG_TRACE_LEVEL   3

#define ft_err(fmt, args...)   \
do {									\
	if (ftlog_level >= FTLOG_ERROR_LEVEL) {			\
		pr_err(fmt, ##args); \
	}								   \
} while (0)

#define ft_debug(fmt, args...)   \
do {									\
	if (ftlog_level >= FTLOG_DEBUG_LEVEL) {		\
		pr_err(fmt, ##args); \
	}								   \
} while (0)

#define ft_trace(fmt, args...)\
do {									\
	if (ftlog_level >= FTLOG_TRACE_LEVEL) {			\
		pr_err(fmt, ##args);\
	}						\
} while (0)


void mutex_coulomb_lock(void)
{
	mutex_lock(&coulomb_lock);
}

void mutex_coulomb_unlock(void)
{
	mutex_unlock(&coulomb_lock);
}

void wake_up_gauge_coulomb(void)
{
	unsigned long flags;

	ft_debug("wake_up_gauge_coulomb\n");
	spin_lock_irqsave(&slock, flags);
	if (wake_lock_active(&wlock) == 0)
		wake_lock(&wlock);
	spin_unlock_irqrestore(&slock, flags);

	coulomb_thread_timeout = true;
	wake_up(&wait_que);
}

void gauge_coulomb_consumer_init(struct gauge_consumer *coulomb, struct device *dev, char *name)
{
	coulomb->name = name;
	INIT_LIST_HEAD(&coulomb->list);
	coulomb->dev = dev;
}
void gauge_coulomb_dump_list(void)
{
	struct list_head *pos;
	struct list_head *phead = &coulomb_head_plus;
	struct gauge_consumer *ptr;
	int car = gauge_get_coulomb();

	if (list_empty(phead) != true) {
		ft_debug("dump plus list start\n");
		list_for_each(pos, phead) {
			ptr = container_of(pos, struct gauge_consumer, list);
			ft_debug("+dump list name:%s start:%ld end:%ld car:%d int:%d\n", ptr->name,
			ptr->start, ptr->end, car, ptr->variable);
		}
	}

	phead = &coulomb_head_minus;
	if (list_empty(phead) != true) {
		ft_debug("dump minus list start\n");
		list_for_each(pos, phead) {
			ptr = container_of(pos, struct gauge_consumer, list);
			ft_debug("-dump list name:%s start:%ld end:%ld car:%d int:%d\n", ptr->name,
			ptr->start, ptr->end, car, ptr->variable);
		}
	}
}


void gauge_coulomb_before_reset(void)
{
	ft_err("gauge_coulomb_before_reset\n");
	reset_coulomb = gauge_get_coulomb();
}

void gauge_coulomb_after_reset(void)
{
	struct list_head *pos;
	struct list_head *phead;
	struct gauge_consumer *ptr;
	unsigned long now = reset_coulomb;
	unsigned long duraction;

	ft_err("gauge_coulomb_after_reset\n");
	mutex_coulomb_lock();
	gauge_set_coulomb_interrupt1_ht(0);
	gauge_set_coulomb_interrupt1_lt(0);

	gauge_coulomb_dump_list();
	/* check plus list */
	phead = &coulomb_head_plus;
	list_for_each(pos, phead) {
		ptr = container_of(pos, struct gauge_consumer, list);
		if (ptr->end > now) {
			ptr->start = 0;
			duraction = ptr->end - now;
			if (duraction <= 0)
				duraction = 1;
			ptr->end = duraction;
			ptr->variable = duraction;
			ft_debug("[gauge_coulomb_after_reset]+ %s %ld %ld %d\n", dev_name(ptr->dev),
			ptr->start, ptr->end, ptr->variable);
		} else {
			struct list_head *ptmp;

			ptmp = pos;
			pos = pos->next;
			list_del_init(ptmp);
			ft_err("[gauge_coulomb_after_reset]+ %s s:%ld e:%ld car:%ld %d int:%d timeout\n", ptr->name,
			ptr->start, ptr->end, now, pre_coulomb, ptr->variable);
			if (ptr->callback) {
				mutex_coulomb_unlock();
				ptr->callback(ptr);
				mutex_coulomb_lock();
			}
		}
	}
	pos = coulomb_head_plus.next;
	if (list_empty(pos) != true) {
		ptr = container_of(pos, struct gauge_consumer, list);
		gauge_set_coulomb_interrupt1_ht(ptr->end - now);
	}


	/* check minus list */
	phead = &coulomb_head_minus;
	list_for_each(pos, phead) {
		ptr = container_of(pos, struct gauge_consumer, list);
		if (ptr->end < now) {
			ptr->start = 0;
			duraction = now - ptr->end;
			if (duraction <= 0)
				duraction = 1;
			ptr->end = duraction;
			ptr->variable = duraction;
			ft_debug("[gauge_coulomb_after_reset]- %s %ld %ld %d\n", dev_name(ptr->dev),
			ptr->start, ptr->end, ptr->variable);
		} else {
			struct list_head *ptmp;

			ptmp = pos;
			pos = pos->next;
			list_del_init(ptmp);
			ft_err("[gauge_coulomb_after_reset]- %s s:%ld e:%ld car:%ld %d int:%d timeout\n", ptr->name,
			ptr->start, ptr->end, now, pre_coulomb, ptr->variable);

			if (ptr->callback) {
				mutex_coulomb_unlock();
				ptr->callback(ptr);
				mutex_coulomb_lock();
			}
		}
	}
	pos = coulomb_head_minus.next;
	if (list_empty(pos) != true) {
		ptr = container_of(pos, struct gauge_consumer, list);
		gauge_set_coulomb_interrupt1_lt(now - ptr->end);
	}

	gauge_coulomb_dump_list();
	mutex_coulomb_unlock();
}

void gauge_coulomb_start(struct gauge_consumer *coulomb, int car)
{
	struct list_head *pos;
	struct list_head *phead;
	struct gauge_consumer *ptr = NULL;
	int hw_car, now_car;
	bool wake = false;
	int car_now;

	if (car == 0)
		return;

	mutex_coulomb_lock();

	car_now = gauge_get_coulomb();
	/* del from old list */
	if (list_empty(&coulomb->list) != true) {
		ft_trace("coulomb_start del name:%s s:%ld e:%ld v:%d car:%d\n",
		coulomb->name,
		coulomb->start, coulomb->end, coulomb->variable, car_now);
		list_del_init(&coulomb->list);
	}

	coulomb->start = car_now;
	coulomb->end = coulomb->start + car;
	coulomb->variable = car;
	now_car = coulomb->start;

	if (car > 0)
		phead = &coulomb_head_plus;
	else
		phead = &coulomb_head_minus;

	/* add node to list */
	list_for_each(pos, phead) {
		ptr = container_of(pos, struct gauge_consumer, list);
		if (car > 0) {
			if (coulomb->end < ptr->end)
				break;
		} else
			if (coulomb->end > ptr->end)
				break;
	}
	list_add(&coulomb->list, pos->prev);

	if (car > 0) {
		list_for_each(pos, phead) {
			ptr = container_of(pos, struct gauge_consumer, list);
			if (ptr->end - now_car <= 0)
				wake = true;
			else
				break;
		}
		hw_car = ptr->end - now_car;
		gauge_set_coulomb_interrupt1_ht(hw_car);
	} else {
		list_for_each(pos, phead) {
			ptr = container_of(pos, struct gauge_consumer, list);
			if (ptr->end - now_car >= 0)
				wake = true;
			else
				break;
		}
		hw_car = now_car - ptr->end;
		gauge_set_coulomb_interrupt1_lt(hw_car);
	}
	mutex_coulomb_unlock();

	if (wake == true)
		wake_up_gauge_coulomb();

	ft_debug("coulomb_start dev:%s name:%s s:%ld e:%ld v:%d car:%d w:%d\n",
	dev_name(coulomb->dev), coulomb->name, coulomb->start, coulomb->end,
	coulomb->variable, car, wake);


}

void gauge_coulomb_stop(struct gauge_consumer *coulomb)
{

	ft_debug("coulomb_stop node:%s %ld %ld %d\n",
	dev_name(coulomb->dev), coulomb->start, coulomb->end,
	coulomb->variable);

	mutex_coulomb_lock();
	list_del_init(&coulomb->list);
	mutex_coulomb_unlock();

}

static struct timespec sstart[10];
void gauge_coulomb_int_handler(void)
{
	int car, hw_car;
	struct list_head *pos;
	struct list_head *phead;
	struct gauge_consumer *ptr = NULL;

	get_monotonic_boottime(&sstart[0]);
	car = gauge_get_coulomb();
	ft_trace("[gauge_coulomb_int_handler] car:%d preCar:%d\n", car, pre_coulomb);
	get_monotonic_boottime(&sstart[1]);

	if (list_empty(&coulomb_head_plus) != true) {
		pos = coulomb_head_plus.next;
		phead = &coulomb_head_plus;
		for (pos = phead->next; pos != phead;) {
			struct list_head *ptmp;

			ptr = container_of(pos, struct gauge_consumer, list);
			if (ptr->end <= car) {
				ptmp = pos;
				pos = pos->next;
				list_del_init(ptmp);
				ft_err(
					"[gauge_coulomb_int_handler]+ %s s:%ld e:%ld car:%d %d int:%d timeout\n",
					ptr->name,
					ptr->start, ptr->end, car, pre_coulomb, ptr->variable);
				if (ptr->callback) {
					mutex_coulomb_unlock();
					ptr->callback(ptr);
					mutex_coulomb_lock();
				}

			} else
				break;
		}

		if (list_empty(&coulomb_head_plus) != true) {
			pos = coulomb_head_plus.next;
			ptr = container_of(pos, struct gauge_consumer, list);
			hw_car = ptr->end - car;
			ft_trace("[gauge_coulomb_int_handler]+ %s %ld %ld %d now:%d dif:%d\n", ptr->name,
					ptr->start, ptr->end, ptr->variable, car, hw_car);
			gauge_set_coulomb_interrupt1_ht(hw_car);
		} else
		ft_trace("+ list is empty\n");
	} else
		ft_trace("+ list is empty\n");

	if (list_empty(&coulomb_head_minus) != true) {
		pos = coulomb_head_minus.next;
		phead = &coulomb_head_minus;
		for (pos = phead->next; pos != phead;) {
			struct list_head *ptmp;

			ptr = container_of(pos, struct gauge_consumer, list);
			if (ptr->end >= car) {
				ptmp = pos;
				pos = pos->next;
				list_del_init(ptmp);
				ft_err(
					"[gauge_coulomb_int_handler]- %s s:%ld e:%ld car:%d %d int:%d timeout\n",
					ptr->name,
					ptr->start, ptr->end, car, pre_coulomb, ptr->variable);
				if (ptr->callback) {
					mutex_coulomb_unlock();
					ptr->callback(ptr);
					mutex_coulomb_lock();
				}

			} else
				break;
		}

		if (list_empty(&coulomb_head_minus) != true) {
			pos = coulomb_head_minus.next;
			ptr = container_of(pos, struct gauge_consumer, list);
			hw_car = car - ptr->end;
			ft_trace("[gauge_coulomb_int_handler]- %s %ld %ld %d now:%d dif:%d\n", ptr->name,
					ptr->start, ptr->end, ptr->variable, car, hw_car);
			gauge_set_coulomb_interrupt1_lt(hw_car);
		} else
		ft_trace("+ list is empty\n");
	} else
		ft_trace("- list is empty\n");

	pre_coulomb = car;
	get_monotonic_boottime(&sstart[2]);
	sstart[0] = timespec_sub(sstart[1], sstart[0]);
	sstart[1] = timespec_sub(sstart[2], sstart[1]);
}

static int gauge_coulomb_thread(void *arg)
{
	unsigned long flags;
	struct timespec start, end, duraction;

	while (1) {
		wait_event(wait_que, (coulomb_thread_timeout == true));
		coulomb_thread_timeout = false;
		get_monotonic_boottime(&start);
		mutex_coulomb_lock();
		gauge_coulomb_int_handler();

		spin_lock_irqsave(&slock, flags);
		wake_unlock(&wlock);
		spin_unlock_irqrestore(&slock, flags);

		mutex_coulomb_unlock();
		get_monotonic_boottime(&end);
		duraction = timespec_sub(end, start);

		if ((duraction.tv_nsec / 1000000) > 50)
			ft_err("gauge_coulomb_thread time:%d ms %d %d\n", (int)(duraction.tv_nsec / 1000000),
				(int)(sstart[0].tv_nsec / 1000000),
				(int)(sstart[1].tv_nsec / 1000000));
	}

	return 0;
}

void gauge_coulomb_service_init(void)
{
	ft_err("gauge coulomb_service_init\n");
	mutex_init(&coulomb_lock);
	spin_lock_init(&slock);
	wake_lock_init(&wlock, WAKE_LOCK_SUSPEND, "gauge coulomb wakelock");
	init_waitqueue_head(&wait_que);
	kthread_run(gauge_coulomb_thread, NULL, "gauge_coulomb_thread");

	pmic_register_interrupt_callback(FG_BAT1_INT_L_NO, wake_up_gauge_coulomb);
	pmic_register_interrupt_callback(FG_BAT1_INT_H_NO, wake_up_gauge_coulomb);
	pre_coulomb = gauge_get_coulomb();
}