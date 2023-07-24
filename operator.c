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
 * @file   operator.c
 * @date   July, 2023
 * @brief  Provide an extreme mechanism for executing critical or time-sensitive operations
 *         that cannot be interrupted or preempted, while also minimizing bus arbitration.
 */
#define pr_fmt(fmt) "%s:%s:%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

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
#include <linux/poll.h>
#include <linux/uaccess.h>
#include "operator.h"

/* Constants */
#define OP_CMD_AIO       0

#define PRIORITY_DEFAULT 0
#define PRIORITY_RT      1
#define PRIORITY_RT_LOW  2
#define PRIORITY_NORMAL  3

/* Configurable */
#define MAX_ALLOWED_FOPS_SESSIONS     255
#define WORKER_CPU_CORE_INDEX         0
#define WORKER_BUSYWAIT_TIMEOUT_US    1000000
/**
 * Should be greater than worker's timeout
 * since the blocker worst case scenario is
 * = worker's timeout + worker's critical section computation time.
 */
#define BLOCKER_BUSYWAIT_TIMEOUT_US      WORKER_BUSYWAIT_TIMEOUT_US + 500000
/**
 * After the kernel thread is created, a single sleep operation is
 * introduced. Without this pause, the chances of a worker_thread
 * timing out would significantly increase.
 *
 * This value needs to be adjusted based on your specific platform.
 */
#define KTHREAD_SETTLE_TIME_MS  100
#define KTHREAD_PRIORITY        PRIORITY_RT
#define PRIORITY_NORMAL_VALUE   MIN_NICE /* MIN_NICE (highest priority) to MAX_NICE (lowest priority) */

enum kthread_lifecycle {
    KTHREAD_STAT_NULL,
    KTHREAD_STAT_READY,
    KTHREAD_REQ_START,
    KTHREAD_STAT_RUNNING,
    KTHREAD_STAT_COMPLETED,
    KTHREAD_STAT_DEAD,
};

enum response_code {
    RC_NIL = 0,
    RC_INTERNAL_NOT_READY,
    RC_INTERNAL_ERROR,
    RC_INVALID_CMD,
    RC_EXEC_NOK,
    RC_EXEC_NOK_WITH_TIMEOUT,
    RC_EXEC_OK,
    RC_EXEC_BUSY,
};

/* kthread context */
struct kthread_context {
    struct task_struct *task;
    volatile enum kthread_lifecycle lifecycle;

    int exec_count; /* Total execution count */
    int exec_ok_count; /* Total sucessful execution count (only meaningful on worker_thread) */
    int timeout_count; /* Total timeout count:
                          - On worker_thread, timeout waiting for the blocker to enter blocking mode.
                          - On blocker_thread, timeout waiting for the worker to complete its execution. */
};

struct op_list_context {
    struct list_head list;
    struct op_session_context *s_ctx;
};

/* Global operation context */
struct op_glob_context {
    struct kthread_context *kt_ctx;
    int cpu_count;
    volatile bool is_busy;
    wait_queue_head_t worker_waitq; /* A waitqueue for worker_thread to wait on */
    wait_queue_head_t blocker_waitq; /* A waitqueue for blocker_thread to wait on */
    struct workqueue_struct *workq;
    struct mutex mutex;
    struct list_head list; /* Head of the list */
    atomic_t list_count; /* To keep track of the list */
    atomic_t fops_session_count; /* To keep track of the fops open sessions */
    bool is_calibrated; /* Measure the total time required to hog all CPU cores */
    int worker_timeout; /* Specify the timeout value for waiting for the blocker to enter blocking mode */
    int blocker_timeout; /* Specify the timeout value for waiting for the worker to complete its execution */
};

/* Session operation context */
struct op_session_context {
    struct op_glob_context *g_ctx;
    struct work_struct execution_work;
    struct work_struct release_work;
    struct mutex mutex; /* For implementation of mutex-based access control to fops */
    wait_queue_head_t fops_waitq; /* A waitqueue for fops polling to wait on */
    wait_queue_head_t workq_waitq; /* A waitqueue for workqueue entry to wait on */
    bool fops_busy;
    bool workq_busy;
    uint8_t cmd[10];
    uint8_t resp[10];
    size_t cmd_len;
    size_t resp_len;
};

