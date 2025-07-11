/****************************************************************************
 *
 *   Copyright (c) 2019 PX4 Development Team. All rights reserved.
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
 * @file AttitudeControl.cpp
 */

#include <AttitudeControl.hpp>

#include <mathlib/math/Functions.hpp>

#include <iostream>

using namespace matrix;

void AttitudeControl::setProportionalGain(const Vector3f &proportional_gain, const float yaw_weight)
{
	_proportional_gain = proportional_gain;
	_yaw_w = math::constrain(yaw_weight, 0.f, 1.f);

	// compensate for the effect of the yaw weight rescaling the output
	if (_yaw_w > 1e-4f)
	{
		_proportional_gain(2) /= _yaw_w;
	}
}

//** MayurR */
void AttitudeControl::setAtitttudeGain(const Vector3f &rotation_gain, const Vector3f &angularVal_gain, const matrix::Vector3f &integral_gain)
{
	_rotation_gain = rotation_gain;
	_angularVal_gain = angularVal_gain;
	_integral_gain = integral_gain;
}

void AttitudeControl::setInertia(const matrix::Vector3f &inertia)
{
	_inertia = inertia;
}

matrix::Vector3f AttitudeControl::update(const float dt, const Quatf &q_state, const Quatf &q_input, const Vector3f &angular_velocity)
{
	SquareMatrix3f _Ib;
	SquareMatrix3f _R_sp;
	SquareMatrix3f _R_state;

	_Ib.setZero();
	_Ib(0, 0) = _inertia(0);
	_Ib(1, 1) = _inertia(1);
	_Ib(2, 2) = _inertia(2);
	q_sp = q_input;

	_R_sp = Dcmf(Quatf(q_sp(0), q_sp(1), q_sp(2), q_sp(3)));
	_R_state = Dcmf(Quatf(q_state(0), q_state(1), q_state(2), q_state(3)));

	// ======================================================================================================================================================

	// Orientation error based on Rotation matrix.

	// _integral = -angular_velocity * dt;

	// Vector3f e_R = 0.5f * Dcmf(_R_sp.transpose()*_R_state - _R_state.transpose()*_R_sp).vee();
	// Vector3f r_R = -e_R.emult(_rotation_gain) - angular_velocity.emult(_angularVal_gain);
	// Vector3f _torque_sp = _Ib * r_R;

	// ======================================================================================================================================================

	// Orientation error based on Quaternion

	// matrix::Quatf q_err = q_sp.inversed() * q_state;  // Error quaternion (q_sp^-1 * q_state)
    // q_err.normalize(); // Ensure quaternion error is normalized
    // matrix::Vector3f e_R = 1.5f * q_err.imag(); // Scaled by 2 for quaternion-based PD control
    // matrix::Vector3f r_R = -e_R.emult(_rotation_gain) - angular_velocity.emult(_angularVal_gain);
    // matrix::Vector3f _torque_sp = _Ib * r_R;


	// ======================================================================================================================================================

	// Orientation error based on Rotation matrix.

	Vector3f e_w = -angular_velocity;
	SquareMatrix3f Psi = _R_state.transpose() * _R_sp;

	float cos_theta = (Psi.trace() - 1.0f) / 2.0f;
	cos_theta = math::constrain(cos_theta, -1.0f, 1.0f); // Ensure cos_theta is within valid range
	float theta = acosf(cos_theta);

	Vector3f e_R;
	if (theta < 1e-6f) {
	    // If theta is very small, return zero vector (no rotation)
	    e_R.zero();
	} else {
	    float factor = theta / (2.0f * sinf(theta));
	    e_R(0) = factor * (Psi(2, 1) - Psi(1, 2));
	    e_R(1) = factor * (Psi(0, 2) - Psi(2, 0));
	    e_R(2) = factor * (Psi(1, 0) - Psi(0, 1));
	}

	_integral = e_w * dt;

	Vector3f _torque_sp = _Ib * (e_R.emult(_rotation_gain) + e_w.emult(_angularVal_gain) +  _integral.emult(_integral_gain));

	return _torque_sp;
	// ======================================================================================================================================================
}
//** MayurR */

matrix::Vector3f AttitudeControl::update(const Quatf &q) const
{
	Quatf qd = _attitude_setpoint_q;

	// calculate reduced desired attitude neglecting vehicle's yaw to prioritize roll and pitch
	const Vector3f e_z = q.dcm_z();
	const Vector3f e_z_d = qd.dcm_z();
	Quatf qd_red(e_z, e_z_d);

	if (fabsf(qd_red(1)) > (1.f - 1e-5f) || fabsf(qd_red(2)) > (1.f - 1e-5f))
	{
		// In the infinitesimal corner case where the vehicle and thrust have the completely opposite direction,
		// full attitude control anyways generates no yaw input and directly takes the combination of
		// roll and pitch leading to the correct desired yaw. Ignoring this case would still be totally safe and stable.
		qd_red = qd;
	}
	else
	{
		// transform rotation from current to desired thrust vector into a world frame reduced desired attitude
		qd_red *= q;
	}

	// mix full and reduced desired attitude
	Quatf q_mix = qd_red.inversed() * qd;
	q_mix.canonicalize();
	// catch numerical problems with the domain of acosf and asinf
	q_mix(0) = math::constrain(q_mix(0), -1.f, 1.f);
	q_mix(3) = math::constrain(q_mix(3), -1.f, 1.f);
	qd = qd_red * Quatf(cosf(_yaw_w * acosf(q_mix(0))), 0, 0, sinf(_yaw_w * asinf(q_mix(3))));

	// quaternion attitude control law, qe is rotation from q to qd
	const Quatf qe = q.inversed() * qd;

	// using sin(alpha/2) scaled rotation axis as attitude error (see quaternion definition by axis angle)
	// also taking care of the antipodal unit quaternion ambiguity
	const Vector3f eq = 2.f * qe.canonical().imag();

	// calculate angular rates setpoint
	Vector3f rate_setpoint = eq.emult(_proportional_gain);

	// Feed forward the yaw setpoint rate.
	// yawspeed_setpoint is the feed forward commanded rotation around the world z-axis,
	// but we need to apply it in the body frame (because _rates_sp is expressed in the body frame).
	// Therefore we infer the world z-axis (expressed in the body frame) by taking the last column of R.transposed (== q.inversed)
	// and multiply it by the yaw setpoint rate (yawspeed_setpoint).
	// This yields a vector representing the commanded rotatation around the world z-axis expressed in the body frame
	// such that it can be added to the rates setpoint.
	if (std::isfinite(_yawspeed_setpoint))
	{
		rate_setpoint += q.inversed().dcm_z() * _yawspeed_setpoint;
	}

	// limit rates
	for (int i = 0; i < 3; i++)
	{
		rate_setpoint(i) = math::constrain(rate_setpoint(i), -_rate_limit(i), _rate_limit(i));
	}

	// std::cout << "rate_setpoint: " << rate_setpoint(0) << "  "<<rate_setpoint(1) <<"  "<<rate_setpoint(2)<<std::endl;

	return rate_setpoint;
}
