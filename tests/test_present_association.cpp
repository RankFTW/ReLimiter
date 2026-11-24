// Directly tests the production real/generated classification of PresentMon
// observations (PresentAssociation_ClassifyFrame) and the production grouping
// of the presents of one app frame under frame generation
// (PresentFrameGrouper, PresentAssociation_EffectiveIdentity). Which evidence
// and PCL fields a live PresentEvent carries, and how the collector publishes
// the resolved presents, is decided by the native PCL collector and still
// needs capture validation.

#include "present_association.h"

#include <cstdint>
#include <iostream>
#include <vector>

namespace
{
    bool Check(bool condition, const char* message)
    {
        if (!condition) std::cerr << "FAIL: " << message << '\n';
        return condition;
    }

    bool Classifies(uint32_t evidence, PresentFrameClassification expected)
    {
        return PresentAssociation_ClassifyFrame(evidence) == expected;
    }

    bool TestSingleEvidence()
    {
        return Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_NONE, PresentFrameClassification::Unknown),
                     "no evidence stays unknown") &&
            Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_PCL_SIMULATION_MARKER,
                             PresentFrameClassification::Real),
                  "PCL simulation marker classifies real evidence") &&
            Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_APPLICATION,
                             PresentFrameClassification::Real),
                  "PresentMon application frame classifies real evidence") &&
            Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_SIMULATION_FALLBACK,
                             PresentFrameClassification::Generated),
                  "simulation fallback classifies generated evidence") &&
            Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_REPEATED,
                             PresentFrameClassification::Generated),
                  "PresentMon repeated frame classifies generated evidence") &&
            Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_GENERATED,
                             PresentFrameClassification::Generated),
                  "PresentMon vendor-generated frame classifies generated evidence") &&
            Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_MIXED_FRAME_TYPES,
                             PresentFrameClassification::Unknown),
                  "mixed frame types alone decide nothing");
    }

    bool TestCombinedEvidence()
    {
        return Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_PCL_SIMULATION_MARKER |
                                    PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_APPLICATION |
                                    PRESENT_CLASSIFICATION_EVIDENCE_MIXED_FRAME_TYPES,
                                PresentFrameClassification::Real),
                     "agreeing real evidence stays real") &&
            Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_SIMULATION_FALLBACK |
                                 PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_GENERATED,
                             PresentFrameClassification::Generated),
                  "agreeing generated evidence stays generated") &&
            Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_PCL_SIMULATION_MARKER |
                                 PRESENT_CLASSIFICATION_EVIDENCE_SIMULATION_FALLBACK,
                             PresentFrameClassification::Conflicting),
                  "conflicting real/generated evidence is not forced") &&
            Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_APPLICATION |
                                 PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_REPEATED,
                             PresentFrameClassification::Conflicting),
                  "application and repeated display instances conflict");
    }

    using Grouper = PresentFrameGrouper<int, 4>;
    using Resolved = Grouper::Resolved;

    constexpr uint64_t kMs = 10'000;
    constexpr uint64_t kStale = 250 * kMs;

    // Feeds one present; the returned vector holds what it resolved.
    std::vector<Resolved> Feed(
        Grouper& grouper, int present, bool marker, uint64_t now_qpc, bool typed = false, uint32_t marker_frame = 0)
    {
        std::vector<Resolved> out;
        grouper.Add(present, marker, typed, now_qpc, &out, marker_frame);
        return out;
    }

    bool Is(const Resolved& resolved, int present, int head, PresentGroupRole role)
    {
        return resolved.present == present && resolved.head == head && resolved.role == role;
    }

    PresentAppIdentity Identity(uint32_t frame_id, uint64_t sim_start)
    {
        PresentAppIdentity identity;
        identity.pcl_frame_id = frame_id;
        identity.pcl_sim_start_qpc = sim_start;
        identity.pcl_input_ping_qpc = sim_start - 5;
        identity.pcl_present_start_qpc = sim_start + 7;
        identity.app_sim_start_qpc = sim_start + 9;
        return identity;
    }

    bool TestFrameGenerationPair()
    {
        // DLSS-G 2x: the marker is on the interpolated frame (1), the app
        // frame's own present (2) follows without one.
        Grouper grouper;
        bool ok = Check(Feed(grouper, 1, true, 1 * kMs).empty(), "a marker present is held") &&
            Check(Feed(grouper, 2, false, 9 * kMs).empty(), "its markerless present is held") &&
            Check(grouper.PendingCount() == 2, "both presents are pending");
        const std::vector<Resolved> out = Feed(grouper, 3, true, 17 * kMs);
        ok = Check(out.size() == 2, "the next marker resolves the pair") && ok;
        if (out.size() == 2) {
            ok = Check(Is(out[0], 1, 1, PresentGroupRole::Generated),
                       "the first present of the pair is generated") &&
                Check(Is(out[1], 2, 1, PresentGroupRole::Real),
                      "the last present is real and takes the marker present's identity") && ok;
        }

        // The identity moves: Real carries the marker present's, Generated none.
        const PresentAppIdentity marker_identity = Identity(60, 1'000);
        const PresentAppIdentity markerless_identity{};
        const PresentAppIdentity generated = PresentAssociation_EffectiveIdentity(
            PresentGroupRole::Generated, marker_identity, marker_identity);
        const PresentAppIdentity real = PresentAssociation_EffectiveIdentity(
            PresentGroupRole::Real, markerless_identity, marker_identity);
        ok = Check(generated.pcl_frame_id == 0 && generated.pcl_sim_start_qpc == 0 &&
                   generated.pcl_input_ping_qpc == 0 && generated.pcl_present_start_qpc == 0 &&
                   generated.app_sim_start_qpc == 0,
                   "a generated present keeps no app frame identity") && ok;
        ok = Check(generated.fallback_sim_start_qpc == 1'000,
                   "a generated present measures from the frame's simulation start") && ok;
        return Check(real.pcl_frame_id == 60 && real.pcl_sim_start_qpc == 1'000 &&
                     real.pcl_input_ping_qpc == 995 && real.pcl_present_start_qpc == 1'007 &&
                     real.app_sim_start_qpc == 1'009 && real.fallback_sim_start_qpc == 0,
                     "the real present takes the whole identity") && ok;
    }

    bool TestMultiFrameGeneration()
    {
        Grouper grouper;
        Feed(grouper, 1, true, 1 * kMs);
        Feed(grouper, 2, false, 5 * kMs);
        Feed(grouper, 3, false, 9 * kMs);
        const std::vector<Resolved> out = Feed(grouper, 4, true, 13 * kMs);
        return Check(out.size() == 3, "a 3x group resolves three presents") &&
            Check(Is(out[0], 1, 1, PresentGroupRole::Generated), "3x: first generated") &&
            Check(Is(out[1], 2, 1, PresentGroupRole::Generated), "3x: middle generated") &&
            Check(Is(out[2], 3, 1, PresentGroupRole::Real), "3x: last real");
    }

    bool TestWithoutFrameGeneration()
    {
        // One present per marker: nothing is inferred, each is its own.
        Grouper grouper;
        bool ok = Check(Feed(grouper, 1, true, 1 * kMs).empty(), "a lone marker present waits");
        const std::vector<Resolved> first = Feed(grouper, 2, true, 17 * kMs);
        ok = Check(first.size() == 1 && Is(first[0], 1, 1, PresentGroupRole::Standalone),
                   "a single-present group is standalone") && ok;
        const PresentAppIdentity own = Identity(7, 500);
        const PresentAppIdentity same = PresentAssociation_EffectiveIdentity(
            PresentGroupRole::Standalone, own, PresentAppIdentity{});
        ok = Check(same.pcl_frame_id == 7 && same.pcl_sim_start_qpc == 500 &&
                   same.fallback_sim_start_qpc == 0, "standalone keeps its own identity") && ok;

        // PresentMon types its presents here: never held.
        Grouper typed;
        const std::vector<Resolved> immediate = Feed(typed, 1, true, 1 * kMs, true);
        ok = Check(immediate.size() == 1 && Is(immediate[0], 1, 1, PresentGroupRole::Standalone) &&
                   typed.PendingCount() == 0, "a typed marker present passes through at once") && ok;

        // Presents before any marker (a game without PCL) are standalone at once.
        Grouper unmarked;
        const std::vector<Resolved> pass = Feed(unmarked, 1, false, 1 * kMs);
        return Check(pass.size() == 1 && Is(pass[0], 1, 1, PresentGroupRole::Standalone),
                     "presents without any marker pass through at once") && ok;
    }

    bool TestUndisplayedPresentInPair()
    {
        // The grouper sees no display state: an undisplayed (discarded)
        // present carries no PresentMon frame type and is placed by position
        // like any other. The last present of a dropped app frame is
        // therefore real and carries the frame identity, which the collector
        // then counts as a dropped frame; a displayed present before it is
        // generated.
        Grouper grouper;
        std::vector<Resolved> out;
        grouper.Add(1, true, false, 1 * kMs, &out);   // displayed
        grouper.Add(2, false, false, 9 * kMs, &out);  // discarded
        grouper.Add(3, true, false, 17 * kMs, &out);  // next frame, both discarded
        grouper.Add(4, false, false, 25 * kMs, &out);
        grouper.Add(5, true, false, 33 * kMs, &out);
        return Check(out.size() == 4 && Is(out[0], 1, 1, PresentGroupRole::Generated) &&
                     Is(out[1], 2, 1, PresentGroupRole::Real) &&
                     Is(out[2], 3, 3, PresentGroupRole::Generated) &&
                     Is(out[3], 4, 3, PresentGroupRole::Real),
                     "an undisplayed present is placed by position, a dropped frame's last present is real");
    }

    bool TestRepeatedPresentStaysAuthoritative()
    {
        // A PresentMon frame type is never overridden by position.
        Grouper grouper;
        Feed(grouper, 1, true, 1 * kMs);
        const std::vector<Resolved> out = Feed(grouper, 2, false, 9 * kMs, true);
        bool ok = Check(out.size() == 2 && Is(out[0], 1, 1, PresentGroupRole::Standalone) &&
                        Is(out[1], 2, 2, PresentGroupRole::Standalone),
                        "a typed present releases its group unchanged");
        const std::vector<Resolved> rest = Feed(grouper, 3, false, 12 * kMs);
        ok = Check(rest.size() == 1 && Is(rest[0], 3, 3, PresentGroupRole::Standalone),
                   "the rest of that group passes through") && ok;
        const std::vector<Resolved> next = Feed(grouper, 4, true, 17 * kMs);
        ok = Check(next.empty() && grouper.PendingCount() == 1,
                   "the next marker opens a new inferred group") && ok;

        // Typed evidence classifies by PresentMon's verdict alone.
        return Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_PRESENTMON_REPEATED,
                                PresentFrameClassification::Generated),
                     "a repeated present stays generated") && ok;
    }

    bool TestBoundedAndStaleGroups()
    {
        Grouper grouper; // Capacity 4
        Feed(grouper, 1, true, 1 * kMs);
        Feed(grouper, 2, false, 2 * kMs);
        Feed(grouper, 3, false, 3 * kMs);
        Feed(grouper, 4, false, 4 * kMs);
        const std::vector<Resolved> overflow = Feed(grouper, 5, false, 5 * kMs);
        bool ok = Check(overflow.size() == 5 && grouper.PendingCount() == 0,
                        "a group no marker closed within Capacity presents is released");
        for (const Resolved& resolved : overflow) {
            ok = Check(resolved.role == PresentGroupRole::Standalone && resolved.head == resolved.present,
                       "an unbounded group infers nothing") && ok;
        }

        Grouper stale;
        Feed(stale, 1, true, 1 * kMs);
        Feed(stale, 2, false, 9 * kMs);
        std::vector<Resolved> out;
        stale.FlushIfStale(9 * kMs + kStale - 1, kStale, &out);
        ok = Check(out.empty() && stale.PendingCount() == 2, "a recent group is not flushed") && ok;
        stale.FlushIfStale(5 * kMs, kStale, &out);
        ok = Check(out.empty(), "a clock behind the last present flushes nothing") && ok;
        stale.FlushIfStale(9 * kMs + kStale, kStale, &out);
        ok = Check(out.size() == 2 && Is(out[0], 1, 1, PresentGroupRole::Generated) &&
                   Is(out[1], 2, 1, PresentGroupRole::Real) && stale.PendingCount() == 0,
                   "a stale pair resolves as a pair") && ok;
        const std::vector<Resolved> late = Feed(stale, 3, false, 400 * kMs);
        ok = Check(late.size() == 1 && Is(late[0], 3, 3, PresentGroupRole::Standalone),
                   "a present after the stale flush joins no group") && ok;

        Grouper lone;
        Feed(lone, 1, true, 1 * kMs);
        out.clear();
        lone.FlushIfStale(1 * kMs + kStale, kStale, &out);
        return Check(out.size() == 1 && Is(out[0], 1, 1, PresentGroupRole::Standalone),
                     "a stale lone present is standalone") && ok;
    }

    bool TestEarlierPresentClassification()
    {
        return Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_EARLIER_PRESENT_OF_FRAME,
                                PresentFrameClassification::Generated),
                     "an earlier present of the frame is generated") &&
            Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_EARLIER_PRESENT_OF_FRAME |
                                 PRESENT_CLASSIFICATION_EVIDENCE_PCL_SIMULATION_MARKER,
                             PresentFrameClassification::Conflicting),
                  "an earlier present never also carries the marker") &&
            Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_LAST_PRESENT_OF_FRAME,
                             PresentFrameClassification::Real),
                  "the frame's last present is real without the frame's marker") &&
            Check(Classifies(PRESENT_CLASSIFICATION_EVIDENCE_LAST_PRESENT_OF_FRAME |
                                 PRESENT_CLASSIFICATION_EVIDENCE_PCL_SIMULATION_MARKER,
                             PresentFrameClassification::Real),
                  "and with it");
    }

    // Feeds one present the add-on placed in its frame generation cycle.
    std::vector<Resolved> Place(
        Grouper& grouper, int present, uint32_t own_frame, uint32_t frame, bool last, uint64_t now_qpc)
    {
        std::vector<Resolved> out;
        grouper.AddPlaced(present, own_frame, frame, last, now_qpc, &out);
        return out;
    }

    bool IsPlaced(const std::vector<Resolved>& out, int present, int head, PresentGroupRole role, bool identified = true)
    {
        return out.size() == 1 && Is(out[0], present, head, role) && out[0].identified == identified;
    }

    bool TestPlacedCycles()
    {
        // DLSS-G 3x presenting from its own thread: presents 1 to 3 are frame 60's, 4 to 6 frame 61's, 7 to 9
        // frame 62's. The marker of a frame is on the first present after the game's PRESENT_START, whichever
        // cycle that present belongs to; the place says which present is the real frame's.
        Grouper grouper;
        bool ok = Check(IsPlaced(Place(grouper, 1, 60, 60, false, 1 * kMs), 1, 1, PresentGroupRole::Generated),
                        "a placed present resolves at once: the first of its cycle is generated") &&
            Check(IsPlaced(Place(grouper, 2, 0, 60, false, 7 * kMs), 2, 1, PresentGroupRole::Generated),
                  "the middle present is generated, its head the frame's marker present") &&
            // Frame 61's marker came before frame 60's last present: that present is still frame 60's real one.
            Check(IsPlaced(Place(grouper, 3, 61, 60, true, 13 * kMs), 3, 1, PresentGroupRole::Real),
                  "the last present is real and takes its own frame's identity, not the marker it carries");
        ok = Check(IsPlaced(Place(grouper, 4, 0, 61, false, 19 * kMs), 4, 3, PresentGroupRole::Generated),
                   "the next cycle's head is the present that carried its frame, in the cycle before") &&
            // Frame 62's marker came inside frame 61's cycle.
            Check(IsPlaced(Place(grouper, 5, 62, 61, false, 25 * kMs), 5, 3, PresentGroupRole::Generated),
                  "a present carrying the next frame's marker inside the cycle stays generated") &&
            Check(IsPlaced(Place(grouper, 6, 0, 61, true, 31 * kMs), 6, 3, PresentGroupRole::Real),
                  "the cycle's last present is real with its own frame's head") && ok;
        ok = Check(IsPlaced(Place(grouper, 7, 0, 62, false, 37 * kMs), 7, 5, PresentGroupRole::Generated),
                   "frame 62's head is the present inside the cycle before that carried it") &&
            Check(IsPlaced(Place(grouper, 9, 0, 62, true, 49 * kMs), 9, 5, PresentGroupRole::Real),
                  "a cycle that lost a present still names its last present real") && ok;
        return ok;
    }

    bool TestPlacedWithoutMarkerPresent()
    {
        // No present carried the cycle's frame: the present has no identity, and a marker it carries itself is
        // another frame's and is not taken.
        Grouper grouper;
        // A frame that is not known (its id's low half is 0) matches no present, an unused place among the kept
        // marker presents least of all.
        bool ok = Check(IsPlaced(Place(grouper, 30, 0, 0, true, 1 * kMs), 30, 30, PresentGroupRole::Real, false),
                        "a present of an unknown frame has no identity");
        ok = Check(IsPlaced(Place(grouper, 1, 0, 70, true, 1 * kMs), 1, 1, PresentGroupRole::Real, false),
                   "a real present whose frame no present carried has no identity") && ok;
        ok = Check(IsPlaced(Place(grouper, 2, 72, 71, true, 7 * kMs), 2, 2, PresentGroupRole::Real, false),
                   "a marker of another frame on the present is not its identity") && ok;
        ok = Check(IsPlaced(Place(grouper, 3, 0, 72, false, 13 * kMs), 3, 2, PresentGroupRole::Generated),
                   "that marker is the next cycle's") && ok;

        // A frame id met again (the game's count began again): the newer marker present is the frame's.
        Grouper again;
        Place(again, 1, 60, 60, true, 1 * kMs);
        Place(again, 2, 61, 61, true, 2 * kMs);
        Place(again, 3, 60, 60, false, 3 * kMs);
        ok = Check(IsPlaced(Place(again, 4, 0, 60, true, 4 * kMs), 4, 3, PresentGroupRole::Real),
                   "a frame id met again is the newer marker present's") && ok;

        // Only the newest eight marker presents are kept.
        Grouper bounded;
        Place(bounded, 1, 10, 10, true, 1 * kMs);
        for (int i = 0; i < 8; ++i) Place(bounded, 2 + i, 11 + i, 11 + i, true, (2 + i) * kMs);
        ok = Check(IsPlaced(Place(bounded, 20, 0, 10, true, 20 * kMs), 20, 20, PresentGroupRole::Real, false),
                   "a marker present older than the kept ones is forgotten") &&
            Check(IsPlaced(Place(bounded, 21, 0, 18, true, 21 * kMs), 21, 9, PresentGroupRole::Real),
                  "the newest kept marker present is still found") && ok;

        // The kept marker presents go with a stream that stopped.
        Grouper stale;
        Place(stale, 1, 60, 60, false, 1 * kMs);
        std::vector<Resolved> out;
        stale.FlushIfStale(1 * kMs + kStale - 1, kStale, &out);
        ok = Check(IsPlaced(Place(stale, 2, 0, 60, false, 2 * kMs), 2, 1, PresentGroupRole::Generated),
                   "a recent marker present is kept") && ok;
        stale.FlushIfStale(2 * kMs + kStale, kStale, &out);
        return Check(out.empty() &&
                     IsPlaced(Place(stale, 3, 0, 60, true, 400 * kMs), 3, 3, PresentGroupRole::Real, false),
                     "a stale stream's marker presents are let go") && ok;
    }

    bool TestPlacedAndMarkerOrderMixed()
    {
        // A group the marker order was assembling ends at the first placed present.
        Grouper grouper;
        Feed(grouper, 1, true, 1 * kMs);
        Feed(grouper, 2, false, 7 * kMs);
        const std::vector<Resolved> out = Place(grouper, 3, 61, 61, false, 13 * kMs);
        bool ok = Check(out.size() == 3 && Is(out[0], 1, 1, PresentGroupRole::Generated) &&
                        Is(out[1], 2, 1, PresentGroupRole::Real) && out[0].identified && out[1].identified &&
                        Is(out[2], 3, 3, PresentGroupRole::Generated) && out[2].identified &&
                        grouper.PendingCount() == 0,
                        "a placed present resolves the pending group by the marker order, then itself");
        // Placement stops (the gate was turned off): presents pass through until a marker opens a group again.
        const std::vector<Resolved> pass = Feed(grouper, 4, false, 19 * kMs);
        ok = Check(pass.size() == 1 && Is(pass[0], 4, 4, PresentGroupRole::Standalone) && pass[0].identified,
                   "an unplaced present after placed ones joins no group") && ok;
        ok = Check(Feed(grouper, 5, true, 25 * kMs).empty() && grouper.PendingCount() == 1,
                   "the next marker opens a group by the marker order again") && ok;
        return ok;
    }

    bool TestUnplacedPresentsOfAPlacedCycle()
    {
        // Among placed presents some are not found among the places and come by the marker order, with the frame
        // their marker names.
        Grouper grouper;
        Place(grouper, 1, 60, 60, false, 1 * kMs);
        Place(grouper, 2, 0, 60, false, 2 * kMs);
        Place(grouper, 3, 0, 60, true, 3 * kMs);
        // Frame 61's first present has no place: it is held, and the cycle's next present says whose it is.
        bool ok = Check(Feed(grouper, 4, true, 4 * kMs, false, 61).empty() && grouper.PendingCount() == 1,
                        "an unplaced marker present is held");
        std::vector<Resolved> out = Place(grouper, 5, 0, 61, false, 5 * kMs);
        ok = Check(out.size() == 2 && Is(out[0], 4, 4, PresentGroupRole::Generated) && out[0].identified &&
                   Is(out[1], 5, 4, PresentGroupRole::Generated) && out[1].identified &&
                   grouper.PendingCount() == 0,
                   "a held present that carries the placed cycle's frame is an earlier present of that cycle") && ok;
        ok = Check(IsPlaced(Place(grouper, 6, 0, 61, true, 6 * kMs), 6, 4, PresentGroupRole::Real),
                   "and the cycle's real present takes the frame from it") && ok;

        // Two unplaced presents of frame 62, then its real present.
        Feed(grouper, 7, true, 7 * kMs, false, 62);
        Feed(grouper, 8, false, 8 * kMs);
        out = Place(grouper, 9, 0, 62, true, 9 * kMs);
        ok = Check(out.size() == 3 && Is(out[0], 7, 7, PresentGroupRole::Generated) &&
                   Is(out[1], 8, 7, PresentGroupRole::Generated) && Is(out[2], 9, 7, PresentGroupRole::Real) &&
                   out[2].identified,
                   "every held present of the cycle is generated and the placed last one real") && ok;

        // A whole cycle without places ends by the marker order at the next cycle's placed present.
        Feed(grouper, 10, true, 10 * kMs, false, 63);
        Feed(grouper, 11, false, 11 * kMs);
        Feed(grouper, 12, false, 12 * kMs);
        out = Place(grouper, 13, 64, 64, false, 13 * kMs);
        ok = Check(out.size() == 4 && Is(out[0], 10, 10, PresentGroupRole::Generated) &&
                   Is(out[1], 11, 10, PresentGroupRole::Generated) && Is(out[2], 12, 10, PresentGroupRole::Real) &&
                   Is(out[3], 13, 13, PresentGroupRole::Generated) && out[3].identified,
                   "held presents of another frame end their group by the marker order") && ok;
        return ok;
    }
}

int main()
{
    const bool passed = TestSingleEvidence() && TestCombinedEvidence() &&
        TestFrameGenerationPair() && TestMultiFrameGeneration() &&
        TestWithoutFrameGeneration() && TestUndisplayedPresentInPair() &&
        TestRepeatedPresentStaysAuthoritative() && TestBoundedAndStaleGroups() &&
        TestEarlierPresentClassification() && TestPlacedCycles() && TestPlacedWithoutMarkerPresent() &&
        TestPlacedAndMarkerOrderMixed() && TestUnplacedPresentsOfAPlacedCycle();
    if (!passed) return 1;
    std::cout << "present classification tests passed\n";
    return 0;
}
