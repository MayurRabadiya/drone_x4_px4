/****************************************************************************
 *
 *   Copyright (c) 2018 - 2019 PX4 Development Team. All rights reserved.
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
 * @file PositionControl.cpp
 */

#include "PositionControl.hpp"
#include "ControlMath.hpp"
#include <float.h>
#include <mathlib/mathlib.h>
#include <px4_platform_common/defines.h>
#include <geo/geo.h>

#include <iostream>

using namespace matrix;

const trajectory_setpoint_s PositionControl::empty_trajectory_setpoint = {0, {NAN, NAN, NAN}, {NAN, NAN, NAN}, {NAN, NAN, NAN}, {NAN, NAN, NAN}, NAN, NAN};

void PositionControl::setVelocityGains(const Vector3f &P, const Vector3f &I, const Vector3f &D)
{
	_gain_vel_p = P;
	_gain_vel_i = I;
	_gain_vel_d = D;
}

void PositionControl::setVelocityLimits(const float vel_horizontal, const float vel_up, const float vel_down)
{
	_lim_vel_horizontal = vel_horizontal;
	_lim_vel_up = vel_up;
	_lim_vel_down = vel_down;
}

void PositionControl::setThrustLimits(const float min, const float max)
{
	// make sure there's always enough thrust vector length to infer the attitude
	_lim_thr_min = math::max(min, 10e-4f);
	_lim_thr_max = max;
}

void PositionControl::setHorizontalThrustMargin(const float margin)
{
	_lim_thr_xy_margin = margin;
}

void PositionControl::updateHoverThrust(const float hover_thrust_new)
{
	// Given that the equation for thrust is T = a_sp * Th / g - Th
	// with a_sp = desired acceleration, Th = hover thrust and g = gravity constant,
	// we want to find the acceleration that needs to be added to the integrator in order obtain
	// the same thrust after replacing the current hover thrust by the new one.
	// T' = T => a_sp' * Th' / g - Th' = a_sp * Th / g - Th
	// so a_sp' = (a_sp - g) * Th / Th' + g
	// we can then add a_sp' - a_sp to the current integrator to absorb the effect of changing Th by Th'
	const float previous_hover_thrust = _hover_thrust;
	setHoverThrust(hover_thrust_new);

	_vel_int(2) += (_acc_sp(2) - CONSTANTS_ONE_G) * previous_hover_thrust / _hover_thrust + CONSTANTS_ONE_G - _acc_sp(2);
}

void PositionControl::setState(const PositionControlStates &states)
{
	_pos = states.position;
	_vel = states.velocity;
	_yaw = states.yaw;
	_vel_dot = states.acceleration;
}

void PositionControl::setInputSetpoint(const trajectory_setpoint_s &setpoint)
{
	_pos_sp = Vector3f(setpoint.position);
	_vel_sp = Vector3f(setpoint.velocity);
	_acc_sp = Vector3f(setpoint.acceleration);
	_yaw_sp = setpoint.yaw;
	_yawspeed_sp = setpoint.yawspeed;
}

bool PositionControl::update(const float dt, bool drone_x4) //** MayurR //
{
	bool valid = _inputValid();

	if (valid)
	{

		//** MayurR //
		if (!drone_x4)
		{
			_positionControl();
			_velocityControl(dt);
		}
		else
		{
			_positionControlMR(dt);
		}
		// MayurR **//

		_yawspeed_sp = PX4_ISFINITE(_yawspeed_sp) ? _yawspeed_sp : 0.f;
		_yaw_sp = PX4_ISFINITE(_yaw_sp) ? _yaw_sp : _yaw; // TODO: better way to disable yaw control
	}

	// There has to be a valid output acceleration and thrust setpoint otherwise something went wrong
	return valid && _acc_sp.isAllFinite() && _thr_sp.isAllFinite();
}

//** MayurR //
void PositionControl::setPositionControlParam(const matrix::Vector3f &P, const matrix::Vector3f &D, const matrix::Vector3f &I, const float &mass)
{
	_position_gain = P;
	_velocity_gain = D;
	_integral_gain = I;
	_mass = mass;
}

