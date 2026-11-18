#pragma once

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
            releaseHeld();
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

    void enqueue(int channel, int pitch, bool on) noexcept
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
        queue[index] = {channel, pitch, on};
        write.store(next, std::memory_order_release);
    }

private:
    struct Edge
    {
        int channel;
        int pitch;
        bool on;
    };

    void releaseHeld()
    {
        for (std::size_t voice = 0; voice < held.size(); ++voice)
            if (held[voice] >= 0)
                send({static_cast<int>(voice) + 1, held[voice], false});
    }

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
        if (overflow.exchange(false))
            releaseHeld();
    }

    void send(Edge event)
    {
        const Byte bytes[]{ static_cast<Byte>((event.on ? 0x90 : 0x80) | (event.channel - 1)),
                            static_cast<Byte>(event.pitch), static_cast<Byte>(event.on ? 100 : 0) };
        MIDIPacketList packet{};
        const auto first = MIDIPacketListInit(&packet);
        MIDIPacketListAdd(&packet, sizeof packet, first, 0, sizeof bytes, bytes);
        if (MIDISend(port, destination, &packet) == noErr)
            held[static_cast<std::size_t>(event.channel - 1)] = event.on ? event.pitch : -1;
    }

    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    MIDIEndpointRef destination = 0;
    std::thread worker;
    std::array<Edge, 64> queue{};
    std::atomic<std::size_t> read{0}, write{0};
    std::atomic<bool> running{false}, overflow{false};
    std::array<int, 2> held{-1, -1};
};
