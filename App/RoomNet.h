#pragma once
// ---------------------------------------------------------------------------
//  RoomNet - UDP transport for the room app: discovery, clock/RTT measurement and
//  the exchange of envelope frames. No audio ever leaves the computer.
//
//  Packet layout (little endian):  header(16) + payload
//    header : u32 magic 'GPRO', u8 version, u8 type, u16 reserved, u32 roomHash, u32 senderId
//    Hello  : u8 nameLen, name bytes
//    Ping   : f64 sendTime            Pong : f64 echoedSendTime
//    Frames : u32 seq, f64 senderTime(first frame), f64 hopSeconds, u8 count, u8 redundant, u16 pad,
//             count x { 15 x f32 (pFast, pSlow, noise floor per band), u8 active, u8[3] pad }
// ---------------------------------------------------------------------------

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include "RoomEngine.h"
#include <array>
#include <deque>
#include <map>

class RoomNet : private juce::Thread
{
public:
    static constexpr int kDefaultPort = 47810;

    struct PeerInfo { juce::String name; int slot = -1; double rttMs = 0.0; };

    RoomNet (automix::RoomEngine& e, int udpPort = kDefaultPort)
        : juce::Thread ("RoomNet"), engine (e), port (udpPort)
    {
        do { selfId = (uint32_t) juce::Random::getSystemRandom().nextInt(); } while (selfId == 0);
        computerName = juce::SystemStats::getComputerName().substring (0, 40);
        setRoom ("room");
        engine.onSendFrames = [this] (uint32_t seq, double t, double hop, const automix::RemoteFrame* f, int n, int red)
        {
            queueFrames (seq, t, hop, f, n, red);
        };
    }

    ~RoomNet() override { engine.onSendFrames = nullptr; stopThread (2000); }

    bool start()
    {
        bound = socket.bindToPort (port);
        if (! bound) return false;
        startThread (juce::Thread::Priority::high);
        return true;
    }
    bool isBound() const { return bound; }

    void setRoom (const juce::String& name)
    {
        // case-insensitive FNV-1a hash of the trimmed room name
        uint32_t h = 2166136261u;
        for (unsigned char c : name.trim().toLowerCase().toStdString()) { h ^= c; h *= 16777619u; }
        roomHash.store (h);
        const juce::ScopedLock sl (lock);
        resetPeersFlag = true;
    }

    // for networks that block broadcasts: connect straight to an address
    void addManualPeer (const juce::String& address)
    {
        const juce::ScopedLock sl (lock);
        manualTargets.addIfNotAlreadyThere (address.trim());
    }

    std::vector<PeerInfo> getPeers() const
    {
        const juce::ScopedLock sl (lock);
        return peerInfos;
    }

    // ---- audio thread: hand a finished packet to the network thread ----
    void queueFrames (uint32_t seq, double t, double hop, const automix::RemoteFrame* f, int n, int red)
    {
        n = std::min (n, automix::RoomEngine::kMaxFramesPerPacket);
        int s1, z1, s2, z2;
        fifo.prepareToWrite (1, s1, z1, s2, z2);
        if (z1 < 1) return;                                           // network thread too slow: drop
        auto& o = out[(size_t) s1];
        uint8_t* p = o.data.data();
        writeHeader (p, 4);
        size_t off = sizeof (Header);
        const uint8_t cnt = (uint8_t) n, rd = (uint8_t) std::min (red, n);
        const uint16_t pad = 0;
        std::memcpy (p + off, &seq, 4);  off += 4;
        std::memcpy (p + off, &t, 8);    off += 8;
        std::memcpy (p + off, &hop, 8);  off += 8;
        std::memcpy (p + off, &cnt, 1);  off += 1;
        std::memcpy (p + off, &rd, 1);   off += 1;
        std::memcpy (p + off, &pad, 2);  off += 2;
        for (int i = 0; i < n; ++i)
        {
            WireFrame w {};
            for (int b = 0; b < automix::kBands; ++b) { w.p[b] = f[i].pFast[b]; w.p[5 + b] = f[i].pSlow[b]; w.p[10 + b] = f[i].nf[b]; }
            w.active = f[i].active;
            std::memcpy (p + off, &w, sizeof (WireFrame)); off += sizeof (WireFrame);
        }
        o.len = (int) off;
        fifo.finishedWrite (1);
    }

private:
#pragma pack(push, 1)
    struct Header { uint32_t magic; uint8_t version, type; uint16_t reserved; uint32_t room; uint32_t sender; };
    struct WireFrame { float p[15]; uint8_t active; uint8_t pad[3]; };
#pragma pack(pop)
    static_assert (sizeof (Header) == 16 && sizeof (WireFrame) == 64, "wire layout");
    enum Type : uint8_t { Hello = 1, Ping = 2, Pong = 3, Frames = 4 };
    static constexpr uint32_t kMagic = 0x4F525047u;