static void sched_set_mode_critical (struct task_struct *task)
{
#if KTHREAD_PRIORITY == PRIORITY_DEFAULT
#elif KTHREAD_PRIORITY == PRIORITY_RT
        sched_set_fifo (task);
#elif KTHREAD_PRIORITY == PRIORITY_RT_LOW
        sched_set_fifo_low (task);
#elif KTHREAD_PRIORITY == PRIORITY_NORMAL
        sched_set_normal (task, PRIORITY_NORMAL_VALUE);
#else
    #error "KTHREAD_PRIORITY is not set"
#endif
}

static void sched_set_mode_exit (struct task_struct *task)
{
    sched_set_normal (task, MAX_NICE);
}

static int blocker_thread (void *data)
{
    int timeout;
    int local_cpu;
    unsigned long flags;
    struct op_glob_context *op_ctx = (struct op_glob_context *)data;
    struct kthread_context *local_ctx;

    local_cpu = smp_processor_id ();

    if (local_cpu == WORKER_CPU_CORE_INDEX) {
        pr_err ("[cpu-%d] Running on the wrong core.\n", local_cpu);
        goto exit;
    }

    local_ctx = &op_ctx->kt_ctx[local_cpu];

    pr_info ("[cpu-%d] Started.\n", local_cpu);

    //msleep (KTHREAD_SETTLE_TIME_MS);

    do {
        /* Reset blocker parameters */
        timeout = BLOCKER_BUSYWAIT_TIMEOUT_US;

        /* Indicate to worker_thread that blocker_thread is ready for action */
        local_ctx->lifecycle = KTHREAD_STAT_READY;
        smp_wmb ();
        wake_up_interruptible_all (&op_ctx->worker_waitq);

        /* Wait for start command from worker_thread */
        while (wait_event_interruptible (op_ctx->blocker_waitq,
               (local_ctx->lifecycle == KTHREAD_REQ_START) ||
               kthread_should_stop ())) {};

        if (local_ctx->lifecycle != KTHREAD_REQ_START ||
            kthread_should_stop ()) {
            goto exit;
        }

        /* Critical section begins */
        {
            local_irq_save (flags);
            preempt_disable ();

            local_ctx->lifecycle = KTHREAD_STAT_RUNNING;
            smp_wmb();

            while (--timeout) {
                smp_rmb ();

                if (!op_ctx->is_busy) {
                    break;
                }

                udelay (1);
            }

            local_irq_restore (flags);
            preempt_enable ();
        }
        /* Critical section ends */

        local_ctx->exec_count++;

        if (!timeout) {
            local_ctx->timeout_count++;
            pr_info ("[cpu-%d] Timed out while waiting for the worker_thread to end (total = %d, timeout = %d).\n",
                    local_cpu, local_ctx->exec_count, local_ctx->timeout_count);
        } else {
            pr_info ("[cpu-%d] No timeout occurred (total = %d, timeout = %d).\n",
                    local_cpu, local_ctx->exec_count, local_ctx->timeout_count);
        }

    } while (!kthread_should_stop ());

exit:

    sched_set_mode_exit (current);

    local_ctx->lifecycle = KTHREAD_STAT_DEAD;

    /* Graciously exit */
    while (!kthread_should_stop ()) {
        yield ();
    }

    pr_info ("[cpu-%d] Exiting.\n", local_cpu);

    return 0;
}

static void service_workqueue (struct op_glob_context *g_ctx)
{
    int i = atomic_read (&g_ctx->list_count);
    atomic_sub (i, &g_ctx->list_count);

    pr_info ("To process number of entries: %d.\n", i);

    while (i--) {
        struct op_list_context *list_ctx = list_first_entry_or_null (&g_ctx->list, struct op_list_context, list);
        struct op_session_context *s_ctx = list_ctx->s_ctx;

        mutex_lock (&g_ctx->mutex);
        list_del (&list_ctx->list);
        mutex_unlock (&g_ctx->mutex);

        mutex_lock (&s_ctx->mutex);
        s_ctx->workq_busy = false;
        mutex_unlock (&s_ctx->mutex);

        wake_up_interruptible_all (&s_ctx->workq_waitq);
        kfree (list_ctx);
    }
}

