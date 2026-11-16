#pragma once
// ---------------------------------------------------------------------------
//  RoomEngine - everything the "room" app does between audio and network.
//  Pure C++17 (no JUCE) so it can be simulated and tested without audio/network hardware.
//
//  * one local microphone (mono) is analysed and gated by automix::Core
//  * every hop (2 ms) the band envelopes of the local mic are sent to the other peers
//  * envelopes of the other peers arrive in per-peer jitter buffers and are fed to the core
//    as "Remote" channels, so a quiet reaction can be told apart from a neighbour's bleed
//  * the local envelopes are delayed so they line up in time with the network-delayed remote ones
//    (alignment = age of the consumed remote frame, measured with sender timestamps and the
//    clock offset from a min-filter + RTT/2), the audio itself is delayed by the lookahead so the
//    gate decision is ready in time
//
//  Threads: process() runs on the audio thread. addPeer/removePeer/setOneWay/pushFrames are called
//  from the network thread. Everything shared is atomic or a single-producer/single-consumer ring.
//  All times are seconds on a steady clock; the audio thread passes the wall time of the block start.
// ---------------------------------------------------------------------------

#include "AutomixCore.h"
#include <atomic>
#include <functional>

namespace automix
{
class RoomEngine
{
public:
    static constexpr int kMaxPeers = kMaxChannels - 1;      // channel 0 is the local mic
    static constexpr double kHopMs = 2.0;                   // envelope frame interval
    static constexpr int kMaxFramesPerPacket = 16;

    // called on the audio thread at the end of every block that produced frames
    // the first `redundant` frames were already sent in the previous packet (loss protection)
    std::function<void (uint32_t firstSeq, double firstTimeSec, double hopSec, const RemoteFrame* frames, int count, int redundant)> onSendFrames;

    RoomEngine()
    {
        coreParams.closedDb = -80.0f;
        coreParams.holdMs = 150.0f;
        coreParams.thresholdDb = -50.0f;
        coreParams.marginDb = 4.0f;
    }

    void prepare (double sampleRate, int maxBlockSize)
    {
        fs = sampleRate;
        hop = std::max (8, (int) std::lround (kHopMs * 0.001 * fs));
        hopSec = (double) hop / fs;
        lookahead = (int) std::lround (lookaheadMs * 0.001 * fs);
        maxAlign = std::max (0, lookahead - (int) std::lround (0.008 * fs));
        core.prepare (fs, kMaxChannels, lookahead, maxAlign);
        for (int c = 1; c < kMaxChannels; ++c) core.setMode (c, ChannelMode::Off);
        coreParams.lookaheadSamples = lookahead;
        coreParams.forceOpen = true;
        core.setParams (coreParams);
        scratch.assign ((size_t) std::max (1, maxBlockSize), 0.0f);
        hopPos = 0; sendSeq = 0; pending.clear(); history.clear(); pending.reserve (64); history.reserve (64); lastPeers = -1;
        for (auto& s : slots) { if (s.inUse.load()) s.resetConsumer(); else s.reset(); }
        alignCur = 0.0;
    }

    // ---- settings (call from the UI thread; applied at the next block) ----
    void setUserParams (float thresholdDb, float marginDb)
    {
        pThresh.store (thresholdDb); pMargin.store (marginDb); paramsDirty.store (true);
    }
    void setLookaheadMs (float ms) { lookaheadMs = ms; }     // before prepare()
    // manual timing trim in ms (+ = our envelopes are delayed less). Use if the neighbour's voice opens your mic.
    void setAlignTrimMs (float ms) { alignTrimMs.store (ms); }

