#!/bin/bash
cd ~/raspilot
export PYTHONPATH="$PWD"
pkill -f controlsd
pkill -f pandad
pkill -f boardd
pkill -f ubloxd
pkill -f upload_files.py
taskset -a --cpu-list 0,1,2,3 python ~/raspilot/selfdrive/controls/controlsd.py & 
taskset -a --cpu-list 0,1,2,3 python ~/raspilot/selfdrive/pandad.py &
#taskset -a --cpu-list 0,1 ~/raspilot/selfdrive/boardd/boardd &
taskset -a --cpu-list 2,3,0,1 ~/raspilot/selfdrive/locationd/ubloxd &
pkill -f dashboard
taskset -a --cpu-list 2,3,0,1 python ~/raspilot/dashboard.py &
sleep 10
bash ~/raspilot/fix_niceness.sh