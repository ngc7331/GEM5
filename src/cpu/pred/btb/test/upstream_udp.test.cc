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

} // anonymous namespace
