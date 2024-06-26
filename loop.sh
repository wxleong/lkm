#!/bin/bash

MAX_TRIAL=999999999
trial=1
fail_count=0

sudo rmmod mod
sudo insmod mod.ko

while [ $trial -le $MAX_TRIAL ]
do
    sudo ./test

    dmesg=$(sudo dmesg -c)

    find_fail=$(echo $dmesg | grep "Timed out while waiting")
    find_pass=$(echo $dmesg | grep "No timeout occurred")

    if [ -z "$find_fail" ] && [ -n "$find_pass" ]
    then
        echo "Trial $trial of $MAX_TRIAL PASSED ( fail count = $fail_count )."
    else
        ((fail_count++))
        echo "Trial $trial of $MAX_TRIAL FAILED ( fail count = $fail_count )."
    fi

    ((trial++))
done



