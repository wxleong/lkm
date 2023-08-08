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
 * @file   chardev.c
 * @date   July, 2023
 * @brief  Refer to chardev.h
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
#include <linux/dmaengine.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/poll.h>

#include "chardev.h"

/* Constants */
#define CLASS_NAME    "eval"
#define DEVICE_NAME   "op"
#define MINOR_NUM     0 /* Device major number is dynamically assigned */

struct this_context {
    struct common_context common;

    struct cdev cdev;
    struct device dev;
    dev_t major_num;

    chardev_child_context *child_ctx;
    const struct chardev_child_funcs *child_funcs;
};

struct fops_context {
    struct this_context *parent;
    chardev_child_session_context *child_ctx;
};

static struct this_context *context_cast(chardev_context *ctx)
{
    if (ctx != NULL && ctx->magic == CHARDEV_CONTEXT_MAGIC) {
        return container_of(ctx, struct this_context, common);
    }

    return NULL;
}

static int fops_open(struct inode *inode, struct file *file)
{
    int rc;
    struct this_context *this;
    struct fops_context *fops_ctx;

    this = container_of(inode->i_cdev, struct this_context, cdev);

    if (!this || !this->child_funcs)
    {
        rc = -EFAULT;
        goto out;
    }

    fops_ctx = kzalloc(sizeof(*fops_ctx), GFP_KERNEL);
    if (fops_ctx == NULL) {
        rc = -ENOMEM;
        goto out;
    }

    fops_ctx->parent = this;

    rc = this->child_funcs->open(this->child_ctx, &fops_ctx->child_ctx);
    if (rc) {
        goto out_free_fops_ctx;
    }

    file->private_data = fops_ctx;

    return 0;

out_free_fops_ctx:
    kfree(fops_ctx);
out:
    return rc;
}

static ssize_t fops_read(struct file *file, char __user *buf, size_t size, loff_t *off)
{
    struct fops_context *fops_ctx = file->private_data;
    char *child_buf;
    ssize_t rc;
    ssize_t len_to_read;
    wait_queue_head_t *wq;
    __poll_t mask;

    if (!(file->f_flags & O_NONBLOCK)) {
        /* Blocking */
        might_sleep();
        mask = fops_ctx->parent->child_funcs->poll(fops_ctx->child_ctx, &wq);
        if (___wait_event(*wq, mask != 0, TASK_INTERRUPTIBLE, 0, 0,
            schedule(); mask = fops_ctx->parent->child_funcs->poll(fops_ctx->child_ctx, &wq);)) {
            rc = -EINTR;
            goto out;
        }
    }

    rc = fops_ctx->parent->child_funcs->read1(fops_ctx->child_ctx, &child_buf);
    if (rc <= 0) {
        goto out_read_finalize;
    }

    if (*off > rc) {
        rc = -EFAULT;
        goto out_read_finalize;
    }

    len_to_read = min_t(ssize_t, rc - *off, size);
    if (!len_to_read) {
        rc = 0;
        goto out_read_finalize;
    }

    rc = copy_to_user(buf, child_buf + *off, len_to_read);
    if (rc) {
        rc = -EFAULT;
        goto out_read_finalize;
    }

    rc = fops_ctx->parent->child_funcs->read2(fops_ctx->child_ctx, len_to_read);
    if (rc <= 0) {
        goto out_set_off_zero;
    }

    *off += rc;
    goto out;

out_read_finalize:
    fops_ctx->parent->child_funcs->read2(fops_ctx->child_ctx, 0);
out_set_off_zero:
    *off = 0;
out:
    return rc;
}

static ssize_t fops_write(struct file *file, const char __user *buf, size_t size, loff_t *off)
{
    struct fops_context *fops_ctx = file->private_data;
    char *child_buf;
    ssize_t rc;

    rc = fops_ctx->parent->child_funcs->write1(fops_ctx->child_ctx, &child_buf);
    if (rc <= 0) {
        goto out_write_finalize;
    }

    if (rc < size) {
        rc = -EFBIG;
        goto out_write_finalize;
    }

    rc = copy_from_user(child_buf, buf, size);
    if (rc) {
        rc = -EFAULT;
        goto out_write_finalize;
    }

    /* Process the command */
    rc = fops_ctx->parent->child_funcs->write2(fops_ctx->child_ctx, size);

    /*
     * The received command will be processed immediately;
     * therefore, partial write operations are not supported here.
     */
    *off = 0;

out_write_finalize:
    return rc;

}

