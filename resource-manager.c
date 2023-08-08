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
 * @file   resource-manager.c
 * @date   July, 2023
 * @brief  Refer to resource-manager.h
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
#include <linux/poll.h>
#include <linux/uaccess.h>
#include "resource-manager.h"

enum session_life_stages {
    SESSION_STAGE_STARTING = 0,
    SESSION_STAGE_RTW, /* Ready to write */
    SESSION_STAGE_BUSY,
    SESSION_STAGE_RTR, /* Ready to read */
    SESSION_STAGE_ENDED,
};

struct this_context {
    struct common_context common;

    struct kthread_context *kt_ctx;
    struct workqueue_struct *workq;
    struct mutex mutex;
    int session_max;
    int session_count;

    struct device_attribute session_max_attr;
    struct device_attribute session_count_attr;
    struct attribute *attrs[3]; /* Based on number of device_attribute + 1 for NULL termination */
    struct attribute_group attr_group;

    resource_manager_child_context *child_ctx;
    const struct resource_manager_child_funcs *child_funcs;
};

struct session_context {
    struct common_context common;

    struct this_context *parent;
    struct mutex mutex;
    struct work_struct witem_open;
    struct work_struct witem_execute;
    struct work_struct witem_release;
    wait_queue_head_t waitq; /* For polling implementation use only. */
    enum session_life_stages life_stage;
    int rc;

    resource_manager_child_session_context *child_ctx;
};

static struct this_context *resource_manager_context_cast(resource_manager_context *ctx)
{
    if (ctx != NULL && ctx->magic == RESOURCE_MANAGER_CONTEXT_MAGIC) {
        return container_of(ctx, struct this_context, common);
    }

    return NULL;
}

static struct session_context *resource_manager_session_context_cast(resource_manager_session_context *ctx)
{
    if (ctx != NULL && ctx->magic == RESOURCE_MANAGER_SESSION_CONTEXT_MAGIC) {
        return container_of(ctx, struct session_context, common);
    }

    return NULL;
}

static void witem_open_handler(struct work_struct *work)
{
    bool is_orderly = true;
    struct session_context *session_ctx =
        container_of(work, struct session_context, witem_open);
    struct this_context *this = session_ctx->parent;

    mutex_lock(&session_ctx->mutex);

    session_ctx->rc = this->child_funcs->open(this->child_ctx, &session_ctx->child_ctx, &is_orderly);
    if (session_ctx->life_stage != SESSION_STAGE_ENDED) {
        session_ctx->life_stage = SESSION_STAGE_RTW;
    }

    mutex_unlock(&session_ctx->mutex);
}

static void witem_execute_handler(struct work_struct *work)
{
    bool is_orderly = true;
    struct session_context *session_ctx =
        container_of(work, struct session_context, witem_execute);
    struct this_context *this = session_ctx->parent;

    mutex_lock(&session_ctx->mutex);

    session_ctx->rc = this->child_funcs->execute(session_ctx->child_ctx, &is_orderly);
    if (session_ctx->life_stage != SESSION_STAGE_ENDED) {
        session_ctx->life_stage = SESSION_STAGE_RTR;
    }
    wake_up_interruptible(&session_ctx->waitq);

    mutex_unlock(&session_ctx->mutex);
}

static void witem_release_handler(struct work_struct *work)
{
    bool is_orderly = true;
    struct session_context *session_ctx =
        container_of(work, struct session_context, witem_release);
    struct this_context *this = session_ctx->parent;

    mutex_lock(&session_ctx->mutex);
    session_ctx->rc = this->child_funcs->release(session_ctx->child_ctx, &is_orderly);
    mutex_unlock(&session_ctx->mutex);
}

static int resource_manager_session_open(chardev_child_context *child_ctx,
                                         chardev_child_session_context **child_s_ctx)
{
    int rc;
    bool is_orderly = false;
    struct this_context *this = resource_manager_context_cast(child_ctx);
    struct session_context *session_ctx;

    mutex_lock(&this->mutex);

    /* If session_max is set to zero, means no limit on the number of sessions. */
    if (this->session_count >= this->session_max && this->session_max) {
        rc = -EAGAIN;
        goto out_mutex_unlock;
    } else {
        this->session_count++;
    }

    session_ctx = kzalloc(sizeof(*session_ctx), GFP_KERNEL);
    if (!session_ctx) {
        rc = -ENOMEM;
        goto out_decrement_session_count;
    }
    session_ctx->common.magic = RESOURCE_MANAGER_SESSION_CONTEXT_MAGIC;
    session_ctx->life_stage = SESSION_STAGE_STARTING;

    /* Sanity check */
    if (resource_manager_session_context_cast(&session_ctx->common) != session_ctx) {
        rc = -EFAULT;
        goto out_free_session_ctx;
    }

    rc = this->child_funcs->open(this->child_ctx, &session_ctx->child_ctx, &is_orderly);
    if (rc) {
        goto out_free_session_ctx;
    }

    INIT_WORK(&session_ctx->witem_open, witem_open_handler);
    INIT_WORK(&session_ctx->witem_execute, witem_execute_handler);
    INIT_WORK(&session_ctx->witem_release, witem_release_handler);
    mutex_init(&session_ctx->mutex);
    init_waitqueue_head(&session_ctx->waitq);
    session_ctx->parent = this;

    /* Check whether orderly execution has been requested. */
    if (is_orderly) {
        queue_work(this->workq, &session_ctx->witem_open);
        flush_work(&session_ctx->witem_open);
        if ((rc = session_ctx->rc)) {
            session_ctx->rc = 0;
            goto out_mutex_destroy;
        }
    } else {
        session_ctx->life_stage = SESSION_STAGE_RTW;
    }

    *child_s_ctx = &session_ctx->common;

    goto out_mutex_unlock;

out_mutex_destroy:
    mutex_destroy(&session_ctx->mutex);
out_free_session_ctx:
    kfree(session_ctx);
out_decrement_session_count:
    this->session_count--;
out_mutex_unlock:
    mutex_unlock(&this->mutex);
    return rc;
}

