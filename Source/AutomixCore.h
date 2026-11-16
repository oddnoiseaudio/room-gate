#pragma once
// ---------------------------------------------------------------------------
//  AutomixCore  -  bleed-compensated speaker activity detector + lookahead gate
//  Pure C++17, no JUCE dependency (so it can be unit tested stand-alone).
//
//  Idea
//  ----
//  Every mic j hears its own speaker plus the other speakers' bleed:
//
//        P_jb  =  own_jb  +  sum_{i != j} g_ijb * own_ib      (power, per band b)
//
//  g_ijb is the coupling (bleed) of source i into mic j in frequency band b
//  (acoustic bleed is strongly coloured, so a broadband ratio is far too noisy).
//  It is learned online: whenever mic i is clearly active and mic j is not,
//  the slow-envelope ratio P_jb / P_ib is an estimate of g_ijb (tracked as a robust
//  upper quantile in the dB domain; own speech of mic j cannot drag it up, the
//  safety margin covers the remaining spread).
//
//  Mic j counts as "speaking" when what is left after subtracting the predicted
//  bleed (times a safety margin) exceeds an absolute threshold and a noise-floor
//  guard. A quiet "mhm" on mic 2 therefore opens mic 2 even while speaker 1 is
//  loud, but mic 2 stays shut while it only contains speaker 1's bleed.
//
//  Audio is delayed by a lookahead, so gates are fully open when the first
//  consonant arrives.
//
//  Channel modes
//  -------------
//  Audio  : real audio input, analysed and gated (plugin: every channel).
//  Remote : a microphone on another computer. Only its band envelopes arrive
//           (RemoteFrame), nothing is gated. Used by the room app.
//  Off    : ignored.
//
//  For the room app the local envelopes can be delayed by setAlignDelay() so that
//  they line up in time with the (network-delayed) remote envelopes.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#ifndef M_PI
 #define M_PI 3.14159265358979323846
#endif

namespace automix
{
constexpr int kMaxChannels = 8;
constexpr int kBands = 5;
constexpr int kLearnDecim = 16;
constexpr double kBandHz[kBands] = { 200.0, 450.0, 1000.0, 2200.0, 4500.0 };

enum class ChannelMode : uint8_t { Audio, Remote, Off };

// Band envelopes of one microphone at one point in time (what is sent over the network).
struct RemoteFrame
{
    float pFast[kBands] {};
    float pSlow[kBands] {};
    float nf[kBands] {};
    uint8_t active = 0;
};

struct Params
{
    float thresholdDb = -50.0f; // absolute speech level (after bleed removal) needed to open a mic
    float bleedDb     = -6.0f;  // ceiling for the learned bleed coupling (power)
    float marginDb    = 4.0f;   // how far above predicted bleed a mic has to be
    float holdMs      = 150.0f; // how long a mic stays open after speech stops
    float fadeInMs    = 2.0f;   // gate opening ramp
    float releaseMs   = 80.0f;  // gate closing ramp
    float closedDb    = -60.0f; // attenuation of closed mics (<= -90 => silence)
    int   lookaheadSamples = 480;
    bool  learnBleed  = true;
    bool  keepLastOpen = false; // keep the last speaker open during pauses
    bool  forceOpen   = false;  // bypass: every audio channel stays open (e.g. nobody else in the room)
};

namespace detail
{
inline double dbToPow (double db) { return std::pow (10.0, db / 10.0); }
inline double dbToAmp (double db) { return std::pow (10.0, db / 20.0); }

struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    void setHighpass (double fs, double f, double q)
    {
        const double w = 2.0 * M_PI * f / fs, c = std::cos (w), s = std::sin (w), al = s / (2.0 * q);
        const double a0 = 1.0 + al;
        b0 = (1.0 + c) * 0.5 / a0; b1 = -(1.0 + c) / a0; b2 = b0;
        a1 = -2.0 * c / a0;        a2 = (1.0 - al) / a0;
    }
    void setBandpass (double fs, double f, double q)
    {
        const double w = 2.0 * M_PI * f / fs, c = std::cos (w), s = std::sin (w), al = s / (2.0 * q);
        const double a0 = 1.0 + al;
        b0 = al / a0; b1 = 0.0; b2 = -al / a0;
        a1 = -2.0 * c / a0; a2 = (1.0 - al) / a0;
    }
    void reset() { z1 = z2 = 0; }
    inline double process (double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};
} // namespace detail

class Core
{
public:
    // maxAlignSamples > 0 only for the room app (history of local envelopes for time alignment)
    void prepare (double sampleRate, int numChannels, int maxLookaheadSamples, int maxAlignSamples = 0)
    {
        fs = sampleRate;
        nch = std::clamp (numChannels, 1, kMaxChannels);
        maxLook = std::max (0, maxLookaheadSamples);
        ringSize = maxLook + 1;
        alignMax = std::max (0, maxAlignSamples);
        histSize = alignMax + 1;
        for (int c = 0; c < nch; ++c)
        {
            ring[(size_t) c].assign ((size_t) ringSize, 0.0f);
            hist[(size_t) c].assign ((size_t) histSize, std::array<float, 3 * kBands> {});
        }
        modes.fill (ChannelMode::Audio);

        for (int c = 0; c < nch; ++c)
        {
            auto& ch = chans[(size_t) c];
            ch.hp1.setHighpass (fs, 100.0, 0.7071);   // rumble / plosive / handling-noise guard
            ch.hp2.setHighpass (fs, 100.0, 0.7071);
            for (int b = 0; b < kBands; ++b)
                ch.bp[(size_t) b].setBandpass (fs, std::min (kBandHz[b], fs * 0.45), 1.2);
        }

        aFastAtt = coef (2.0);
        aFastRel = coef (10.0);
        aSlow    = coef (40.0);
        aNfDown  = coef (150.0);
        aNfUp    = coef (2500.0);
        // bleed estimate = robust upper-quantile (~80 %) tracker in the dB domain, evaluated every kLearnDecim samples.
        // Errors are winsorised, so the mic's own speech (huge ratios) can't drag the estimate up.
        aGDown   = 1.0 - std::exp (-(double) kLearnDecim / (700.0  * 0.001 * fs));
        aGUp     = 1.0 - std::exp (-(double) kLearnDecim / (175.0  * 0.001 * fs));
        reset();
        setParams (params);
    }

