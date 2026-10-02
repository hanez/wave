#include "WaveLcdComponent.h"

#include "../PluginProcessor.h"

namespace wave::ui
{
WaveLcdComponent::WaveLcdComponent(WaveEmulationAudioProcessor& processor)
    : owner(processor)
{
    setOpaque(false);
    setInterceptsMouseClicks(false, false);
    setTooltip("480 x 64 Waldorf Wave firmware LCD");
    static_cast<void>(copyFirmwareDisplay());
    startTimerHz(60);
}

WaveLcdComponent::~WaveLcdComponent()
{
    stopTimer();
}

void WaveLcdComponent::paint(juce::Graphics& graphics)
{
    auto display = getLocalBounds().toFloat();
    if (panelEmbedded)
    {
        graphics.setColour(juce::Colour::fromRGB(69, 200, 187));
        graphics.fillRect(display);
    }
    else
    {
        auto bezel = display.reduced(1.0f);
        graphics.setColour(juce::Colours::black.withAlpha(0.48f));
        graphics.fillRoundedRectangle(bezel.translated(0.0f, 2.0f), 8.0f);
        graphics.setGradientFill(juce::ColourGradient(
            juce::Colour::fromRGB(74, 88, 86), bezel.getTopLeft(),
            juce::Colour::fromRGB(18, 24, 24), bezel.getBottomRight(), false));
        graphics.fillRoundedRectangle(bezel, 8.0f);
        graphics.setColour(juce::Colour::fromRGB(104, 127, 124));
        graphics.drawRoundedRectangle(bezel, 8.0f, 1.0f);

        auto available = bezel.reduced(18.0f, 11.0f);
        constexpr auto lcdAspect = static_cast<float>(LcdFramebuffer::width)
                                   / static_cast<float>(LcdFramebuffer::height);
        auto displayWidth = available.getWidth();
        auto displayHeight = displayWidth / lcdAspect;
        if (displayHeight > available.getHeight())
        {
            displayHeight = available.getHeight();
            displayWidth = displayHeight * lcdAspect;
        }
        display = juce::Rectangle<float>(displayWidth, displayHeight)
                      .withCentre(available.getCentre());

        graphics.setColour(juce::Colour::fromRGB(69, 200, 187));
        graphics.fillRect(display);
        graphics.setColour(juce::Colour::fromRGB(27, 61, 61).withAlpha(0.28f));
        graphics.drawRect(display, 1.0f);
    }

    // The LCD component is deliberately a read-only monitor. Every displayed
    // bit comes from the engine's emulated firmware VRAM; no UI parameter or
    // mouse gesture can write, annotate or substitute pixels here.
    graphics.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
    graphics.drawImage(pixelImage, display, juce::RectanglePlacement::stretchToFit);
}

void WaveLcdComponent::timerCallback()
{
    if (copyFirmwareDisplay())
        repaint();
}

bool WaveLcdComponent::copyFirmwareDisplay()
{
    // A mode page is rasterised directly into firmware VRAM over several CPU
    // blocks. Keep the previous complete hardware frame visible until that
    // engine transaction finishes; publishing those intermediate scans is the
    // flash seen on External Edit and Instrument Edit. This monitor remains
    // read-only and never writes or reconstructs LCD content.
    const auto& runtime = owner.getMasterFirmwareRuntime();
    const auto modeTransitionActive
        = owner.isPanelModeDisplayTransitionActive();
    if (hasDisplayedFrame && modeTransitionActive)
    {
        modeTransitionWasActive = true;
        return false;
    }

    const auto completingModeTransition
        = modeTransitionWasActive && !modeTransitionActive;
    if (completingModeTransition)
        modeTransitionWasActive = false;

    // Suppress residual passes of the same completed mode page. A new
    // Performance/frame publication ends that guard immediately. Continuous
    // MIDI/controller redraws must also have a finite hold time.
    if (postCommitGuardActive && !completingModeTransition)
    {
        const auto writes = runtime.lcdVideoWriteCount();
        const auto page = runtime.lcdDisplayPage();
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto sameRecall = owner.getCurrentProgram() == postCommitProgram
                               && runtime.lcdDisplayRevision() == postCommitRevision;
        if (sameRecall && now - postCommitStartedMs < 500.0)
        {
            if (writes != postCommitWriteCount || page != postCommitPage)
            {
                postCommitWriteCount = writes;
                postCommitPage = page;
                postCommitLastChangeMs = now;
                return false;
            }
            if (now - postCommitLastChangeMs < 300.0)
                return false;
        }
        postCommitGuardActive = false;
    }

    const auto writesBefore = runtime.lcdVideoWriteCount();
    if (writesBefore == 0)
    {
        if (!hasDisplayedFrame && displayedWriteCount == 0)
            return false;

        framebuffer.clear();
        displayedWriteCount = 0;
        displayedPage = 0;
        hasDisplayedFrame = false;
    }
    else
    {
        const auto pageBefore = runtime.lcdDisplayPage();
        auto revisionBefore = runtime.lcdDisplayRevision();

        // Most timer ticks see an unchanged display. Avoid copying all four
        // VRAM pages until the firmware has written or selected a new frame.
        if (hasDisplayedFrame && writesBefore == displayedWriteCount
            && pageBefore == displayedPage && revisionBefore == displayedRevision)
            return false;

        auto video = runtime.lcdVideoSnapshot();
        auto pageAfter = runtime.lcdDisplayPage();
        auto writesAfter = runtime.lcdVideoWriteCount();
        auto revisionAfter = runtime.lcdDisplayRevision();

        // Prefer a snapshot taken between firmware writes. The Wavetable/Data
        // encoder can legitimately make the OS draw on every audio block,
        // though, so waiting for two completely quiet UI ticks can freeze the
        // monitor for the entire gesture. Retry once to reduce tearing, then
        // publish the newest scan just as the physical LCD would while its RAM
        // is being changed continuously.
        if (writesBefore != writesAfter || pageBefore != pageAfter
            || revisionBefore != revisionAfter)
        {
            revisionBefore = revisionAfter;
            video = runtime.lcdVideoSnapshot();
            pageAfter = runtime.lcdDisplayPage();
            writesAfter = runtime.lcdVideoWriteCount();
        }

        framebuffer.loadHardwareVideoRam(video.data(), video.size(), pageAfter);
        displayedWriteCount = writesAfter;
        // If publication raced this snapshot, leave its old revision cached
        // so the next UI tick retries even after the final VRAM write.
        displayedRevision = revisionBefore;
        displayedPage = pageAfter;
        hasDisplayedFrame = true;
        if (completingModeTransition)
        {
            postCommitGuardActive = true;
            postCommitWriteCount = writesAfter;
            postCommitPage = pageAfter;
            postCommitLastChangeMs = juce::Time::getMillisecondCounterHiRes();
            postCommitStartedMs = postCommitLastChangeMs;
            postCommitProgram = owner.getCurrentProgram();
            postCommitRevision = runtime.lcdDisplayRevision();
        }
    }
    rebuildPixelImage();
    return true;
}

void WaveLcdComponent::rebuildPixelImage()
{
    pixelImage.clear(pixelImage.getBounds(), juce::Colours::transparentBlack);
    juce::Graphics pixels(pixelImage);
    pixels.setColour(juce::Colour::fromRGB(17, 51, 52).withAlpha(0.96f));
    for (int y = 0; y < LcdFramebuffer::height; ++y)
        for (int x = 0; x < LcdFramebuffer::width; ++x)
            if (framebuffer.pixel(x, y))
                pixels.fillRect(x, y, 1, 1);
}
} // namespace wave::ui
