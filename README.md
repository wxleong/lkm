# A Linux Kernel Module Reference

The kernel module is designed to be non-monolithic with reusability in mind, comprising multiple layers:
- lkm: The primary entry point for the kernel module.
- chardev: This layer facilitates character-type device functionality.
- resource-manager: This layer enables concurrent access and orderly execution of shared hardware resources. The term "orderly" here refers to an operation being processed sequentially, with no overlapping allowed.
- realtime: This layer supports the execution of critical or time-sensitive operations that cannot be interrupted or preempted while also minimizing bus arbitration by hogging CPU cores.

Build:
```
$ make CONFIG_CHARDEV_LKM=m
```

Insert the kernel module:
```
$ sudo insmod mod.ko
```

Check if the kernel module has successfully initialized by verifying the presence of `/dev/op0`.
```
$ sudo ls /dev | grep op0
```

Run the sample userland application:
```
$ sudo ./test
```

Configure the kernel module using sysfs attribute groups:
```
$ ls -l /sys/class/eval/op0/realtime/
total 0
-rw-r--r-- 1 root root 4096 Sep  6 07:40 blocker_timeout
-rw-r--r-- 1 root root 4096 Sep  6 07:40 cpu_allowed
-r--r--r-- 1 root root 4096 Sep  6 07:40 cpu_max
-rw-r--r-- 1 root root 4096 Sep  6 07:40 worker_timeout

$ ls -l /sys/class/eval/op0/resource-manager/
total 0
-r--r--r-- 1 root root 4096 Sep  6 07:41 session_count
-rw-r--r-- 1 root root 4096 Sep  6 07:41 session_max
```

To read from an attribute:
```
$ cat /sys/class/eval/op0/realtime/cpu_max
```

To configure an attribute:
```
$ sudo sh -c "echo 1 > /sys/class/eval/op0/realtime/cpu_allowed"
```

Attribute Table:

| Group | Attribute | Permission | Description |
| ----- | --------- | ---------- | ----------- |
| realtime | blocker_timeout | RW | The timeout value for a worker waiting for the blockers to enter the blocking mode.  |
| realtime | worker_timeout | RW | The timeout value for the blockers waiting for the worker to complete its execution.  |
| realtime | cpu_max | RO | Number of physical CPU core.  |
| realtime | cpu_allowed | RW | `<0` results in non-real-time execution.<br>`0` allows execution to occur in a kthread running with a real-time scheduling policy (e.g., SCHED_FIFO).<br>`1`, in addition to `0`, disables local interrupts but without hogging other CPU cores.<br>`>1`, in addition to `0`, hog other CPUs (and disables local interrupts) up to a maximum of `cpu_max-1` cores. |
| resource-manager | session_max | RW | Maximum allowed sessions open simultaneously. |
| resource-manager | session_count | RO | Number of active sessions. |
> A worker is a single kthread where critical or time-sensitive operations are executed, whereas blockers are multiple kthreads used to hog CPU cores, preventing them from executing other tasks and wait for the worker to complete its execution.

> 'local interrupts' refers to interrupts that are associated with a specific CPU core.

# KUnit Setup

The objective is to run KUnit in a CI/CD environment, and Docker is used for this purpose. Here are the steps:

First, launch a container:
```
$ docker run --name kunit -it -v /dev/shm --tmpfs /dev/shm:rw,exec,size=1G --hostname kunit --env LANG=C.UTF-8 ubuntu:22.04
```

From this point onwards, execute all commands within the container.

Install the required dependencies:
```
$ apt update
$ apt install -y git python3 gcc make flex bison bc pkg-config libncurses5-dev
```

Download the Linux kernel source code:
```
$ cd ~
$ git clone https://github.com/torvalds/linux --single-branch -b v6.0
```

Run the default KUnit tests:
```
$ cd ~/linux
$ ./tools/testing/kunit/kunit.py run
```

Integrate your project into the Linux kernel source tree:
```
$ cd ~/linux/drivers/char
$ git clone https://github.com/wxleong/lkm
```

Apply the patch to the kernel source code. This will modify the Makefile and Kconfig to include your project in the tree:
```
$ cd ~/linux
$ git apply ~/linux/drivers/char/lkm/patch/0001-Integrate-LKM-to-Linux-v6.0-for-Kunit-testing.patch
```

Run the KUnit tests for your project:
```
$ cd ~/linux
$ ./tools/testing/kunit/kunit.py run --kconfig_add CONFIG_CHARDEV_LKM=y --kconfig_add CONFIG_CHARDEV_LKM_TEST=y
```

These steps set up a Docker container, install the necessary dependencies, download the Linux kernel source code, integrate your project, apply the required patch, and run KUnit tests for your project within the Linux kernel source tree.