    void reset()
    {
        for (int c = 0; c < nch; ++c)
        {
            auto& ch = chans[(size_t) c];
            ch.hp1.reset(); ch.hp2.reset();
            for (auto& f : ch.bp) f.reset();
            ch.pFast.fill (0.0); ch.pSlow.fill (0.0); ch.nf.fill (1e-11);
            ch.active = false; ch.hold = 0; ch.ramp = 0; ch.gain = (float) closedGain;
            std::fill (ring[(size_t) c].begin(), ring[(size_t) c].end(), 0.0f);
            for (auto& h : hist[(size_t) c]) h.fill (0.0f);
        }
        for (auto& a : g) for (auto& b : a) b.fill (detail::dbToPow (params.bleedDb));
        for (auto& a : gdB) for (auto& b : a) b.fill ((double) params.bleedDb);
        wr = 0; hw = 0;
        learnCounter = 0;
        lastIdx = -1;
    }

    // forget everything learned about crosstalk and the noise floors (start learning from scratch)
    void resetLearning()
    {
        for (auto& a : gdB) for (auto& b : a) b.fill ((double) std::min (params.bleedDb, -0.5f));
        for (auto& a : g)   for (auto& b : a) b.fill (detail::dbToPow ((double) std::min (params.bleedDb, -0.5f)));
        for (int c = 0; c < nch; ++c) if (modes[(size_t) c] == ChannelMode::Audio) chans[(size_t) c].nf.fill (1e-11);
    }

    void setParams (const Params& p)
    {
        params = p;
        params.lookaheadSamples = std::clamp (p.lookaheadSamples, 0, maxLook);
        thrPow     = detail::dbToPow (p.thresholdDb);
        margin     = detail::dbToPow (p.marginDb);
        gCeil      = detail::dbToPow (std::min (p.bleedDb, -0.5f));
        closedGain = p.closedDb <= -90.0f ? 0.0 : detail::dbToAmp (p.closedDb);
        holdSamples = (int) std::lround (p.holdMs * 0.001 * fs) + params.lookaheadSamples; // hold is measured in audio time
        upStep     = 1.0 / std::max (1.0, p.fadeInMs * 0.001 * fs);
        downStep   = 1.0 / std::max (1.0, p.releaseMs * 0.001 * fs);
        for (size_t i = 0; i < (size_t) kMaxChannels; ++i)
            for (size_t j = 0; j < (size_t) kMaxChannels; ++j)
                for (size_t b = 0; b < (size_t) kBands; ++b)
                {
                    double d = p.learnBleed ? std::min (gdB[i][j][b], (double) std::min (p.bleedDb, -0.5f)) : (double) std::min (p.bleedDb, -0.5f);
                    gdB[i][j][b] = d; g[i][j][b] = detail::dbToPow (d);
                }
    }

