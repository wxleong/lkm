/***************************************************************************************
 * Copyright 2023 Infineon Technologies AG ( www.infineon.com ).                       *
 * All rights reserved.                                                                *
 *                                                                                     *
 * Licensed  Material-Property of Infineon Technologies AG.                            *
 * This software is made available solely pursuant to the terms of Infineon            *
 * Technologies AG agreement which governs its use. This code and the information      *
 * contained in it are proprietary and confidential to Infineon Technologies AG.       *
 * No person is allowed to copy, reprint, reproduce or publish any part of this code,  *
 * nor disclose its contents to others, nor make any use of it, nor allow or assist    *
 * others to make any use of it - unless by prior Written express authorization of     *
 * Infineon Technologies AG and then only to the extent authorized.                    *
 *                                                                                     *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,            *
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY,           *
 * FITNESS FOR A PARTICULAR PURPOSE, OR NON-INFRINGEMENT, ARE DISCLAIMED.  IN NO       *
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,     *
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,                 *
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;         *
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY             *
 * WHETHER IN  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR            *
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF              *
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.                                          *
 *                                                                                     *
 ***************************************************************************************/
/**
 * @file   realtime.c
 * @date   July, 2023
 * @brief  Provide an extended mechanism for executing critical or time-sensitive operations
 *         that cannot be interrupted or preempted, while also minimizing bus arbitration
 *         through hogging cpu cores.
 */
#define pr_fmt(fmt) "%s:%s:%s:%d: " fmt, KBUILD_MODNAME, __FILE__, __func__, __LINE__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpumask.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/smp.h>
#include <linux/preempt.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/ktime.h>
#include <linux/cdev.h>
#include <linux/dmaengine.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include "realtime.h"
#include "chardev.h"

/* Constants */
#define PRIORITY_DEFAULT 0
#define PRIORITY_RT      1
#define PRIORITY_RT_LOW  2
#define PRIORITY_NORMAL  3

/* Configurable */
#define WORKER_CPU_CORE_INDEX           0
#define WORKER_BUSYWAIT_TIMEOUT_US      1000000
/**
 * Should be greater than worker's timeout
 * since the blocker worst case scenario is
 * = worker's timeout + worker's critical section computation time.
 */
#define BLOCKER_BUSYWAIT_TIMEOUT_US     WORKER_BUSYWAIT_TIMEOUT_US + 500000
#define KTHREAD_PRIORITY                PRIORITY_RT
#define PRIORITY_NORMAL_VALUE           MIN_NICE /* MIN_NICE(highest priority) to MAX_NICE(lowest priority) */

enum kthread_lifecycle {
    KTHREAD_STAT_NULL,
    KTHREAD_STAT_READY,
    KTHREAD_REQ_START,
    KTHREAD_STAT_RUNNING,
    KTHREAD_STAT_COMPLETED,
    KTHREAD_STAT_DEAD,
};

/* kthread context */
struct kthread_context {
    struct task_struct *task;
    volatile enum kthread_lifecycle lifecycle;

    int exec_count;    /* Total execution count */
    int timeout_count; /* Total timeout count:
                          - On worker_thread, timeout waiting for the blocker to enter blocking mode.
                          - On blocker_thread, timeout waiting for the worker to complete its execution. */
};

struct realtime_list_context {
    struct list_head list;
    struct session_context *session_ctx;
};

struct this_context {
    struct common_context common;

    struct kthread_context *kt_ctx;
    int cpu_max;
    int cpu_allowed;
    volatile bool is_busy;
    wait_queue_head_t worker_waitq; /* A waitqueue for worker_thread to wait on */
    wait_queue_head_t blocker_waitq; /* A waitqueue for blocker_thread to wait on */
    int worker_timeout; /* Specify the timeout value for waiting for the blocker to enter blocking mode */
    int blocker_timeout; /* Specify the timeout value for waiting for the worker to complete its execution */
    struct session_context *requestor_ctx;
    struct mutex mutex; /* Primarily used to safeguard access from attribute groups */

