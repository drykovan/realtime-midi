#include "PluginEditor.h"
#include "DevBuild.h"
#include <algorithm>
#include <bitset>
#include <juce_audio_utils/juce_audio_utils.h>

void DetectedNotes::push(Event event) noexcept
{
    const auto sequence = published.load(std::memory_order_relaxed) + 1;
    // The sequence tag lets the reader detect a slot replaced during a read.
    const auto payload = ((sequence & 0xffff) << 48)
                       | (static_cast<std::uint64_t>(event.time) << 16)
                       | (event.voice << 8) | (event.on ? 128 : 0) | event.pitch;
    slots[sequence % capacity].store(payload, std::memory_order_relaxed);
    published.store(sequence, std::memory_order_release);
}

std::optional<DetectedNotes::Event> DetectedNotes::read(std::uint64_t sequence) const noexcept
{
    const auto payload = slots[sequence % capacity].load(std::memory_order_relaxed);
    if ((payload >> 48) != (sequence & 0xffff))
        return std::nullopt;
    return Event{static_cast<std::uint32_t>(payload >> 16), static_cast<int>(payload & 127),
                 static_cast<std::size_t>((payload >> 8) & 1), (payload & 128) != 0};
}

namespace
{
const auto background = juce::Colour::fromRGB(23, 25, 28);
const auto panel = juce::Colour::fromRGB(32, 35, 39);
const auto border = juce::Colour::fromRGB(52, 57, 63);
const auto foreground = juce::Colour::fromRGB(238, 241, 242);
const auto secondary = juce::Colour::fromRGB(170, 181, 188);
const auto accent = juce::Colour::fromRGB(155, 217, 195);
constexpr int historyMs = 5000;
constexpr int historyTop = 16, historyHeight = 107;
constexpr int controlsTop = historyTop + historyHeight + 12, controlsHeight = 81;
constexpr int hintsTop = controlsTop + controlsHeight + 12, hintsHeight = 131;
constexpr int statusTop = hintsTop + hintsHeight + 12, statusHeight = 28;

juce::String noteName(int pitch)
{
    return juce::MidiMessage::getMidiNoteName(pitch, true, true, 4);
}

class PickerKeyboard final : public juce::MidiKeyboardComponent
{
public:
    PickerKeyboard(juce::MidiKeyboardState& state, std::bitset<128>& notes)
        : MidiKeyboardComponent(state, horizontalKeyboard), selected(notes)
    {
        clearKeyMappings();
        setScrollButtonsVisible(false);
        setWantsKeyboardFocus(false);
        setColour(keyDownOverlayColourId, accent.withAlpha(0.78f));
    }

    std::function<void()> onSelectionChange;

private:
    bool mouseDownOnKey(int note, const juce::MouseEvent&) override
    {
        selected.flip(note);
        repaint();
        if (onSelectionChange)
            onSelectionChange();
        return false;
    }

    bool mouseDraggedToKey(int, const juce::MouseEvent&) override { return false; }

    juce::String getWhiteNoteText(int note) override { return noteName(note); }

    void drawWhiteNote(int note, juce::Graphics& g, juce::Rectangle<float> bounds, bool,
                       bool over, juce::Colour line, juce::Colour label) override
    {
        MidiKeyboardComponent::drawWhiteNote(note, g, bounds, selected[note], over, line, label);
    }

    void drawBlackNote(int note, juce::Graphics& g, juce::Rectangle<float> bounds, bool,
                       bool over, juce::Colour fill) override
    {
        MidiKeyboardComponent::drawBlackNote(note, g, bounds, selected[note], over, fill);
    }

    std::bitset<128>& selected;
};

class NotePicker final : public juce::Component
{
public:
    NotePicker(const juce::String& initial, int defaultOctave,
               std::function<void(const juce::String&)> apply)
    {
        const auto names = juce::StringArray::fromTokens(initial, ", ", "");
        int initialOctave = defaultOctave;
        bool first = true;
        for (int note = 0; note < 128; ++note)
            if (names.contains(noteName(note)))
            {
                selected.set(note);
                if (first)
                    initialOctave = note / 12 - 1;
                first = false;
            }

        for (int i = -1; i <= 9; ++i)
            octave.addItem("Octave " + juce::String(i), i + 2);
        octave.onChange = [this] { showOctave(); };
        octave.setSelectedId(initialOctave + 2, juce::dontSendNotification);
        showOctave();
        keyboard.onSelectionChange = [this, apply = std::move(apply)]
        {
            juce::StringArray namesToApply;
            for (int note = 0; note < 128; ++note)
                if (selected[note])
                    namesToApply.add(noteName(note));
            apply(namesToApply.joinIntoString(", "));
        };
        addAndMakeVisible(octave);
        addAndMakeVisible(keyboard);
        setSize(338, 144);
    }