    // ---- room app: channel modes, remote envelopes, time alignment ----
    void setMode (int c, ChannelMode m)
    {
        if (c < 0 || c >= nch || modes[(size_t) c] == m) return;
        modes[(size_t) c] = m;
        auto& ch = chans[(size_t) c];
        ch.pFast.fill (0.0); ch.pSlow.fill (0.0); ch.nf.fill (m == ChannelMode::Audio ? 1e-11 : 0.0);
        ch.active = false; ch.hold = 0; ch.ramp = 0;
        // forget what was learned about this channel as a source/receiver
        for (size_t o = 0; o < (size_t) kMaxChannels; ++o)
            for (size_t b = 0; b < (size_t) kBands; ++b)
            {
                gdB[(size_t) c][o][b] = gdB[o][(size_t) c][b] = (double) std::min (params.bleedDb, -0.5f);
                g[(size_t) c][o][b] = g[o][(size_t) c][b] = gCeil;
            }
    }
    ChannelMode getMode (int c) const { return modes[(size_t) c]; }

    void setRemoteFrame (int c, const RemoteFrame& f)
    {
        if (c < 0 || c >= nch || modes[(size_t) c] != ChannelMode::Remote) return;
        auto& ch = chans[(size_t) c];
        for (size_t b = 0; b < (size_t) kBands; ++b) { ch.pFast[b] = f.pFast[b]; ch.pSlow[b] = f.pSlow[b]; ch.nf[b] = f.nf[b]; }
        ch.active = f.active != 0;
    }

    RemoteFrame getLocalFrame (int c) const
    {
        RemoteFrame f;
        const auto& ch = chans[(size_t) c];
        for (size_t b = 0; b < (size_t) kBands; ++b) { f.pFast[b] = (float) ch.pFast[b]; f.pSlow[b] = (float) ch.pSlow[b]; f.nf[b] = (float) ch.nf[b]; }
        f.active = ch.active ? 1 : 0;
        return f;
    }

    void setAlignDelay (int samples) { alignSamples = std::clamp (samples, 0, alignMax); }
    int  getAlignDelay() const { return alignSamples; }

    int   getLatencySamples() const { return params.lookaheadSamples; }
    int   getNumChannels()    const { return nch; }
    bool  isActive (int c)    const { return chans[(size_t) c].active; }
    float getGain (int c)     const { return chans[(size_t) c].gain; }
    float getLevelDb (int c)  const
    {
        double s = 0; for (double v : chans[(size_t) c].pFast) s += v;
        return (float) (10.0 * std::log10 (s + 1e-12));
    }
    // mean bleed coupling i -> j in dB (average over bands, in dB)
    float getBleedDb (int i, int j) const
    {
        double s = 0; for (double v : g[(size_t) i][(size_t) j]) s += 10.0 * std::log10 (v + 1e-12);
        return (float) (s / kBands);
    }
    float getBleedBandDb (int i, int j, int b) const { return (float) gdB[(size_t) i][(size_t) j][(size_t) b]; }