    struct device_attribute cpu_max_attr;
    struct device_attribute cpu_allowed_attr;
    struct device_attribute worker_timeout_attr;
    struct device_attribute blocker_timeout_attr;
    struct attribute *attrs[5]; /* Based on number of device_attribute + 1 for NULL termination */
    struct attribute_group attr_group;

    realtime_child_context *child_ctx;
    const struct realtime_child_funcs *child_funcs;
};

struct session_context {
    struct common_context common;

    struct this_context *parent;
    wait_queue_head_t waitq; /* A waitqueue for session to wait on */
    bool is_busy;
    int rc;

    realtime_child_session_context *child_ctx;
};

static struct this_context *realtime_context_cast(realtime_context *ctx)
{
    if (ctx != NULL && ctx->magic == REALTIME_CONTEXT_MAGIC) {
        return container_of(ctx, struct this_context, common);
    }

    return NULL;
}

static struct session_context *realtime_session_context_cast(realtime_session_context *ctx)
{
    if (ctx != NULL && ctx->magic == REALTIME_SESSION_CONTEXT_MAGIC) {
        return container_of(ctx, struct session_context, common);
    }

    return NULL;
}

static void sched_set_mode_critical(struct task_struct *task)
{
#if KTHREAD_PRIORITY == PRIORITY_DEFAULT
#elif KTHREAD_PRIORITY == PRIORITY_RT
        sched_set_fifo(task);
#elif KTHREAD_PRIORITY == PRIORITY_RT_LOW
        sched_set_fifo_low(task);
#elif KTHREAD_PRIORITY == PRIORITY_NORMAL
        sched_set_normal(task, PRIORITY_NORMAL_VALUE);
#else
    #error "KTHREAD_PRIORITY is not set"
#endif
}

static void sched_set_mode_normal(struct task_struct *task)
{
    sched_set_normal(task, PRIORITY_NORMAL_VALUE);
}

static int blocker_thread(void *data)
{
    int timeout;
    int local_cpu;
    unsigned long flags;
    struct this_context *this = (struct this_context *)data;
    struct kthread_context *local_ctx;

    local_cpu = smp_processor_id();

    if (local_cpu == WORKER_CPU_CORE_INDEX) {
        pr_err("[cpu-%d] Running on the wrong core.\n", local_cpu);
        goto exit;
    }

    local_ctx = &this->kt_ctx[local_cpu];

    pr_info("[cpu-%d] Started.\n", local_cpu);

    do {
        /* Reset blocker parameters */
        mutex_lock(&this->mutex);
        timeout = this->blocker_timeout;
        mutex_unlock(&this->mutex);

        /* Indicate to worker_thread that blocker_thread is ready for action */
        local_ctx->lifecycle = KTHREAD_STAT_READY;
        wake_up_interruptible_all(&this->worker_waitq);

        /* Wait for start command from worker_thread */
        while (wait_event_interruptible(this->blocker_waitq,
              (local_ctx->lifecycle == KTHREAD_REQ_START) ||
               kthread_should_stop())) {};

        if (local_ctx->lifecycle != KTHREAD_REQ_START ||
            kthread_should_stop()) {
            goto exit;
        }

        /* Critical section begins */
        {
            local_irq_save(flags);
            preempt_disable();

            local_ctx->lifecycle = KTHREAD_STAT_RUNNING;
            smp_wmb(); /* To prevent out-of-order execution on processor and compiler */

            while (timeout) {
                if (!this->is_busy) {
                    break;
                }
                udelay(1);
                timeout--;
            }

            local_irq_restore(flags);
            preempt_enable();
        }
        /* Critical section ends */

        mutex_lock(&this->mutex);
        local_ctx->exec_count++;

        if (!timeout) {
            local_ctx->timeout_count++;
            mutex_unlock(&this->mutex);
            pr_info("[cpu-%d] Timed out while waiting for the worker_thread to end (total runs = %d, total timeouts = %d).\n",
                    local_cpu, local_ctx->exec_count, local_ctx->timeout_count);
        } else {
            mutex_unlock(&this->mutex);
            pr_info("[cpu-%d] No timeout occurred (total runs = %d, total timeouts = %d).\n",
                    local_cpu, local_ctx->exec_count, local_ctx->timeout_count);
        }


    } while (!kthread_should_stop());

exit:

    sched_set_mode_normal(current);

    local_ctx->lifecycle = KTHREAD_STAT_DEAD;

    /* Graciously exit */
    while (!kthread_should_stop()) {
        yield();
    }

    pr_info("[cpu-%d] Exiting.\n", local_cpu);

    return 0;
}

