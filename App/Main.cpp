// oddnoise Room Gate
// Two (or more) people sit in the same room, each on their own computer with their own Discord.
// Every computer runs this app between the microphone and Discord, so a voice is only sent by the
// computer whose owner is actually talking. The apps exchange small level envelopes over the local
// network (no audio) to tell "my own voice" from "my neighbour's voice leaking into my mic".

#include <juce_audio_utils/juce_audio_utils.h>
#include "RoomEngine.h"
#include "RoomNet.h"

using namespace juce;

namespace
{
const Colour kBg      { 0xff17191c };
const Colour kPanel   { 0xff22262b };
const Colour kAccent  { 0xff39d98a };
const Colour kWarn    { 0xffe0a030 };
const Colour kDim     { 0xff5b636c };
const Colour kText    { 0xffe6e9ec };

bool looksLikeVirtualCable (const String& name)
{
    return name.containsIgnoreCase ("CABLE Input") || name.containsIgnoreCase ("VB-Audio")
        || name.containsIgnoreCase ("Voicemeeter") || name.containsIgnoreCase ("Virtual Cable")
        || name.containsIgnoreCase ("VAC ") || name.containsIgnoreCase ("BlackHole") || name.containsIgnoreCase ("Loopback");
}
}

//==============================================================================
class MainComponent : public Component,
                      private Timer,
                      private AudioIODeviceCallback
{
public:
    MainComponent (int udpPort) : net (engine, udpPort)
    {
        PropertiesFile::Options o;
        o.applicationName = "oddnoise Room Gate";
        o.filenameSuffix = ".settings";
        o.osxLibrarySubFolder = "Application Support";
        o.folderName = "oddnoise Room Gate";
        props.setStorageParameters (o);
        auto* us = props.getUserSettings();

        // ---- controls ----
        addAndMakeVisible (inLabel);  addAndMakeVisible (outLabel);
        addAndMakeVisible (inBox);    addAndMakeVisible (outBox);
        addAndMakeVisible (roomLabel); addAndMakeVisible (roomEdit);
        addAndMakeVisible (bufLabel);  addAndMakeVisible (bufBox);
        addAndMakeVisible (sensLabel); addAndMakeVisible (sens);
        addAndMakeVisible (sepLabel);  addAndMakeVisible (sep);
        addAndMakeVisible (trimLabel); addAndMakeVisible (trim);
        addAndMakeVisible (audioBtn);
        addAndMakeVisible (ipEdit); addAndMakeVisible (ipBtn); addAndMakeVisible (ipLabel);
        addAndMakeVisible (hint);

        for (auto* l : { &inLabel, &outLabel, &roomLabel, &bufLabel, &sensLabel, &sepLabel, &trimLabel, &ipLabel })
        {
            l->setColour (Label::textColourId, kText);
            l->setFont (FontOptions (13.0f));
        }
        inLabel.setText ("Microphone", dontSendNotification);
        outLabel.setText ("Output (to Discord)", dontSendNotification);
        roomLabel.setText ("Room name", dontSendNotification);
        bufLabel.setText ("Network buffer", dontSendNotification);
        sensLabel.setText ("Mic sensitivity", dontSendNotification);
        sepLabel.setText ("Separation", dontSendNotification);
        trimLabel.setText ("Timing trim (ms)", dontSendNotification);
        ipLabel.setText ("Connect by address (only if rooms are not found automatically)", dontSendNotification);

        hint.setReadOnly (true); hint.setMultiLine (true, true); hint.setScrollbarsShown (false);
        hint.setColour (TextEditor::backgroundColourId, Colours::transparentBlack);
        hint.setColour (TextEditor::outlineColourId, Colours::transparentBlack);
        hint.setColour (TextEditor::textColourId, kText);
        hint.setFont (FontOptions (13.0f));

        roomEdit.setText (us->getValue ("room", "room"), dontSendNotification);
        roomEdit.onTextChange = [this] { settingsDirty = true; };
        roomEdit.onReturnKey = [this] { applyRoom(); };
        roomEdit.onFocusLost = [this] { applyRoom(); };

        bufBox.addItem ("Normal (60 ms)", 1);
        bufBox.addItem ("Large (100 ms) - unstable Wi-Fi", 2);
        bufBox.setSelectedId (us->getIntValue ("buffer", 1), dontSendNotification);
        bufBox.onChange = [this] { settingsDirty = true; applyBuffer(); };

        auto setupSlider = [] (Slider& s, double lo, double hi, double step, double v, const String& suffix)
        {
            s.setSliderStyle (Slider::LinearHorizontal);
            s.setTextBoxStyle (Slider::TextBoxRight, false, 70, 20);
            s.setRange (lo, hi, step); s.setValue (v, dontSendNotification); s.setTextValueSuffix (suffix);
            s.setColour (Slider::trackColourId, kAccent);
            s.setColour (Slider::thumbColourId, kText);
            s.setColour (Slider::textBoxTextColourId, kText);
            s.setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
        };
        setupSlider (sens, -70.0, -30.0, 0.5, us->getDoubleValue ("threshold", -50.0), " dB");
        setupSlider (sep,  0.0, 12.0, 0.1, us->getDoubleValue ("margin", 4.0), " dB");
        setupSlider (trim, -10.0, 10.0, 0.5, us->getDoubleValue ("trim", 0.0), " ms");
        sens.onValueChange = [this] { settingsDirty = true; applyParams(); };
        sep.onValueChange  = [this] { settingsDirty = true; applyParams(); };
        trim.onValueChange = [this] { settingsDirty = true; applyParams(); };
        sens.setTooltip ("Lower = even very quiet speech opens your mic. Raise it if room noise opens it.");
        sep.setTooltip ("Higher = cleaner separation from your neighbour, but very quiet interjections may be cut. Lower = more sensitive.");
        trim.setTooltip ("Only needed if your neighbour's voice still gets through: try a negative value.");

        audioBtn.setButtonText ("Audio settings...");
        audioBtn.onClick = [this] { showAudioSettings(); };
        ipBtn.setButtonText ("Connect");
        ipBtn.onClick = [this] { if (ipEdit.getText().isNotEmpty()) net.addManualPeer (ipEdit.getText()); };

        // ---- audio ----
        std::unique_ptr<XmlElement> saved (us->getXmlValue ("audioDeviceState"));
        if (saved == nullptr)
            for (auto* type : deviceManager.getAvailableDeviceTypes())
                if (type->getTypeName() == "Windows Audio") { deviceManager.setCurrentAudioDeviceType ("Windows Audio", true); break; }
        const auto err = deviceManager.initialise (1, 2, saved.get(), true);
        if (err.isNotEmpty()) lastError = err;
        outputConfirmed = us->getBoolValue ("outputConfirmed", false);
        if (! outputConfirmed) autoPickCable();

        engine.setLookaheadMs (bufBox.getSelectedId() == 2 ? 100.0f : 60.0f);
        applyParams();
        net.setRoom (roomEdit.getText());
        if (! net.start()) lastError = "Network port " + String (RoomNet::kDefaultPort) + " is already in use. Is the app running twice?";
        deviceManager.addAudioCallback (this);

        refreshDeviceLists();
        inBox.onChange  = [this] { chooseDevice (true); };
        outBox.onChange = [this] { chooseDevice (false); };

        setSize (560, 640);
        startTimerHz (30);
    }

    ~MainComponent() override
    {
        stopTimer();
        deviceManager.removeAudioCallback (this);
        saveSettings();
    }

    void paint (Graphics& g) override
    {
        g.fillAll (kBg);
        g.setColour (kText);
        g.setFont (FontOptions (20.0f, Font::bold));
        g.drawText ("oddnoise  Room Gate", 16, 8, 400, 28, Justification::centredLeft);

        // status
        auto peers = net.getPeers();
        const bool connected = ! peers.empty();
        g.setColour (kPanel);
        g.fillRoundedRectangle (statusArea.toFloat(), 6.0f);
        auto row = statusArea.reduced (12, 6);
        g.setColour (connected ? kAccent : kWarn);
        g.fillEllipse (row.removeFromLeft (14).withSizeKeepingCentre (12, 12).toFloat());
        row.removeFromLeft (8);
        g.setColour (kText);
        g.setFont (FontOptions (14.0f, Font::bold));
        g.drawText (statusText, row, Justification::centredLeft);

        // meters
        g.setColour (kPanel);
        g.fillRoundedRectangle (meterArea.toFloat(), 6.0f);
        const unsigned mask = engine.getPeerOpenMask();
        auto drawRow = [&] (Rectangle<int> r, const String& name, bool open, float lvlDb, bool showLevel)
        {
            r = r.reduced (12, 5);
            g.setColour (open ? kAccent : kDim.darker (0.4f));
            g.fillEllipse (r.removeFromLeft (20).withSizeKeepingCentre (14, 14).toFloat());
            r.removeFromLeft (8);
            g.setColour (kText);
            g.setFont (FontOptions (13.0f, Font::bold));
            g.drawText (name, r.removeFromLeft (150), Justification::centredLeft);
            g.setColour (open ? kAccent : kDim);
            g.setFont (FontOptions (12.0f));
            g.drawText (open ? "speaking" : "closed", r.removeFromRight (70), Justification::centredRight);
            if (showLevel)
            {
                auto bar = r.withSizeKeepingCentre (r.getWidth(), 10);
                g.setColour (kBg); g.fillRoundedRectangle (bar.toFloat(), 3.0f);
                const float n = jlimit (0.0f, 1.0f, (lvlDb + 90.0f) / 70.0f);
                g.setColour (open ? kAccent : kDim);
                g.fillRoundedRectangle (bar.withWidth ((int) (bar.getWidth() * n)).toFloat(), 3.0f);
            }
        };
        auto rows = meterArea;
        drawRow (rows.removeFromTop (30), "You", engine.isLocalOpen() || peers.empty(), engine.getLocalLevelDb(), true);
        int shown = 0;
        for (auto& p : peers)
        {
            if (++shown > 4) break;
            drawRow (rows.removeFromTop (30), p.name, p.slot >= 0 && ((mask >> p.slot) & 1u), 0.0f, false);
        }

        g.setColour (kDim);
        g.setFont (FontOptions (11.0f));
        g.drawText (latencyText, getLocalBounds().removeFromBottom (20).reduced (16, 0), Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        r.removeFromTop (34);
        statusArea = r.removeFromTop (34);
        r.removeFromTop (8);
        meterArea = r.removeFromTop (30 + 4 * 30 > 150 ? 150 : 150);
        r.removeFromTop (12);

        auto line = [&] (Label& l, Component& c)
        {
            auto row = r.removeFromTop (28);
            l.setBounds (row.removeFromLeft (150));
            c.setBounds (row);
            r.removeFromTop (6);
        };
        line (inLabel, inBox);
        line (outLabel, outBox);
        hint.setBounds (r.removeFromTop (58));
        r.removeFromTop (6);
        line (roomLabel, roomEdit);
        line (sensLabel, sens);
        line (sepLabel, sep);
        line (bufLabel, bufBox);
        line (trimLabel, trim);
        r.removeFromTop (4);
        audioBtn.setBounds (r.removeFromTop (28).removeFromLeft (150));
        r.removeFromTop (10);
        ipLabel.setBounds (r.removeFromTop (20));
        auto ipRow = r.removeFromTop (28);
        ipBtn.setBounds (ipRow.removeFromRight (90));
        ipRow.removeFromRight (6);
        ipEdit.setBounds (ipRow);
    }

private:
    //==========================================================================
    void timerCallback() override
    {
        auto peers = net.getPeers();
        if (! net.isBound()) statusText = lastError.isNotEmpty() ? lastError : "Network error";
        else if (peers.empty()) statusText = "Searching for others in room \"" + roomEdit.getText().trim() + "\"...  (your mic stays open)";
        else
        {
            StringArray names; for (auto& p : peers) names.add (p.name);
            statusText = "Connected: " + names.joinIntoString (", ");
        }

        const double ms = engine.getLatencySamples() / jmax (1.0, engine.getSampleRate()) * 1000.0;
        latencyText = "Delay added to your voice: " + String ((int) std::lround (ms)) + " ms   |   timing alignment: "
                    + String (engine.getAlignMs(), 1) + " ms"
                    + (peers.empty() ? String() : "   |   round trip: " + String (peers.front().rttMs, 1) + " ms");

        auto out = deviceManager.getAudioDeviceSetup().outputDeviceName;
        String h;
        if (lastError.isNotEmpty() && net.isBound()) h = lastError;
        else if (! outputConfirmed)
            h = "No virtual audio cable found, so nothing is sent out yet. Install the free VB-CABLE (vb-audio.com/Cable), "
                "restart this app, or pick your cable as the output above.";
        else if (looksLikeVirtualCable (out))
            h = "In Discord: Settings > Voice > Input Device = \"CABLE Output\". Turn off Discord's noise suppression, "
                "echo cancellation and automatic input sensitivity.";
        else
            h = "Warning: this output does not look like a virtual cable. The gated microphone is played on it.";
        if (hint.getText() != h) hint.setText (h, dontSendNotification);

        if (settingsDirty && ++saveTick > 30) { saveSettings(); saveTick = 0; settingsDirty = false; }
        repaint();
    }

    void applyParams()
    {
        engine.setUserParams ((float) sens.getValue(), (float) sep.getValue());
        engine.setAlignTrimMs ((float) trim.getValue());
    }
    void applyRoom() { net.setRoom (roomEdit.getText()); settingsDirty = true; }
    void applyBuffer()
    {
        deviceManager.removeAudioCallback (this);
        engine.setLookaheadMs (bufBox.getSelectedId() == 2 ? 100.0f : 60.0f);
        deviceManager.addAudioCallback (this);                       // prepares the engine again
    }

    void saveSettings()
    {
        auto* us = props.getUserSettings();
        us->setValue ("room", roomEdit.getText());
        us->setValue ("buffer", bufBox.getSelectedId());
        us->setValue ("threshold", sens.getValue());
        us->setValue ("margin", sep.getValue());
        us->setValue ("trim", trim.getValue());
        us->setValue ("outputConfirmed", outputConfirmed.load());
        if (auto xml = deviceManager.createStateXml()) us->setValue ("audioDeviceState", xml.get());
        props.saveIfNeeded();
    }

    // first start: use a virtual cable as output if there is one
    void autoPickCable()
    {
        for (auto* type : deviceManager.getAvailableDeviceTypes())
        {
            type->scanForDevices();
            for (auto& name : type->getDeviceNames (false))
                if (name.containsIgnoreCase ("CABLE Input") || (looksLikeVirtualCable (name) && ! name.containsIgnoreCase ("CABLE Output")))
                {
                    deviceManager.setCurrentAudioDeviceType (type->getTypeName(), true);
                    auto setup = deviceManager.getAudioDeviceSetup();
                    setup.outputDeviceName = name;
                    setup.useDefaultOutputChannels = true;
                    if (deviceManager.setAudioDeviceSetup (setup, true).isEmpty()) { outputConfirmed = true; settingsDirty = true; return; }
                }
        }
    }

    void refreshDeviceLists()
    {
        inBox.clear (dontSendNotification); outBox.clear (dontSendNotification);
        auto* type = deviceManager.getCurrentDeviceTypeObject();
        if (type == nullptr) return;
        const auto setup = deviceManager.getAudioDeviceSetup();
        auto ins = type->getDeviceNames (true), outs = type->getDeviceNames (false);
        for (int i = 0; i < ins.size(); ++i)  inBox.addItem (ins[i], i + 1);
        for (int i = 0; i < outs.size(); ++i) outBox.addItem (outs[i], i + 1);
        inBox.setText (setup.inputDeviceName, dontSendNotification);
        outBox.setText (setup.outputDeviceName, dontSendNotification);
    }

    void chooseDevice (bool input)
    {
        auto setup = deviceManager.getAudioDeviceSetup();
        if (input) { setup.inputDeviceName = inBox.getText(); setup.useDefaultInputChannels = true; }
        else       { setup.outputDeviceName = outBox.getText(); setup.useDefaultOutputChannels = true; outputConfirmed = true; }
        const auto err = deviceManager.setAudioDeviceSetup (setup, true);
        lastError = err;
        settingsDirty = true;
        refreshDeviceLists();
    }

    void showAudioSettings()
    {
        auto* sel = new AudioDeviceSelectorComponent (deviceManager, 0, 1, 0, 2, false, false, true, false);
        sel->setSize (520, 420);
        DialogWindow::LaunchOptions lo;
        lo.content.setOwned (sel);
        lo.dialogTitle = "Audio settings";
        lo.dialogBackgroundColour = kBg;
        lo.escapeKeyTriggersCloseButton = true;
        lo.useNativeTitleBar = true;
        lo.resizable = false;
        lo.launchAsync();
        outputConfirmed = true;
        settingsDirty = true;
        startTimer (33);
        Timer::callAfterDelay (1000, [safe = SafePointer<Component> (this)] { if (safe != nullptr) static_cast<MainComponent*> (safe.getComponent())->refreshDeviceLists(); });
    }

    //==========================================================================
    void audioDeviceAboutToStart (AudioIODevice* device) override
    {
        const double fs = device->getCurrentSampleRate();
        const int block = jmax (device->getCurrentBufferSizeSamples(), 512);
        mono.assign ((size_t) jmax (block * 4, 16384), 0.0f);
        engine.prepare (fs, (int) mono.size());
        applyParams();
    }
    void audioDeviceStopped() override {}

    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int numSamples,
                                           const AudioIODeviceCallbackContext&) override
    {
        // the engine wants the time at the START of the block; the device callback fires when the block has been recorded
        const double t = Time::getMillisecondCounterHiRes() * 0.001 - (double) numSamples / jmax (1.0, engine.getSampleRate());
        int done = 0;
        while (done < numSamples)
        {
            const int n = jmin (numSamples - done, (int) mono.size());
            const float* src = (numIn > 0 && in[0] != nullptr) ? in[0] + done : nullptr;
            if (src == nullptr) { std::fill (mono.begin(), mono.begin() + n, 0.0f); }
            engine.process (src != nullptr ? src : mono.data(), mono.data(), n, nullptr, t + (double) done / jmax (1.0, engine.getSampleRate()));
            for (int c = 0; c < numOut; ++c)
                if (out[c] != nullptr)
                {
                    if (outputConfirmed) std::copy (mono.begin(), mono.begin() + n, out[c] + done);
                    else std::fill (out[c] + done, out[c] + done + n, 0.0f);
                }
            done += n;
        }
    }

    //==========================================================================
    automix::RoomEngine engine;
    RoomNet net;
    AudioDeviceManager deviceManager;
    ApplicationProperties props;
    std::vector<float> mono;

    Label inLabel, outLabel, roomLabel, bufLabel, sensLabel, sepLabel, trimLabel, ipLabel;
    ComboBox inBox, outBox, bufBox;
    TextEditor roomEdit, ipEdit, hint;
    Slider sens, sep, trim;
    TextButton audioBtn, ipBtn;
    Rectangle<int> statusArea, meterArea;
    String statusText, latencyText, lastError;
    std::atomic<bool> outputConfirmed { false };
    bool settingsDirty = false;
    int saveTick = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

//==============================================================================
class RoomGateApp : public JUCEApplication
{
public:
    const String getApplicationName() override    { return "oddnoise Room Gate"; }
    const String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING_FALLBACK; }
    bool moreThanOneInstanceAllowed() override    { return true; }   // second instance only reports "port in use"

    void initialise (const String& commandLine) override
    {
        int port = RoomNet::kDefaultPort;                            // --port N is for testing two instances on one PC
        const auto args = StringArray::fromTokens (commandLine, true);
        for (int i = 0; i + 1 < args.size(); ++i) if (args[i] == "--port") port = args[i + 1].getIntValue();
        window = std::make_unique<Window> (getApplicationName(), port);
    }
    void shutdown() override { window.reset(); }
    void systemRequestedQuit() override { quit(); }

private:
    static constexpr const char* JUCE_APPLICATION_VERSION_STRING_FALLBACK = "0.1.0";

    struct Window : DocumentWindow
    {
        Window (const String& name, int port) : DocumentWindow (name, kBg, DocumentWindow::minimiseButton | DocumentWindow::closeButton)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent (port), true);
            setResizable (false, false);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }
        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<Window> window;
};

START_JUCE_APPLICATION (RoomGateApp)
