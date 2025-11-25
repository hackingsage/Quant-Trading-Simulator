#include "quant/bs.hpp"

namespace quant {
    // -----------------------------------------
    // Normal PDF and CDF
    // -----------------------------------------

    double norm_pdf(double x) {
        static const double INV_SQRT_2PI = 0.3989422804014327;
        return INV_SQRT_2PI * std::exp(-0.5 * x * x);
    }

    double norm_cdf(double x) {
        return 0.5 * (1.0 + std::erf(x / std::sqrt(2.0)));
    }

    // -----------------------------------------
    // Black-Scholes d1, d2
    // -----------------------------------------

    static double d1(const BSInputs& in) {
        return (std::log(in.S / in.K) + (in.r + 0.5 * in.sigma * in.sigma) * in.T)
                / (in.sigma * std::sqrt(in.T));
    }

    static double d2(const BSInputs& in) {
        return d1(in) - in.sigma * std::sqrt(in.T);
    }

    // -----------------------------------------
    // Prices
    // -----------------------------------------

    double bs_call(const BSInputs& in) {
        double D1 = d1(in);
        double D2 = D2 = D1 - in.sigma * std::sqrt(in.T);
        return in.S * norm_cdf(D1) - in.K * std::exp(-in.r * in.T) * norm_cdf(D2);
    }

    double bs_put(const BSInputs& in) {
        double D1 = d1(in);
        double D2 = D1 - in.sigma * std::sqrt(in.T);
        return in.K * std::exp(-in.r * in.T) * norm_cdf(-D2) - in.S * norm_cdf(-D1);
    }

    // -----------------------------------------
    // Call Greeks
    // -----------------------------------------

    double call_delta(const BSInputs& in) {
        return norm_cdf(d1(in));
    }

    double call_gamma(const BSInputs& in) {
        return norm_pdf(d1(in)) / (in.S * in.sigma * std::sqrt(in.T));
    }

    double call_vega(const BSInputs& in) {
        return in.S * norm_pdf(d1(in)) * std::sqrt(in.T);
    }

    double call_theta(const BSInputs& in) {
        double D1 = d1(in);
        double D2 = D1 - in.sigma * std::sqrt(in.T);

        double term1 = - (in.S * norm_pdf(D1) * in.sigma) / (2.0 * std::sqrt(in.T));
        double term2 = in.r * in.K * std::exp(-in.r * in.T) * norm_cdf(D2);
        return term1 - term2;
    }

    double call_rho(const BSInputs& in) {
        return in.K * in.T * std::exp(-in.r * in.T) * norm_cdf(d2(in));
    }

    // -----------------------------------------
    // Put Greeks
    // -----------------------------------------

    double put_delta(const BSInputs& in) {
        return norm_cdf(d1(in)) - 1.0;
    }

    double put_theta(const BSInputs& in) {
        double D1 = d1(in);
        double D2 = D1 - in.sigma * std::sqrt(in.T);

        double term1 = - (in.S * norm_pdf(D1) * in.sigma) / (2.0 * std::sqrt(in.T));
        double term2 = in.r * in.K * std::exp(-in.r * in.T) * norm_cdf(-D2);
        return term1 + term2;
    }

    double put_rho(const BSInputs& in) {
        return -in.K * in.T * std::exp(-in.r * in.T) * norm_cdf(-d2(in));
    }

}