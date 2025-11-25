#pragma once
#include <cmath>

/*
 Black–Scholes utilities:
 - Standard normal PDF/CDF helpers
 - European option pricing (call/put) and Greeks under BS assumptions
 Parameters use annualized r and sigma; time T in years.
*/

namespace quant {
    // Standard normal cumulative distribution function Φ(x)
    double norm_cdf(double x);
    // Standard normal probability density function φ(x)
    double norm_pdf(double x);

    // Inputs for Black–Scholes closed-form. Units: r, sigma annualized; T in years.
    struct BSInputs {
        double S;     // spot
        double K;     // strike
        double r;     // risk-free rate
        double sigma; // volatility
        double T;     // maturity (years)
    };

    // European call price under Black–Scholes (no dividends)
    double bs_call(const BSInputs& in);
    // European put price under Black–Scholes (no dividends)
    double bs_put(const BSInputs& in);

    // Greeks for call option
    double call_delta(const BSInputs& in);
    double call_gamma(const BSInputs& in);
    double call_vega(const BSInputs& in);
    double call_theta(const BSInputs& in);
    double call_rho(const BSInputs& in);

    // Greeks for put option
    double put_delta(const BSInputs& in);
    double put_theta(const BSInputs& in);
    double put_rho(const BSInputs& in);
}
