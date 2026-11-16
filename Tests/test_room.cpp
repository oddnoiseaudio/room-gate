// Simulates two computers in one room, each with its own mic that also hears the other speaker.
// The two RoomEngine instances exchange their envelope frames over a simulated network
// (latency, jitter, packet loss) and gate their own mic. Compare with test_core (one gate, both mics visible).
// Build: g++ -O2 -std=c++17 -I../Source test_room.cpp -o test_room
#include "RoomEngine.h"
#include "synth.h"
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <string>

using namespace automix;

struct Event { double t0, t1; std::string label; };

struct Net
{
    struct Pkt { double at; int to; uint32_t from, seq; double ts, hop; int red; std::vector<RemoteFrame> f; };
    std::multimap<double, Pkt> q;
    std::mt19937 rng { 7 };
    double baseMs, jitterMs, loss;
    void send (double nowSec, int to, uint32_t from, uint32_t seq, double ts, double hop, const RemoteFrame* f, int n, int red)
    {
        std::uniform_real_distribution<double> U (0, 1);
        if (U (rng) < loss) return;
        Pkt p { nowSec + (baseMs + jitterMs * U (rng)) * 0.001, to, from, seq, ts, hop, red, std::vector<RemoteFrame> (f, f + n) };
        q.insert ({ p.at, p });
    }
};

struct Scn { const char* name; double bleedDb, baseMs, jitterMs, loss; double backDb; float lookMs; };