    struct OutPacket { std::array<uint8_t, 1200> data; int len = 0; };
    struct Peer
    {
        juce::String ip; int port = 0; juce::String name; int slot = -1;
        double lastSeen = 0, nextPing = 0;
        std::deque<double> rtts; double rttMin = 0.0;
    };

    static double now() { return juce::Time::getMillisecondCounterHiRes() * 0.001; }

    void writeHeader (uint8_t* p, uint8_t type) const
    {
        Header h { kMagic, 1, type, 0, roomHash.load(), selfId };
        std::memcpy (p, &h, sizeof (h));
    }

    void sendTo (const juce::String& ip, int udpPort, const void* data, int len)
    {
        socket.write (ip, udpPort, data, len);
    }

    void sendHello (const juce::String& ip, int udpPort)
    {
        uint8_t buf[80];
        writeHeader (buf, Hello);
        const auto utf8 = computerName.toUTF8();
        const int len = (int) std::min<size_t> (utf8.sizeInBytes() - 1, 48);
        buf[sizeof (Header)] = (uint8_t) len;
        std::memcpy (buf + sizeof (Header) + 1, utf8.getAddress(), (size_t) len);
        sendTo (ip, udpPort, buf, (int) sizeof (Header) + 1 + len);
    }

    void broadcastHello()
    {
        sendHello ("255.255.255.255", port);
        for (const auto& a : juce::IPAddress::getAllAddresses())
        {
            if (a.isIPv6 || a == juce::IPAddress::local()) continue;
            sendHello (juce::IPAddress::getInterfaceBroadcastAddress (a).toString(), port);
        }
        juce::StringArray manual;
        { const juce::ScopedLock sl (lock); manual = manualTargets; }
        for (auto& m : manual)
        {
            const int colon = m.lastIndexOfChar (':');
            if (colon > 0) sendHello (m.substring (0, colon), m.substring (colon + 1).getIntValue());   // "host:port" (used for tests)
            else           sendHello (m, port);
        }
    }

    Peer& getPeer (uint32_t id, const juce::String& ip, int fromPort, double t)
    {
        auto it = peers.find (id);
        if (it == peers.end())
        {
            Peer p; p.ip = ip; p.port = fromPort; p.lastSeen = t; p.nextPing = t;
            p.slot = engine.addPeer (id);
            it = peers.emplace (id, p).first;
            sendHello (ip, fromPort);                       // let them find us right away
        }
        auto& p = it->second;
        p.ip = ip; p.port = fromPort; p.lastSeen = t;
        return p;
    }

