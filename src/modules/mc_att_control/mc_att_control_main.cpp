/****************************************************************************
 *
 *   Copyright (c) 2013-2018 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file mc_att_control_main.cpp
 * Multicopter attitude controller.
 *
 * @author Lorenz Meier		<lorenz@px4.io>
 * @author Anton Babushkin	<anton.babushkin@me.com>
 * @author Sander Smeets	<sander@droneslab.com>
 * @author Matthias Grob	<maetugr@gmail.com>
 * @author Beat Küng		<beat-kueng@gmx.net>
 *
 */

#include "mc_att_control.hpp"

#include <drivers/drv_hrt.h>
#include <mathlib/math/Limits.hpp>
#include <mathlib/math/Functions.hpp>

#include "AttitudeControl/AttitudeControlMath.hpp"

#include <iostream>

using namespace matrix;

MulticopterAttitudeControl::MulticopterAttitudeControl(bool vtol) :
	ModuleParams(nullptr),
	WorkItem(MODULE_NAME, px4::wq_configurations::nav_and_controllers),
	_vehicle_attitude_setpoint_pub(vtol ? ORB_ID(mc_virtual_attitude_setpoint) : ORB_ID(vehicle_attitude_setpoint)),
	_loop_perf(perf_alloc(PC_ELAPSED, MODULE_NAME": cycle")),
	_vtol(vtol)
{
	parameters_updated();
	// Rate of change 5% per second -> 1.6 seconds to ramp to default 8% MPC_MANTHR_MIN
	_manual_throttle_minimum.setSlewRate(0.05f);
	// Rate of change 50% per second -> 2 seconds to ramp to 100%
	_manual_throttle_maximum.setSlewRate(0.5f);
}

MulticopterAttitudeControl::~MulticopterAttitudeControl()
{
	perf_free(_loop_perf);
}

bool
MulticopterAttitudeControl::init()
{
	if (!_vehicle_attitude_sub.registerCallback()) {
		PX4_ERR("callback registration failed");
		return false;
	}

	//** MayurR */
	if (!_vehicle_angular_velocity_sub.registerCallback()) {
		PX4_ERR("_vehicle_angular_velocity_sub callback registration failed");
		return false;
	}
	//** MayurR */

	return true;
}

void
MulticopterAttitudeControl::parameters_updated()
{
	// Store some of the parameters in a more convenient way & precompute often-used values
	_attitude_control.setProportionalGain(Vector3f(_param_mc_roll_p.get(), _param_mc_pitch_p.get(), _param_mc_yaw_p.get()), _param_mc_yaw_weight.get());

	//** MayurR */
	_attitude_control.setAtitttudeGain(Vector3f(_param_mc_krx_gain.get(), _param_mc_kry_gain.get(), _param_mc_krz_gain.get()),
					   Vector3f(_param_mc_kax_gain.get(), _param_mc_kay_gain.get(), _param_mc_kaz_gain.get()),
					   Vector3f(_param_mc_kix_gain.get(), _param_mc_kiy_gain.get(), _param_mc_kiz_gain.get()));

	_attitude_control.setInertia(Vector3f(_param_mc_inertia_xx.get(), _param_mc_inertia_yy.get(), _param_mc_inertia_zz.get()));

	_drone_x4 = _param_ca_controller.get();
	_px4_control = _param_mc_px4_controller.get();
	//** MayurR */

	// angular rate limits
	using math::radians;
	_attitude_control.setRateLimit(Vector3f(radians(_param_mc_rollrate_max.get()), radians(_param_mc_pitchrate_max.get()),
						radians(_param_mc_yawrate_max.get())));

	_man_tilt_max = math::radians(_param_mpc_man_tilt_max.get());
}

