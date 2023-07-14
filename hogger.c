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
 * @file   hogger.c
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

/* Constants */
#define PRIORITY_DEFAULT 0
#define PRIORITY_RT      1
#define PRIORITY_RT_LOW  2
#define PRIORITY_NORMAL  3

#define CLASS_NAME    "evaluation"
#define DEVICE_NAME   "critical"
#define MINOR_NUM     0

/* Configurable */
//#define INFINITE_TEST_LOOP
//#define WORKER_EXIT_ON_TIMEOUT
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
    KTHREAD_REQ_EXIT,
    KTHREAD_STAT_DEAD,
};

enum execution_status {
    EXEC_STAT_NOK_WITH_TIMEOUT = -2,
    EXEC_STAT_NOK,
    EXEC_STAT_OK,
    EXEC_STAT_BUSY
};

typedef struct {
    struct task_struct *task;
    volatile enum kthread_lifecycle lifecycle;
    int exec_count;
} cpu_context_struct;

wait_queue_head_t g_wq;
static cpu_context_struct *g_cpu_contexts;
volatile static bool g_worker_lock;
static int g_cpu_count;
#ifdef INFINITE_TEST_LOOP
static int g_total_swi_ok_count;
static int g_total_swi_nok_count;
static int g_total_worker_timeout_count;
static int g_total_blocker_timeout_count;
#endif

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

static int blocker_thread (void *unused)
{
    int timeout;
    int local_cpu;
    unsigned long flags;
    cpu_context_struct *local_ctx;

    local_cpu = smp_processor_id ();

    if (local_cpu == WORKER_CPU_CORE_INDEX) {
        pr_err ("[%d] blocker_thread is running on the wrong core\n", local_cpu);
        goto exit;
    }

    local_ctx = &g_cpu_contexts[local_cpu];

    pr_info ("[%d] blocker_thread is running\n", local_cpu);

    msleep (KTHREAD_SETTLE_TIME_MS);

    do {
        /* Reset blocker parameters */
        timeout = BLOCKER_BUSYWAIT_TIMEOUT_US;

        /* Indicate to worker_thread that blocker_thread is ready for action */
        local_ctx->lifecycle = KTHREAD_STAT_READY;
        smp_wmb ();
        wake_up_all (&g_wq);

        /* Wait for start command from worker_thread */
        wait_event_cmd (g_wq,
                        (local_ctx->lifecycle == KTHREAD_REQ_START) ||
                        (local_ctx->lifecycle == KTHREAD_REQ_EXIT),,
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
                bool temp;

                smp_rmb ();
                temp = g_worker_lock;

                if (!temp) {
                    break;
                }

                udelay (1);
            }

            local_ctx->exec_count++;

            local_irq_restore (flags);
            preempt_enable ();
        }
        /* Critical section ends */

        if (!timeout) {
            pr_info ("[%d] The blocker_thread timed out while waiting for the worker_thread to end\n", smp_processor_id ());
#ifdef INFINITE_TEST_LOOP
            g_total_blocker_timeout_count++;
#endif
        }

#ifdef INFINITE_TEST_LOOP

        msleep(TEST_LOOP_DELAY_MS);
    } while (!kthread_should_stop ());
#else
    } while (0);
#endif


exit:

    sched_set_mode_exit (current);

    local_ctx->lifecycle = KTHREAD_STAT_DEAD;
    smp_wmb ();
    wake_up_all (&g_wq);

#ifdef INFINITE_TEST_LOOP
    while (!kthread_should_stop ()) {
        yield ();
    }
#endif

    pr_info ("[%d] blocker_thread exiting. Accumulated execution count is %d\n", smp_processor_id (), local_ctx->exec_count);

    return 0;
}