static int worker_thread (void *data)
{
    int timeout;
    int local_cpu;
    int i;
    uint16_t ret = -1;
    unsigned long flags;
    struct op_glob_context *op_ctx = (struct op_glob_context *)data;
    struct kthread_context *blocker_ctx;
    struct kthread_context *local_ctx;

    local_cpu = smp_processor_id ();

    if (local_cpu != WORKER_CPU_CORE_INDEX) {
        pr_err ("[cpu-%d] Running on the wrong core.\n", local_cpu);
        goto exit;
    }

    local_ctx = &op_ctx->kt_ctx[local_cpu];

    pr_info ("[cpu-%d] Started.\n", local_cpu);

    do {
        /* Wait for blocker threads to become ready */
        for (i = 0; i < op_ctx->cpu_count; i++) {
            if (i == WORKER_CPU_CORE_INDEX) {
                continue;
            }

            blocker_ctx = &op_ctx->kt_ctx[i];

            while (wait_event_interruptible (op_ctx->worker_waitq,
                   (blocker_ctx->lifecycle == KTHREAD_STAT_READY) ||
                   (blocker_ctx->lifecycle == KTHREAD_STAT_DEAD) ||
                   kthread_should_stop ())) {};

            if (blocker_ctx->lifecycle != KTHREAD_STAT_READY ||
                kthread_should_stop ()) {
                goto exit;
            }
        }
        local_ctx->lifecycle = KTHREAD_STAT_READY;

        /* Wait for user request */
        while (wait_event_interruptible (op_ctx->worker_waitq,
               atomic_read (&op_ctx->list_count) ||
               kthread_should_stop ())) {};
        if (kthread_should_stop ()) {
            goto exit;
        }

        /* Reset worker parameters */
        local_ctx->lifecycle = KTHREAD_STAT_RUNNING;
        timeout = WORKER_BUSYWAIT_TIMEOUT_US;
        op_ctx->is_busy = true;
        local_ctx->exec_count++;

        /* Issue start command to blocker threads */
        for (i = 0; i < op_ctx->cpu_count; i++) {
            if (i == WORKER_CPU_CORE_INDEX) {
                continue;
            }

            blocker_ctx = &op_ctx->kt_ctx[i];
            blocker_ctx->lifecycle = KTHREAD_REQ_START;
        }
        wake_up_interruptible_all (&op_ctx->blocker_waitq);

        /* Critical section begins */
        {
            local_irq_save (flags);
            preempt_disable ();

            /* Wait for blocker_thread to enter critical section */
            for (i = 0; i < op_ctx->cpu_count; i++) {
                if (i == WORKER_CPU_CORE_INDEX) {
                    continue;
                }

                blocker_ctx = &op_ctx->kt_ctx[i];

                do {
                    smp_rmb ();
                    if (blocker_ctx->lifecycle == KTHREAD_STAT_RUNNING) {
                        break;
                    }

                    udelay (1);
                } while (--timeout);

                if (!timeout) {
                    break;
                }
            }

            /* The computation logic begins here */

            do {
                int delay_ms = 400;
                while (--delay_ms) {
                    udelay(1000);
                }
                ret = 0;
            } while (false);

            /* The computation logic ends here */

            op_ctx->is_busy = false;
            smp_wmb ();

            local_irq_restore (flags);
            preempt_enable ();
        }
        /* Critical section ends */

        if (!timeout) {
            local_ctx->timeout_count++;
            pr_info ("[cpu-%d] Timed out while waiting for the blocker_thread (cpu-%d) to start.\n", local_cpu, i);
        }

        if (ret != 0) {
            pr_info ("[cpu-%d] Execution has failed (total = %d, ok = %d, timeout = %d).\n",
                    local_cpu, local_ctx->exec_count, local_ctx->exec_ok_count, local_ctx->timeout_count);
        } else {
            local_ctx->exec_ok_count++;
            pr_info ("[cpu-%d] Execution has passed (total = %d, ok = %d, timeout = %d).\n",
                    local_cpu, local_ctx->exec_count, local_ctx->exec_ok_count, local_ctx->timeout_count);
        }

        local_ctx->lifecycle = KTHREAD_STAT_COMPLETED;

        /* Service all open requests (execution_handler) */
        service_workqueue (op_ctx);

    } while (!kthread_should_stop ());

exit:

    sched_set_mode_exit (current);

    local_ctx->lifecycle = KTHREAD_STAT_DEAD;

    /* Service all open requests (execution_handler) */
    service_workqueue (op_ctx);

    /* Graciously exit */
    while (!kthread_should_stop ()) {
        yield ();
    }

    pr_info ("[%d] Exiting.\n", local_cpu);

    return 0;
}