float
MulticopterAttitudeControl::throttle_curve(float throttle_stick_input)
{
	float thrust = 0.f;

	switch (_param_mpc_thr_curve.get()) {
	case 1: // no rescaling to hover throttle
		thrust = math::interpolate(throttle_stick_input, -1.f, 1.f,
					   _manual_throttle_minimum.getState(), _param_mpc_thr_max.get());
		break;

	default: // 0 or other: rescale such that a centered throttle stick corresponds to hover throttle
		thrust = math::interpolateNXY(throttle_stick_input, {-1.f, 0.f, 1.f},
		{_manual_throttle_minimum.getState(), _param_mpc_thr_hover.get(), _param_mpc_thr_max.get()});
		break;
	}

	return math::min(thrust, _manual_throttle_maximum.getState());
}

void
MulticopterAttitudeControl::generate_attitude_setpoint(const Quatf &q, float dt, bool reset_yaw_sp)
{
	vehicle_attitude_setpoint_s attitude_setpoint{};
	const float yaw = Eulerf(q).psi();

	attitude_setpoint.yaw_sp_move_rate = _manual_control_setpoint.yaw * math::radians(_param_mpc_man_y_max.get());

	// Avoid accumulating absolute yaw error with arming stick gesture in case heading_good_for_control stays true
	if ((_manual_control_setpoint.throttle < -.9f) && (_param_mc_airmode.get() != 2)) {
		reset_yaw_sp = true;
	}

	// Make sure not absolute heading error builds up
	if (reset_yaw_sp) {
		_man_yaw_sp = yaw;

	} else {
		_man_yaw_sp = wrap_pi(_man_yaw_sp + attitude_setpoint.yaw_sp_move_rate * dt);
	}

	/*
	 * Input mapping for roll & pitch setpoints
	 * ----------------------------------------
	 * We control the following 2 angles:
	 * - tilt angle, given by sqrt(roll*roll + pitch*pitch)
	 * - the direction of the maximum tilt in the XY-plane, which also defines the direction of the motion
	 *
	 * This allows a simple limitation of the tilt angle, the vehicle flies towards the direction that the stick
	 * points to, and changes of the stick input are linear.
	 */
	_man_roll_input_filter.setParameters(dt, _param_mc_man_tilt_tau.get());
	_man_pitch_input_filter.setParameters(dt, _param_mc_man_tilt_tau.get());

	// we want to fly towards the direction of (roll, pitch)
	Vector2f v = Vector2f(_man_roll_input_filter.update(_manual_control_setpoint.roll * _man_tilt_max),
			      -_man_pitch_input_filter.update(_manual_control_setpoint.pitch * _man_tilt_max));
	float v_norm = v.norm(); // the norm of v defines the tilt angle

	if (v_norm > _man_tilt_max) { // limit to the configured maximum tilt angle
		v *= _man_tilt_max / v_norm;
	}

	Quatf q_sp_rp = AxisAnglef(v(0), v(1), 0.f);
	// The axis angle can change the yaw as well (noticeable at higher tilt angles).
	// This is the formula by how much the yaw changes:
	//   let a := tilt angle, b := atan(y/x) (direction of maximum tilt)
	//   yaw = atan(-2 * sin(b) * cos(b) * sin^2(a/2) / (1 - 2 * cos^2(b) * sin^2(a/2))).
	const Quatf q_sp_yaw(cosf(_man_yaw_sp / 2.f), 0.f, 0.f, sinf(_man_yaw_sp / 2.f));

	if (_vtol) {
		// Modify the setpoints for roll and pitch such that they reflect the user's intention even
		// if a large yaw error(yaw_sp - yaw) is present. In the presence of a yaw error constructing
		// an attitude setpoint from the yaw setpoint will lead to unexpected attitude behaviour from
		// the user's view as the tilt will not be aligned with the heading of the vehicle.

		AttitudeControlMath::correctTiltSetpointForYawError(q_sp_rp, q, q_sp_yaw);
	}

	// Align the desired tilt with the yaw setpoint
	Quatf q_sp = q_sp_yaw * q_sp_rp;

	q_sp.copyTo(attitude_setpoint.q_d);

	// Transform to euler angles for logging only
	const Eulerf euler_sp(q_sp);
	attitude_setpoint.roll_body = euler_sp(0);
	attitude_setpoint.pitch_body = euler_sp(1);
	attitude_setpoint.yaw_body = euler_sp(2);

	attitude_setpoint.thrust_body[2] = -throttle_curve(_manual_control_setpoint.throttle);
	attitude_setpoint.timestamp = hrt_absolute_time();
	_vehicle_attitude_setpoint_pub.publish(attitude_setpoint);
}

