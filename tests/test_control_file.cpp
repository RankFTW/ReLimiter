#include "control_file.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// The developer control file's command parser (control_file.h): which lines a
// harness write turns into commands. The poller thread, the present-callback
// execution and the status file need a live game.

namespace
{
    int s_failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                            \
            ++s_failures;                                                        \
        }                                                                       \
    } while (false)

    using Commands = std::vector<std::pair<uint64_t, std::string>>;

    // Parses text after after_seq; the callback refuses commands once `accept`
    // of them were taken (a full queue).
    uint64_t Parse(const std::string& text, uint64_t after_seq, Commands* out, size_t accept = SIZE_MAX)
    {
        return ControlFile_Parse(text.data(), text.size(), after_seq,
            [&](uint64_t seq, const char* command, size_t length) {
                if (out->size() >= accept) return false;
                out->emplace_back(seq, std::string(command, length));
                return true;
            });
    }

    // Only complete lines count: a harness that writes in place could leave
    // the last line half written.
    void TestCompleteLinesOnly()
    {
        Commands commands;
        CHECK(Parse("1 ping\n2 set target_fps 60\n3 capture st", 0, &commands) == 2);
        CHECK(commands == Commands({{1, "ping"}, {2, "set target_fps 60"}}));

        commands.clear();
        CHECK(Parse("4 capture start\r\n  5\tmark  pass 1 \t\n", 0, &commands) == 5);
        CHECK(commands == Commands({{4, "capture start"}, {5, "mark  pass 1"}}));
    }

    // Commands at or below the last taken seq are not run again, and a line
    // whose seq does not grow is skipped: rewriting the file with the same or
    // an older command never repeats it.
    void TestSeqOnlyGrows()
    {
        Commands commands;
        CHECK(Parse("3 ping\n4 set target_fps 60\n", 3, &commands) == 4);
        CHECK(commands == Commands({{4, "set target_fps 60"}}));

        commands.clear();
        CHECK(Parse("7 capture start\n6 capture stop\n7 mark again\n8 ping\n", 0, &commands) == 8);
        CHECK(commands == Commands({{7, "capture start"}, {8, "ping"}}));

        // Stale commands at startup: the highest seq is taken, nothing runs later.
        commands.clear();
        const uint64_t stale = Parse("1 ping\n2 capture start\n", 0, &commands);
        CHECK(stale == 2);
        commands.clear();
        CHECK(Parse("1 ping\n2 capture start\n", stale, &commands) == 2 && commands.empty());
    }

    // Malformed lines are skipped without stopping the rest.
    void TestMalformedLines()
    {
        Commands commands;
        const std::string longest(kControlFileCommandMax, 'x');
        const std::string too_long(kControlFileCommandMax + 1, 'x');
        const std::string text = "ping\n0 ping\n2ping\n3\n4   \n5 " + too_long + "\n6 " + longest +
            "\n1234567890123456789 ping\n7 mark ok\n";
        CHECK(Parse(text, 0, &commands) == 7);
        CHECK(commands == Commands({{6, longest}, {7, "mark ok"}}));
    }

    // A refused command (full queue) stops the parse below it, so the next
    // poll offers it again instead of losing it.
    void TestRefusedCommandIsRetried()
    {
        Commands commands;
        const std::string text = "1 ping\n2 capture start\n3 capture stop\n";
        CHECK(Parse(text, 0, &commands, 1) == 1);
        CHECK(commands == Commands({{1, "ping"}}));
        commands.clear();
        CHECK(Parse(text, 1, &commands) == 3);
        CHECK(commands == Commands({{2, "capture start"}, {3, "capture stop"}}));
    }
}

int main()
{
    TestCompleteLinesOnly();
    TestSeqOnlyGrows();
    TestMalformedLines();
    TestRefusedCommandIsRetried();
    if (s_failures) {
        std::fprintf(stderr, "%d control file check(s) failed\n", s_failures);
        return 1;
    }
    std::printf("control file parser: all checks passed\n");
    return 0;
}