static int resource_manager_session_release(chardev_child_session_context *child_s_ctx)
{
    int rc;
    bool is_orderly = false;
    struct session_context *session_ctx = resource_manager_session_context_cast(child_s_ctx);
    struct this_context *this = session_ctx->parent;

    mutex_lock(&this->mutex);

    mutex_lock(&session_ctx->mutex);
    if (session_ctx->life_stage == SESSION_STAGE_ENDED) {
        mutex_unlock(&session_ctx->mutex);
        rc = -EBUSY;
        goto out;
    } else {
        session_ctx->life_stage = SESSION_STAGE_ENDED;
    }
    mutex_unlock(&session_ctx->mutex);

    flush_work(&session_ctx->witem_open);
    flush_work(&session_ctx->witem_execute);

    rc = this->child_funcs->release(session_ctx->child_ctx, &is_orderly);
    if (rc) {
        goto out;
    }

    /* Check whether orderly execution has been requested. */
    if (is_orderly) {
        queue_work(this->workq, &session_ctx->witem_release);
        flush_work(&session_ctx->witem_release);
        if ((rc = session_ctx->rc)) {
            session_ctx->rc = 0;
            goto out;
        }
    }

    mutex_destroy(&session_ctx->mutex);
    kfree(session_ctx);

    this->session_count--;
out:
    mutex_unlock(&this->mutex);
    return rc;
}

static __poll_t resource_manager_session_poll(chardev_child_session_context *child_s_ctx,
                                              wait_queue_head_t **waitq)
{
    __poll_t mask;
    struct session_context *session_ctx = resource_manager_session_context_cast(child_s_ctx);

    mutex_lock(&session_ctx->mutex);

    *waitq = &session_ctx->waitq;

    if (session_ctx->life_stage == SESSION_STAGE_RTR) {
        mask = EPOLLIN | EPOLLRDNORM; /* Indicates that there is data available for reading from fd */
    } else if (session_ctx->life_stage == SESSION_STAGE_ENDED) {
        mask = EPOLLHUP;
    } else {
        mask = 0; /* Continue polling */
    }

    mutex_unlock(&session_ctx->mutex);

    return mask;
}

static ssize_t resource_manager_session_write1(chardev_child_session_context *child_s_ctx, char **buf)
{
    int rc;
    struct session_context *session_ctx = resource_manager_session_context_cast(child_s_ctx);
    struct this_context *this = session_ctx->parent;

    mutex_lock(&session_ctx->mutex);

    switch (session_ctx->life_stage) {
        case SESSION_STAGE_STARTING:
        case SESSION_STAGE_BUSY:
        case SESSION_STAGE_RTR:
            rc = -EBUSY;
            goto out;
        case SESSION_STAGE_ENDED:
            rc = -EBADF;
            goto out;
        default:
            break;
    }

    rc = this->child_funcs->write1(session_ctx->child_ctx, buf);

out:
    return rc;
}

static ssize_t resource_manager_session_write2(chardev_child_session_context *child_s_ctx, size_t len)
{
    int rc;
    bool is_orderly = false;
    struct session_context *session_ctx = resource_manager_session_context_cast(child_s_ctx);
    struct this_context *this = session_ctx->parent;

    if (!len) {
        rc = 0;
        goto out;
    }

    rc = this->child_funcs->write2(session_ctx->child_ctx, len);
    if (rc <= 0) {
        goto out;
    }

    session_ctx->rc = this->child_funcs->execute(session_ctx->child_ctx, &is_orderly);

    /* Check whether orderly execution has been requested. */
    if (is_orderly) {
        session_ctx->life_stage = SESSION_STAGE_BUSY;
        queue_work(this->workq, &session_ctx->witem_execute);
    } else {
        session_ctx->life_stage = SESSION_STAGE_RTR;
    }

out:
    mutex_unlock(&session_ctx->mutex);
    return rc;
}