void
MulticopterAttitudeControl::Run()
{
	if (should_exit()) {
		_vehicle_attitude_sub.unregisterCallback();
		//** MayurR */
		_vehicle_angular_velocity_sub.unregisterCallback();
		//** MayurR */
		exit_and_cleanup();
		return;
	}

	perf_begin(_loop_perf);

	// Check if parameters have changed
	if (_parameter_update_sub.updated()) {
		// clear update
		parameter_update_s param_update;
		_parameter_update_sub.copy(&param_update);

		updateParams();
		parameters_updated();
	}

	// run controller on attitude updates
	vehicle_attitude_s v_att;

	if (_vehicle_attitude_sub.update(&v_att))
	 {
		// Guard against too small (< 0.2ms) and too large (> 20ms) dt's.
		const float dt = math::constrain(((v_att.timestamp_sample - _last_run) * 1e-6f), 0.0002f, 0.02f);
		_last_run = v_att.timestamp_sample;

		const Quatf q{v_att.q};

		/* check for updates in other topics */
		_manual_control_setpoint_sub.update(&_manual_control_setpoint);
		_vehicle_control_mode_sub.update(&_vehicle_control_mode);

		if (_vehicle_status_sub.updated()) {
			vehicle_status_s vehicle_status;

			if (_vehicle_status_sub.copy(&vehicle_status)) {
				_vehicle_type_rotary_wing = (vehicle_status.vehicle_type == vehicle_status_s::VEHICLE_TYPE_ROTARY_WING);
				_vtol = vehicle_status.is_vtol;
				_vtol_in_transition_mode = vehicle_status.in_transition_mode;
				_vtol_tailsitter = vehicle_status.is_vtol_tailsitter;

				const bool armed = (vehicle_status.arming_state == vehicle_status_s::ARMING_STATE_ARMED);
				_spooled_up = armed && hrt_elapsed_time(&vehicle_status.armed_time) > _param_com_spoolup_time.get() * 1_s;
			}
		}

		if (_vehicle_land_detected_sub.updated()) {
			vehicle_land_detected_s vehicle_land_detected;

			if (_vehicle_land_detected_sub.copy(&vehicle_land_detected)) {
				_landed = vehicle_land_detected.landed;
			}
		}

		if (_vehicle_local_position_sub.updated()) {
			vehicle_local_position_s vehicle_local_position;

			if (_vehicle_local_position_sub.copy(&vehicle_local_position)) {
				_heading_good_for_control = vehicle_local_position.heading_good_for_control;
			}
		}

		bool attitude_setpoint_generated = false;

		const bool is_hovering = (_vehicle_type_rotary_wing && !_vtol_in_transition_mode);

		// vehicle is a tailsitter in transition mode
		const bool is_tailsitter_transition = (_vtol_tailsitter && _vtol_in_transition_mode);

		const bool run_att_ctrl = _vehicle_control_mode.flag_control_attitude_enabled && (is_hovering
					  || is_tailsitter_transition);

		if (run_att_ctrl) {

			// Generate the attitude setpoint from stick inputs if we are in Manual/Stabilized mode
			if (_vehicle_control_mode.flag_control_manual_enabled &&
			    !_vehicle_control_mode.flag_control_altitude_enabled &&
			    !_vehicle_control_mode.flag_control_velocity_enabled &&
			    !_vehicle_control_mode.flag_control_position_enabled) {

				generate_attitude_setpoint(q, dt, _reset_yaw_sp);
				attitude_setpoint_generated = true;

			} else {
				_man_roll_input_filter.reset(0.f);
				_man_pitch_input_filter.reset(0.f);
			}

			// Check for new attitude setpoint
			if (_vehicle_attitude_setpoint_sub.updated()) {
				vehicle_attitude_setpoint_s vehicle_attitude_setpoint;

				if (_vehicle_attitude_setpoint_sub.copy(&vehicle_attitude_setpoint)
				    && (vehicle_attitude_setpoint.timestamp > _last_attitude_setpoint)) {

					_attitude_control.setAttitudeSetpoint(Quatf(vehicle_attitude_setpoint.q_d), vehicle_attitude_setpoint.yaw_sp_move_rate);
					_thrust_setpoint_body = Vector3f(vehicle_attitude_setpoint.thrust_body);
					_last_attitude_setpoint = vehicle_attitude_setpoint.timestamp;
				}
			}

			// Check for a heading reset
			if (_quat_reset_counter != v_att.quat_reset_counter) {
				const Quatf delta_q_reset(v_att.delta_q_reset);

				// for stabilized attitude generation only extract the heading change from the delta quaternion
				_man_yaw_sp = wrap_pi(_man_yaw_sp + Eulerf(delta_q_reset).psi());

				if (v_att.timestamp > _last_attitude_setpoint) {
					// adapt existing attitude setpoint unless it was generated after the current attitude estimate
					_attitude_control.adaptAttitudeSetpoint(delta_q_reset);
				}

				_quat_reset_counter = v_att.quat_reset_counter;
			}

			//** MayurR */
			Vector3f rates_sp;
			if (_px4_control == 1){_drone_x4 = 1;}
			if (_drone_x4 == 1)
			{
	 			vehicle_angular_velocity_s angular_velocity;
				tilting_drone_x4_attitude_setpoint_s x4_attitude_setpoint;
				tilting_drone_x4_gains_s x4_gains;

				if(_tilting_drone_x4_gains_sub.update(&x4_gains))
				{
					_attitude_control.setAtitttudeGain(Vector3f(x4_gains.rot_gain[0], x4_gains.rot_gain[1], x4_gains.rot_gain[2]),
					   								   Vector3f(x4_gains.angular_gain[0], x4_gains.angular_gain[1], x4_gains.angular_gain[2]),
					   								   Vector3f(x4_gains.rot_integral_gain[0], x4_gains.rot_integral_gain[1], x4_gains.rot_integral_gain[2]));
				}

				if(_vehicle_angular_velocity_sub.update(&angular_velocity) && _tilting_drone_x4_attitude_setpoint_sub.update(&x4_attitude_setpoint))
				{
					Vector3f _angular_velocity =  Vector3f(angular_velocity.xyz);
					Quatf q_input = Quatf(x4_attitude_setpoint.q[0], x4_attitude_setpoint.q[1], x4_attitude_setpoint.q[2], x4_attitude_setpoint.q[3]);
					rates_sp = _attitude_control.update(dt, q, q_input, _angular_velocity);

					vehicle_torque_setpoint_s vehicle_torque_setpoint{};
					vehicle_torque_setpoint.timestamp_sample = angular_velocity.timestamp_sample;
					vehicle_torque_setpoint.timestamp = hrt_absolute_time();

					vehicle_torque_setpoint.xyz[0] = rates_sp(0);
					vehicle_torque_setpoint.xyz[1] = rates_sp(1);
					vehicle_torque_setpoint.xyz[2] = rates_sp(2);

					_vehicle_torque_setpoint_pub.publish(vehicle_torque_setpoint);
				}
			}
			else
			{
				rates_sp = _attitude_control.update(q);
			}
			//** MayurR */

			const hrt_abstime now = hrt_absolute_time();
			autotune_attitude_control_status_s pid_autotune;

			if (_autotune_attitude_control_status_sub.copy(&pid_autotune)) {
				if ((pid_autotune.state == autotune_attitude_control_status_s::STATE_ROLL
				     || pid_autotune.state == autotune_attitude_control_status_s::STATE_PITCH
				     || pid_autotune.state == autotune_attitude_control_status_s::STATE_YAW
				     || pid_autotune.state == autotune_attitude_control_status_s::STATE_TEST)
				    && ((now - pid_autotune.timestamp) < 1_s)) {
					rates_sp += Vector3f(pid_autotune.rate_sp);
				}
			}

			// publish rate setpoint
			vehicle_rates_setpoint_s rates_setpoint{};
			rates_setpoint.roll = rates_sp(0);
			rates_setpoint.pitch = rates_sp(1);
			rates_setpoint.yaw = rates_sp(2);
			_thrust_setpoint_body.copyTo(rates_setpoint.thrust_body);
			rates_setpoint.timestamp = hrt_absolute_time();

			_vehicle_rates_setpoint_pub.publish(rates_setpoint);
		}

		if (_landed) {
			_manual_throttle_minimum.update(0.f, dt);

		} else {
			_manual_throttle_minimum.update(_param_mpc_manthr_min.get(), dt);
		}

		if (_spooled_up) {
			_manual_throttle_maximum.update(1.f, dt);

		} else {
			_manual_throttle_maximum.setForcedValue(0.f);
		}

		// reset yaw setpoint during transitions, tailsitter.cpp generates
		// attitude setpoint for the transition
		_reset_yaw_sp = !attitude_setpoint_generated || !_heading_good_for_control || (_vtol && _vtol_in_transition_mode);
	}

	perf_end(_loop_perf);
}