static int worker_thread(void *data)
{
    int timeout;
    int local_cpu;
    int cpu_allowed;
    int i;
    int rc;
    unsigned long flags;
    struct this_context *this =(struct this_context *)data;
    struct session_context *session_ctx;
    struct kthread_context *blocker_ctx;
    struct kthread_context *local_ctx;

    local_cpu = smp_processor_id();

    if (local_cpu != WORKER_CPU_CORE_INDEX) {
        pr_err("[cpu-%d] Running on the wrong core.\n", local_cpu);
        goto exit;
    }

    local_ctx = &this->kt_ctx[local_cpu];

    pr_info("[cpu-%d] Started.\n", local_cpu);

    do {
        /* Wait for blocker threads to become ready */
        for (i = 0; i < this->cpu_allowed; i++) {
            if (i == WORKER_CPU_CORE_INDEX) {
                continue;
            }

            blocker_ctx = &this->kt_ctx[i];

            while (wait_event_interruptible(this->worker_waitq,
                  (blocker_ctx->lifecycle == KTHREAD_STAT_READY) ||
                  (blocker_ctx->lifecycle == KTHREAD_STAT_DEAD) ||
                   kthread_should_stop())) {};

            if (blocker_ctx->lifecycle != KTHREAD_STAT_READY ||
                kthread_should_stop()) {
                goto exit;
            }
        }
        local_ctx->lifecycle = KTHREAD_STAT_READY;

        /* Wait for user request */
        while (wait_event_interruptible(this->worker_waitq,
               this->requestor_ctx ||
               kthread_should_stop())) {};

        if (kthread_should_stop()) {
            goto exit;
        }

        mutex_lock(&this->mutex);

        /* Retrieve pending request */
        session_ctx = this->requestor_ctx;
        this->requestor_ctx = NULL;

        /* Reset worker parameters */
        local_ctx->lifecycle = KTHREAD_STAT_RUNNING;
        timeout = this->worker_timeout;
        this->is_busy = true;
        local_ctx->exec_count++;
        cpu_allowed = this->cpu_allowed;

        mutex_unlock(&this->mutex);

        /* Issue start command to blocker threads */
        for (i = 0; i < cpu_allowed; i++) {
            if (i == WORKER_CPU_CORE_INDEX) {
                continue;
            }

            blocker_ctx = &this->kt_ctx[i];
            blocker_ctx->lifecycle = KTHREAD_REQ_START;
        }

        wake_up_interruptible_all(&this->blocker_waitq);

        /* Critical section begins */

        local_irq_save(flags);
        preempt_disable();

        /*
         * Wait for the blocker_thread to enter the critical section.
         * If a timeout occurs while waiting, the process will
         * proceed regardless.
         */
        for (i = 0; i < cpu_allowed; i++) {
            if (i == WORKER_CPU_CORE_INDEX) {
                continue;
            }

            blocker_ctx = &this->kt_ctx[i];

            while (timeout) {
                if (blocker_ctx->lifecycle == KTHREAD_STAT_RUNNING) {
                    break;
                }
                udelay(1);
                timeout--;
            }

            if (!timeout) {
                break;
            }
        }

        rc = this->child_funcs->execute(session_ctx->child_ctx);

        this->is_busy = false;
        smp_wmb(); /* To prevent out-of-order execution on processor and compiler */

        local_irq_restore(flags);
        preempt_enable();

        /* Critical section ends */

        if (!timeout) {
            mutex_lock(&this->mutex);
            local_ctx->timeout_count++;
            mutex_unlock(&this->mutex);
            pr_info("[cpu-%d] Timed out while waiting for the blocker_thread(cpu-%d) to start (total runs = %d, total timeouts = %d).\n",
                    local_cpu, i, local_ctx->exec_count, local_ctx->timeout_count);
        } else {
            pr_info("[cpu-%d] No timeout occurred (total runs = %d, total timeouts = %d).\n",
                    local_cpu, local_ctx->exec_count, local_ctx->timeout_count);
        }

        local_ctx->lifecycle = KTHREAD_STAT_COMPLETED;

        session_ctx->rc = rc;
        session_ctx->is_busy = false;
        wake_up_interruptible_all(&session_ctx->waitq);

    } while (!kthread_should_stop());

exit:

    sched_set_mode_normal(current);

    local_ctx->lifecycle = KTHREAD_STAT_DEAD;

    /* Graciously exit */
    while (!kthread_should_stop()) {
        yield();
    }

    pr_info("[%d] Exiting.\n", local_cpu);

    return 0;
}