static __poll_t fops_poll(struct file *file, struct poll_table_struct *poll_table)
{
    struct fops_context *fops_ctx = file->private_data;
    wait_queue_head_t *wq;
    __poll_t mask;

    mask = fops_ctx->parent->child_funcs->poll(fops_ctx->child_ctx, &wq);

    poll_wait(file, wq, poll_table);

    return mask;
}

static int fops_release(struct inode *inode, struct file *file)
{
    struct fops_context *fops_ctx = file->private_data;

    fops_ctx->parent->child_funcs->release(fops_ctx->child_ctx);
    kfree(fops_ctx);
    file->private_data = NULL;

    return 0;
}

static const struct file_operations chardev_fops = {
    .owner = THIS_MODULE,
    .open = fops_open,
    .read = fops_read,
    .write = fops_write,
    .poll = fops_poll,
    .release = fops_release,
};

int chardev_init(chardev_context **cd_ctx, const struct attribute_group **attr_groups)
{
    int rc;
    void *priv_data = NULL; /* Device private data */
    struct class *class;
    struct this_context *this;

    pr_info("chardev_init started.\n");

    this = kzalloc(sizeof(*this), GFP_KERNEL);
    if (!this) {
        rc = -ENOMEM;
        goto out;
    }

    this->common.magic = CHARDEV_CONTEXT_MAGIC;

    /* Sanity check */
    if (context_cast(&this->common) != this) {
        rc = -EFAULT;
        goto out_free_this;
    }

    *cd_ctx = &this->common;

    /* Allocate a major number dynamically */
    rc = alloc_chrdev_region(&this->major_num, 0, 1, DEVICE_NAME);
    if (rc < 0) {
        pr_err("chardev: failed to allocate major number.\n");
        goto out_free_this;
    }

    /* Initialize a cdev structure */
    cdev_init(&this->cdev, &chardev_fops);
    this->cdev.owner = THIS_MODULE;

    /* Initialize a device structure */
    device_initialize(&this->dev);

    /* Initialize a class structure */
    class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(class)) {
        pr_err("chardev: couldn't create device class.\n");
        rc = PTR_ERR(class);
        goto out_put_device;
    }

    this->dev.class = class;
    //this->dev.class->shutdown_pre = NULL;
    //this->dev.release = NULL;
    //this->dev.parent = NULL;
    this->dev.groups = attr_groups;

    this->dev.devt = MKDEV(MAJOR(this->major_num), MINOR_NUM);

    rc = dev_set_name(&this->dev, "%s%d", DEVICE_NAME, MINOR_NUM);
    if (rc) {
        goto out_destroy_class;
    }

    dev_set_drvdata(&this->dev, priv_data);

    /* Create a char device */
    rc = cdev_device_add(&this->cdev, &this->dev);
    if (rc) {
        dev_err(&this->dev,
            "unable to cdev_device_add() %s, major %d, minor %d, err=%d\n",
            dev_name(&this->dev), MAJOR(this->dev.devt),
            MINOR(this->dev.devt), rc);
        goto out_destroy_class;
    }

    rc = 0;
    goto out;

out_destroy_class:
    class_destroy(class);
out_put_device:
    put_device(&this->dev);
    unregister_chrdev_region(this->major_num , 1);
out_free_this:
    kfree(this);
out:
    pr_info("chardev_init ended.\n");
    return rc;
}

int chardev_set_hook(chardev_context *cd_ctx,
                     chardev_child_context *child_ctx,
                     const struct chardev_child_funcs *child_funcs)
{
    struct this_context *this = context_cast(cd_ctx);

    if (!this || !child_ctx ||
        !child_funcs ||
        !child_funcs->open ||
        !child_funcs->write1 ||
        !child_funcs->write2 ||
        !child_funcs->read1 ||
        !child_funcs->read2 ||
        !child_funcs->poll ||
        !child_funcs->release) {
        return -EINVAL;
    }

    this->child_ctx = child_ctx;
    this->child_funcs = child_funcs;

    return 0;
}

void chardev_exit(chardev_context **cd_ctx)
{
    struct this_context *this = context_cast(*cd_ctx);

    pr_info("chardev_exit started.\n");

    /* Release the char device and its associated resources */
    cdev_device_del(&this->cdev, &this->dev);
    unregister_chrdev_region(this->major_num , 1);
    class_destroy(this->dev.class);

    /* Release chardev_context */
    kfree(this);
    *cd_ctx = NULL;

    pr_info("chardev_exit ended.\n");
}