int MulticopterAttitudeControl::task_spawn(int argc, char *argv[])
{
	bool vtol = false;

	if (argc > 1) {
		if (strcmp(argv[1], "vtol") == 0) {
			vtol = true;
		}
	}

	MulticopterAttitudeControl *instance = new MulticopterAttitudeControl(vtol);

	if (instance) {
		_object.store(instance);
		_task_id = task_id_is_work_queue;

		if (instance->init()) {
			return PX4_OK;
		}

	} else {
		PX4_ERR("alloc failed");
	}

	delete instance;
	_object.store(nullptr);
	_task_id = -1;

	return PX4_ERROR;
}

int MulticopterAttitudeControl::custom_command(int argc, char *argv[])
{
	return print_usage("unknown command");
}

int MulticopterAttitudeControl::print_usage(const char *reason)
{
	if (reason) {
		PX4_WARN("%s\n", reason);
	}

	PRINT_MODULE_DESCRIPTION(
		R"DESCR_STR(
### Description
This implements the multicopter attitude controller. It takes attitude
setpoints (`vehicle_attitude_setpoint`) as inputs and outputs a rate setpoint.

The controller has a P loop for angular error

Publication documenting the implemented Quaternion Attitude Control:
Nonlinear Quadrocopter Attitude Control (2013)
by Dario Brescianini, Markus Hehn and Raffaello D'Andrea
Institute for Dynamic Systems and Control (IDSC), ETH Zurich

https://www.research-collection.ethz.ch/bitstream/handle/20.500.11850/154099/eth-7387-01.pdf

)DESCR_STR");

	PRINT_MODULE_USAGE_NAME("mc_att_control", "controller");
	PRINT_MODULE_USAGE_COMMAND("start");
	PRINT_MODULE_USAGE_ARG("vtol", "VTOL mode", true);
	PRINT_MODULE_USAGE_DEFAULT_COMMANDS();

	return 0;
}


/**
 * Multicopter attitude control app start / stop handling function
 */
extern "C" __EXPORT int mc_att_control_main(int argc, char *argv[])
{
	return MulticopterAttitudeControl::main(argc, argv);
}
