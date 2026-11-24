// The place record of frame generation's native Presents (src/fg_present_places.h): which Present was in progress
// on a thread at a time, as the PC Latency collector asks when the Present's ETW record arrives. Proves the lookup
// and the ring's bounds on one thread; that a place is written for the right Present, and that its times bracket
// the start ETW records, need a capture.

#include "fg_present_places.h"

#include <cstdint>
#include <iostream>

namespace
{
    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ++g_failures;
        }
    }

    constexpr uint32_t kThread = 4242;

    FgPresentPlace Made(uint64_t frame, uint8_t index, uint8_t outputs, int64_t entered, int64_t returned,
        uint32_t thread = kThread)
    {
        FgPresentPlace place;
        place.frame = frame;
        place.index = index;
        place.outputs = outputs;
        place.entered = entered;
        place.returned = returned;
        place.thread = thread;
        return place;
    }

    void TestFindsThePresentInProgress()
    {
        FgPresentPlaces<16> places;
        FgPresentPlace found;
        Check(!places.Find(kThread, 100, &found), "nothing is found before a Present was placed");

        // One 3x cycle of frame 60 and the first Present of frame 61.
        places.Write(Made(60, 0, 3, 1'000, 1'040));
        places.Write(Made(60, 1, 3, 1'600, 1'650));
        places.Write(Made(60, 2, 3, 2'200, 2'230));
        places.Write(Made(61, 0, 3, 2'300, 2'345));

        Check(places.Find(kThread, 1'620, &found) && found.frame == 60 && found.index == 1 && found.outputs == 3 &&
              found.entered == 1'600 && found.returned == 1'650 && found.thread == kThread,
              "a start inside a Present finds that Present whole");
        Check(places.Find(kThread, 1'000, &found) && found.index == 0 && found.frame == 60,
              "a start at the Present's entry is inside it");
        Check(places.Find(kThread, 2'230, &found) && found.index == 2 && found.frame == 60,
              "a start at the Present's return is inside it");
        Check(places.Find(kThread, 2'301, &found) && found.frame == 61 && found.index == 0,
              "the newest Present is found");
        Check(!places.Find(kThread, 999, &found), "a start before every Present finds none");
        Check(!places.Find(kThread, 1'300, &found), "a start between two Presents finds none");
        Check(!places.Find(kThread, 2'400, &found), "a start after the newest Present's return finds none");
        Check(!places.Find(kThread + 1, 1'620, &found), "another thread's Present is not this one's");
    }

    void TestKeepsTheNewestOnly()
    {
        FgPresentPlaces<8> places;
        // Twenty Presents, 100 apart and 40 long: the ring keeps the newest seven (the slot the writer takes next
        // is left out).
        for (int i = 0; i < 20; ++i) {
            places.Write(Made(100 + static_cast<uint64_t>(i), static_cast<uint8_t>(i % 3), 3, 1'000 + i * 100,
                1'040 + i * 100));
        }
        FgPresentPlace found;
        for (int i = 13; i < 20; ++i) {
            Check(places.Find(kThread, 1'020 + i * 100, &found) && found.frame == 100u + static_cast<uint64_t>(i) &&
                  found.index == i % 3, "each of the newest Presents is found");
        }
        Check(!places.Find(kThread, 1'020 + 12 * 100, &found), "a Present older than the ring keeps is not found");
        Check(!places.Find(kThread, 1'020, &found), "the oldest Present is long overwritten");
    }

    void TestPresentsOutOfOrder()
    {
        // Two threads' Presents written behind their returns, not in the order of their starts: a Present can then
        // fail to be found, and one found is the right one.
        FgPresentPlaces<16> places;
        Check(places.Write(Made(60, 0, 3, 1'000, 1'040)), "a Present is written");
        places.Write(Made(60, 1, 3, 1'600, 1'700));
        places.Write(Made(90, 0, 2, 1'500, 1'720, kThread + 1));
        places.Write(Made(60, 2, 3, 2'200, 2'230));
        FgPresentPlace found;
        for (int64_t start = 900; start <= 2'300; start += 10) {
            if (places.Find(kThread, start, &found)) {
                Check(found.thread == kThread && found.entered <= start && start <= found.returned,
                      "a Present found for a thread was in progress on it at the start");
            }
            if (places.Find(kThread + 1, start, &found)) {
                Check(found.thread == kThread + 1 && found.frame == 90 && found.entered <= start &&
                      start <= found.returned, "the other thread's Present is found as its own only");
            }
        }
        Check(places.Find(kThread, 2'210, &found) && found.index == 2, "Presents in order are still found");
    }
}

int main()
{
    TestFindsThePresentInProgress();
    TestKeepsTheNewestOnly();
    TestPresentsOutOfOrder();
    if (g_failures != 0) {
        std::cerr << g_failures << " present place checks failed\n";
        return 1;
    }
    std::cout << "present place tests passed\n";
    return 0;
}
