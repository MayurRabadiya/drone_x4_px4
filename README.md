# PX4-Autopilot For tilting_drone_X4

- The PX4-Autopilot is an open source autopilot software for controlling multi-rotor unmanned aerial vehicles. It supports the Pixhawk series of flight controllers, as well as many other boards. It is developed by the PX4 team, a community of hobbyists, researchers and industry professionals.
- This repository contains PX4-Autopilot Firmware with custom SITL and controller.

### Clone repository
```
git clone https://github.com/MayurRabadiya/drone_x4_px4.git --recursive
```

### Controller Location
    drone_x4_px4
    ├── src
        └── modules
            ├── mc_pos_control
            │    └── PositionControl
            │         └── PositionControl.cpp
            └── mc_att_control
                └── AttitudeControl
                        └── AttitudeControl.cpp
                        
### Allocation Matrix and Control allocator
    drone_x4_px4
    ├── src
        └── modules
            ├── ActuatorEffectiveness
            │    └── ActuatorEffectivenessRotors.cpp
            │         
            └── control_allocator
                └── ControlAllocator.cpp
                        └── AttitudeControl.cpp
### Run SITL
```
make px4_sitl gz_tilting_drone_x4
```
### Parameters
In following file you can change the gain Parameters.
```
drone_x4_px4/ROMFS/px4fmu_common/init.d-posix/airframes/7242_gz_tilting_drone_x4
```
