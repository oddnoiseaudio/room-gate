# oddnoise Room Gate

A Windows app for Discord calls where two or more people sit in the same room, each on their own PC with their own Discord. Without it, everyone is heard twice: once directly and once as crosstalk in the neighbour's microphone.

Room Gate sits between your microphone and Discord. Each PC only sends its owner's voice, and the apps gate each other's mics automatically. Short interjections such as "mhm" still get through.

## How it works

The apps find each other on the local network and exchange tiny level envelopes (5 frequency bands, no audio). Each app uses them to learn how much of the neighbour's voice leaks into its own mic, subtracts that, and opens the mic only if own speech remains. A short lookahead (60 ms by default) keeps the first syllable intact. The detection technique is the same as in [oddnoise Podcast Automix](https://github.com/oddnoiseaudio/podcast-automix), but this is a separate application.

## Installation (once per PC)

1. Install the free virtual audio cable [VB-CABLE](https://vb-audio.com/Cable/) and restart.
2. Download `oddnoise-Room-Gate-Windows.zip` from the [Releases](../../releases) page, unzip it and run `oddnoise Room Gate.exe`. Allow it through the Windows firewall for private networks (UDP port 47810).
3. Choose your microphone. The output is set to *CABLE Input* automatically if the cable is found.
4. In Discord: Settings > Voice > Input Device = *CABLE Output*. Turn **off** noise suppression, echo cancellation and automatic input sensitivity.
5. Everyone enters the same room name (default `room`). Peers appear automatically and a light shows who is speaking.

## Notes

- Your voice is delayed by about 60 ms (100 ms with *Large* buffer for unstable Wi-Fi).
- Without a connected peer your mic stays open.
- If discovery is blocked on your network, enter the other PC's IP address under *Connect by address*.
- If the neighbour's voice still leaks through, raise *Separation* or try a negative *Timing trim*.
- Tested in simulation and with two instances over localhost; not yet tested on real PCs.

## Building from source

Windows (Visual Studio 2022 or Build Tools, plus CMake):

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target OddnoiseRoomGate
```

Result: `build\OddnoiseRoomGate_artefacts\Release\oddnoise Room Gate.exe`. JUCE is fetched automatically; use `-DJUCE_DIR=<path>` for a local checkout.

Simulation test (no JUCE needed): `g++ -O2 -std=c++17 -ISource -ITests Tests/test_room.cpp -o room_test && ./room_test`

## Layout

- `Source/AutomixCore.h`: bleed-compensated detection and gating (no JUCE)
- `Source/RoomEngine.h`: network alignment, jitter buffer, fail-open logic (no JUCE)
- `App/RoomNet.h`: UDP discovery and transport
- `App/Main.cpp`: user interface and audio device handling
- `Tests/`: simulation and localhost smoke test

## License

This project uses [JUCE](https://juce.com) under the AGPLv3 and is therefore licensed under the GNU Affero General Public License v3.0. See `LICENSE`.