static int realtime_session_open(resource_manager_child_context *child_ctx,
                                 resource_manager_child_session_context **child_s_ctx,
                                 bool *is_orderly)
{
    int rc;
    struct this_context *this = realtime_context_cast(child_ctx);
    struct session_context *session_ctx;

    /*
     * Orderly execution of open() is not neccessary in this context.
     * Nevertheless, we enable orderly execution because we lack knowledge
     * of whether the child's open() requires orderly processing.
     */
    if (!*is_orderly) {
        /* If execution is not orderly, request for it */
        *is_orderly = true;
        rc = 0;
        goto out;
    }

    session_ctx = kzalloc(sizeof(*session_ctx), GFP_KERNEL);
    if (!session_ctx) {
        rc = -ENOMEM;
        goto out;
    }
    session_ctx->common.magic = REALTIME_SESSION_CONTEXT_MAGIC;

    /* Sanity check */
    if (realtime_session_context_cast(&session_ctx->common) != session_ctx) {
        rc = -EFAULT;
        goto out_free_session_ctx;
    }

    rc = this->child_funcs->open(this->child_ctx, &session_ctx->child_ctx);
    if (rc) {
        goto out_free_session_ctx;
    }

    session_ctx->parent = this;
    init_waitqueue_head(&session_ctx->waitq);

    *child_s_ctx = &session_ctx->common;

    goto out;

out_free_session_ctx:
    kfree(session_ctx);
out:
    return rc;
}

static int realtime_session_release(resource_manager_child_session_context *child_s_ctx,
                                    bool *is_orderly)
{
    struct session_context *session_ctx = realtime_session_context_cast(child_s_ctx);
    struct this_context *this = session_ctx->parent;

    /*
     * Orderly execution of release() is not neccessary in this context.
     * Nevertheless, we enable orderly execution because we lack knowledge
     * of whether the child's release() requires orderly processing.
     */
    if (!*is_orderly) {
        /* If execution is not orderly, request for it */
        *is_orderly = true;
        return 0;
    }

    while (wait_event_interruptible(session_ctx->waitq, !session_ctx->is_busy)) {};

    this->child_funcs->release(session_ctx->child_ctx);
    kfree(session_ctx);

    return 0;
}

static ssize_t realtime_session_write1(resource_manager_child_session_context *child_s_ctx,
                                       char **buf)
{
    struct session_context *session_ctx = realtime_session_context_cast(child_s_ctx);
    struct this_context *this = session_ctx->parent;

    return this->child_funcs->write1(session_ctx->child_ctx, buf);
}

static ssize_t realtime_session_write2(resource_manager_child_session_context *child_s_ctx,
                                       size_t len)
{
    struct session_context *session_ctx = realtime_session_context_cast(child_s_ctx);
    struct this_context *this = session_ctx->parent;

    return this->child_funcs->write2(session_ctx->child_ctx, len);
}