    void resized() override
    {
        octave.setBounds(0, 0, 110, 26);
        keyboard.setBounds(0, 36, getWidth(), getHeight() - 36);
        keyboard.setKeyWidth(static_cast<float>(getWidth()) / 7.0f);
    }

private:
    void showOctave()
    {
        const int firstNote = (octave.getSelectedId() - 1) * 12;
        keyboard.setAvailableRange(firstNote, std::min(127, firstNote + 11));
        keyboard.setLowestVisibleKey(firstNote);
    }

    std::bitset<128> selected;
    juce::MidiKeyboardState keyboardState;
    PickerKeyboard keyboard{keyboardState, selected};
    juce::ComboBox octave;
};

void stylePickerButton(juce::DrawableButton& button)
{
    button.setTooltip(button.getName());
    juce::Path whiteKeys;
    whiteKeys.addRoundedRectangle(1.0f, 2.0f, 20.0f, 13.0f, 1.5f);
    for (const float x : {5.0f, 9.0f, 13.0f, 17.0f})
    {
        whiteKeys.startNewSubPath(x, x == 13.0f ? 2.0f : 9.0f);
        whiteKeys.lineTo(x, 15.0f);
    }
    juce::Path blackKeys;
    for (const float x : {5.0f, 9.0f, 17.0f})
        blackKeys.addRoundedRectangle(x - 1.25f, 2.0f, 2.5f, 7.5f, 0.6f);
    juce::Path icon;
    juce::PathStrokeType(1.2f).createStrokedPath(icon, whiteKeys);
    icon.addPath(blackKeys);
    juce::DrawablePath drawing;
    drawing.setPath(icon);
    drawing.setFill(foreground);
    button.setImages(&drawing);
}

void styleLabel(juce::Label& label, const juce::String& text, float height,
                juce::Colour colour, bool bold = false)
{
    label.setText(text, juce::dontSendNotification);
    label.setFont(juce::FontOptions(height, bold ? juce::Font::bold : juce::Font::plain));
    label.setColour(juce::Label::textColourId, colour);
    label.setBorderSize(juce::BorderSize<int>(0));
    label.setMinimumHorizontalScale(1.0f);
    label.setJustificationType(juce::Justification::centredLeft);
}
void styleSlider(juce::Slider& slider, const juce::String& title, const juce::String& suffix)
{
    slider.setTitle(title);
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.textFromValueFunction = [suffix](double value)
    {
        return juce::String(juce::roundToInt(value)) + suffix;
    };
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 65, 26);
    slider.setColour(juce::Slider::backgroundColourId, border);
    slider.setColour(juce::Slider::trackColourId, accent);
    slider.setColour(juce::Slider::thumbColourId, foreground);
    slider.setColour(juce::Slider::textBoxTextColourId, foreground);
    slider.setColour(juce::Slider::textBoxOutlineColourId, border);
    slider.setColour(juce::Slider::textBoxBackgroundColourId, panel);
}

}