static int spawner (struct op_glob_context *g_ctx)
{
    int cpu_id;
    int cpu;
    char string[256] = {0};

    for_each_online_cpu (cpu) {
        g_ctx->cpu_count++;
    }

    pr_info ("[cpu-%d] Started.\n", smp_processor_id ());

    if (g_ctx->cpu_count < 2) {
        pr_err ("Unable to proceed due to insufficient CPU cores.\n");
        return -ENXIO;
    }

    pr_info ("[cpu-%d] total CPU count: %d.\n", smp_processor_id (), g_ctx->cpu_count);

    g_ctx->kt_ctx = kzalloc (sizeof (*g_ctx->kt_ctx) * g_ctx->cpu_count, GFP_KERNEL);
    if (!g_ctx->kt_ctx) {
        pr_err ("kzalloc has failed.\n");
        return -ENOMEM;
    }

    init_waitqueue_head (&g_ctx->worker_waitq);
    init_waitqueue_head (&g_ctx->blocker_waitq);
    mutex_init (&g_ctx->mutex);
    INIT_LIST_HEAD (&g_ctx->list);

    g_ctx->workq = alloc_workqueue("tpm_dev_wq", WQ_MEM_RECLAIM, 0);
    if (!g_ctx->workq) {
        return -ENOMEM;
    }

    /* Create and initialize kthreads */
    for (cpu_id = 0; cpu_id < g_ctx->cpu_count; cpu_id++) {
        struct kthread_context *kt_ctx = &g_ctx->kt_ctx[cpu_id];

        kt_ctx->lifecycle = KTHREAD_STAT_NULL;

        /* We want the spawner_thread and worker_thread to operate on the same core */
        if (cpu_id == WORKER_CPU_CORE_INDEX) {
            snprintf (string, sizeof(string), "worker_thread (cpu-%d)", cpu_id);
            kt_ctx->task = kthread_create (worker_thread, (void *)g_ctx, string);
        } else {
            snprintf (string, sizeof (string), "blocker_thread (cpu-%d)", cpu_id);
            kt_ctx->task = kthread_create (blocker_thread, (void *)g_ctx, string);
        }

        if (IS_ERR_OR_NULL (kt_ctx->task)) {
            pr_err ("kthread_create(worker_thread/blocker_thread) has failed, exiting.\n");
            return 0;
        }

        pr_info ("[cpu-%d] Created a worker/blocker kthread and bound it to the cpu-%d, scheduling policy %d.\n", smp_processor_id (), cpu_id, kt_ctx->task->policy);

        kthread_bind (kt_ctx->task, cpu_id);

        /* Set scheduling policy */
        sched_set_mode_critical (current);
    }

    /* Launch kthreads */
    for (cpu_id = g_ctx->cpu_count - 1; cpu_id >= 0; cpu_id--) {
        struct kthread_context *kt_ctx = &g_ctx->kt_ctx[cpu_id];
        wake_up_process (kt_ctx->task);
    }

    return 0;
}