    // ---- network thread API ----
    int addPeer (uint32_t id)
    {
        for (int i = 0; i < kMaxPeers; ++i)
            if (slots[(size_t) i].inUse.load() && slots[(size_t) i].id.load() == id) return i;
        for (int i = 0; i < kMaxPeers; ++i)
        {
            auto& s = slots[(size_t) i];
            if (! s.inUse.load())
            {
                s.id.store (id);
                s.started = false; s.expected = 0; s.lastEntry = Entry {};
                s.winMin[0] = s.winMin[1] = 1e9; s.winMax[0] = s.winMax[1] = -1e9; s.winStart = -1.0;
                s.xMin.store (0.0); s.xMax.store (0.0); s.oneWay.store (0.004);
                s.needReset.store (true);
                s.inUse.store (true);
                return i;
            }
        }
        return -1;
    }
    void removePeer (uint32_t id)
    {
        for (auto& s : slots) if (s.inUse.load() && s.id.load() == id) s.inUse.store (false);
    }
    void setOneWay (uint32_t id, double seconds)
    {
        for (auto& s : slots) if (s.inUse.load() && s.id.load() == id) s.oneWay.store (std::clamp (seconds, 0.0, 0.2));
    }
    // frames of one packet; senderTimeSec = sender time of the first frame, arrivalSec = our clock when it arrived
    void pushFrames (uint32_t id, uint32_t seq, double senderTimeSec, double senderHopSec,
                     const RemoteFrame* f, int count, int redundant, double arrivalSec)
    {
        for (auto& s : slots)
        {
            if (! s.inUse.load() || s.id.load() != id) continue;
            for (int i = 0; i < count; ++i)
            {
                const uint32_t sq = seq + (uint32_t) i;
                const double tk = senderTimeSec + i * senderHopSec;
                if (! s.started) { s.started = true; s.expected = sq; }
                const int32_t diff = (int32_t) (sq - s.expected);
                if (diff < 0) continue;                              // late duplicate
                if (diff > 0)                                        // lost packets: keep the timeline by repeating the last frame
                    for (int k = 1; k <= std::min (diff, 50); ++k)
                    {
                        Entry e = s.lastEntry; e.t = tk - (std::min (diff, 50) - k + 1) * senderHopSec; s.push (e);
                    }
                Entry e; e.f = f[i]; e.t = tk;
                s.push (e);
                s.lastEntry = e;
                s.expected = sq + 1;

                if (i < redundant) continue;                         // repeated frames arrive late by design: ignore for timing stats
                // clock offset: smallest (arrival - sender time) over a sliding 2 x 2 s window = offset + best-case one-way delay
                const double x = arrivalSec - tk;
                if (s.winStart < 0) s.winStart = arrivalSec;
                if (arrivalSec - s.winStart > 2.0)
                {
                    s.winMin[1] = s.winMin[0]; s.winMin[0] = 1e9;
                    s.winMax[1] = s.winMax[0]; s.winMax[0] = -1e9;
                    s.winStart = arrivalSec;
                }
                s.winMin[0] = std::min (s.winMin[0], x);
                s.winMax[0] = std::max (s.winMax[0], x);
                s.xMin.store (std::min (s.winMin[0], s.winMin[1]));
                s.xMax.store (std::max (s.winMax[0], s.winMax[1]));
            }
            return;
        }
    }

    // ---- audio thread ----
    // in/out: mono. gainOut (optional): applied gain per sample. nowSec: clock at the start of this block.
    void process (const float* in, float* out, int n, float* gainOut, double nowSec)
    {
        if (n > (int) scratch.size()) scratch.resize ((size_t) n);
        std::copy (in, in + n, scratch.begin());
        float* io[kMaxChannels] = {};
        float* gio[kMaxChannels] = {};

        updatePeers();
        if (paramsDirty.exchange (false))
        {
            coreParams.thresholdDb = pThresh.load(); coreParams.marginDb = pMargin.load();
            core.setParams (coreParams);
        }

        int done = 0;
        while (done < n)
        {
            if (hopPos == 0) consumeRemote (nowSec + (double) done / fs);
            const int chunk = std::min (hop - hopPos, n - done);
            io[0] = scratch.data() + done;
            gio[0] = gainOut != nullptr ? gainOut + done : nullptr;
            core.process (io, chunk, gainOut != nullptr ? gio : nullptr);
            hopPos += chunk; done += chunk;
            if (hopPos >= hop)
            {
                hopPos = 0;
                pending.push_back (core.getLocalFrame (0));
                if ((int) pending.size() >= kMaxFramesPerPacket) flushFrames();
            }
        }
        flushFrames();
        std::copy (scratch.begin(), scratch.begin() + n, out);

        levelDb.store (core.getLevelDb (0));
        localOpen.store (core.isActive (0));
        unsigned mask = 0;
        for (int i = 0; i < kMaxPeers; ++i) if (slots[(size_t) i].inUse.load() && core.isActive (i + 1)) mask |= 1u << i;
        peerOpenMask.store (mask);
    }