void PositionControl::_positionControlMR(const float dt)
{
//**=============================================================================================================================================================================== */

	vehicle_attitude_s att;
	_attitude_sub.update(&att);

	Quatf q_state(att.q[0], att.q[1], att.q[2], att.q[3]);
	Dcmf Rb(Quatf(q_state(0), q_state(1), q_state(2), q_state(3)));

	ControlMath::setZeroIfNanVector3f(_pos_sp);
	ControlMath::setZeroIfNanVector3f(_vel_sp);

	Vector3f e_p = _pos - _pos_sp;
	Vector3f e_v = _vel - _vel_sp;

	ControlMath::setZeroIfNanVector3f(e_p);
	ControlMath::setZeroIfNanVector3f(_acc_sp);

	Vector3f r_p = e_p.emult(_position_gain) + e_v.emult(_velocity_gain) + _mass * Vector3f(0.0f, 0.0f, CONSTANTS_ONE_G);
	_thr_sp = Rb.transpose() * r_p ;

//**=============================================================================================================================================================================== */


	// Quatf uav_base_q(0.0f, 1.0f, 0.0f, 0.0f);
	// Quatf ned_enu_q(0.0f, 0.707107f, 0.707107f, 0.0f);
	// Quatf q_sp_(q_state(0), q_state(1), q_state(2), q_state(3));
	// Quatf q = (ned_enu_q * q_sp_) * uav_base_q;

	Eulerf euler(q_state);

	float roll = euler.phi() * 57.2958f;
	float pitch = euler.theta() * 57.2958f;
	float yaw = euler.psi() * 57.2958f;

	std::cout << std::endl;
	// std::cout << "_thrust_sp_1: " <<   _thr_sp(0) << "  " <<    _thr_sp(1) << "  " <<   _thr_sp(2) << std::endl;
	// std::cout << "     _pos_sp: " <<   _pos_sp(0) << "  " <<    _pos_sp(1) << "  " <<   _pos_sp(2) << std::endl;
	std::cout << "        _pos: " <<      _pos(0) << "  " <<       _pos(1) << "  " <<      _pos(2) << std::endl;
	std::cout << "       Eular: " <<         roll << "  " << pitch << "  " <<  yaw << std::endl;
	// // std::cout << "         e_p: " <<       e_p(0) << "  " <<       e_p(1) << "  " <<       e_p(2) << std::endl;
	// // std::cout << "         r_p: " <<       r_p(0) << "  " <<       r_p(1) << "  " <<       r_p(2) << std::endl;

	std::cout << std::endl;

}
// MayurR **//

void PositionControl::_positionControl()
{
	// P-position controller
	Vector3f vel_sp_position = (_pos_sp - _pos).emult(_gain_pos_p);

	// std::cout << "        _pos: " <<_pos(0) << "  " <<_pos(1) << "  " <<_pos(2) << std::endl;
	// std::cout << "     _pos_sp: " <<_pos_sp(0) << "  " <<_pos_sp(1) << "  " <<_pos_sp(2) << std::endl;
	// std::cout << "vel_sp_position: " <<_gain_pos_p(0) << "  " <<_gain_pos_p(1) << "  " <<_gain_pos_p(2) << std::endl;
	// std::cout << std::endl;


	// Position and feed-forward velocity setpoints or position states being NAN results in them not having an influence
	ControlMath::addIfNotNanVector3f(_vel_sp, vel_sp_position);

	// make sure there are no NAN elements for further reference while constraining
	ControlMath::setZeroIfNanVector3f(vel_sp_position);

	// Constrain horizontal velocity by prioritizing the velocity component along the
	// the desired position setpoint over the feed-forward term.
	_vel_sp.xy() = ControlMath::constrainXY(vel_sp_position.xy(), (_vel_sp - vel_sp_position).xy(), _lim_vel_horizontal);
	// Constrain velocity in z-direction.
	_vel_sp(2) = math::constrain(_vel_sp(2), -_lim_vel_up, _lim_vel_down);
}