static int realtime_session_execute(resource_manager_child_session_context *child_s_ctx,
                                    bool *is_orderly)
{
    int rc;
    int cpu_allowed;
    struct session_context *session_ctx = realtime_session_context_cast(child_s_ctx);
    struct this_context *this = session_ctx->parent;

    /*
     * In theory, execution can occur in three different modes:
     * 1. Non-orderly and time-sharing (or time-slicing)
     * x. Non-orderly and real-time. However, this option is not
     *    possible given that the definition of real-time execution
     *    here is uninterruptible and non-preemptible. Therefore,
     *    being real-time already implies being orderly.
     * 2. Orderly and time-sharing
     * 3. Orderly and real-time
     *
     * The term "orderly" here refers to an operation being
     * processed sequentially.
     *
     * The term "real-time" here refers to an operation that is
     * processed without any interruptions or preemption. The
     * opposite of real-time is time-sharing or time-slicing.
     *
     * Currently, only 2nd & 3rd mode are supported for the sake
     * of simplicity.
     */
    if (!*is_orderly) {
        /* If execution is not orderly, request for it. */
        *is_orderly = true;
        rc = 0;
        goto out;
    }

    /* At this stage, the execution is orderly. */

    mutex_lock(&this->mutex);
    cpu_allowed = this->cpu_allowed;
    mutex_unlock(&this->mutex);

    if (cpu_allowed) {
        /* Request real-time execution. */

        this->requestor_ctx = session_ctx;
        session_ctx->is_busy = true;
        wake_up_interruptible_all(&this->worker_waitq);

        while (wait_event_interruptible(session_ctx->waitq, !session_ctx->is_busy)) {};

        rc = session_ctx->rc;
        session_ctx->rc = 0;
    } else {
        /* Execution without real-time. */
        rc = this->child_funcs->execute(session_ctx->child_ctx);
    }

out:
    return rc;
}

static int realtime_session_read1(resource_manager_child_session_context *child_s_ctx,
                                  char **buf)
{
    struct session_context *session_ctx = realtime_session_context_cast(child_s_ctx);
    struct this_context *this = session_ctx->parent;

    return this->child_funcs->read1(session_ctx->child_ctx, buf);
}

static int realtime_session_read2(resource_manager_child_session_context *child_s_ctx,
                                  size_t len)
{
    struct session_context *session_ctx = realtime_session_context_cast(child_s_ctx);
    struct this_context *this = session_ctx->parent;

    return this->child_funcs->read2(session_ctx->child_ctx, len);
}

static const struct resource_manager_child_funcs rm_child = {
    .open = realtime_session_open,
    .write1 = realtime_session_write1,
    .write2 = realtime_session_write2,
    .execute = realtime_session_execute,
    .read1 = realtime_session_read1,
    .read2 = realtime_session_read2,
    .release = realtime_session_release,
};

const struct resource_manager_child_funcs *realtime_get_hook_resource_manager(void)
{
    return &rm_child;
}

static ssize_t cpu_max_show(struct device *dev,
                            struct device_attribute *attr, char *buf)
{
    struct this_context *this =
        container_of(attr, struct this_context, cpu_max_attr);
    int i;

    mutex_lock(&this->mutex);
    i = this->cpu_max;
    mutex_unlock(&this->mutex);

    return sprintf(buf, "%d\n", i);
}

static ssize_t cpu_allowed_show(struct device *dev,
                                struct device_attribute *attr, char *buf)
{
    struct this_context *this =
        container_of(attr, struct this_context, cpu_allowed_attr);
    int i;

    mutex_lock(&this->mutex);
    i = this->cpu_allowed;
    mutex_unlock(&this->mutex);

    return sprintf(buf, "%d\n", i);
}

static ssize_t cpu_allowed_store(struct device *dev, struct device_attribute *attr,
                                 const char *buf, size_t count)
{
    struct this_context *this =
        container_of(attr, struct this_context, cpu_allowed_attr);

    int temp = 0;
    sscanf(buf, "%d", &temp);

    mutex_lock(&this->mutex);
    this->cpu_allowed = min_t(int, (temp < 0) ? 0 : temp, this->cpu_max);
    mutex_unlock(&this->mutex);
    return count;
}

static ssize_t worker_timeout_show(struct device *dev,
                                   struct device_attribute *attr, char *buf)
{
    struct this_context *this =
        container_of(attr, struct this_context, worker_timeout_attr);
    int i;

    mutex_lock(&this->mutex);
    i = this->worker_timeout;
    mutex_unlock(&this->mutex);

    return sprintf(buf, "%d\n", i);
}