static void run (const Scn& sc, unsigned seed, bool verbose)
{
    std::mt19937 rng (seed);
    const double T = 24.0; const int N = (int) (T * FS);
    std::vector<Event> ev[2];
    auto add = [&] (int s, double t0, double d, const char* l) { ev[s].push_back ({ t0, t0 + d, l }); };
    for (double t = 0.5; t < 5.5; t += 0.55 + 0.35 * (rng() % 100) / 100.0) add (0, t, 0.30 + 0.2 * (rng() % 100) / 100.0, "A-word");
    add (1, 1.60, 0.30, "B-mhm");  add (1, 3.20, 0.38, "B-aha");  add (1, 4.70, 0.22, "B-ja");
    for (double t = 6.5; t < 11.5; t += 0.60 + 0.3 * (rng() % 100) / 100.0) add (1, t, 0.30 + 0.25 * (rng() % 100) / 100.0, "B-word");
    add (0, 8.15, 0.30, "A-mhm"); add (0, 9.90, 0.25, "A-ja");
    for (double t = 12.5; t < 15.5; t += 0.55) { add (0, t, 0.4, "A-ovl"); add (1, t + 0.15, 0.4, "B-ovl"); }
    add (0, 16.5, 3.5, "A-long"); add (1, 17.4, 0.18, "B-mh-short"); add (1, 18.6, 0.25, "B-aha2"); add (1, 19.5, 0.20, "B-mhm2");
    add (1, 21.0, 2.2, "B-long"); add (0, 22.0, 0.22, "A-ja2");

    std::vector<double> own[2] = { std::vector<double> ((size_t) N, 0.0), std::vector<double> ((size_t) N, 0.0) };
    for (int s = 0; s < 2; ++s)
        for (auto& e : ev[s])
        {
            const bool bc = e.label.find ("mhm") != std::string::npos || e.label.find ("aha") != std::string::npos
                            || e.label.find ("ja") != std::string::npos || e.label.find ("mh-short") != std::string::npos;
            auto seg = synthSegment (rng, e.t1 - e.t0, s == 0 ? 115 : 190, (bc ? sc.backDb : -22.0) + ((int) (rng() % 5) - 2), bc);
            const int off = (int) (e.t0 * FS);
            for (size_t k = 0; k < seg.size() && off + k < (size_t) N; ++k) own[s][off + k] += seg[k];
        }
    const int dly = (int) (0.003 * FS); const double g = std::pow (10.0, sc.bleedDb / 20.0);
    std::vector<float> mic[2]; for (auto& m : mic) m.assign ((size_t) N, 0.f);
    std::uniform_real_distribution<double> U (-1, 1);
    for (int j = 0; j < 2; ++j)
    {
        const int i = 1 - j; double lp = 0, pink = 0; const double nAmp = std::pow (10.0, -62.0 / 20.0) * 1.7;
        for (int n = 0; n < N; ++n)
        {
            double b = (n >= dly ? own[i][(size_t) (n - dly)] : 0.0); lp += 0.35 * (b - lp); b = lp;
            pink = 0.97 * pink + 0.25 * U (rng);
            mic[j][(size_t) n] = (float) (own[j][(size_t) n] + g * b + nAmp * pink);
        }
    }

    // two engines
    RoomEngine eng[2]; Net net { {}, {}, sc.baseMs, sc.jitterMs, sc.loss };
    double now = 0.0;
    for (int e = 0; e < 2; ++e)
    {
        if (std::getenv ("AC")) eng[e].acousticSec = atof (std::getenv ("AC")) * 0.001; eng[e].setLookaheadMs (sc.lookMs); eng[e].prepare (FS, 4096);
        eng[e].addPeer (e == 0 ? 2u : 1u);
        eng[e].setOneWay (e == 0 ? 2u : 1u, sc.baseMs * 0.001);
        eng[e].setUserParams (-50.0f, 4.0f);
        eng[e].onSendFrames = [&, e] (uint32_t seq, double ts, double hs, const RemoteFrame* f, int n, int red) { net.send (now, 1 - e, e == 0 ? 1u : 2u, seq, ts, hs, f, n, red); };
    }
    std::vector<float> out[2], gain[2]; for (int e = 0; e < 2; ++e) { out[e].assign ((size_t) N, 0.f); gain[e].assign ((size_t) N, 0.f); }
    const int B = 480;
    for (int pos = 0; pos < N; pos += B)
    {
        const int len = std::min (B, N - pos);
        for (int e = 0; e < 2; ++e)
        {
            // deliver everything that has arrived by now
            for (auto it = net.q.begin(); it != net.q.end() && it->first <= pos / FS; )
            {
                auto& p = it->second;
                if (p.to == e) eng[e].pushFrames (p.from, p.seq, p.ts, p.hop, p.f.data(), (int) p.f.size(), p.red, p.at);
                ++it;
            }
        }
        for (auto it = net.q.begin(); it != net.q.end() && it->first <= pos / FS; ) it = net.q.erase (it);
        now = (pos + len) / FS;
        for (int e = 0; e < 2; ++e)
            eng[e].process (mic[e].data() + pos, out[e].data() + pos, len, gain[e].data() + pos, pos / FS);
    }
    // NB: deliveries are dropped once consumed by the loop above, both engines see all their packets

    const int L = eng[0].getLatencySamples();
    double worst = 1, sum = 0; int cnt = 0;
    if (verbose) std::printf ("\n=== %s (seed %u): net %.0f ms + 0..%.0f ms jitter, %.0f %% loss, bleed %.0f dB, lookahead %d smp ===\n", sc.name, seed, sc.baseMs, sc.jitterMs, 100 * sc.loss, sc.bleedDb, L);
    for (int s = 0; s < 2; ++s)
        for (auto& e : ev[s])
        {
            double num = 0, den = 0;
            for (int n = (int) (e.t0 * FS); n < std::min (N - 1, (int) (e.t1 * FS)); ++n)
            {
                const double x = own[s][(size_t) n], gn = gain[s][(size_t) std::min (N - 1, n + L)];
                num += gn * gn * x * x; den += x * x;
            }
            const double pass = den > 0 ? num / den : 1.0;
            worst = std::min (worst, pass); sum += pass; ++cnt;
            if (verbose && pass < 0.98) std::printf ("  mic%d %-11s %5.2f-%5.2fs  passed energy %6.2f %%\n", s + 1, e.label.c_str(), e.t0, e.t1, 100 * pass);
        }
    double leakMs[2] = { 0, 0 };
    for (int s = 0; s < 2; ++s)
    {
        std::vector<char> ownQuiet ((size_t) N, 1), otherTalk ((size_t) N, 0);
        for (auto& e : ev[s]) for (int n = std::max (0, (int) ((e.t0 - 0.05) * FS)); n < std::min (N, (int) ((e.t1 + 0.35) * FS)); ++n) ownQuiet[(size_t) n] = 0;
        for (auto& e : ev[1 - s]) for (int n = (int) (e.t0 * FS); n < std::min (N, (int) (e.t1 * FS)); ++n) otherTalk[(size_t) n] = 1;
        for (int n = 0; n + L < N; ++n)
            if (ownQuiet[(size_t) n] && otherTalk[(size_t) n] && gain[s][(size_t) (n + L)] > 0.5f) leakMs[s] += 1000.0 / FS;
    }
    std::printf ("%-30s seed %u: worst event %6.2f %%, mean %6.2f %% | wrongly open while only the other speaks: mic1 %.0f ms, mic2 %.0f ms | align %.1f / %.1f ms\n",
                 sc.name, seed, 100 * worst, 100 * sum / cnt, leakMs[0], leakMs[1], eng[0].getAlignMs(), eng[1].getAlignMs());
}

int main (int argc, char** argv)
{
    const bool verbose = argc > 1 && std::string (argv[1]) == "-v";
    const Scn scs[] = {
        { "wired LAN (1 ms)",                       -16, 1,  0.5, 0.00, -30, 40 },
        { "good WLAN (4 ms, +6 jitter)",            -16, 4,  6.0, 0.00, -30, 40 },
        { "bad WLAN (8 ms, +20 jitter, 3% loss) 40",-16, 8, 20.0, 0.03, -30, 40 },
        { "bad WLAN (8 ms, +20 jitter, 3% loss) 80",-16, 8, 20.0, 0.03, -30, 80 },
        { "close table, WLAN",                      -10, 4,  6.0, 0.01, -30, 40 },
        { "close table, wired",                     -10, 1,  0.5, 0.00, -30, 40 },
    };
    for (auto& sc : scs) for (unsigned seed : { 1u, 2u, 3u }) run (sc, seed, verbose);
    return 0;
}