    // ---- status for the UI ----
    int    getNumPeers() const { int c = 0; for (auto& s : slots) if (s.inUse.load()) ++c; return c; }
    bool   isLocalOpen() const { return localOpen.load(); }
    float  getLocalLevelDb() const { return levelDb.load(); }
    unsigned getPeerOpenMask() const { return peerOpenMask.load(); }
    double getAlignMs() const { return alignMsStat.load(); }
    int    getLatencySamples() const { return lookahead; }
    double getSampleRate() const { return fs; }
    double getHopSeconds() const { return hopSec; }

private:
    static constexpr int kRing = 256;

    struct Entry { RemoteFrame f; double t = 0.0; };

    struct Slot
    {
        std::atomic<bool> inUse { false }, needReset { false };
        std::atomic<uint32_t> id { 0 };
        std::atomic<double> oneWay { 0.004 }, xMin { 0.0 }, xMax { 0.0 };
        // producer (network thread)
        bool started = false; uint32_t expected = 0; Entry lastEntry;
        double winMin[2] = { 1e9, 1e9 }, winMax[2] = { -1e9, -1e9 }; double winStart = -1.0;
        std::array<Entry, kRing> buf;
        std::atomic<uint32_t> w { 0 }, r { 0 };
        // consumer (audio thread)
        bool wasInUse = false, primed = false, haveAlign = false;
        int target = 6;                 // jitter-buffer depth in frames
        int extra = 0;                  // extra frames to line up with slower peers
        int hopsSinceUnderrun = 0, sinceChange = 0;
        double alignSec = 0.0;          // smoothed age of the frames we feed to the core
        RemoteFrame current;

        void reset()
        {
            inUse.store (false); w.store (0); r.store (0);
            wasInUse = primed = haveAlign = false; target = 6; extra = 0; hopsSinceUnderrun = 0; alignSec = 0.0; current = RemoteFrame {};
        }
        void resetConsumer()
        {
            r.store (w.load());
            wasInUse = primed = haveAlign = false; target = 6; extra = 0; hopsSinceUnderrun = 0; sinceChange = 0; alignSec = 0.0; current = RemoteFrame {};
        }
        void push (const Entry& e)
        {
            const uint32_t wi = w.load (std::memory_order_relaxed);
            if (wi - r.load (std::memory_order_acquire) >= (uint32_t) kRing) return;   // full: drop
            buf[wi % kRing] = e;
            w.store (wi + 1, std::memory_order_release);
        }
        int queued() const { return (int) (w.load (std::memory_order_acquire) - r.load (std::memory_order_relaxed)); }
        bool pop (Entry& e)
        {
            const uint32_t ri = r.load (std::memory_order_relaxed);
            if (ri == w.load (std::memory_order_acquire)) return false;
            e = buf[ri % kRing];
            r.store (ri + 1, std::memory_order_release);
            return true;
        }
    };

    void flushFrames()
    {
        if (pending.empty()) return;
        // every packet repeats the most recent frames of the previous one, so a single lost packet costs nothing
        const int red = std::min ((int) history.size(), std::max (0, kMaxFramesPerPacket - (int) pending.size()));
        std::vector<RemoteFrame> out (history.end() - red, history.end());
        out.insert (out.end(), pending.begin(), pending.end());
        if (onSendFrames) onSendFrames (sendSeq - (uint32_t) red, (double) (sendSeq - (uint32_t) red) * hopSec, hopSec, out.data(), (int) out.size(), red);
        sendSeq += (uint32_t) pending.size();
        history.insert (history.end(), pending.begin(), pending.end());
        if (history.size() > 10) history.erase (history.begin(), history.end() - 10);
        pending.clear();
    }

