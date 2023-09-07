# A Linux Kernel Module Reference

The kernel module is designed to be non-monolithic with reusability in mind, comprising multiple layers:
- lkm: The primary entry point for the kernel module.
- chardev: This layer facilitates character-type device functionality.
- resource-manager: This layer enables concurrent access and orderly execution of shared hardware resources. The term "orderly" here refers to an operation being processed sequentially, with no overlapping allowed.
- realtime: This layer supports the execution of critical or time-sensitive operations that cannot be interrupted or preempted while also minimizing bus arbitration by hogging CPU cores.

Build:
```
$ make
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
| realtime | cpu_allowed | RW | `0` results in non-real-time execution; `1` ensures real-time execution without hogging other CPU cores; `>1` enables real-time execution while hogging other CPUs up to a maximum of `cpu_max-1` cores.  |
| resource-manager | session_max | RW | Maximum allowed sessions open simultaneously. |
| resource-manager | session_count | RO | Number of active sessions. |
> A worker is a single kthread where critical or time-sensitive operations are executed, whereas blockers are multiple kthreads used to hog CPU cores, preventing them from executing other tasks and wait for the worker to complete its execution.