void PositionControl::_velocityControl(const float dt)
{
	// Constrain vertical velocity integral
	_vel_int(2) = math::constrain(_vel_int(2), -CONSTANTS_ONE_G, CONSTANTS_ONE_G);

	// PID velocity control
	Vector3f vel_error = _vel_sp - _vel;
	Vector3f acc_sp_velocity = vel_error.emult(_gain_vel_p) + _vel_int - _vel_dot.emult(_gain_vel_d);

	// No control input from setpoints or corresponding states which are NAN
	ControlMath::addIfNotNanVector3f(_acc_sp, acc_sp_velocity);

	_accelerationControl();

	// Integrator anti-windup in vertical direction
	if ((_thr_sp(2) >= -_lim_thr_min && vel_error(2) >= 0.f) ||
		(_thr_sp(2) <= -_lim_thr_max && vel_error(2) <= 0.f))
	{
		vel_error(2) = 0.f;
	}

	// Prioritize vertical control while keeping a horizontal margin
	const Vector2f thrust_sp_xy(_thr_sp);
	const float thrust_sp_xy_norm = thrust_sp_xy.norm();
	const float thrust_max_squared = math::sq(_lim_thr_max);

	// Determine how much vertical thrust is left keeping horizontal margin
	const float allocated_horizontal_thrust = math::min(thrust_sp_xy_norm, _lim_thr_xy_margin);
	const float thrust_z_max_squared = thrust_max_squared - math::sq(allocated_horizontal_thrust);

	// Saturate maximal vertical thrust
	_thr_sp(2) = math::max(_thr_sp(2), -sqrtf(thrust_z_max_squared));

	// Determine how much horizontal thrust is left after prioritizing vertical control
	const float thrust_max_xy_squared = thrust_max_squared - math::sq(_thr_sp(2));
	float thrust_max_xy = 0.f;

	if (thrust_max_xy_squared > 0.f)
	{
		thrust_max_xy = sqrtf(thrust_max_xy_squared);
	}

	// Saturate thrust in horizontal direction
	if (thrust_sp_xy_norm > thrust_max_xy)
	{
		_thr_sp.xy() = thrust_sp_xy / thrust_sp_xy_norm * thrust_max_xy;
	}

	// Use tracking Anti-Windup for horizontal direction: during saturation, the integrator is used to unsaturate the output
	// see Anti-Reset Windup for PID controllers, L.Rundqwist, 1990
	const Vector2f acc_sp_xy_produced = Vector2f(_thr_sp) * (CONSTANTS_ONE_G / _hover_thrust);
	const float arw_gain = 2.f / _gain_vel_p(0);

	// The produced acceleration can be greater or smaller than the desired acceleration due to the saturations and the actual vertical thrust (computed independently).
	// The ARW loop needs to run if the signal is saturated only.
	const Vector2f acc_sp_xy = _acc_sp.xy();
	const Vector2f acc_limited_xy = (acc_sp_xy.norm_squared() > acc_sp_xy_produced.norm_squared())
										? acc_sp_xy_produced
										: acc_sp_xy;
	vel_error.xy() = Vector2f(vel_error) - arw_gain * (acc_sp_xy - acc_limited_xy);

	// Make sure integral doesn't get NAN
	ControlMath::setZeroIfNanVector3f(vel_error);
	// Update integral part of velocity control
	_vel_int += vel_error.emult(_gain_vel_i) * dt;
}

void PositionControl::_accelerationControl()
{
	// Assume standard acceleration due to gravity in vertical direction for attitude generation
	float z_specific_force = -CONSTANTS_ONE_G;

	if (!_decouple_horizontal_and_vertical_acceleration)
	{
		// Include vertical acceleration setpoint for better horizontal acceleration tracking
		z_specific_force += _acc_sp(2);
	}

	Vector3f body_z = Vector3f(-_acc_sp(0), -_acc_sp(1), -z_specific_force).normalized();
	// std::cout << "body_z: " << body_z(0) << "  " << body_z(1) << "  " << body_z(2) << std::endl;

	ControlMath::limitTilt(body_z, Vector3f(0, 0, 1), _lim_tilt);
	// Convert to thrust assuming hover thrust produces standard gravity
	const float thrust_ned_z = _acc_sp(2) * (_hover_thrust / CONSTANTS_ONE_G) - _hover_thrust;

	// std::cout << "thrust_ned_z: " << thrust_ned_z << std::endl;
	// Project thrust to planned body attitude
	const float cos_ned_body = (Vector3f(0, 0, 1).dot(body_z));
	const float collective_thrust = math::min(thrust_ned_z / cos_ned_body, -_lim_thr_min);

	// std::cout << "collective_thrust: "<< collective_thrust  << std::endl;
	_thr_sp = body_z * collective_thrust;
	// std::cout << "_thr_sp: " << _thr_sp(0) << "  " << _thr_sp(1) << "  " << _thr_sp(2) << std::endl;
}

