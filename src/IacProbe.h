#pragma once

#include "NoteSequence.h"
#include <CoreMIDI/CoreMIDI.h>
#include <array>
#include <atomic>
#include <chrono>
#include <thread>

// Test-only output. CoreMIDI calls run on a worker, outside the audio callback.
class IacProbe
{
public:
    ~IacProbe() { stop(); }

    void start()
    {
        if (worker.joinable())
            return;
        if (MIDIClientCreate(CFSTR("Realtime MIDI Test"), nullptr, nullptr, &client) != noErr)
            return;
        for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); ++i)
        {
            const auto candidate = MIDIGetDestination(i);
            CFStringRef name = nullptr;
            if (MIDIObjectGetStringProperty(candidate, kMIDIPropertyDisplayName, &name) != noErr)
                continue;
            const bool match = CFEqual(name, CFSTR("IAC Driver Bus 1"));
            CFRelease(name);
            if (match)
            {
                destination = candidate;
                break;
            }
        }
        if (!destination || MIDIOutputPortCreate(client, CFSTR("IAC Output"), &port) != noErr)
        {
            MIDIClientDispose(client);
            client = 0;
            return;
        }
        running.store(true);
        worker = std::thread([this]
        {
            while (running.load())
            {
                drain();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            drain();
            if (held)
                send(false);
        });
    }

    void stop()
    {
        running.store(false);
        if (worker.joinable())
            worker.join();
        if (client)
            MIDIClientDispose(client);
        client = 0;
        port = 0;
        destination = 0;
    }

    void enqueue(bool on) noexcept
    {
        if (!running.load(std::memory_order_relaxed))
            return;

        const auto index = write.load(std::memory_order_relaxed);
        const auto next = (index + 1) % queue.size();
        if (next == read.load(std::memory_order_acquire))
        {
            overflow.store(true, std::memory_order_release);
            return;
        }
        queue[index] = on;
        write.store(next, std::memory_order_release);
    }

private:
    void drain()
    {
        const auto end = write.load(std::memory_order_acquire);
        auto index = read.load(std::memory_order_relaxed);
        while (index != end)
        {
            send(queue[index]);
            index = (index + 1) % queue.size();
        }
        read.store(index, std::memory_order_release);
        if (overflow.exchange(false) && held)
            send(false);
    }

    void send(bool on)
    {
        using Event = realtime_midi::NoteEvent;
        const Byte bytes[]{ static_cast<Byte>((on ? 0x90 : 0x80) | (Event::channel - 1)),
                            Event::pitch, static_cast<Byte>(on ? Event::velocity : 0) };
        MIDIPacketList packet{};
        const auto first = MIDIPacketListInit(&packet);
        MIDIPacketListAdd(&packet, sizeof packet, first, 0, sizeof bytes, bytes);
        if (MIDISend(port, destination, &packet) == noErr)
            held = on;
    }

    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    MIDIEndpointRef destination = 0;
    std::thread worker;
    std::array<bool, 64> queue{};
    std::atomic<std::size_t> read{0}, write{0};
    std::atomic<bool> running{false}, overflow{false};
    bool held = false;
};