static ssize_t worker_timeout_store(struct device *dev, struct device_attribute *attr,
                                    const char *buf, size_t count)
{
    struct this_context *this =
        container_of(attr, struct this_context, worker_timeout_attr);

    int temp = 0;
    sscanf(buf, "%d", &temp);

    mutex_lock(&this->mutex);
    this->worker_timeout = (temp < 0) ? 0 : temp;
    mutex_unlock(&this->mutex);
    return count;
}

static ssize_t blocker_timeout_show(struct device *dev,
                                    struct device_attribute *attr, char *buf)
{
    struct this_context *this =
        container_of(attr, struct this_context, blocker_timeout_attr);
    int i;

    mutex_lock(&this->mutex);
    i = this->blocker_timeout;
    mutex_unlock(&this->mutex);

    return sprintf(buf, "%d\n", i);
}

static ssize_t blocker_timeout_store(struct device *dev, struct device_attribute *attr,
                                     const char *buf, size_t count)
{
    struct this_context *this =
        container_of(attr, struct this_context, blocker_timeout_attr);

    int temp = 0;
    sscanf(buf, "%d", &temp);

    mutex_lock(&this->mutex);
    this->blocker_timeout = (temp < 0) ? 0 : temp;
    mutex_unlock(&this->mutex);
    return count;
}

const struct attribute_group *realtime_get_attribute_group(realtime_context *rt_ctx)
{
    struct this_context *this = realtime_context_cast(rt_ctx);
    return &this->attr_group;
}

static int spawner(struct this_context *this)
{
    int rc = 0;
    int cpu_id;
    int cpu;
    char string[256] = {0};

    for_each_online_cpu(cpu) {
        this->cpu_max++;
    }

    pr_info("[cpu-%d] Started.\n", smp_processor_id());

    if (this->cpu_max < 2) {
        pr_err("Unable to proceed due to insufficient CPU cores.\n");
        rc = -ENXIO;
        goto out;
    }

    pr_info("[cpu-%d] total CPU count: %d.\n", smp_processor_id(), this->cpu_max);

    this->kt_ctx = kzalloc(sizeof(*this->kt_ctx)*this->cpu_max, GFP_KERNEL);
    if (!this->kt_ctx) {
        pr_err("kzalloc has failed.\n");
        rc = -ENOMEM;
        goto out;
    }

    init_waitqueue_head(&this->worker_waitq);
    init_waitqueue_head(&this->blocker_waitq);
    mutex_init(&this->mutex);

    this->worker_timeout = WORKER_BUSYWAIT_TIMEOUT_US;
    this->blocker_timeout = BLOCKER_BUSYWAIT_TIMEOUT_US;
    this->cpu_allowed = this->cpu_max;

    this->cpu_max_attr.attr.name = "cpu_max";
    this->cpu_max_attr.attr.mode = 0444;
    this->cpu_max_attr.show = cpu_max_show;
    this->cpu_allowed_attr.attr.name = "cpu_allowed";
    this->cpu_allowed_attr.attr.mode = 0644;
    this->cpu_allowed_attr.show = cpu_allowed_show;
    this->cpu_allowed_attr.store = cpu_allowed_store;
    this->worker_timeout_attr.attr.name = "worker_timeout";
    this->worker_timeout_attr.attr.mode = 0644;
    this->worker_timeout_attr.show = worker_timeout_show;
    this->worker_timeout_attr.store = worker_timeout_store;
    this->blocker_timeout_attr.attr.name = "blocker_timeout";
    this->blocker_timeout_attr.attr.mode = 0644;
    this->blocker_timeout_attr.show = blocker_timeout_show;
    this->blocker_timeout_attr.store = blocker_timeout_store;
    this->attrs[0] = &this->cpu_max_attr.attr;
    this->attrs[1] = &this->cpu_allowed_attr.attr;
    this->attrs[2] = &this->worker_timeout_attr.attr;
    this->attrs[3] = &this->blocker_timeout_attr.attr;
    this->attrs[4] = NULL;
    this->attr_group.name = "realtime"; /* Optional */
    this->attr_group.attrs = this->attrs;

    /* Create and initialize kthreads */
    for (cpu_id = 0; cpu_id < this->cpu_max; cpu_id++) {
        struct kthread_context *kt_ctx = &this->kt_ctx[cpu_id];

        kt_ctx->lifecycle = KTHREAD_STAT_NULL;

        /* We want the spawner_thread and worker_thread to operate on the same core */
        if (cpu_id == WORKER_CPU_CORE_INDEX) {
            snprintf(string, sizeof(string), "worker_thread(cpu-%d)", cpu_id);
            kt_ctx->task = kthread_create(worker_thread, (void *)this, string);
        } else {
            snprintf(string, sizeof(string), "blocker_thread(cpu-%d)", cpu_id);
            kt_ctx->task = kthread_create(blocker_thread, (void *)this, string);
        }

        if (IS_ERR_OR_NULL(kt_ctx->task)) {
            pr_err("kthread_create(worker_thread/blocker_thread) has failed, exiting.\n");
            rc = -EFAULT;
            goto out_free_kt_ctx;
        }

        pr_info("[cpu-%d] Created a worker/blocker kthread and bound it to the cpu-%d, scheduling policy %d.\n", smp_processor_id(), cpu_id, kt_ctx->task->policy);

        kthread_bind(kt_ctx->task, cpu_id);

        /* Set scheduling policy */
        sched_set_mode_critical(kt_ctx->task);
    }

    /* Launch kthreads */
    for (cpu_id = this->cpu_max - 1; cpu_id >= 0; cpu_id--) {
        struct kthread_context *kt_ctx = &this->kt_ctx[cpu_id];
        wake_up_process(kt_ctx->task);
    }

    goto out;

out_free_kt_ctx:
    kfree(this->kt_ctx);
out:
    return rc;
}

