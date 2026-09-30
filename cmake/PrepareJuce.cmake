# JUCE 8.0.15 only notifies repaint listeners (and checks its exit flag) when
# DXGI WaitForVBlank succeeds. Wine returns an unsupported error, freezing
# repainting and window teardown. Fall back to a paced notification on failure.
function(wave_prepare_juce source_dir)
    set(path "${source_dir}/modules/juce_gui_basics/native/juce_VBlank_windows.cpp")
    file(READ "${path}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    string(REPLACE "        for (;;)\n" "        while (! threadShouldExit())\n" contents "${contents}")
    set(original [=[            if (output->WaitForVBlank() == S_OK)
            {
                const auto now = Time::getMillisecondCounterHiRes();

                if (now - lastVBlankEvent.exchange (now) < 1.0)
                    sleep (1);

                const auto stateToRead = state.fetch_or (flagPaintPending);

                if ((stateToRead & flagExit) != 0)
                    return;

                if ((stateToRead & flagPaintPending) != 0)
                    continue;

                triggerAsyncUpdate();
            }
            else
            {
                sleep (1);
            }]=])
    set(fixed [=[            // DXGI can lack VBlank support under Wine or a remote display.
            // Repaints still need a clock; pace the fallback instead of spinning.
            if (output->WaitForVBlank() != S_OK)
                sleep (16);

            const auto now = Time::getMillisecondCounterHiRes();

            if (now - lastVBlankEvent.exchange (now) < 1.0)
                sleep (1);

            const auto stateToRead = state.fetch_or (flagPaintPending);

            if ((stateToRead & flagExit) != 0)
                return;

            if ((stateToRead & flagPaintPending) != 0)
                continue;

            triggerAsyncUpdate();]=])
    string(FIND "${contents}" "${fixed}" already_fixed)
    if(NOT already_fixed EQUAL -1)
        return()
    endif()
    string(FIND "${contents}" "${original}" match)
    if(match EQUAL -1)
        message(FATAL_ERROR "Pinned JUCE VBlank source changed; review the Wine repaint/shutdown fix")
    endif()
    string(REPLACE "${original}" "${fixed}" contents "${contents}")
    file(WRITE "${path}" "${contents}")
endfunction()
