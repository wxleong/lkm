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
#define PRIORITY_DEFAULT 0
#define PRIORITY_RT      1
#define PRIORITY_RT_LOW  2
#define PRIORITY_NORMAL  3

/* Configurable */
#define WORKER_CPU_CORE_INDEX   0
#define WORKER_BUSYWAIT_TIMEOUT_US       1000000
/**
 * Should be greater than worker's timeout
 * since the blocker worst case scenario is
 * = worker's timeout + worker's critical section computation time.
 */
#define BLOCKER_BUSYWAIT_TIMEOUT_US      WORKER_BUSYWAIT_TIMEOUT_US + 500000
/**
 * The sleep operation (in blocker_thread) at the end of the
 * operation is essential for enhancing system responsiveness.
 * The sleep is not placed during the wait_event (wait_event_cmd).
 * This is because the blocker thread has only one wait_event before entering the
 * critical section, and this wait is timing critical, allowing no relaxation.
 *
 * In contrast, in `worker_thread`, a sleep operation is not required.
 * The initial `wait_event` in `worker_thread` is designed to wait for
 * the readiness of `blocker_thread`, and it is not timing critical.
 *
 * This value needs to be adjusted based on your specific platform.
 */
#define TEST_LOOP_DELAY_MS      100
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

enum execution_status {
    EXEC_STAT_NOK_WITH_TIMEOUT = 0,
    EXEC_STAT_NOK,
    EXEC_STAT_OK,
    EXEC_STAT_BUSY
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

/* Global operation context */
struct op_glob_context {
    struct kthread_context *kt_ctx;
    int cpu_count;
    volatile bool is_busy;

    wait_queue_head_t worker_waitq; /* A waitqueue for worker_thread to wait on */
    wait_queue_head_t blocker_waitq; /* A waitqueue for blocker_thread to wait on */
    wait_queue_head_t workq_waitq; /* A waitqueue for workqueue's workers to wait on */

    struct workqueue_struct *workq;
    atomic_t workq_counter; /* To keep track of active workers */

    bool is_calibrated; /* Measure the total time required to hog all CPU cores */
    int worker_timeout; /* Specify the timeout value for waiting for the blocker to enter blocking mode */
    int blocker_timeout; /* Specify the timeout value for waiting for the worker to complete its execution */
};

/* Session operation context */
struct op_session_context {
    struct op_glob_context *parent;
    struct work_struct work;
    struct mutex mutex; /* For implementation of mutex-based access control to fops */
    wait_queue_head_t async_waitq; /* For implementation of fops polling */
    bool fops_is_busy;
    char response;
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
        pr_err ("[%d] blocker_thread is running on the wrong core\n", local_cpu);
        goto exit;
    }

    local_ctx = &op_ctx->kt_ctx[local_cpu];

    pr_info ("[%d] blocker_thread is running\n", local_cpu);

    /* ?? */
    //msleep (KTHREAD_SETTLE_TIME_MS);

    do {
        /* Reset blocker parameters */
        timeout = BLOCKER_BUSYWAIT_TIMEOUT_US;

        /* Indicate to worker_thread that blocker_thread is ready for action */
        local_ctx->lifecycle = KTHREAD_STAT_READY;
        smp_wmb ();
        wake_up_all (&op_ctx->worker_waitq);

        /* Wait for start command from worker_thread */
        wait_event_cmd (op_ctx->blocker_waitq,
                        (local_ctx->lifecycle == KTHREAD_REQ_START) ||
                        kthread_should_stop (),,
                        smp_rmb ());

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
            pr_info ("[cpu-%d] The blocker_thread timed out while waiting for the worker_thread to end\n", local_cpu);
        }

        pr_info ("[cpu-%d] blocker_thread status (total = %d, timeout = %d).\n",
                local_cpu, local_ctx->exec_count, local_ctx->timeout_count);

    } while (!kthread_should_stop ());

