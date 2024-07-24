# PX4 Drone Autopilot For Tilting Drone_X4

- The PX4-Autopilot is an open source autopilot software for controlling multi-rotor unmanned aerial vehicles. It supports the Pixhawk series of flight controllers, as well as many other boards. It is developed by the PX4 team, a community of hobbyists, researchers and industry professionals.

- This repository contains PX4-Autopilot Firmware with custom SITL and controller.

### Clone repository
```
git clone https://github.com/MayurRabadiya/drone_x4_px4.git --recursive

```
### Run SITL
```
make px4_sitl gz_tilting_drone_x4

```
### Parameters
In following file you can change the Parameters.
```
drone_x4_px4/ROMFS/px4fmu_common/init.d-posix/airframes/7242_gz_tilting_drone_x4
```
```
CA_CONTROLLER  --> This parameter is to switch the controller allocation matrix between Alexis's or PX4's.
MC_PX4_CONTROL --> This parameter is to switch the controller between Alexis's or PX4's controller. Only switchable for PX4 not for Drone_X4.
```