    // adds/removes remote channels in the core and keeps the alignment up to date (once per block)
    void updatePeers()
    {
        int peers = 0;
        double maxDelay = 0.0;
        for (int i = 0; i < kMaxPeers; ++i)
        {
            auto& s = slots[(size_t) i];
            const bool use = s.inUse.load();
            if (use && ! s.wasInUse)
            {
                if (s.needReset.exchange (false)) s.r.store (s.w.load());
                s.primed = false; s.haveAlign = false; s.target = 6; s.extra = 0; s.hopsSinceUnderrun = 0;
                core.setMode (i + 1, ChannelMode::Remote);
            }
            else if (! use && s.wasInUse)
                core.setMode (i + 1, ChannelMode::Off);
            s.wasInUse = use;
            if (use)
            {
                // jitter-buffer depth: must cover the spread of arrival delays (batching + network jitter) seen recently
                const double spread = std::max (0.0, s.xMax.load() - s.xMin.load());
                const int want = std::clamp ((int) std::ceil ((spread + 0.003) / hopSec), 3, 40);
                if (want > s.target) { s.target = want; s.sinceChange = 0; }
                else if (want + 2 < s.target && ++s.sinceChange > 250) { --s.target; s.sinceChange = 0; }   // ~2.5 s of calm: shrink slowly
                ++peers;
                if (s.haveAlign) maxDelay = std::max (maxDelay, s.alignSec);
            }
        }
        if (peers != lastPeers)
        {
            coreParams.forceOpen = (peers == 0);       // nobody else here: the mic simply stays open
            core.setParams (coreParams);
            lastPeers = peers;
        }
        // The neighbour's voice needs ~3 ms (1 m) to reach our mic, so our own envelope has to be delayed that much less.
        const double want = std::clamp ((maxDelay - acousticSec - (double) alignTrimMs.load() * 0.001) * fs, 0.0, (double) maxAlign);
        if (peers > 0 && maxDelay > 0.0)
        {
            const double step = 0.0005 * fs;           // glide: at most 0.5 ms per block
            alignCur += std::clamp (want - alignCur, -step, step);
        }
        core.setAlignDelay ((int) std::lround (alignCur));
        alignMsStat.store (alignCur / fs * 1000.0);

        // several peers: give the faster ones extra buffering so everybody lines up with the slowest (once per ~0.5 s)
        if (++extraTick >= 50)
        {
            extraTick = 0;
            for (int i = 0; i < kMaxPeers; ++i)
            {
                auto& s = slots[(size_t) i];
                if (! s.wasInUse || ! s.haveAlign) continue;
                if (maxDelay - s.alignSec > 1.5 * hopSec && s.extra < 20) ++s.extra;
                else if (maxDelay - s.alignSec < 0.5 * hopSec && s.extra > 0 && s.alignSec >= maxDelay - 1e-9) --s.extra;
            }
        }
    }

    // once per hop: take one envelope frame per peer out of its jitter buffer and give it to the core
    void consumeRemote (double wallSec)
    {
        for (int i = 0; i < kMaxPeers; ++i)
        {
            auto& s = slots[(size_t) i];
            if (! s.wasInUse) continue;
            const int depth = s.target + s.extra;
            int q = s.queued();
            if (! s.primed)
            {
                if (q >= depth) s.primed = true;
                else { core.setRemoteFrame (i + 1, s.current); continue; }
            }
            while (q > depth + 2) { Entry drop; s.pop (drop); --q; }     // too much backlog: skip ahead
            Entry e;
            if (s.pop (e))
            {
                s.current = e.f; ++s.hopsSinceUnderrun;
                // age of this frame in "our" time: our clock now minus its sender time, corrected by the clock offset
                const double offset = s.xMin.load() - s.oneWay.load();
                const double age = wallSec - e.t - offset;
                if (! s.haveAlign) { s.alignSec = age; s.haveAlign = true; }
                else s.alignSec += 0.01 * (age - s.alignSec);
            }
            else
            {
                // underrun: hold the last frame, refill, and make the buffer a little deeper
                s.primed = false; s.target = std::min (40, s.target + 1); s.hopsSinceUnderrun = 0; s.sinceChange = 0;
            }
            core.setRemoteFrame (i + 1, s.current);
        }
    }

    double fs = 48000.0, hopSec = 0.002;
    int hop = 96, hopPos = 0, lookahead = 0, maxAlign = 0, lastPeers = -1, extraTick = 0;
    float lookaheadMs = 60.0f;
public:
    // Our envelopes are delayed 3 ms *more* than the measured age: that is the safe side (a slightly late local view only
    // makes us treat the neighbour's tail as bleed for a moment). It also absorbs a few ms of input-latency difference between the PCs.
    double acousticSec = -0.003;
private:
    Core core;
    Params coreParams;
    std::vector<float> scratch;
    std::array<Slot, kMaxPeers> slots;
    std::vector<RemoteFrame> pending, history;
    uint32_t sendSeq = 0;
    double alignCur = 0.0;

    std::atomic<float> pThresh { -50.0f }, pMargin { 4.0f }, levelDb { -100.0f }, alignTrimMs { 0.0f };
    std::atomic<bool> paramsDirty { false }, localOpen { false };
    std::atomic<unsigned> peerOpenMask { 0 };
    std::atomic<double> alignMsStat { 0.0 };
};
} // namespace automix
