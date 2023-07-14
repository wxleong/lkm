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
 * @brief  A humble character device
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
#define CLASS_NAME    "evaluation"
#define DEVICE_NAME   "critical"
#define MINOR_NUM     0 /* Device major number is dynamically assigned */

/* kthread context */
/*struct kthread_context {
    struct task_struct *task;
    volatile enum kthread_lifecycle lifecycle;
    size_t exec_count;
};*/

/* Global context */
struct global_context {
    struct cdev cdev;
    struct device dev;
    dev_t major_num;

    //struct kthread_context *kt_worker;
    //struct kthread_context **kt_blockers;
    
};

/* Session Context */
struct session_context {
    struct global_context *ctx;


};

static struct global_context *g_ctx;

static int fops_open (struct inode *inode, struct file *file)
{
    struct global_context *ctx;
    struct session_context *priv;

    ctx = container_of(inode->i_cdev, struct global_context, cdev);

    priv = kzalloc (sizeof (*priv), GFP_KERNEL);
    if (priv == NULL) {
        goto out;
    }

    priv->ctx = ctx;
    file->private_data = priv;

    return 0;

out:
    return -ENOMEM;
}

static ssize_t fops_read (struct file *, char __user *, size_t, loff_t *)
{
    return 0;
}

static ssize_t fops_write (struct file *, const char __user *, size_t, loff_t *)
{
    return 0;
}

static __poll_t fops_poll (struct file *, struct poll_table_struct *)
{
    __poll_t mask = 0;

    /* mask = EPOLLIN | EPOLLRDNORM; */ /* Indicates fd is ready for reading */
    /* mask = EPOLLOUT | EPOLLWRNORM; */ /* Indicates fd is ready for writing */

    return mask;
}

static int fops_release (struct inode *inode, struct file *file)
{
    struct session_context *priv = file->private_data;

    kfree(priv);

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

static int __init chardev_init (void)
{
    int rc;
    void *priv_data = NULL; /* Device private data */
    struct class *class;

    pr_info ("[%d] chardev_init is running\n", smp_processor_id ());

    g_ctx = kzalloc (sizeof (*g_ctx), GFP_KERNEL);
    if (g_ctx == NULL) {
        rc = -ENOMEM;
        goto out;
    }

    /* Allocate a major number dynamically */
    rc = alloc_chrdev_region (&g_ctx->major_num, 0, 1, DEVICE_NAME);
    if (rc < 0) {
        pr_err("chardev: failed to allocate major number\n");
        goto out;
    }

    /* Initialize a cdev structure */
    cdev_init (&g_ctx->cdev, &chardev_fops);
    g_ctx->cdev.owner = THIS_MODULE;

    /* Initialize a device structure */
    device_initialize (&g_ctx->dev);

    /* Initialize a class structure */
    class = class_create (THIS_MODULE, CLASS_NAME);
    if (IS_ERR(class)) {
        pr_err("chardev: couldn't create device class\n");
        rc = PTR_ERR(class);
        goto out_put_device;
    }

    g_ctx->dev.class = class;
    //g_ctx->dev.class->shutdown_pre = NULL;
    //g_ctx->dev.release = NULL;
    //g_ctx->dev.parent = NULL;
    //g_ctx->dev.groups = NULL; /* sysfs attribute_group */

    g_ctx->dev.devt = MKDEV(MAJOR(g_ctx->major_num), MINOR_NUM);

    rc = dev_set_name(&g_ctx->dev, "%s%d", DEVICE_NAME, MINOR_NUM);
    if (rc) {
        goto out_destroy_class;
    }

    dev_set_drvdata (&g_ctx->dev, priv_data);

    /* Create a char device */
    rc = cdev_device_add (&g_ctx->cdev, &g_ctx->dev);
    if (rc) {
        dev_err (&g_ctx->dev,
            "unable to cdev_device_add() %s, major %d, minor %d, err=%d\n",
            dev_name (&g_ctx->dev), MAJOR (g_ctx->dev.devt),
            MINOR (g_ctx->dev.devt), rc);
        goto out_destroy_class;
    }

    pr_info ("[%d] chardev_init exiting\n", smp_processor_id ());
    return 0;

out_destroy_class:
    class_destroy (class);
out_put_device:
    put_device (&g_ctx->dev);
    unregister_chrdev_region (g_ctx->major_num , 1);
out:
    return rc;
}

static void __exit chardev_exit (void)
{
    pr_info ("[%d] chardev_exit is running\n", smp_processor_id ());

    cdev_device_del (&g_ctx->cdev, &g_ctx->dev);
    unregister_chrdev_region (g_ctx->major_num , 1);
    class_destroy (g_ctx->dev.class);

    pr_info ("[%d] chardev_exit exiting\n", smp_processor_id());
}

module_init(chardev_init);
module_exit(chardev_exit);

MODULE_DESCRIPTION("A Humble Character Device Module");
MODULE_LICENSE("GPL");
MODULE_VERSION("202306");
