#pragma once

#include "WaveLcdModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

class WaveEmulationAudioProcessor;

namespace wave::ui
{
class WaveLcdComponent final : public juce::Component,
                               public juce::SettableTooltipClient,
                               private juce::Timer
{
public:
    explicit WaveLcdComponent(WaveEmulationAudioProcessor& processor);
    ~WaveLcdComponent() override;

    void paint(juce::Graphics& graphics) override;
    void setPanelEmbedded(bool shouldBeEmbedded) noexcept
    {
        panelEmbedded = shouldBeEmbedded;
        setOpaque(panelEmbedded);
        repaint();
    }

private:
    void timerCallback() override;
    [[nodiscard]] bool copyFirmwareDisplay();
    void rebuildPixelImage();

    WaveEmulationAudioProcessor& owner;
    LcdFramebuffer framebuffer;
    juce::Image pixelImage { juce::Image::ARGB, LcdFramebuffer::width,
                             LcdFramebuffer::height, true };
    uint64_t displayedWriteCount = 0;
    uint64_t displayedRevision = 0;
    uint8_t displayedPage = 0;
    uint64_t postCommitWriteCount = 0;
    uint8_t postCommitPage = 0;
    double postCommitLastChangeMs = 0.0;
    double postCommitStartedMs = 0.0;
    uint64_t postCommitRevision = 0;
    int postCommitProgram = -1;
    bool hasDisplayedFrame = false;
    bool modeTransitionWasActive = false;
    bool postCommitGuardActive = false;
    bool panelEmbedded = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveLcdComponent)
};
} // namespace wave::ui