    void handle (const uint8_t* d, int n, const juce::String& ip, int fromPort, double arrival)
    {
        if (n < (int) sizeof (Header)) return;
        Header h; std::memcpy (&h, d, sizeof (h));
        if (h.magic != kMagic || h.version != 1 || h.room != roomHash.load() || h.sender == selfId) return;
        auto& peer = getPeer (h.sender, ip, fromPort, arrival);
        if (peer.slot < 0) return;                           // room is full
        const uint8_t* pl = d + sizeof (Header);
        const int plen = n - (int) sizeof (Header);

        switch (h.type)
        {
            case Hello:
                if (plen >= 1 && plen >= 1 + pl[0]) peer.name = juce::String::fromUTF8 ((const char*) pl + 1, pl[0]);
                break;
            case Ping:
                if (plen >= 8)
                {
                    uint8_t buf[sizeof (Header) + 8];
                    writeHeader (buf, Pong);
                    std::memcpy (buf + sizeof (Header), pl, 8);
                    sendTo (ip, fromPort, buf, (int) sizeof (buf));
                }
                break;
            case Pong:
                if (plen >= 8)
                {
                    double t0; std::memcpy (&t0, pl, 8);
                    const double rtt = arrival - t0;
                    if (rtt >= 0.0 && rtt < 1.0)
                    {
                        peer.rtts.push_back (rtt);
                        if (peer.rtts.size() > 16) peer.rtts.pop_front();
                        peer.rttMin = *std::min_element (peer.rtts.begin(), peer.rtts.end());
                        engine.setOneWay (h.sender, peer.rttMin * 0.5);
                    }
                }
                break;
            case Frames:
                if (plen >= 24)
                {
                    uint32_t seq; double t, hop; uint8_t cnt, red;
                    std::memcpy (&seq, pl, 4); std::memcpy (&t, pl + 4, 8); std::memcpy (&hop, pl + 12, 8);
                    cnt = pl[20]; red = pl[21];
                    if (cnt > automix::RoomEngine::kMaxFramesPerPacket || plen < 24 + (int) cnt * (int) sizeof (WireFrame) || hop <= 0.0 || hop > 0.1) break;
                    automix::RemoteFrame fr[automix::RoomEngine::kMaxFramesPerPacket];
                    for (int i = 0; i < cnt; ++i)
                    {
                        WireFrame w; std::memcpy (&w, pl + 24 + i * sizeof (WireFrame), sizeof (w));
                        for (int b = 0; b < automix::kBands; ++b) { fr[i].pFast[b] = w.p[b]; fr[i].pSlow[b] = w.p[5 + b]; fr[i].nf[b] = w.p[10 + b]; }
                        fr[i].active = w.active;
                    }
                    engine.pushFrames (h.sender, seq, t, hop, fr, cnt, std::min<int> (red, cnt), arrival);
                }
                break;
            default: break;
        }
    }

    void run() override
    {
        double nextHello = 0.0;
        uint8_t buf[1600];
        while (! threadShouldExit())
        {
            // ---- receive ----
            int ready = socket.waitUntilReady (true, 1);
            int guard = 0;
            while (ready == 1 && guard++ < 200)
            {
                juce::String ip; int fromPort = 0;
                const int n = socket.read (buf, (int) sizeof (buf), false, ip, fromPort);
                if (n > 0) handle (buf, n, ip, fromPort, now());
                ready = socket.waitUntilReady (true, 0);
            }

            const double t = now();

            // ---- send queued frame packets to everybody ----
            int s1, z1, s2, z2;
            fifo.prepareToRead (fifo.getNumReady(), s1, z1, s2, z2);
            for (int i = 0; i < z1 + z2; ++i)
            {
                const auto& o = out[(size_t) (i < z1 ? s1 + i : s2 + (i - z1))];
                for (auto& kv : peers) sendTo (kv.second.ip, kv.second.port, o.data.data(), o.len);
            }
            fifo.finishedRead (z1 + z2);

            // ---- housekeeping ----
            if (t >= nextHello) { broadcastHello(); nextHello = t + 1.0; }
            for (auto& kv : peers)
            {
                auto& p = kv.second;
                if (t >= p.nextPing)
                {
                    uint8_t b[sizeof (Header) + 8];
                    writeHeader (b, Ping);
                    std::memcpy (b + sizeof (Header), &t, 8);
                    sendTo (p.ip, p.port, b, (int) sizeof (b));
                    p.nextPing = t + (p.rtts.size() < 8 ? 0.1 : 0.5);   // measure quickly at first
                }
            }
            bool reset = false;
            { const juce::ScopedLock sl (lock); reset = resetPeersFlag; resetPeersFlag = false; }
            for (auto it = peers.begin(); it != peers.end();)
            {
                if (reset || t - it->second.lastSeen > 3.0) { engine.removePeer (it->first); it = peers.erase (it); }
                else ++it;
            }
            {
                std::vector<PeerInfo> infos;
                for (auto& kv : peers) infos.push_back ({ kv.second.name.isEmpty() ? juce::String ("Peer") : kv.second.name, kv.second.slot, kv.second.rttMin * 1000.0 });
                const juce::ScopedLock sl (lock);
                peerInfos = std::move (infos);
            }
        }
    }

    automix::RoomEngine& engine;
    int port;
    juce::DatagramSocket socket { true };
    bool bound = false;
    uint32_t selfId = 1;
    juce::String computerName;
    std::atomic<uint32_t> roomHash { 0 };
    std::map<uint32_t, Peer> peers;                       // network thread only

    juce::CriticalSection lock;
    std::vector<PeerInfo> peerInfos;
    juce::StringArray manualTargets;
    bool resetPeersFlag = false;

    juce::AbstractFifo fifo { 64 };
    std::array<OutPacket, 64> out;
};