exit:

    sched_set_mode_exit (current);

    local_ctx->lifecycle = KTHREAD_STAT_DEAD;

    /* Graciously exit */
    while (!kthread_should_stop ()) {
        yield ();
    }

    pr_info ("[cpu-%d] blocker_thread exiting.\n", local_cpu);

    return 0;
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
        pr_err ("[cpu-%d] worker_thread is running on the wrong core\n", local_cpu);
        goto exit;
    }

    local_ctx = &op_ctx->kt_ctx[local_cpu];

    pr_info ("[cpu-%d] worker_thread is running\n", local_cpu);

    do {
        /* Wait for blocker threads to become ready */
        for (i = 0; i < op_ctx->cpu_count; i++) {
            if (i == WORKER_CPU_CORE_INDEX) {
                continue;
            }

            blocker_ctx = &op_ctx->kt_ctx[i];

            wait_event_cmd (op_ctx->worker_waitq,
                            (blocker_ctx->lifecycle == KTHREAD_STAT_READY) ||
                            (blocker_ctx->lifecycle == KTHREAD_STAT_DEAD) ||
                            kthread_should_stop (),,
                            smp_rmb ());

            if (blocker_ctx->lifecycle != KTHREAD_STAT_READY ||
                kthread_should_stop ()) {
                goto exit;
            }
        }
        local_ctx->lifecycle = KTHREAD_STAT_READY;

        /* Wait for user request */
        wait_event_cmd (op_ctx->worker_waitq,
                        atomic_read (&op_ctx->workq_counter) ||
                        kthread_should_stop (),,
                        smp_rmb ());
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
        wake_up_all (&op_ctx->blocker_waitq);

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
            pr_info ("[cpu-%d] The worker_thread timed out while waiting for the blocker_thread (cpu-%d) to start\n", local_cpu, i);
        }

        if (ret != 0) {
            pr_info ("[cpu-%d] worker_thread execution has failed (total = %d, ok = %d, timeout = %d).\n",
                    local_cpu, local_ctx->exec_count, local_ctx->exec_ok_count, local_ctx->timeout_count);
        } else {
            local_ctx->exec_ok_count++;
            pr_info ("[cpu-%d] worker_thread execution has passed (total = %d, ok = %d, timeout = %d).\n",
                    local_cpu, local_ctx->exec_count, local_ctx->exec_ok_count, local_ctx->timeout_count);
        }

        /* Service all requests (work_handler) */
        local_ctx->lifecycle = KTHREAD_STAT_COMPLETED;
        wake_up_all (&op_ctx->workq_waitq);

    } while (!kthread_should_stop ());

exit:

    sched_set_mode_exit (current);

    local_ctx->lifecycle = KTHREAD_STAT_DEAD;

    /* Graciously exit */
    while (!kthread_should_stop ()) {
        yield ();
    }

    pr_info ("[%d] worker_thread exiting.\n", local_cpu);

    return 0;
}

static int spawner (struct op_glob_context *op_g_ctx)
{
    int cpu_id;
    int cpu;
    char string[256] = {0};

    for_each_online_cpu (cpu) {
        op_g_ctx->cpu_count++;
    }

    pr_info ("[cpu-%d] spawner is running\n", smp_processor_id ());

    if (op_g_ctx->cpu_count < 2) {
        pr_err ("Unable to proceed due to insufficient CPU cores\n");
        return -ENXIO;
    }

    pr_info ("[cpu-%d] total CPU count: %d\n", smp_processor_id (), op_g_ctx->cpu_count);

    op_g_ctx->kt_ctx = kzalloc (sizeof (*op_g_ctx->kt_ctx) * op_g_ctx->cpu_count, GFP_KERNEL);
    if (!op_g_ctx->kt_ctx) {
        pr_err ("kzalloc has failed\n");
        return -ENOMEM;
    }

    init_waitqueue_head (&op_g_ctx->worker_waitq);
    init_waitqueue_head (&op_g_ctx->blocker_waitq);
    init_waitqueue_head (&op_g_ctx->workq_waitq);

    op_g_ctx->workq = alloc_workqueue("tpm_dev_wq", WQ_MEM_RECLAIM, 0);
    if (!op_g_ctx->workq) {
        return -ENOMEM;
    }

    /* Create and initialize kthreads */
    for (cpu_id = 0; cpu_id < op_g_ctx->cpu_count; cpu_id++) {
        struct kthread_context *kt_ctx = &op_g_ctx->kt_ctx[cpu_id];

        kt_ctx->lifecycle = KTHREAD_STAT_NULL;

        /* We want the spawner_thread and worker_thread to operate on the same core */
        if (cpu_id == WORKER_CPU_CORE_INDEX) {
            snprintf (string, sizeof(string), "worker_thread (cpu-%d)", cpu_id);
            kt_ctx->task = kthread_create (worker_thread, (void *)op_g_ctx, string);
        } else {
            snprintf (string, sizeof (string), "blocker_thread (cpu-%d)", cpu_id);
            kt_ctx->task = kthread_create (blocker_thread, (void *)op_g_ctx, string);
        }

        if (IS_ERR_OR_NULL (kt_ctx->task)) {
            pr_err ("spawner kthread_create(worker_thread/blocker_thread) has failed, exiting spawner...\n");
            return 0;
        }

        pr_info ("[cpu-%d] Created a worker/blocker kthread and bound it to the cpu-%d, scheduling policy %d\n", smp_processor_id (), cpu_id, kt_ctx->task->policy);

        kthread_bind (kt_ctx->task, cpu_id);

        /* Set scheduling policy */
        sched_set_mode_critical (current);
    }

    /* Launch kthreads */
    for (cpu_id = op_g_ctx->cpu_count - 1; cpu_id >= 0; cpu_id--) {
        struct kthread_context *kt_ctx = &op_g_ctx->kt_ctx[cpu_id];
        wake_up_process (kt_ctx->task);
    }

    return 0;
}