RealtimeMidiEditor::RealtimeMidiEditor(juce::AudioProcessor& processor,
                                      juce::AudioProcessorValueTreeState& parameters,
                                      ReadHints read, WriteHints write, const DetectedNotes& notes)
    : AudioProcessorEditor(processor),
      detectedNotes(notes), lastRead(notes.latest() > DetectedNotes::capacity ? notes.latest() - DetectedNotes::capacity : 0),
      attackAttachment(*parameters.getParameter("attackSensitivity"), attackSlider),
      windowAttachment(*parameters.getParameter("analysisWindow"), windowSlider),
      hintsAttachment(parameters, "pitchHints", hintsToggle),
      readHints(std::move(read)), writeHints(std::move(write))
{
#if defined(REALTIME_MIDI_DEV_BUILD)
    const auto build = juce::String(REALTIME_MIDI_DEV_BUILD).replaceCharacter('T', ' ').dropLastCharacters(1) + " UTC";
#else
    const juce::String build = "Release";
#endif
    styleLabel(buildStatus, juce::String(JucePlugin_VersionString) + " / " + build, 12.0f, secondary);
    buildStatus.setTitle("Plugin version and build");
    addAndMakeVisible(buildStatus);

    styleLabel(attackLabel, "Attack Sensitivity", 14.0f, secondary);
    styleLabel(windowLabel, "Analysis Window", 14.0f, secondary);
    styleSlider(attackSlider, attackLabel.getText(), " %");
    styleSlider(windowSlider, windowLabel.getText(), " ms");
    addAndMakeVisible(attackLabel);
    addAndMakeVisible(windowLabel);
    addAndMakeVisible(attackSlider);
    addAndMakeVisible(windowSlider);

    styleLabel(fundamentalLabel, "Fundamental", 14.0f, secondary);
    styleLabel(overtoneLabel, "Overtones", 14.0f, secondary);
    styleLabel(hintStatus, "", 13.0f, secondary);
    hintsToggle.setColour(juce::ToggleButton::textColourId, foreground);
    addAndMakeVisible(hintsToggle);
    for (auto* label : {&fundamentalLabel, &overtoneLabel, &hintStatus})
        addAndMakeVisible(*label);
    for (int voice = 0; voice < 2; ++voice)
    {
        auto& field = hintFields[voice];
        field.setTitle(voice == 0 ? "Fundamental pitch hints" : "Overtone pitch hints");
        field.setTextToShowWhenEmpty("No notes selected", secondary);
        field.setColour(juce::TextEditor::backgroundColourId, background);
        field.setColour(juce::TextEditor::textColourId, foreground);
        field.setColour(juce::TextEditor::outlineColourId, border);
        field.setColour(juce::TextEditor::focusedOutlineColourId, accent);
        confirmedHints[voice] = readHints(voice);
        field.setText(confirmedHints[voice], false);
        auto apply = [this, voice]
        {
            auto& editor = hintFields[voice];
            if (editor.getText() == confirmedHints[voice])
            {
                setHintError(voice, false);
                return;
            }
            if (!writeHints(voice, editor.getText()))
            {
                setHintError(voice, true);
                return;
            }
            confirmedHints[voice] = readHints(voice);
            editor.setText(confirmedHints[voice], false);
            setHintError(voice, false);
        };
        field.onReturnKey = apply;
        field.onFocusLost = apply;
        field.onEscapeKey = [this, voice]
        {
            confirmedHints[voice] = readHints(voice);
            hintFields[voice].setText(confirmedHints[voice], false);
            setHintError(voice, false);
        };
        addAndMakeVisible(field);
    }
    for (auto* button : {&fundamentalPicker, &overtonePicker})
    {
        stylePickerButton(*button);
        addAndMakeVisible(*button);
    }
    fundamentalPicker.onClick = [this] { openNotePicker(0); };
    overtonePicker.onClick = [this] { openNotePicker(1); };
    startTimerHz(30);
    setOpaque(true);
    setSize(440, statusTop + statusHeight);
}

void RealtimeMidiEditor::openNotePicker(int voice)
{
    auto picker = std::make_unique<NotePicker>(
        readHints(voice), voice == 0 ? 2 : 5,
        [editor = juce::Component::SafePointer<RealtimeMidiEditor>(this), voice](const juce::String& notes)
        {
            if (editor != nullptr && editor->writeHints(voice, notes))
            {
                editor->confirmedHints[voice] = editor->readHints(voice);
                editor->hintFields[voice].setText(editor->confirmedHints[voice], false);
                editor->setHintError(voice, false);
            }
        });
    auto& button = voice == 0 ? fundamentalPicker : overtonePicker;
    juce::CallOutBox::launchAsynchronously(std::move(picker), button.getScreenBounds(), nullptr);
}

void RealtimeMidiEditor::setHintError(int voice, bool invalid)
{
    hintErrors[voice] = invalid;
    hintFields[voice].setColour(juce::TextEditor::outlineColourId,
                                invalid ? juce::Colour::fromRGB(231, 159, 136) : border);
    hintFields[voice].setColour(juce::TextEditor::focusedOutlineColourId,
                                invalid ? juce::Colour::fromRGB(231, 159, 136) : accent);
    hintStatus.setText(hintErrors[0] || hintErrors[1]
                           ? "Use notes like G3, Bb4 or C#5. Last valid list kept."
                           : "",
                       juce::dontSendNotification);
}

void RealtimeMidiEditor::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounter();
    const auto latest = detectedNotes.latest();
    if (latest - lastRead > DetectedNotes::capacity)
    {
        for (auto& row : recent)
            row.clear();
        lastRead = latest - DetectedNotes::capacity;
    }
    bool refresh = latest != lastRead;
    while (lastRead < latest)
    {
        const auto event = detectedNotes.read(++lastRead);
        if (!event)
            continue;
        auto& row = recent[event->voice];
        if (!row.empty() && !row.back().end)
            row.back().end = event->time;
        if (event->on)
            row.push_back({event->pitch, event->time, std::nullopt});
    }
    for (auto& row : recent)
    {
        refresh = refresh || !row.empty();
        while (!row.empty() && row.front().end
               && static_cast<int>(now - *row.front().end) >= historyMs)
            row.pop_front();
    }
    if (refresh)
        repaint(20, historyTop, getWidth() - 40, historyHeight);

    if (++hintRefresh < 8)
        return;
    hintRefresh = 0;
    for (int voice = 0; voice < 2; ++voice)
        if (!hintFields[voice].hasKeyboardFocus(false)
            && hintFields[voice].getText() == confirmedHints[voice])
        {
            const auto saved = readHints(voice);
            if (confirmedHints[voice] != saved)
            {
                confirmedHints[voice] = saved;
                hintFields[voice].setText(saved, false);
            }
        }
}

