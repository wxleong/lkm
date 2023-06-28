#!/bin/bash

MAX_TRIAL=999999999
trial=1
fail_count=0
blocker_timeout_count=0
worker_timeout_count=0

while [ $trial -le $MAX_TRIAL ]
do
    sudo insmod chardev.ko
    sudo rmmod chardev

    dmesg=$(sudo dmesg -c)

    find_fail=$(echo $dmesg | grep "has failed")
    find_pass=$(echo $dmesg | grep "has passed")
    find_blocker_timeout=$(echo $dmesg | grep "blocker_thread timed out")
    find_worker_timeout=$(echo $dmesg | grep "worker_thread timed out")

    if [ -n "$find_blocker_timeout" ]
    then
        ((blocker_timeout_count++))
    fi

    if [ -n "$find_worker_timeout" ]
    then
        ((worker_timeout_count++))
    fi

    if [ -z "$find_fail" ] && [ -n "$find_pass" ]
    then
        echo "Trial $trial of $MAX_TRIAL PASSED ( fail count = $fail_count ) ( worker timeout count = $worker_timeout_count , blocker timeout count = $blocker_timeout_count )."
    else
        ((fail_count++))
        echo "Trial $trial of $MAX_TRIAL FAILED ( fail count = $fail_count ) ( worker timeout count = $worker_timeout_count , blocker timeout count = $blocker_timeout_count )."
    fi

    ((trial++))
done