static void execution_handler (struct work_struct *work)
{
    struct op_session_context *s_ctx =
            container_of(work, struct op_session_context, execution_work);
    struct op_glob_context *g_ctx = s_ctx->g_ctx;
    struct op_list_context *op_list_ctx;

    op_list_ctx = kzalloc (sizeof (*op_list_ctx), GFP_KERNEL);
    if (op_list_ctx == NULL) {
        pr_err ("Memory allocation has failed, unable to process the command.\n");
        mutex_lock (&s_ctx->mutex);
        goto out;
    }
    INIT_LIST_HEAD (&op_list_ctx->list);
    op_list_ctx->s_ctx = s_ctx;

    /* Request to start operation */
    s_ctx->workq_busy = true;
    mutex_lock (&g_ctx->mutex);
    list_add (&op_list_ctx->list, g_ctx->list.prev);
    mutex_unlock (&g_ctx->mutex);
    atomic_inc (&g_ctx->list_count);
    wake_up_interruptible_all (&g_ctx->worker_waitq);

    /* Wait for the operation */
    while (wait_event_interruptible (s_ctx->workq_waitq,
           !s_ctx->workq_busy ||
           g_ctx->kt_ctx[WORKER_CPU_CORE_INDEX].lifecycle == KTHREAD_STAT_DEAD)) {};

    mutex_lock (&s_ctx->mutex);

    if (g_ctx->kt_ctx[WORKER_CPU_CORE_INDEX].lifecycle == KTHREAD_STAT_DEAD) {
        s_ctx->resp[0] = RC_INTERNAL_ERROR;
        s_ctx->resp_len = 1;
    } else {
#if 1
        s_ctx->resp[0] = RC_EXEC_OK;
        s_ctx->resp_len = 1;
#endif
    }

out:
    s_ctx->fops_busy = false;

    mutex_unlock (&s_ctx->mutex);
    wake_up_interruptible_all (&s_ctx->fops_waitq);

    pr_info ("Exiting.\n");
}

static void release_helper (struct op_session_context *s_ctx)
{
    struct op_glob_context *g_ctx = s_ctx->g_ctx;

    while (wait_event_interruptible (s_ctx->fops_waitq, !s_ctx->fops_busy)) {};
    mutex_destroy (&s_ctx->mutex);
    kfree (s_ctx);
    atomic_dec (&g_ctx->fops_session_count);
}

static void release_handler (struct work_struct *work)
{
    struct op_session_context *s_ctx =
            container_of(work, struct op_session_context, release_work);

    pr_info ("Entering.\n");
    release_helper (s_ctx);
    pr_info ("Exiting.\n");
}

ssize_t op_fops_read (struct op_session_context *s_ctx,
                      struct file *file, char *buf, size_t size, loff_t *off)
{
    ssize_t ret;
    int rc;

    mutex_lock (&s_ctx->mutex);

    if (s_ctx->fops_busy) {
        if (file->f_flags & O_NONBLOCK) {
            /* Non-blocking */
            pr_info ("O_NONBLOCK mode, returning busy status.\n");

            s_ctx->resp[0] = RC_EXEC_BUSY;
            s_ctx->resp_len = 1;
        } else {
            /* Blocking */
            pr_info ("~O_NONBLOCK mode, wait for completion before reading the response.\n");

            mutex_unlock (&s_ctx->mutex);
            if (wait_event_interruptible (s_ctx->fops_waitq, !s_ctx->fops_busy)) {
                return -EINTR;
            }
            mutex_lock (&s_ctx->mutex);
        }
    } else {
        pr_info ("Command has been processed, returning the response.\n");
    }

    ret = s_ctx->resp_len;
    if (ret) {
        rc = copy_to_user (buf, s_ctx->resp, ret);
        if (rc) {
            ret = -EFAULT;
        } else {
            s_ctx->resp_len = 0;
        }
    }

    mutex_unlock (&s_ctx->mutex);
    return ret;
}

ssize_t op_fops_write (struct op_session_context *s_ctx,
                       struct file *file, const char *buf, size_t size, loff_t *off)
{
    ssize_t ret;
    struct op_glob_context *g_ctx = s_ctx->g_ctx;

    /* Only 1 byte long command is supported for now */
    if (size != 1) {
        return -E2BIG;
    }

    pr_info ("Receiving command.\n");

    mutex_lock (&s_ctx->mutex);

    if (s_ctx->fops_busy) {
        ret = -EBUSY;
        goto out;
    }

    s_ctx->cmd_len = size;
    if (copy_from_user (s_ctx->cmd, buf, size)) {
        ret = -EFAULT;
        goto out;
    }

