// Real-time smoke test of the actual UDP transport: two RoomEngine+RoomNet pairs in one process,
// connected over localhost, fed with synthetic speech (A speaks, B interjects, and the other way round).
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include "RoomEngine.h"
#include "RoomNet.h"
#include "synth.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace automix;

struct Ev { double t0, t1; bool bc; };

int main()
{
    const double T = 12.0; const int N = (int) (T * FS);
    std::mt19937 rng (3);
    std::vector<Ev> ev[2];
    for (double t = 0.5; t < 5.5; t += 0.7) ev[0].push_back ({ t, t + 0.4, false });
    ev[1].push_back ({ 1.6, 1.9, true }); ev[1].push_back ({ 3.2, 3.58, true }); ev[1].push_back ({ 4.7, 4.92, true });
    for (double t = 6.5; t < 11.5; t += 0.8) ev[1].push_back ({ t, t + 0.45, false });
    ev[0].push_back ({ 8.15, 8.45, true }); ev[0].push_back ({ 9.9, 10.15, true });
    std::vector<double> own[2] = { std::vector<double> ((size_t) N, 0.0), std::vector<double> ((size_t) N, 0.0) };
    for (int s = 0; s < 2; ++s)
        for (auto& e : ev[s])
        {
            auto seg = synthSegment (rng, e.t1 - e.t0, s == 0 ? 115 : 190, e.bc ? -30.0 : -22.0, e.bc);
            for (size_t k = 0; k < seg.size(); ++k) own[s][(size_t) (e.t0 * FS) + k] += seg[k];
        }
    std::vector<float> mic[2]; for (auto& m : mic) m.assign ((size_t) N, 0.f);
    const int dly = (int) (0.003 * FS); const double g = std::pow (10.0, -16.0 / 20.0);
    std::uniform_real_distribution<double> U (-1, 1);
    for (int j = 0; j < 2; ++j)
    {
        const int i = 1 - j; double lp = 0, pink = 0;
        for (int n = 0; n < N; ++n)
        {
            double b = n >= dly ? own[i][(size_t) (n - dly)] : 0.0; lp += 0.35 * (b - lp); b = lp;
            pink = 0.97 * pink + 0.25 * U (rng);
            mic[j][(size_t) n] = (float) (own[j][(size_t) n] + g * b + std::pow (10.0, -62.0 / 20.0) * 1.7 * pink);
        }
    }

    RoomEngine eng[2]; std::unique_ptr<RoomNet> net[2];
    const int ports[2] = { 47811, 47812 };
    for (int e = 0; e < 2; ++e)
    {
        if (const char* l = std::getenv ("LOOK")) eng[e].setLookaheadMs ((float) std::atof (l));
        eng[e].prepare (FS, 4096);
        net[e] = std::make_unique<RoomNet> (eng[e], ports[e]);
        net[e]->setRoom ("smoke");
        net[e]->addManualPeer ("127.0.0.1:" + juce::String (ports[1 - e]));
        if (! net[e]->start()) { std::printf ("cannot bind port %d\n", ports[e]); return 1; }
    }

    std::vector<float> gain[2], outBuf[2];
    for (int e = 0; e < 2; ++e) { gain[e].assign ((size_t) N, 0.f); outBuf[e].assign ((size_t) N, 0.f); }
    const double t0 = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const double tshift = std::getenv ("TSHIFT") ? std::atof (std::getenv ("TSHIFT")) * 0.001 : 0.0;
    std::vector<std::thread> th;
    for (int e = 0; e < 2; ++e)
        th.emplace_back ([&, e]
        {
            const int B = 480;
            for (int pos = 0; pos + B <= N; pos += B)
            {
                const double due = t0 + (pos + B) / FS;                       // a real device delivers the block once it is recorded
                while (juce::Time::getMillisecondCounterHiRes() * 0.001 < due) std::this_thread::sleep_for (std::chrono::microseconds (500));
                eng[e].process (mic[e].data() + pos, outBuf[e].data() + pos, B, gain[e].data() + pos, juce::Time::getMillisecondCounterHiRes() * 0.001 - (double) B / FS - tshift);
            }
        });
    for (auto& t : th) t.join();

    auto peers0 = net[0]->getPeers(), peers1 = net[1]->getPeers();
    std::printf ("peers found: A sees %d, B sees %d | rtt A->B %.2f ms | align %.1f / %.1f ms\n", (int) peers0.size(), (int) peers1.size(),
                 peers0.empty() ? -1.0 : peers0[0].rttMs, eng[0].getAlignMs(), eng[1].getAlignMs());
    const int L = eng[0].getLatencySamples();
    const double skip = 3.0;                                                   // ignore the learning phase
    for (int s = 0; s < 2; ++s)
    {
        double worst = 1, sum = 0; int cnt = 0;
        for (auto& e : ev[s])
        {
            if (e.t0 < skip) continue;
            double num = 0, den = 0;
            for (int n = (int) (e.t0 * FS); n < (int) (e.t1 * FS); ++n)
            { const double x = own[s][(size_t) n], gn = gain[s][(size_t) std::min (N - 1, n + L)]; num += gn * gn * x * x; den += x * x; }
            const double p = den > 0 ? num / den : 1.0; worst = std::min (worst, p); sum += p; ++cnt;
        }
        double leak = 0;
        std::vector<char> quiet ((size_t) N, 1), other ((size_t) N, 0);
        for (auto& e : ev[s]) for (int n = std::max (0, (int) ((e.t0 - 0.05) * FS)); n < std::min (N, (int) ((e.t1 + 0.35) * FS)); ++n) quiet[(size_t) n] = 0;
        for (auto& e : ev[1 - s]) for (int n = (int) (e.t0 * FS); n < std::min (N, (int) (e.t1 * FS)); ++n) other[(size_t) n] = 1;
        for (int n = (int) (skip * FS); n + L < N; ++n) if (quiet[(size_t) n] && other[(size_t) n] && gain[s][(size_t) (n + L)] > 0.5f) { leak += 1000.0 / FS; if (n % 2400 == 0) std::printf ("  leak mic %c at %.2f s\n", s == 0 ? 'A' : 'B', n / FS); }
        std::printf ("mic %c (after %.0f s of learning): %d events, worst pass %.1f %%, mean pass %.1f %%, wrongly open while only the other speaks: %.0f ms\n",
                     s == 0 ? 'A' : 'B', skip, cnt, 100 * worst, cnt ? 100 * sum / cnt : 0.0, leak);
    }
    return 0;
}