void RealtimeMidiEditor::paint(juce::Graphics& graphics)
{
    graphics.fillAll(background);
    for (const auto area : {juce::Rectangle<int>(20, historyTop, getWidth() - 40, historyHeight),
                            juce::Rectangle<int>(20, controlsTop, getWidth() - 40, controlsHeight),
                            juce::Rectangle<int>(20, hintsTop, getWidth() - 40, hintsHeight)})
    {
        graphics.setColour(panel);
        graphics.fillRoundedRectangle(area.toFloat(), 9.0f);
        graphics.setColour(border);
        graphics.drawRoundedRectangle(area.toFloat().reduced(0.5f), 9.0f, 1.0f);
    }
    graphics.setColour(border);
    graphics.drawHorizontalLine(statusTop, 20.0f, static_cast<float>(getWidth() - 20));
    graphics.setFont(juce::FontOptions(14.0f));
    graphics.setColour(foreground);
    graphics.drawText("Recent Notes", 34, historyTop + 7, 120, 24, juce::Justification::centredLeft);
    const auto now = juce::Time::getMillisecondCounter();
    const float right = static_cast<float>(getWidth() - 34);
    constexpr float left = 124.0f;
    const float pixelsPerMs = (right - left) / historyMs;
    graphics.setColour(secondary);
    graphics.setFont(juce::FontOptions(11.0f));
    graphics.drawText("Last 5 s", static_cast<int>(right) - 56, historyTop + 7, 56, 24,
                      juce::Justification::centredRight);
    graphics.setColour(border);
    for (int seconds = 0; seconds <= 5; ++seconds)
    {
        const float x = right - 1000.0f * seconds * pixelsPerMs;
        graphics.drawVerticalLine(juce::roundToInt(x), historyTop + 34.0f, historyTop + 92.0f);
    }
    for (std::size_t voice = 0; voice < recent.size(); ++voice)
    {
        const int y = historyTop + 35 + 31 * static_cast<int>(voice);
        graphics.setColour(secondary);
        graphics.setFont(juce::FontOptions(13.0f));
        graphics.drawText(voice == 0 ? "Fundamental" : "Overtones",
                          34, y, 88, 24, juce::Justification::centredLeft);
        for (const auto& note : recent[voice])
        {
            const auto position = [&](std::uint32_t time)
            {
                return right - static_cast<int>(now - time) * pixelsPerMs;
            };
            const float start = std::max(left, position(note.start));
            const float end = std::min(right, note.end ? position(*note.end) : right);
            if (end <= start)
                continue;
            const auto hue = static_cast<float>((note.pitch % 12) * 7 % 12) / 12.0f;
            const auto colour = juce::Colour::fromHSV(hue, 0.45f, 0.95f, note.end ? 0.48f : 0.87f);
            const juce::Rectangle<float> strip(start, y + 2.0f, end - start, 20.0f);
            graphics.setColour(colour);
            graphics.fillRoundedRectangle(strip, 3.0f);
            if (strip.getWidth() >= 17.0f)
            {
                graphics.setColour(juce::Colours::black);
                graphics.setFont(juce::FontOptions(10.0f));
                graphics.drawFittedText(noteName(note.pitch), strip.toNearestInt().reduced(2, 0),
                                        juce::Justification::centredLeft, 1, 0.7f);
            }
        }
    }
}

void RealtimeMidiEditor::resized()
{
    attackLabel.setBounds(34, controlsTop + 2, 122, 38);
    windowLabel.setBounds(34, controlsTop + 41, 122, 38);
    attackSlider.setBounds(158, controlsTop + 2, getWidth() - 192, 38);
    windowSlider.setBounds(158, controlsTop + 41, getWidth() - 192, 38);
    hintsToggle.setBounds(32, hintsTop + 5, 140, 28);
    fundamentalLabel.setBounds(34, hintsTop + 36, 110, 26);
    overtoneLabel.setBounds(34, hintsTop + 70, 110, 26);
    hintFields[0].setBounds(158, hintsTop + 36, getWidth() - 224, 26);
    hintFields[1].setBounds(158, hintsTop + 70, getWidth() - 224, 26);
    fundamentalPicker.setBounds(getWidth() - 62, hintsTop + 36, 28, 26);
    overtonePicker.setBounds(getWidth() - 62, hintsTop + 70, 28, 26);
    hintStatus.setBounds(34, hintsTop + 99, getWidth() - 68, 26);
    buildStatus.setBounds(34, statusTop + 1, getWidth() - 68, statusHeight - 3);
}