static ssize_t resource_manager_session_read1(chardev_child_session_context *child_s_ctx, char **buf)
{
    int rc;
    struct session_context *session_ctx = resource_manager_session_context_cast(child_s_ctx);
    struct this_context *this = session_ctx->parent;

    mutex_lock(&session_ctx->mutex);

    switch (session_ctx->life_stage) {
        case SESSION_STAGE_STARTING:
        case SESSION_STAGE_RTW:
        case SESSION_STAGE_BUSY:
            rc = -EBUSY;
            goto out;
        case SESSION_STAGE_ENDED:
            rc = -EBADF;
            goto out;
        default:
            break;
    }

    rc = this->child_funcs->read1(session_ctx->child_ctx, buf);

    /* Prioritize the child's error */
    if (rc >= 0 && session_ctx->rc) {
        rc = session_ctx->rc;
    }

    /* Error has been considered */
    session_ctx->rc = 0;

out:
    return rc;
}

static ssize_t resource_manager_session_read2(chardev_child_session_context *child_s_ctx, size_t len)
{
    struct session_context *session_ctx = resource_manager_session_context_cast(child_s_ctx);
    struct this_context *this = session_ctx->parent;
    ssize_t rc;

    rc = this->child_funcs->read2(session_ctx->child_ctx, len);
    if (!rc) {
        session_ctx->life_stage = SESSION_STAGE_RTW;
    }

    mutex_unlock(&session_ctx->mutex);
    return rc;
}

static ssize_t session_max_show(struct device *dev,
                                struct device_attribute *attr, char *buf)
{
    struct this_context *this =
        container_of(attr, struct this_context, session_max_attr);

    return sprintf(buf, "%d\n", this->session_max);
}

static ssize_t session_max_store(struct device *dev, struct device_attribute *attr,
                                 const char *buf, size_t count)
{
    struct this_context *this =
        container_of(attr, struct this_context, session_max_attr);

    int temp = 0;
    sscanf(buf, "%d", &temp);

    mutex_lock(&this->mutex);
    this->session_max = (temp < 0) ? 0 : temp;
    mutex_unlock(&this->mutex);
    return count;
}

static ssize_t session_count_show(struct device *dev,
                                  struct device_attribute *attr, char *buf)
{
    struct this_context *this =
        container_of(attr, struct this_context, session_count_attr);
    int i;

    mutex_lock(&this->mutex);
    i = this->session_count;
    mutex_unlock(&this->mutex);

    return sprintf(buf, "%d\n", i);
}

const struct attribute_group *resource_manager_get_attribute_group(resource_manager_context *rt_ctx)
{
    struct this_context *this = resource_manager_context_cast(rt_ctx);
    return &this->attr_group;
}

int resource_manager_init(resource_manager_context **rm_ctx)
{
    int rc = 0;
    struct this_context *this;

    this = kzalloc(sizeof(*this), GFP_KERNEL);
    if (!this) {
        return -ENOMEM;
    }
    this->common.magic = RESOURCE_MANAGER_CONTEXT_MAGIC;

    /* Sanity check */
    if (resource_manager_context_cast(&this->common) != this) {
        rc = -EFAULT;
        goto out_free_this;
    }

    mutex_init(&this->mutex);

    this->session_count_attr.attr.name = "session_count";
    this->session_count_attr.attr.mode = 0444;
    this->session_count_attr.show = session_count_show;
    this->session_max_attr.attr.name = "session_max";
    this->session_max_attr.attr.mode = 0644;
    this->session_max_attr.show = session_max_show;
    this->session_max_attr.store = session_max_store;
    this->attrs[0] = &this->session_count_attr.attr;
    this->attrs[1] = &this->session_max_attr.attr;
    this->attrs[2] = NULL;
    this->attr_group.name = "resource-manager"; /* Optional */
    this->attr_group.attrs = this->attrs;

    /* Create a single thread workqueue for strict ordering. */
    this->workq = alloc_workqueue("tpm_dev_wq", WQ_MEM_RECLAIM | WQ_UNBOUND, 1);
    if (!this->workq) {
        rc = -ENOMEM;
        goto out_destroy_mutex;
    }

    *rm_ctx = &this->common;

    goto out;

out_destroy_mutex:
    mutex_destroy(&this->mutex);
out_free_this:
    kfree(this);
out:
    return rc;
}

void resource_manager_exit(resource_manager_context **rm_ctx)
{
    struct this_context *this = resource_manager_context_cast(*rm_ctx);

    destroy_workqueue(this->workq);
    mutex_destroy(&this->mutex);
    kfree(this);
}

int resource_manager_set_hook(resource_manager_context *rm_ctx,
                              resource_manager_child_context *child_ctx,
                              const struct resource_manager_child_funcs *child_funcs)
{
    struct this_context *this = resource_manager_context_cast(rm_ctx);

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

static const struct chardev_child_funcs cd_child_funcs = {
    .open = resource_manager_session_open,
    .read1 = resource_manager_session_read1,
    .read2 = resource_manager_session_read2,
    .write1 = resource_manager_session_write1,
    .write2 = resource_manager_session_write2,
    .poll = resource_manager_session_poll,
    .release = resource_manager_session_release,
};

const struct chardev_child_funcs *resource_manager_get_hook_chardev(void)
{
    return &cd_child_funcs;
}

