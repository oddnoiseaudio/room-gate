#pragma once
// synthetic speech used by the stand-alone tests
#include <cmath>
#include <random>
#include <vector>
static const double FS = 48000.0;

// simple TDF2 biquad bandpass
struct BP
{
    double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set (double f, double q)
    {
        const double w = 2 * M_PI * f / FS, al = std::sin (w) / (2 * q), a0 = 1 + al;
        b0 = al / a0; b2 = -al / a0; a1 = -2 * std::cos (w) / a0; a2 = (1 - al) / a0;
    }
    double p (double x) { double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};

// speech-like segment: glottal-ish harmonic source -> 3 formants, syllabic envelope, some fricative noise
static std::vector<double> synthSegment (std::mt19937& rng, double dur, double f0, double rmsDb, bool backchannel)
{
    std::uniform_real_distribution<double> U (0, 1);
    const int N = (int) (dur * FS);
    std::vector<double> y ((size_t) N, 0.0);
    BP f1, f2, f3;
    double ph = 0, nextSyl = 0, env = 0, envT = 0;
    int sylLen = 1; int sylPos = 0;
    double F1 = 500, F2 = 1500, F3 = 2600;
    for (int n = 0; n < N; ++n)
    {
        if (n >= nextSyl)
        {
            sylLen = (int) ((backchannel ? 0.11 + 0.06 * U (rng) : 0.12 + 0.12 * U (rng)) * FS);
            sylPos = 0; nextSyl = n + sylLen;
            F1 = 300 + 500 * U (rng); F2 = 900 + 1400 * U (rng); F3 = 2400 + 600 * U (rng);
            if (backchannel && U (rng) < 0.5) { F1 = 280; F2 = 1000; } // nasal-ish "mhm"
            f1.set (F1, 8); f2.set (F2, 10); f3.set (F3, 12);
            envT = 0.5 + 0.5 * U (rng);
        }
        const double u = (double) sylPos++ / sylLen;
        env = envT * std::sin (M_PI * u) * (0.7 + 0.3 * std::sin (2 * M_PI * 0.5 * n / FS));
        const double f = f0 * (1.0 + 0.08 * std::sin (2 * M_PI * 1.7 * n / FS) + (backchannel ? 0.12 * u : 0.0));
        ph += f / FS; ph -= std::floor (ph);
        const double src = (ph < 0.4 ? std::sin (M_PI * ph / 0.4) : 0.0) - 0.3 * ph;   // crude glottal pulse
        const double noise = U (rng) * 2 - 1;
        double v = f1.p (src) + 0.7 * f2.p (src) + 0.4 * f3.p (src) + 0.15 * (f2.p (noise) + f3.p (noise));
        if (u < 0.12 && ! backchannel) v += 0.6 * noise * (u / 0.12 > 0.5 ? 0.3 : 1.0) * 0.5; // consonant burst
        y[(size_t) n] = v * env;
    }
    double e = 0; for (double s : y) e += s * s; e = std::sqrt (e / N + 1e-30);
    const double gain = std::pow (10.0, rmsDb / 20.0) / e;
    for (double& s : y) s *= gain;
    return y;
}

