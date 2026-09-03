#ifndef __CPU_PRED_BTB_UPSTREAM_UDP_HH__
#define __CPU_PRED_BTB_UPSTREAM_UDP_HH__

#include <cstdint>
#include <deque>
#include <unordered_set>
#include <vector>

#include "base/types.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

class UpstreamUDP
{
  public:
    struct Config
    {
        unsigned numThreads;
        unsigned offPathThreshold;
        uint64_t seniorityHoldCycles;
        unsigned bloomOneBits;
        unsigned bloomTwoBits;
        unsigned bloomFourBits;
        unsigned bloomHashes;
        unsigned bloomOneEntries;
        unsigned bloomTwoEntries;
        unsigned bloomFourEntries;
        uint64_t bloomClearPeriod;
        unsigned bloomClearUnusefulPermille;
    };

    enum class Decision
    {
        OnPath,
        UsefulSetHit,
        Filtered
    };

    struct CommitResult
    {
        bool seniorityHit{false};
        bool trained{false};
    };

    struct Events
    {
        uint64_t penaltyZero{0};
        uint64_t penaltyOne{0};
        uint64_t penaltyTwoOrMore{0};
        uint64_t onPathCandidates{0};
        uint64_t offPathCandidates{0};
        uint64_t uniqueOffPathCandidates{0};
        uint64_t repeatedOffPathCandidates{0};
        uint64_t usefulSetHits{0};
        uint64_t usefulSetFalsePositiveProxy{0};
        uint64_t bloomOneQueries{0};
        uint64_t bloomTwoQueries{0};
        uint64_t bloomFourQueries{0};
        uint64_t bloomOneHits{0};
        uint64_t bloomTwoHits{0};
        uint64_t bloomFourHits{0};
        uint64_t bloomOneInsertions{0};
        uint64_t bloomTwoInsertions{0};
        uint64_t bloomFourInsertions{0};
        uint64_t bloomOneClears{0};
        uint64_t bloomTwoClears{0};
        uint64_t bloomFourClears{0};
        uint64_t seniorityAdds{0};
        uint64_t seniorityDuplicateAdds{0};
        uint64_t seniorityExpired{0};
        uint64_t issuedPrefetches{0};
        uint64_t agedUnuseful{0};
        uint64_t evictionUseful{0};
        uint64_t evictionUnuseful{0};
        uint64_t takenBtbMisses{0};
        uint64_t takenBtbMissAlreadyOffPath{0};
        uint64_t takenBtbMissNewOffPath{0};
        uint64_t takenBtbMissExposedCandidates{0};
        uint64_t bloomClears{0};
    };

    struct Snapshot
    {
        uint64_t bloomOneInsertions{0};
        uint64_t bloomTwoInsertions{0};
        uint64_t bloomFourInsertions{0};
        uint64_t bloomOneBitsSet{0};
        uint64_t bloomTwoBitsSet{0};
        uint64_t bloomFourBitsSet{0};
        uint64_t bloomOneExactEntries{0};
        uint64_t bloomTwoExactEntries{0};
        uint64_t bloomFourExactEntries{0};
        uint64_t seniorityEntries{0};
        uint64_t outstandingPrefetches{0};
    };

    struct TakenBtbMissResult
    {
        bool alreadyOffPath{false};
        uint64_t exposedCandidates{0};
    };

    explicit UpstreamUDP(const Config &config);

    bool addConfidencePenalty(ThreadID tid, unsigned penalty);
    TakenBtbMissResult signalTakenBtbMiss(ThreadID tid);
    bool resetPathConfidence(ThreadID tid);
    bool isOffPath(ThreadID tid) const;

    Decision decide(Addr blockAddr, ThreadID tid);
    void recordFilteredCandidate(Addr blockAddr, ThreadID tid,
                                 uint64_t cycle);
    void recordIssuedPrefetch(Addr blockAddr, ThreadID tid,
                              uint64_t cycle);
    void notifyEviction(Addr prefetchVaddr, ThreadID tid, bool unused,
                        uint64_t cycle);
    CommitResult notifyCommit(Addr instAddr, ThreadID tid, uint64_t cycle);
    void advance(uint64_t cycle);
    Events drainEvents();
    Snapshot snapshot() const;

  private:
    static constexpr unsigned LineShift = 6;
    static constexpr unsigned StreamBufferEntries = 8;

    class Bloom
    {
      public:
        Bloom(unsigned bits, unsigned hashes, uint64_t seed);

        bool contains(uint64_t value) const;
        void insert(uint64_t value);
        void clear();
        uint64_t occupancy() const { return setBits; }

      private:
        static uint64_t mix(uint64_t value);

        unsigned bitCount;
        unsigned hashCount;
        uint64_t hashSeed;
        std::vector<uint64_t> words;
        uint64_t setBits{0};
    };

    struct UsefulSetLookup
    {
        bool oneHit{false};
        bool twoHit{false};
        bool fourHit{false};
        bool exactHit{false};

        bool hit() const { return oneHit || twoHit || fourHit; }
    };

    struct TimedLine
    {
        uint64_t line;
        uint64_t cycle;
    };

    struct StreamBuffer
    {
        uint64_t firstLine{0};
        unsigned count{0};
    };

    uint64_t lineAddress(Addr addr) const;
    bool usefulSetContains(uint64_t line) const;
    UsefulSetLookup lookupUsefulSet(uint64_t line);
    bool trainLine(uint64_t line, ThreadID tid);
    void flushStream(ThreadID tid);
    void insertRun(uint64_t firstLine, unsigned count);
    void insertOne(uint64_t line);
    void insertTwo(uint64_t line);
    void insertFour(uint64_t line);
    void maybeClear(Bloom &filter, std::unordered_set<uint64_t> &exactSet,
                    uint64_t &insertions, uint64_t capacity,
                    uint64_t &clearEvents);
    void checkThread(ThreadID tid) const;

    const unsigned numThreads;
    const unsigned offPathThreshold;
    const uint64_t seniorityHoldCycles;
    const uint64_t bloomClearPeriod;
    const unsigned bloomClearUnusefulPermille;
    const uint64_t bloomOneCapacity;
    const uint64_t bloomTwoCapacity;
    const uint64_t bloomFourCapacity;

    Bloom bloomOne;
    Bloom bloomTwo;
    Bloom bloomFour;
    std::unordered_set<uint64_t> bloomOneExact;
    std::unordered_set<uint64_t> bloomTwoExact;
    std::unordered_set<uint64_t> bloomFourExact;

    std::vector<uint64_t> pathConfidence;
    std::vector<bool> forcedOffPath;
    std::vector<uint64_t> episodeOnPathCandidates;
    std::vector<std::unordered_set<uint64_t>> episodeOffPathLines;
    std::vector<std::deque<TimedLine>> seniorityFtq;
    std::vector<std::deque<TimedLine>> outstandingPrefetches;
    std::vector<StreamBuffer> streamBuffers;

    uint64_t bloomOneInsertions{0};
    uint64_t bloomTwoInsertions{0};
    uint64_t bloomFourInsertions{0};
    uint64_t windowStartCycle{0};
    uint64_t windowEvictedPrefetches{0};
    uint64_t windowUnusefulEvictions{0};
    Events pendingEvents;
};

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_UPSTREAM_UDP_HH__