    ret = size;
    switch (s_ctx->cmd[0]) {
        case OP_CMD_AIO:
            s_ctx->fops_busy = true;

            /* Queue the work to workqueue */
            queue_work (g_ctx->workq, &s_ctx->execution_work);
            break;
        default:
            s_ctx->resp[0] = RC_INVALID_CMD;
            s_ctx->resp_len = 1;
            break;
    }

out:
    mutex_unlock (&s_ctx->mutex);
    *off = 0;
    return ret;
}

__poll_t op_fops_poll (struct op_session_context * s_ctx,
                       struct file *file,
                       struct poll_table_struct *poll_table)
{
    __poll_t mask = 0;

    pr_info ("Polling.\n");

    poll_wait (file, &s_ctx->fops_waitq, poll_table);

    mutex_lock (&s_ctx->mutex);

    if (!s_ctx->fops_busy) {
        mask = EPOLLIN | EPOLLRDNORM; /* Indicates that there is data available for reading from fd */
    }

    mutex_unlock (&s_ctx->mutex);

    return mask;
}

struct op_session_context *op_fops_open (struct op_glob_context *g_ctx)
{
    struct op_session_context *ctx;

    if (atomic_read (&g_ctx->fops_session_count) >= MAX_ALLOWED_FOPS_SESSIONS) {
        return ERR_PTR (-ENOMEM);
    }

    atomic_inc (&g_ctx->fops_session_count);

    ctx = kzalloc (sizeof (*ctx), GFP_KERNEL);
    if (ctx) {
        ctx->g_ctx = g_ctx;
        INIT_WORK (&ctx->execution_work, execution_handler);
        INIT_WORK (&ctx->release_work, release_handler);
        mutex_init (&ctx->mutex);
        init_waitqueue_head (&ctx->fops_waitq);
        init_waitqueue_head (&ctx->workq_waitq);
    }

    return ctx;
}

int op_fops_release (struct op_session_context *s_ctx, struct file *file)
{
    struct op_glob_context *g_ctx = s_ctx->g_ctx;

    if (s_ctx->fops_busy) {
        if (file->f_flags & O_NONBLOCK) {
            /* Non-blocking */
            pr_info ("O_NONBLOCK mode, process it in the background.\n");
            queue_work (g_ctx->workq, &s_ctx->release_work);
            return 0;
        }
    }

    pr_info ("Wait and release.\n");
    release_helper (s_ctx);

    return 0;
}

int op_init (struct op_glob_context **g_ctx)
{
    *g_ctx = kzalloc (sizeof (**g_ctx), GFP_KERNEL);
    if (*g_ctx == NULL) {
        return -ENOMEM;
    }

    return spawner (*g_ctx);
}

void op_exit (struct op_glob_context *op_ctx)
{
    int cpu_id;

    /* Issues stop signal to all kthreads */
    for(cpu_id = 0; cpu_id < op_ctx->cpu_count; cpu_id++) {
        struct kthread_context *kt_ctx = &op_ctx->kt_ctx[cpu_id];
        if (!IS_ERR_OR_NULL (kt_ctx->task)) {
            kthread_stop (kt_ctx->task);
        }
    }

    /* Wake up all waiting threads */
    wake_up_interruptible_all (&op_ctx->worker_waitq);
    wake_up_interruptible_all (&op_ctx->blocker_waitq);

    /* Wait for all kthreads to exit */
    for(cpu_id = 0; cpu_id < op_ctx->cpu_count; cpu_id++) {
        struct kthread_context *kt_ctx = &op_ctx->kt_ctx[cpu_id];
        wait_queue_head_t *waitq;

        if (cpu_id == WORKER_CPU_CORE_INDEX) {
            waitq = &op_ctx->worker_waitq;
        } else {
            waitq = &op_ctx->blocker_waitq;
        }

        while (wait_event_interruptible (
               *waitq, kt_ctx->lifecycle == KTHREAD_STAT_DEAD)) {};
    }

    flush_workqueue (op_ctx->workq);
    destroy_workqueue (op_ctx->workq);

    mutex_destroy (&op_ctx->mutex);

    kfree (op_ctx->kt_ctx);
    kfree (op_ctx);
}
