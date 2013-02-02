/*
 *
 *    Copyright (C) 2013 Tim Blechmann
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "SC_PlugIn.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

static InterfaceTable *ft;

// adapted from DiodeLadderFilter
//
// Copyright (c) 2012 Dominique Wurtz (www.blaukraut.info)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.


typedef enum {scalar, slope} ArgType;

template <int ArgType>
struct QParameter
{};

template <>
struct QParameter<scalar>
{
	QParameter(double k, double A):
		k_(k), A_(A)
	{}

	void getParameters(double & k, double & A)
	{
		k = k_;
		A = A_;
	}

	double k_, A_;
};

template <>
struct QParameter<slope>
{
	QParameter(double k, double kSlope, double A, double ASlope):
		k_(k), kSlope_(kSlope), A_(A), ASlope_(ASlope)
	{}

	void getParameters(double & k, double & A)
	{
		k = k_;
		A = A_;
		k_ += kSlope_;
		A_ += ASlope_;
	}

	double k_, A_;
	double kSlope_, ASlope_;
};

// TODO: add some oversampling
struct DiodeLadderFilter:
	public SCUnit
{
public:
	DiodeLadderFilter()
	{
		std::fill(z, z + 5, 0);
		hpCutoff = std::numeric_limits<float>::signaling_NaN();

		float newQ = in0(2);
		set_q(newQ);

		if (inRate(1) == calc_FullRate) {
			switch (inRate(2)) {
			case calc_ScalarRate:
				set_calc_function<DiodeLadderFilter, &DiodeLadderFilter::next_xi<true> >();
				break;

			case calc_BufRate:
			default:
				set_calc_function<DiodeLadderFilter, &DiodeLadderFilter::next_xk<true> >();
				break;
			}
		} else {
			switch (inRate(2)) {
			case calc_ScalarRate:
				set_calc_function<DiodeLadderFilter, &DiodeLadderFilter::next_xi<false> >();
				break;

			case calc_BufRate:
			default:
				set_calc_function<DiodeLadderFilter, &DiodeLadderFilter::next_xk<false> >();
				break;
			}
		}
	}

private:
	template <bool AudioRateFrequency, typename QParam>
	void next(int inNumSamples, QParam & qParameter)
	{
		if (AudioRateFrequency)
			next_a(inNumSamples, qParameter);
		else
			next_k(inNumSamples, qParameter);
	}

	template <bool AudioRateFrequency>
	void next_xi(int inNumSamples)
	{
		QParameter<scalar> qParam(m_k, m_A);
		next<AudioRateFrequency>(inNumSamples, qParam);
	}

	template <bool AudioRateFrequency>
	void next_xk(int inNumSamples)
	{
		float newQ = in0(2);
		newQ = sc_clip(newQ, 0, 1);

		if (newQ != q) {
			double oldA = m_A, oldk = m_k;
			double newA, newk;

			update_kA(newQ, newk, newA);
			m_A = newA;
			m_k = newk;

			double kSlope = calcSlope(newk, oldk);
			double ASlope = calcSlope(newA, oldA);

			QParameter<slope> qParam(oldk, kSlope, oldA, ASlope);
			next<AudioRateFrequency>(inNumSamples, qParam);
		} else {
			next_xi<AudioRateFrequency>(inNumSamples);
		}
	}

	template <typename QParam>
	void next_k(int inNumSamples, QParam & qParameter)
	{
		float newFreq        = in0(1);
		float newHPCutoff    = in0(3);

		if (newHPCutoff != hpCutoff)
			setFeedbackHPF(newHPCutoff * sampleDur());

		double a, a_inv, a2, b, b2, c, g, g0;
		calcFilterCoefficients(newFreq, a, a2, a_inv, b, b2, c, g, g0);

		double z0 = z[0];
		double z1 = z[1];
		double z2 = z[2];
		double z3 = z[3];
		double z4 = z[4];

		const float * inSig = zin(0);
		float * outSig = zout(0);

		for (int i = 0; i != inNumSamples; ++i) {
			double x = ZXP(inSig);
			double A, k;

			qParameter.getParameters(k, A);

			ZXP(outSig) = tick(x, a, a2, a_inv, b, b2, c, g, g0, z0, z1, z2, z3, z4, k, A);
		}
		z[0] = z0;
		z[1] = z1;
		z[2] = z2;
		z[3] = z3;
		z[4] = z4;
	}

	template <typename QParam>
	void next_a(int inNumSamples, QParam & qParameter)
	{
		float newHPCutoff    = in0(3);

		if (newHPCutoff != hpCutoff)
			setFeedbackHPF(newHPCutoff * sampleDur());

		const float * inSig = zin(0);
		const float * inFreq = zin(1);
		float * outSig = zout(0);

		double z0 = z[0];
		double z1 = z[1];
		double z2 = z[2];
		double z3 = z[3];
		double z4 = z[4];
		for (int i = 0; i != inNumSamples; ++i) {
			double freq = ZXP(inFreq);

			double a, a_inv, a2, b, b2, c, g, g0;
			calcFilterCoefficients(freq, a, a2, a_inv, b, b2, c, g, g0);

			double k, A;
			qParameter.getParameters(k, A);

			double x = ZXP(inSig);
			ZXP(outSig) = tick(x, a, a2, a_inv, b, b2, c, g, g0, z0, z1, z2, z3, z4, k, A);
		}
		z[0] = z0;
		z[1] = z1;
		z[2] = z2;
		z[3] = z3;
		z[4] = z4;
	}

	double tick(double x, double a, double a2, double ainv, double b, double b2, double c, double g, double g0,
				double & z0, double & z1, double & z2, double & z3, double & z4, double k, double A)
	{
		// current state
		const double s0 = (a2*a*z0 + a2*b*z1 + z2*(b2 - 2*a2)*a + z3*(b2 - 3*a2)*b) * c;
		const double s = bh*s0 - z4;

		// solve feedback loop (linear)
		double y5 = (g*x + s) / (1 + g*k);

		// input clipping
		const double y0 = saturate(x - k*y5);
		y5 = g*y0 + s;

		// compute integrator outputs
		const double y4 = g0*y0 + s0;
		const double y3 = (b*y4 - z3) * ainv;
		const double y2 = (b*y3 - a*y4 - z2) * ainv;
		const double y1 = (b*y2 - a*y3 - z1) * ainv;

		// update filter state
		z0 += 4*a*(y0 - y1 + y2);
		z1 += 2*a*(y1 - 2*y2 + y3);
		z2 += 2*a*(y2 - 2*y3 + y4);
		z3 += 2*a*(y3 - 2*y4);
		z4 = bh*y4 + ah*y5;

		double result = A * y4;

		return result;
	}

	void calcFilterCoefficients(const double newFreq, double & a, double & a2, double & a_inv,
								double & b, double & b2, double & c, double & g, double & g0)
	{
		double fc = sc_max(10, newFreq) * sampleDur();
		fc = sc_min(fc, 0.25);
		a = M_PI * fc; // PI is Nyquist frequency
//		a = 2 * tan(0.5*a); // dewarping, not required with 2x oversampling

		a_inv = 1/a;

		a2 = a*a;
		b = 2*a + 1;
		b2 = b*b;
		c = 1 / (2*a2*a2 - 4*a2*b2 + b2*b2);
		g0 = 2*a2*a2*c;
		g = g0 * bh;
	}

	void setFeedbackHPF(double fc)
	{
		hpCutoff = fc;
		const double K = fc * M_PI;
		ah = (K - 2) / (K + 2);
		bh = 2 / (K + 2);
	}

	void set_q(double q_)
	{
		q = sc_clip(q_, 0, 1);
		update_kA(q_, m_k, m_A);
	}

	static void update_kA(double q, double & k, double & A)
	{
		k = 20 * q;
		A = 1 + 0.5*k; // resonance gain compensation
	}

	static double saturate (double sample)
	{
		return sample / (1+ std::abs(sample));
	}

	float q, hpCutoff;
	double m_k, m_A;
	double z[5];
	double ah, bh;
};

DEFINE_XTORS(DiodeLadderFilter)

PluginLoad(NovaFilters)
{
	ft = inTable;
	DefineSimpleUnit(DiodeLadderFilter);
}