    // In-place processing. Only channels in Audio mode read/write io[c]; others may be nullptr.
    // If gainOut != nullptr it receives the applied gain per sample/channel (Audio channels).
    void process (float* const* io, int numSamples, float* const* gainOut = nullptr)
    {
        const int L = params.lookaheadSamples;
        using Arr = std::array<std::array<double, kBands>, kMaxChannels>;
        Arr q {}, own {}, vS {}, vN {};
        std::array<double, kMaxChannels> nfTot {};

        for (int n = 0; n < numSamples; ++n)
        {
            // ---------------- detector filterbank + envelopes ----------------
            bool anyActiveNow = false;
            for (int c = 0; c < nch; ++c)
                anyActiveNow = anyActiveNow || (modes[(size_t) c] != ChannelMode::Off && chans[(size_t) c].active);

            for (int c = 0; c < nch; ++c)
            {
                auto& ch = chans[(size_t) c];
                const size_t ci = (size_t) c;
                const auto mode = modes[ci];

                if (mode == ChannelMode::Audio)
                {
                    const double x = ch.hp2.process (ch.hp1.process ((double) io[c][n]));
                    for (int b = 0; b < kBands; ++b)
                    {
                        const size_t bi = (size_t) b;
                        const double d = ch.bp[bi].process (x);
                        const double e = d * d;
                        ch.pFast[bi] += (e > ch.pFast[bi] ? aFastAtt : aFastRel) * (e - ch.pFast[bi]);
                        ch.pSlow[bi] += aSlow * (e - ch.pSlow[bi]);

                        // noise floor: follows down always, up only while NO mic is active (otherwise it would learn the bleed as 'noise')
                        if (ch.pSlow[bi] < ch.nf[bi])  ch.nf[bi] += aNfDown * (ch.pSlow[bi] - ch.nf[bi]);
                        else if (! anyActiveNow)       ch.nf[bi] += aNfUp   * (ch.pSlow[bi] - ch.nf[bi]);
                        ch.nf[bi] = std::max (ch.nf[bi], 1e-12);
                    }

                    // envelope view used for decisions: optionally delayed to line up with remote envelopes
                    if (alignMax > 0)
                    {
                        auto& slot = hist[ci][(size_t) hw];
                        for (size_t b = 0; b < (size_t) kBands; ++b)
                        {
                            slot[b] = (float) ch.pFast[b]; slot[kBands + b] = (float) ch.pSlow[b]; slot[2 * kBands + b] = (float) ch.nf[b];
                        }
                        int rd = hw - alignSamples; if (rd < 0) rd += histSize;
                        const auto& d = hist[ci][(size_t) rd];
                        for (size_t b = 0; b < (size_t) kBands; ++b)
                        {
                            q[ci][b]  = std::max (0.0, (double) d[b] - (double) d[2 * kBands + b]);
                            vS[ci][b] = d[kBands + b]; vN[ci][b] = d[2 * kBands + b];
                        }
                    }
                    else
                    {
                        for (size_t b = 0; b < (size_t) kBands; ++b)
                        {
                            q[ci][b] = std::max (0.0, ch.pFast[b] - ch.nf[b]);
                            vS[ci][b] = ch.pSlow[b]; vN[ci][b] = ch.nf[b];
                        }
                    }
                }
                else if (mode == ChannelMode::Remote)
                {
                    for (size_t b = 0; b < (size_t) kBands; ++b)
                    {
                        q[ci][b] = std::max (0.0, ch.pFast[b] - ch.nf[b]);
                        vS[ci][b] = ch.pSlow[b]; vN[ci][b] = ch.nf[b];
                    }
                }
                else
                {
                    q[ci].fill (0.0); vS[ci].fill (0.0); vN[ci].fill (0.0);
                }
                double nfSum = 0; for (double v : vN[ci]) nfSum += v;
                nfTot[ci] = nfSum;
            }
            if (alignMax > 0 && ++hw >= histSize) hw = 0;

            // ---------------- per-band bleed-compensated own-level estimate ----------------
            own = q;
            for (int it = 0; it < 3; ++it)
                for (int j = 0; j < nch; ++j)
                {
                    if (modes[(size_t) j] == ChannelMode::Off) continue;
                    for (int b = 0; b < kBands; ++b)
                    {
                        double bp = 0;
                        for (int i = 0; i < nch; ++i) if (i != j) bp += g[(size_t) i][(size_t) j][(size_t) b] * own[(size_t) i][(size_t) b];
                        own[(size_t) j][(size_t) b] = std::max (0.0, q[(size_t) j][(size_t) b] - bp);
                    }
                }

            // ---------------- decisions (audio channels only) ----------------
            for (int j = 0; j < nch; ++j)
            {
                if (modes[(size_t) j] != ChannelMode::Audio) continue;
                auto& ch = chans[(size_t) j];
                double resid = 0;
                for (int b = 0; b < kBands; ++b)
                {
                    double bp = 0;
                    for (int i = 0; i < nch; ++i) if (i != j) bp += g[(size_t) i][(size_t) j][(size_t) b] * own[(size_t) i][(size_t) b];
                    resid += std::max (0.0, q[(size_t) j][(size_t) b] - margin * bp);
                }
                const double need = std::max (thrPow, noiseGuard * nfTot[(size_t) j]);

                if (! ch.active)
                {
                    if (resid > need) { ch.active = true; ch.hold = holdSamples; lastIdx = j; }
                }
                else
                {
                    if (resid > hysteresis * need) ch.hold = holdSamples;
                    else if (ch.hold > 0)          --ch.hold;
                    else                           ch.active = false;
                }
            }

            // ---------------- bleed learning (decimated) ----------------
            if (params.learnBleed && ++learnCounter >= kLearnDecim)
            {
                learnCounter = 0;
                const double ceilDb = (double) std::min (params.bleedDb, -0.5f);
                for (int i = 0; i < nch; ++i)
                {
                    if (modes[(size_t) i] == ChannelMode::Off || ! chans[(size_t) i].active) continue;
                    for (int j = 0; j < nch; ++j)
                    {
                        if (j == i || modes[(size_t) j] != ChannelMode::Audio || chans[(size_t) j].active) continue;
                        for (int b = 0; b < kBands; ++b)
                        {
                            const size_t bi = (size_t) b;
                            const double si = vS[(size_t) i][bi] - vN[(size_t) i][bi];
                            if (si < 8.0 * vN[(size_t) i][bi]) continue;          // band must be well above its noise floor
                            const double r  = std::max (1e-12, vS[(size_t) j][bi] - vN[(size_t) j][bi]) / si;
                            double& gd = gdB[(size_t) i][(size_t) j][bi];
                            const double e = std::clamp (10.0 * std::log10 (r) - gd, -12.0, 6.0);
                            gd += (e > 0 ? aGUp : aGDown) * e;
                            gd = std::clamp (gd, -70.0, ceilDb);
                            g[(size_t) i][(size_t) j][bi] = detail::dbToPow (gd);
                        }
                    }
                }
            }

            // ---------------- gating + lookahead (audio channels only) ----------------
            bool anyActive = false;
            for (int c = 0; c < nch; ++c) anyActive = anyActive || (modes[(size_t) c] != ChannelMode::Off && chans[(size_t) c].active);

            for (int c = 0; c < nch; ++c)
            {
                if (modes[(size_t) c] != ChannelMode::Audio) continue;
                auto& ch = chans[(size_t) c];
                const bool target = ch.active || params.forceOpen || (params.keepLastOpen && ! anyActive && c == lastIdx);
                if (target) ch.ramp = std::min (1.0, ch.ramp + upStep);
                else        ch.ramp = std::max (0.0, ch.ramp - downStep);
                const double s  = ch.ramp * ch.ramp * (3.0 - 2.0 * ch.ramp);
                const double gn = closedGain + (1.0 - closedGain) * s;
                ch.gain = (float) gn;

                auto& rb = ring[(size_t) c];
                rb[(size_t) wr] = io[c][n];
                int rd = wr - L; if (rd < 0) rd += ringSize;
                io[c][n] = (float) (rb[(size_t) rd] * gn);
                if (gainOut != nullptr) gainOut[c][n] = ch.gain;
            }
            if (++wr >= ringSize) wr = 0;
        }
    }

private:
    struct Channel
    {
        detail::Biquad hp1, hp2;
        std::array<detail::Biquad, kBands> bp;
        std::array<double, kBands> pFast {}, pSlow {}, nf {};
        bool   active = false;
        int    hold = 0;
        double ramp = 0;
        float  gain = 0;
    };