int realtime_init(realtime_context **rt_ctx)
{
    int rc;
    struct this_context *this;

    this = kzalloc(sizeof(*this), GFP_KERNEL);
    if (!this) {
        return -ENOMEM;
    }
    this->common.magic = REALTIME_CONTEXT_MAGIC;

    /* Sanity check */
    if (realtime_context_cast(&this->common) != this) {
        rc = -EFAULT;
        goto out_free_this;
    }

    *rt_ctx = &this->common;

    return spawner(this);

out_free_this:
    kfree(this);
    return rc;
}

int realtime_set_hook(realtime_context *rt_ctx,
                      realtime_child_context *child_ctx,
                      const struct realtime_child_funcs *child_funcs)
{
    struct this_context *this = realtime_context_cast(rt_ctx);

    /* Check for any missing implementations */
    if (!this || !child_ctx ||
        !child_funcs ||
        !child_funcs->open ||
        !child_funcs->write1 ||
        !child_funcs->write2 ||
        !child_funcs->execute ||
        !child_funcs->read1 ||
        !child_funcs->read2 ||
        !child_funcs->release) {
        return -EINVAL;
    }

    this->child_ctx = child_ctx;
    this->child_funcs = child_funcs;

    return 0;
}

void realtime_exit(realtime_context **rt_ctx)
{
    int cpu_id;
    struct this_context *this = realtime_context_cast(*rt_ctx);

    /* Issues stop signal to all kthreads */
    for (cpu_id = 0; cpu_id < this->cpu_max; cpu_id++) {
        struct kthread_context *kt_ctx = &this->kt_ctx[cpu_id];
        if (!IS_ERR_OR_NULL(kt_ctx->task)) {
            kthread_stop(kt_ctx->task);
        }
    }

    /* Wake up all waiting threads */
    wake_up_interruptible_all(&this->worker_waitq);
    wake_up_interruptible_all(&this->blocker_waitq);

    /* Wait for all kthreads to exit */
    for (cpu_id = 0; cpu_id < this->cpu_max; cpu_id++) {
        struct kthread_context *kt_ctx = &this->kt_ctx[cpu_id];
        wait_queue_head_t *waitq;

        if (cpu_id == WORKER_CPU_CORE_INDEX) {
            waitq = &this->worker_waitq;
        } else {
            waitq = &this->blocker_waitq;
        }

        while (wait_event_interruptible(
               *waitq, kt_ctx->lifecycle == KTHREAD_STAT_DEAD)) {};

    }

    kfree(this->kt_ctx);
    mutex_destroy(&this->mutex);
    kfree(this);
    *rt_ctx = NULL;
}