bool PositionControl::_inputValid()
{
	bool valid = true;

	// Every axis x, y, z needs to have some setpoint
	for (int i = 0; i <= 2; i++)
	{
		valid = valid && (PX4_ISFINITE(_pos_sp(i)) || PX4_ISFINITE(_vel_sp(i)) || PX4_ISFINITE(_acc_sp(i)));
	}

	// x and y input setpoints always have to come in pairs
	valid = valid && (PX4_ISFINITE(_pos_sp(0)) == PX4_ISFINITE(_pos_sp(1)));
	valid = valid && (PX4_ISFINITE(_vel_sp(0)) == PX4_ISFINITE(_vel_sp(1)));
	valid = valid && (PX4_ISFINITE(_acc_sp(0)) == PX4_ISFINITE(_acc_sp(1)));

	// For each controlled state the estimate has to be valid
	for (int i = 0; i <= 2; i++)
	{
		if (PX4_ISFINITE(_pos_sp(i)))
		{
			valid = valid && PX4_ISFINITE(_pos(i));
		}

		if (PX4_ISFINITE(_vel_sp(i)))
		{
			valid = valid && PX4_ISFINITE(_vel(i)) && PX4_ISFINITE(_vel_dot(i));
		}
	}

	return valid;
}

void PositionControl::getLocalPositionSetpoint(vehicle_local_position_setpoint_s &local_position_setpoint) const
{
	local_position_setpoint.x = _pos_sp(0);
	local_position_setpoint.y = _pos_sp(1);
	local_position_setpoint.z = _pos_sp(2);
	local_position_setpoint.yaw = _yaw_sp;
	local_position_setpoint.yawspeed = _yawspeed_sp;
	local_position_setpoint.vx = _vel_sp(0);
	local_position_setpoint.vy = _vel_sp(1);
	local_position_setpoint.vz = _vel_sp(2);
	_acc_sp.copyTo(local_position_setpoint.acceleration);
	_thr_sp.copyTo(local_position_setpoint.thrust);
}

void PositionControl::getAttitudeSetpoint(vehicle_attitude_setpoint_s &attitude_setpoint) const
{
	ControlMath::thrustToAttitude(_thr_sp, _yaw_sp, attitude_setpoint);
	attitude_setpoint.yaw_sp_move_rate = _yawspeed_sp;
}





	// Matrix<float, 8U, 6U> _mix;

	// float L = 0.183f; // rotor arm length
	// float p = sqrt(2.0f)/2;
	// float kf = 8.54858e-06f; // force constant
	// float kt = 0.016f;
	// float H = kf*kt;

	// float values[8][6] = {
    //     { 1.0f / (4 * p),       -1.0f / (4 * p),       0.0f,         0.0f,                  0.0f,                   L / (4 * (L*L + H*H))},
    //     { H / (4 * L * p),     -H / (4 * L * p),     1.0f / 4,    -1.0f / (4 * L * p),    1.0f / (4 * L * p),     H / (4 * (L*L + H*H))},
    //     { 1.0f / (4 * p),        1.0f / (4 * p),       0.0f,         0.0f,                  0.0f,                   L / (4 * (L*L + H*H))},
    //     {-H / (4 * L * p),    -H / (4 * L * p),     1.0f / 4,    -1.0f / (4 * L * p),   -1.0f / (4 * L * p),    -H / (4 * (L*L + H*H))},
    //     {-1.0f / (4 * p),       1.0f / (4 * p),       0.0f,         0.0f,                  0.0f,                   L / (4 * (L*L + H*H))},
    //     {-H / (4 * L * p),     H / (4 * L * p),     1.0f / 4,     1.0f / (4 * L * p),   -1.0f / (4 * L * p),     H / (4 * (L*L + H*H))},
    //     {-1.0f / (4 * p),      -1.0f / (4 * p),       0.0f,         0.0f,                  0.0f,                   L / (4 * (L*L + H*H))},
    //     { H / (4 * L * p),      H / (4 * L * p),     1.0f / 4,     1.0f / (4 * L * p),    1.0f / (4 * L * p),    -H / (4 * (L*L + H*H))}};

	// for (int i = 0; i < 8; ++i)
	// {
	// 	for (int j = 0; j < 6; ++j)
	// 	{
	// 		_mix(i, j) = values[i][j];
	// 	}
	// }

	// Vector<float, 8U> act;
	// Vector<float,6U> u;
	// u(0) = t(0);
	// u(1) = t(1);
	// u(2) = t(2);
	// u(3) = -4.20465e-05f;
	// u(4) = -0.00259095f;
	// u(5) = 6.7724e-05f;

	// act = _mix * u;

	// matrix::Vector<float, 4> actuator_sp;
	// matrix::Vector<float, 4> servo_sp;


	// for (int j = 0; j < 4; j++)
	// {
	// 	actuator_sp(j) = (sqrtf(powf(act(2 * j), 2) + powf(act(2 * j + 1), 2)));
	// 	servo_sp(j) = atan2f(act(2 * j), act(2 * j +1));
	// }
