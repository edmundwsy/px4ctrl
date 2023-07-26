#!/bin/bash
sleep 3
rostopic pub /uav0/tracker/takeoff_land quadrotor_msgs/TakeoffLand "takeoff_land_cmd: 1"
rostopic pub /uav1/tracker/takeoff_land quadrotor_msgs/TakeoffLand "takeoff_land_cmd: 1"