static int worker_thread (void *unused)
{
    int timeout;
    int local_cpu;
    int i;
    uint16_t ret = -1;
    unsigned long flags;
    cpu_context_struct *blocker_ctx;
    cpu_context_struct *local_ctx;

    local_cpu = smp_processor_id ();

    if (local_cpu != WORKER_CPU_CORE_INDEX) {
        pr_err ("[%d] worker_thread is running on the wrong core\n", local_cpu);
        goto exit;
    }

    local_ctx = &g_cpu_contexts[local_cpu];

    pr_info ("[%d] worker_thread is running\n", local_cpu);

    do {
        /* Wait for blocker threads to become ready */
        for (i = 0; i < g_cpu_count; i++) {
            if (i == WORKER_CPU_CORE_INDEX) {
                continue;
            }

            blocker_ctx = &g_cpu_contexts[i];

            wait_event_cmd (g_wq,
                            (blocker_ctx->lifecycle == KTHREAD_STAT_READY) ||
                            (blocker_ctx->lifecycle == KTHREAD_STAT_DEAD),,
                            smp_rmb ());

            if (blocker_ctx->lifecycle != KTHREAD_STAT_READY ||
                kthread_should_stop ()) {
                goto exit;
            }
        }

        /* Reset worker parameters */
        timeout = WORKER_BUSYWAIT_TIMEOUT_US;
        g_worker_lock = true;
        local_ctx->lifecycle = KTHREAD_STAT_READY;
        smp_wmb ();
        wake_up_all (&g_wq);
        local_ctx->exec_count++;

        /* Issue start command to blocker threads */
        for (i = 0; i < g_cpu_count; i++) {
            if (i == WORKER_CPU_CORE_INDEX) {
                continue;
            }

            blocker_ctx = &g_cpu_contexts[i];
            blocker_ctx->lifecycle = KTHREAD_REQ_START;
            smp_wmb ();
        }
        wake_up_all (&g_wq);

        /* Critical section begins */
        {
            local_irq_save (flags);
            preempt_disable ();

            /* Wait for blocker_thread to enter critical section */
            for (i = 0; i < g_cpu_count; i++) {
                if (i == WORKER_CPU_CORE_INDEX) {
                    continue;
                }

                blocker_ctx = &g_cpu_contexts[i];

                do {
                    smp_rmb ();
                    if (blocker_ctx->lifecycle == KTHREAD_STAT_RUNNING) {
                        break;
                    }

                    udelay (1);
                } while (--timeout);

                if (!timeout) {
#ifdef WORKER_EXIT_ON_TIMEOUT
                    goto exit_cri;
#else
                    break;
#endif
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

#ifdef WORKER_EXIT_ON_TIMEOUT
exit_cri:
#endif
            g_worker_lock = false;
            smp_wmb ();

            local_irq_restore (flags);
            preempt_enable ();
        }
        /* Critical section ends */

        if (!timeout) {
            pr_info ("[%d] The worker_thread timed out while waiting for the blocker_thread (%d) to start\n", local_cpu, i);

#ifdef INFINITE_TEST_LOOP
            g_total_worker_timeout_count++;
#endif
        }

        if (ret != 0) {
#ifdef INFINITE_TEST_LOOP
            g_total_swi_nok_count++;
            pr_info ("[%d] worker_thread execution has failed (ok = %d, nok = %d, worker timeout = %d, blocker timeout = %d).\n",
                    local_cpu, g_total_swi_ok_count, g_total_swi_nok_count, g_total_worker_timeout_count, g_total_blocker_timeout_count);
#else
            pr_info ("[%d] worker_thread execution has failed.\n", local_cpu);
#endif
        } else {
#ifdef INFINITE_TEST_LOOP
            g_total_swi_ok_count++;
            pr_info ("[%d] worker_thread execution has passed (ok = %d, nok = %d, worker timeout = %d, blocker timeout = %d).\n",
                    local_cpu, g_total_swi_ok_count, g_total_swi_nok_count, g_total_worker_timeout_count, g_total_blocker_timeout_count);
#else
            pr_info ("[%d] worker_thread execution has passed.\n", local_cpu);
#endif
        }

#ifdef INFINITE_TEST_LOOP
    } while (!kthread_should_stop ());
#else
    } while (0);
#endif

exit:

    sched_set_mode_exit (current);

    /* Notify blocker_thread to exit */
    for (i = 0; i < g_cpu_count; i++) {
        if (i == WORKER_CPU_CORE_INDEX) {
            continue;
        }

        blocker_ctx = &g_cpu_contexts[i];

        wait_event_cmd (g_wq,
                        blocker_ctx->lifecycle == KTHREAD_STAT_READY ||
                        blocker_ctx->lifecycle == KTHREAD_STAT_DEAD,,
                        smp_rmb ());

        if (blocker_ctx->lifecycle == KTHREAD_STAT_READY) {
            blocker_ctx->lifecycle = KTHREAD_REQ_EXIT;
            smp_wmb ();
            wake_up_all (&g_wq);
        }
    }

    local_ctx->lifecycle = KTHREAD_STAT_DEAD;
    smp_wmb ();
    wake_up_all (&g_wq);

#ifdef INFINITE_TEST_LOOP
    while (!kthread_should_stop ()) {
        yield ();
    }
#endif

    pr_info ("[%d] worker_thread exiting. Accumulated execution count is %d\n", local_cpu, local_ctx->exec_count);

    return 0;
}

static int spawner (void)
{
    int cpu_id;
    int cpu;
    char string[256] = {0};

    for_each_online_cpu (cpu) {
        g_cpu_count++;
    }

    pr_info ("[%d] spawner is running\n", smp_processor_id ());

    if (g_cpu_count < 2) {
        pr_err ("spawner cannot proceed due to insufficient CPU cores\n");
        return 0;
    }

    pr_info ("[%d] total CPU count: %d\n", smp_processor_id (), g_cpu_count);

#ifdef INFINITE_TEST_LOOP
    g_total_swi_ok_count = 0;
    g_total_swi_nok_count = 0;
    g_total_worker_timeout_count = 0;
    g_total_blocker_timeout_count = 0;
#endif

    g_cpu_contexts = kzalloc (sizeof (cpu_context_struct) * g_cpu_count, GFP_KERNEL);

    if (g_cpu_contexts == NULL) {
        pr_err ("spawner kmalloc has failed\n");
        return 0;
    }

    init_waitqueue_head (&g_wq);

    /* Create and initialize kthreads */
    for (cpu_id = 0; cpu_id < g_cpu_count; cpu_id++) {
        cpu_context_struct *ctx = &g_cpu_contexts[cpu_id];

        ctx->lifecycle = KTHREAD_STAT_NULL;

        /* We want the spawner_thread and worker_thread to operate on the same core */
        if (cpu_id == WORKER_CPU_CORE_INDEX) {
            snprintf(string, sizeof(string), "worker_thread (cpu %d)", cpu_id);
            ctx->task = kthread_create(worker_thread, NULL, string);
        } else {
            snprintf (string, sizeof (string), "blocker_thread (cpu %d)", cpu_id);
            ctx->task = kthread_create (blocker_thread, NULL, string);
        }

        if (IS_ERR_OR_NULL (ctx->task)) {
            pr_err ("spawner kthread_create(worker_thread/blocker_thread) has failed, exiting spawner...\n");
            return 0;
        }

        pr_info ("[%d] Created a worker/blocker kthread and bound it to the cpu %d, scheduling policy %d\n", smp_processor_id (), cpu_id, ctx->task->policy);

        kthread_bind (ctx->task, cpu_id);

        /* Set scheduling policy */
        sched_set_mode_critical (current);
    }

    /* Launch kthreads */
    for (cpu_id = g_cpu_count - 1; cpu_id >= 0; cpu_id--) {
        cpu_context_struct *ctx = &g_cpu_contexts[cpu_id];

        wake_up_process (ctx->task);
    }

    return 0;
}
