#include <gtest/gtest.h>

#include "cpu/pred/btb/upstream_udp.hh"

using gem5::branch_prediction::btb_pred::UpstreamUDP;

namespace
{

UpstreamUDP::Config
config(unsigned threshold = 3, uint64_t hold_cycles = 100)
{
    return UpstreamUDP::Config{
        1,
        threshold,
        hold_cycles,
        4096,
        1024,
        1024,
        6,
        128,
        32,
        32,
        1000,
        750,
    };
}

TEST(UpstreamUDPTest, ConfidenceThresholdAndRecovery)
{
    UpstreamUDP udp(config());

    EXPECT_FALSE(udp.isOffPath(0));
    EXPECT_FALSE(udp.addConfidencePenalty(0, 2));
    EXPECT_EQ(udp.decide(0x1000, 0), UpstreamUDP::Decision::OnPath);

    EXPECT_TRUE(udp.addConfidencePenalty(0, 1));
    EXPECT_TRUE(udp.isOffPath(0));
    EXPECT_EQ(udp.decide(0x1000, 0), UpstreamUDP::Decision::Filtered);

    EXPECT_TRUE(udp.resetPathConfidence(0));
    EXPECT_FALSE(udp.isOffPath(0));
    EXPECT_FALSE(udp.resetPathConfidence(0));
}

TEST(UpstreamUDPTest, CommitTrainsFilteredCandidate)
{
    UpstreamUDP udp(config(1));
    ASSERT_TRUE(udp.addConfidencePenalty(0, 1));

    udp.recordFilteredCandidate(0x1000, 0, 1);
    auto first = udp.notifyCommit(0x1004, 0, 2);
    EXPECT_TRUE(first.seniorityHit);
    EXPECT_TRUE(first.trained);

    // The candidate is consumed after its first usefulness observation.
    auto duplicate = udp.notifyCommit(0x1008, 0, 3);
    EXPECT_FALSE(duplicate.seniorityHit);
    EXPECT_FALSE(duplicate.trained);

    // A discontinuity flushes the previous one-line stream to its filter.
    udp.recordFilteredCandidate(0x2000, 0, 4);
    auto second = udp.notifyCommit(0x2000, 0, 5);
    EXPECT_TRUE(second.seniorityHit);
    EXPECT_TRUE(second.trained);
    EXPECT_EQ(udp.decide(0x1000, 0),
              UpstreamUDP::Decision::UsefulSetHit);
}

TEST(UpstreamUDPTest, EightLineStreamUsesCompressedUsefulSet)
{
    UpstreamUDP udp(config(1));
    ASSERT_TRUE(udp.addConfidencePenalty(0, 1));

    constexpr gem5::Addr first = 0x4000;
    for (unsigned index = 0; index < 8; ++index) {
        const gem5::Addr addr = first + index * 64;
        udp.recordFilteredCandidate(addr, 0, index + 1);
        auto result = udp.notifyCommit(addr, 0, index + 1);
        EXPECT_TRUE(result.seniorityHit);
        EXPECT_TRUE(result.trained);
    }

    for (unsigned index = 0; index < 8; ++index) {
        EXPECT_EQ(udp.decide(first + index * 64, 0),
                  UpstreamUDP::Decision::UsefulSetHit);
    }
}

TEST(UpstreamUDPTest, ExpiredCandidateDoesNotTrain)
{
    UpstreamUDP udp(config(1, 10));
    ASSERT_TRUE(udp.addConfidencePenalty(0, 1));

    udp.recordFilteredCandidate(0x3000, 0, 1);
    udp.advance(12);
    const auto result = udp.notifyCommit(0x3000, 0, 12);
    EXPECT_FALSE(result.seniorityHit);
    EXPECT_FALSE(result.trained);
    EXPECT_EQ(udp.decide(0x3000, 0), UpstreamUDP::Decision::Filtered);
}

TEST(UpstreamUDPTest, IssuedPrefetchAgesAsUnuseful)
{
    UpstreamUDP udp(config(1, 10));
    udp.recordIssuedPrefetch(0x5000, 0, 1);
    udp.advance(12);

    auto events = udp.drainEvents();
    EXPECT_EQ(events.agedUnuseful, 1);
    EXPECT_EQ(udp.drainEvents().agedUnuseful, 0);
}

TEST(UpstreamUDPTest, CandidateAndBloomDiagnostics)
{
    auto cfg = config(1);
    cfg.bloomOneBits = 1;
    UpstreamUDP udp(cfg);

    EXPECT_EQ(udp.decide(0x1000, 0), UpstreamUDP::Decision::OnPath);
    ASSERT_TRUE(udp.addConfidencePenalty(0, 1));
    EXPECT_EQ(udp.decide(0x1000, 0), UpstreamUDP::Decision::Filtered);
    EXPECT_EQ(udp.decide(0x1000, 0), UpstreamUDP::Decision::Filtered);

    udp.recordFilteredCandidate(0x1000, 0, 1);
    EXPECT_TRUE(udp.notifyCommit(0x1000, 0, 2).trained);
    udp.recordFilteredCandidate(0x2000, 0, 3);
    EXPECT_TRUE(udp.notifyCommit(0x2000, 0, 4).trained);

    // The one-bit filter must hit, while the exact shadow set does not.
    EXPECT_EQ(udp.decide(0x3000, 0),
              UpstreamUDP::Decision::UsefulSetHit);
    const auto events = udp.drainEvents();
    EXPECT_EQ(events.onPathCandidates, 1);
    EXPECT_EQ(events.offPathCandidates, 3);
    EXPECT_EQ(events.uniqueOffPathCandidates, 2);
    EXPECT_EQ(events.repeatedOffPathCandidates, 1);
    EXPECT_EQ(events.usefulSetHits, 1);
    EXPECT_EQ(events.usefulSetFalsePositiveProxy, 1);
    EXPECT_EQ(events.bloomOneQueries, 3);
    EXPECT_EQ(events.bloomOneHits, 1);
    EXPECT_EQ(events.bloomOneInsertions, 1);

    const auto snapshot = udp.snapshot();
    EXPECT_EQ(snapshot.bloomOneInsertions, 1);
    EXPECT_EQ(snapshot.bloomOneBitsSet, 1);
    EXPECT_EQ(snapshot.bloomOneExactEntries, 1);
}

TEST(UpstreamUDPTest, RealEvictionsDriveBloomClear)
{
    auto cfg = config(1);
    cfg.bloomOneEntries = 1;
    cfg.bloomClearPeriod = 100;
    UpstreamUDP udp(cfg);
    ASSERT_TRUE(udp.addConfidencePenalty(0, 1));

    udp.notifyEviction(0x8000, 0, true, 1);
    udp.notifyEviction(0x9000, 0, true, 2);
    udp.notifyEviction(0xa000, 0, false, 3);

    udp.recordFilteredCandidate(0x1000, 0, 4);
    EXPECT_TRUE(udp.notifyCommit(0x1000, 0, 5).trained);
    udp.recordFilteredCandidate(0x2000, 0, 6);
    EXPECT_TRUE(udp.notifyCommit(0x2000, 0, 7).trained);
    udp.recordFilteredCandidate(0x3000, 0, 8);
    EXPECT_TRUE(udp.notifyCommit(0x3000, 0, 9).trained);

    // Two unused out of three evictions are below the 75% threshold.
    const auto before_threshold = udp.drainEvents();
    EXPECT_EQ(before_threshold.evictionUnuseful, 2);
    EXPECT_EQ(before_threshold.evictionUseful, 1);
    EXPECT_EQ(before_threshold.bloomClears, 0);
    udp.notifyEviction(0xb000, 0, true, 10);
    udp.recordFilteredCandidate(0x4000, 0, 11);
    EXPECT_TRUE(udp.notifyCommit(0x4000, 0, 12).trained);

    const auto events = udp.drainEvents();
    EXPECT_EQ(events.evictionUnuseful, 1);
    EXPECT_EQ(events.evictionUseful, 0);
    EXPECT_EQ(events.bloomOneClears, 1);
    EXPECT_EQ(events.bloomClears, 1);
    EXPECT_EQ(events.agedUnuseful, 0);
}

TEST(UpstreamUDPTest, TakenBtbMissMarksPathBeforeRecovery)
{
    UpstreamUDP udp(config(3));
    EXPECT_EQ(udp.decide(0x1000, 0), UpstreamUDP::Decision::OnPath);
    EXPECT_EQ(udp.decide(0x2000, 0), UpstreamUDP::Decision::OnPath);

    const auto miss = udp.signalTakenBtbMiss(0);
    EXPECT_FALSE(miss.alreadyOffPath);
    EXPECT_EQ(miss.exposedCandidates, 2);
    EXPECT_TRUE(udp.isOffPath(0));
    EXPECT_EQ(udp.decide(0x3000, 0), UpstreamUDP::Decision::Filtered);
    EXPECT_TRUE(udp.resetPathConfidence(0));
    EXPECT_FALSE(udp.isOffPath(0));

    ASSERT_TRUE(udp.addConfidencePenalty(0, 3));
    const auto overlap = udp.signalTakenBtbMiss(0);
    EXPECT_TRUE(overlap.alreadyOffPath);
    EXPECT_EQ(overlap.exposedCandidates, 0);
    EXPECT_TRUE(udp.resetPathConfidence(0));

    const auto events = udp.drainEvents();
    EXPECT_EQ(events.takenBtbMisses, 2);
    EXPECT_EQ(events.takenBtbMissNewOffPath, 1);
    EXPECT_EQ(events.takenBtbMissAlreadyOffPath, 1);
    EXPECT_EQ(events.takenBtbMissExposedCandidates, 2);
}

} // anonymous namespace