    double coef (double ms) const { return 1.0 - std::exp (-1.0 / (ms * 0.001 * fs)); }

    static constexpr double noiseGuard = 2.0;  // speech must exceed 2x the tracked noise floor (after subtraction)
    static constexpr double hysteresis = 0.5;  // stay-open level relative to open level

    double fs = 48000.0;
    int nch = 2, maxLook = 0, ringSize = 1, wr = 0, lastIdx = -1;
    int alignMax = 0, alignSamples = 0, histSize = 1, hw = 0;
    Params params;
    std::array<ChannelMode, kMaxChannels> modes {};
    std::array<Channel, kMaxChannels> chans;
    std::array<std::vector<float>, kMaxChannels> ring;
    std::array<std::vector<std::array<float, 3 * kBands>>, kMaxChannels> hist;
    std::array<std::array<std::array<double, kBands>, kMaxChannels>, kMaxChannels> g {};    // g[i][j][b]: bleed of i into j (linear power)
    std::array<std::array<std::array<double, kBands>, kMaxChannels>, kMaxChannels> gdB {};  // same in dB
    int learnCounter = 0;

    double aFastAtt = 0, aFastRel = 0, aSlow = 0, aNfDown = 0, aNfUp = 0, aGDown = 0, aGUp = 0;
    double thrPow = 1e-5, margin = 2.5, gCeil = 0.1, closedGain = 0.001, upStep = 0.01, downStep = 0.01;
    int holdSamples = 7200;
};
} // namespace automix