static void work_handler (struct work_struct *work)
{
    struct op_session_context *op_s_ctx =
            container_of(work, struct op_session_context, work);
    struct op_glob_context *op_g_ctx = op_s_ctx->parent;

    /* Request to start operation */
    atomic_inc (&op_g_ctx->workq_counter);
    wake_up_all (&op_g_ctx->worker_waitq);

    /* Wait for the operation */
    wait_event (op_g_ctx->workq_waitq,
                op_g_ctx->kt_ctx[WORKER_CPU_CORE_INDEX].lifecycle == KTHREAD_STAT_COMPLETED);
    atomic_dec (&op_g_ctx->workq_counter);

    mutex_lock (&op_s_ctx->mutex);

    pr_info ("Workqueue: Work handler executed\n");

    op_s_ctx->response = (char)EXEC_STAT_OK;
    op_s_ctx->fops_is_busy = false;

    mutex_unlock (&op_s_ctx->mutex);
    wake_up_interruptible (&op_s_ctx->async_waitq);
}

ssize_t op_fops_read (struct op_session_context *op_s_ctx,
                      struct file *file, char *buf, size_t size, loff_t *off)
{
    struct op_glob_context *op_g_ctx = op_s_ctx->parent;
    ssize_t ret = 1;
    int rc;

    mutex_lock (&op_s_ctx->mutex);

    if (op_s_ctx->fops_is_busy) {
        goto out_busy;
    }

    op_s_ctx->fops_is_busy = true;

    /* Queue the work to workqueue */
    queue_work (op_g_ctx->workq, &op_s_ctx->work);

    if (file->f_flags & O_NONBLOCK) {
        /* O_NONBLOCK */
        pr_info ("entered O_NONBLOCK\n");
        goto out_busy;
    } else {
        /* ~O_NONBLOCK */
        pr_info ("entered ~O_NONBLOCK\n");

        mutex_unlock (&op_s_ctx->mutex);

        wait_event_interruptible (op_s_ctx->async_waitq, !op_s_ctx->fops_is_busy);

        mutex_lock (&op_s_ctx->mutex);

        rc = copy_to_user (buf, &op_s_ctx->response, ret);
        if (rc) {
            ret = -EFAULT;
        }

        mutex_unlock (&op_s_ctx->mutex);
    }

    return ret;

out_busy:
        op_s_ctx->response = (char)EXEC_STAT_BUSY;

        rc = copy_to_user (buf, &op_s_ctx->response, ret);
        if (rc) {
            ret = -EFAULT;
        }

        mutex_unlock (&op_s_ctx->mutex);
        return ret;
}

__poll_t op_fops_poll (struct op_session_context * op_s_ctx,
                       struct file *file,
                       struct poll_table_struct *poll_table)
{
    __poll_t mask = 0;

    poll_wait (file, &op_s_ctx->async_waitq, poll_table);

    mutex_lock (&op_s_ctx->mutex);

    if (!op_s_ctx->fops_is_busy) {
        mask = EPOLLIN | EPOLLRDNORM; /* Indicates that there is data available for reading from fd */
    }

    mutex_unlock (&op_s_ctx->mutex);

    return mask;
}

struct op_session_context *op_fops_open (struct op_glob_context *op_g_ctx)
{
    struct op_session_context *ctx;

    ctx = kzalloc (sizeof (*ctx), GFP_KERNEL);
    if (ctx) {
        ctx->parent = op_g_ctx;
        INIT_WORK (&ctx->work, work_handler);
        mutex_init (&ctx->mutex);
        init_waitqueue_head (&ctx->async_waitq);
    }

    return ctx;
}

int op_fops_release (struct op_session_context *op_s_ctx)
{
    mutex_destroy (&op_s_ctx->mutex);
    kfree (op_s_ctx);

    return 0;
}

int op_init (struct op_glob_context **op_g_ctx)
{
    *op_g_ctx = kzalloc (sizeof (**op_g_ctx), GFP_KERNEL);
    if (*op_g_ctx == NULL) {
        return -ENOMEM;
    }

    return spawner (*op_g_ctx);
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
    wake_up_all (&op_ctx->worker_waitq);
    wake_up_all (&op_ctx->blocker_waitq);
    wake_up_all (&op_ctx->workq_waitq);

    /* Wait for all kthreads to exit */
    for(cpu_id = 0; cpu_id < op_ctx->cpu_count; cpu_id++) {
        struct kthread_context *kt_ctx = &op_ctx->kt_ctx[cpu_id];
        wait_event (op_ctx->blocker_waitq,
                    kt_ctx->lifecycle == KTHREAD_STAT_DEAD);
    }

    flush_workqueue (op_ctx->workq);
    destroy_workqueue (op_ctx->workq);

    kfree (op_ctx->kt_ctx);
    kfree (op_ctx);
}
