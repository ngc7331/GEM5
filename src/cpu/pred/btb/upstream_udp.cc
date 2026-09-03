#include "cpu/pred/btb/upstream_udp.hh"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

UpstreamUDP::Bloom::Bloom(unsigned bits, unsigned hashes, uint64_t seed)
    : bitCount(bits), hashCount(hashes), hashSeed(seed),
      words((bits + 63) / 64, 0)
{
    if (bitCount == 0 || hashCount == 0) {
        throw std::invalid_argument("upstream UDP Bloom dimensions must be non-zero");
    }
}

uint64_t
UpstreamUDP::Bloom::mix(uint64_t value)
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

bool
UpstreamUDP::Bloom::contains(uint64_t value) const
{
    for (unsigned hash = 0; hash < hashCount; ++hash) {
        const uint64_t index = mix(value + hashSeed * (hash + 1)) % bitCount;
        if ((words[index / 64] & (1ULL << (index % 64))) == 0) {
            return false;
        }
    }
    return true;
}

void
UpstreamUDP::Bloom::insert(uint64_t value)
{
    for (unsigned hash = 0; hash < hashCount; ++hash) {
        const uint64_t index = mix(value + hashSeed * (hash + 1)) % bitCount;
        const uint64_t bit = 1ULL << (index % 64);
        auto &word = words[index / 64];
        if ((word & bit) == 0) {
            word |= bit;
            ++setBits;
        }
    }
}

void
UpstreamUDP::Bloom::clear()
{
    std::fill(words.begin(), words.end(), 0);
    setBits = 0;
}

UpstreamUDP::UpstreamUDP(const Config &config)
    : numThreads(config.numThreads),
      offPathThreshold(config.offPathThreshold),
      seniorityHoldCycles(config.seniorityHoldCycles),
      bloomClearPeriod(config.bloomClearPeriod),
      bloomClearUnusefulPermille(config.bloomClearUnusefulPermille),
      bloomOneCapacity(config.bloomOneEntries),
      bloomTwoCapacity(config.bloomTwoEntries),
      bloomFourCapacity(config.bloomFourEntries),
      bloomOne(config.bloomOneBits, config.bloomHashes,
               0x243f6a8885a308d3ULL),
      bloomTwo(config.bloomTwoBits, config.bloomHashes,
               0x13198a2e03707344ULL),
      bloomFour(config.bloomFourBits, config.bloomHashes,
                0xa4093822299f31d0ULL),
      pathConfidence(numThreads, 0),
      forcedOffPath(numThreads, false),
      episodeOnPathCandidates(numThreads, 0),
      episodeOffPathLines(numThreads),
      seniorityFtq(numThreads),
      outstandingPrefetches(numThreads),
      streamBuffers(numThreads)
{
    if (numThreads == 0 || offPathThreshold == 0 ||
        seniorityHoldCycles == 0 || bloomClearPeriod == 0 ||
        bloomClearUnusefulPermille > 1000 || bloomOneCapacity == 0 ||
        bloomTwoCapacity == 0 || bloomFourCapacity == 0) {
        throw std::invalid_argument("invalid upstream UDP configuration");
    }
}

void
UpstreamUDP::checkThread(ThreadID tid) const
{
    if (tid >= numThreads) {
        throw std::out_of_range("upstream UDP thread id out of range");
    }
}

bool
UpstreamUDP::addConfidencePenalty(ThreadID tid, unsigned penalty)
{
    checkThread(tid);
    if (penalty == 0) {
        ++pendingEvents.penaltyZero;
    } else if (penalty == 1) {
        ++pendingEvents.penaltyOne;
    } else {
        ++pendingEvents.penaltyTwoOrMore;
    }
    const bool was_off_path = isOffPath(tid);
    pathConfidence[tid] = std::min<uint64_t>(
        std::numeric_limits<uint64_t>::max() - penalty,
        pathConfidence[tid]) + penalty;
    return !was_off_path && isOffPath(tid);
}

UpstreamUDP::TakenBtbMissResult
UpstreamUDP::signalTakenBtbMiss(ThreadID tid)
{
    checkThread(tid);
    TakenBtbMissResult result{
        isOffPath(tid), episodeOnPathCandidates[tid]};
    forcedOffPath[tid] = true;
    ++pendingEvents.takenBtbMisses;
    pendingEvents.takenBtbMissExposedCandidates += result.exposedCandidates;
    if (result.alreadyOffPath) {
        ++pendingEvents.takenBtbMissAlreadyOffPath;
    } else {
        ++pendingEvents.takenBtbMissNewOffPath;
    }
    return result;
}

bool
UpstreamUDP::resetPathConfidence(ThreadID tid)
{
    checkThread(tid);
    const bool was_off_path = isOffPath(tid);
    pathConfidence[tid] = 0;
    forcedOffPath[tid] = false;
    episodeOnPathCandidates[tid] = 0;
    episodeOffPathLines[tid].clear();
    return was_off_path;
}

bool
UpstreamUDP::isOffPath(ThreadID tid) const
{
    checkThread(tid);
    return forcedOffPath[tid] || pathConfidence[tid] >= offPathThreshold;
}

uint64_t
UpstreamUDP::lineAddress(Addr addr) const
{
    return addr >> LineShift;
}

bool
UpstreamUDP::usefulSetContains(uint64_t line) const
{
    return bloomOne.contains(line) || bloomTwo.contains(line >> 1) ||
           bloomFour.contains(line >> 2);
}

UpstreamUDP::UsefulSetLookup
UpstreamUDP::lookupUsefulSet(uint64_t line)
{
    UsefulSetLookup result;
    ++pendingEvents.bloomOneQueries;
    ++pendingEvents.bloomTwoQueries;
    ++pendingEvents.bloomFourQueries;
    result.oneHit = bloomOne.contains(line);
    result.twoHit = bloomTwo.contains(line >> 1);
    result.fourHit = bloomFour.contains(line >> 2);
    pendingEvents.bloomOneHits += result.oneHit;
    pendingEvents.bloomTwoHits += result.twoHit;
    pendingEvents.bloomFourHits += result.fourHit;
    result.exactHit = bloomOneExact.count(line) != 0 ||
                      bloomTwoExact.count(line >> 1) != 0 ||
                      bloomFourExact.count(line >> 2) != 0;
    return result;
}

UpstreamUDP::Decision
UpstreamUDP::decide(Addr blockAddr, ThreadID tid)
{
    checkThread(tid);
    if (!isOffPath(tid)) {
        ++pendingEvents.onPathCandidates;
        ++episodeOnPathCandidates[tid];
        return Decision::OnPath;
    }

    ++pendingEvents.offPathCandidates;
    const uint64_t line = lineAddress(blockAddr);
    if (episodeOffPathLines[tid].insert(line).second) {
        ++pendingEvents.uniqueOffPathCandidates;
    } else {
        ++pendingEvents.repeatedOffPathCandidates;
    }

    const auto lookup = lookupUsefulSet(line);
    if (!lookup.hit()) {
        return Decision::Filtered;
    }
    ++pendingEvents.usefulSetHits;
    if (!lookup.exactHit) {
        ++pendingEvents.usefulSetFalsePositiveProxy;
    }
    return Decision::UsefulSetHit;
}

void
UpstreamUDP::recordFilteredCandidate(Addr blockAddr, ThreadID tid,
                                     uint64_t cycle)
{
    checkThread(tid);
    const uint64_t line = lineAddress(blockAddr);
    auto &entries = seniorityFtq[tid];
    if (entries.empty() || entries.back().line != line) {
        entries.push_back({line, cycle});
        ++pendingEvents.seniorityAdds;
    } else {
        ++pendingEvents.seniorityDuplicateAdds;
    }
}

void
UpstreamUDP::recordIssuedPrefetch(Addr blockAddr, ThreadID tid,
                                  uint64_t cycle)
{
    checkThread(tid);
    ++pendingEvents.issuedPrefetches;
    const uint64_t line = lineAddress(blockAddr);
    auto &entries = outstandingPrefetches[tid];
    if (entries.empty() || entries.back().line != line) {
        entries.push_back({line, cycle});
    }
}

void
UpstreamUDP::notifyEviction(Addr prefetchVaddr, ThreadID tid, bool unused,
                            uint64_t cycle)
{
    checkThread(tid);
    advance(cycle);
    const uint64_t line = lineAddress(prefetchVaddr);
    auto &outstanding = outstandingPrefetches[tid];
    outstanding.erase(
        std::remove_if(outstanding.begin(), outstanding.end(),
            [line](const TimedLine &entry) { return entry.line == line; }),
        outstanding.end());
    ++windowEvictedPrefetches;
    if (unused) {
        ++windowUnusefulEvictions;
        ++pendingEvents.evictionUnuseful;
    } else {
        ++pendingEvents.evictionUseful;
    }
}

bool
UpstreamUDP::trainLine(uint64_t line, ThreadID tid)
{
    if (usefulSetContains(line)) {
        return false;
    }

    auto &stream = streamBuffers[tid];
    if (stream.count == 0) {
        stream.firstLine = line;
        stream.count = 1;
        return true;
    }

    if (line == stream.firstLine + stream.count &&
        stream.count < StreamBufferEntries) {
        ++stream.count;
        if (stream.count == StreamBufferEntries) {
            flushStream(tid);
        }
        return true;
    }

    flushStream(tid);
    stream.firstLine = line;
    stream.count = 1;
    return true;
}

void
UpstreamUDP::maybeClear(Bloom &filter,
                        std::unordered_set<uint64_t> &exactSet,
                        uint64_t &insertions, uint64_t capacity,
                        uint64_t &clearEvents)
{
    const uint64_t min_samples = std::max<uint64_t>(1, bloomClearPeriod / 100);
    const bool high_unuseful = windowEvictedPrefetches != 0 &&
        windowUnusefulEvictions * 1000 >=
            windowEvictedPrefetches * bloomClearUnusefulPermille;
    if (insertions >= capacity && windowEvictedPrefetches > min_samples &&
        high_unuseful) {
        filter.clear();
        exactSet.clear();
        insertions = 0;
        ++clearEvents;
        ++pendingEvents.bloomClears;
    }
}

void
UpstreamUDP::insertOne(uint64_t line)
{
    if (bloomTwo.contains(line >> 1) || bloomFour.contains(line >> 2)) {
        return;
    }
    maybeClear(bloomOne, bloomOneExact, bloomOneInsertions,
               bloomOneCapacity, pendingEvents.bloomOneClears);
    bloomOne.insert(line);
    bloomOneExact.insert(line);
    ++bloomOneInsertions;
    ++pendingEvents.bloomOneInsertions;
}

void
UpstreamUDP::insertTwo(uint64_t line)
{
    if ((line & 1) != 0) {
        insertOne(line);
        insertOne(line + 1);
        return;
    }
    if (bloomFour.contains(line >> 2) || bloomTwo.contains(line >> 1)) {
        return;
    }
    maybeClear(bloomTwo, bloomTwoExact, bloomTwoInsertions,
               bloomTwoCapacity, pendingEvents.bloomTwoClears);
    bloomTwo.insert(line >> 1);
    bloomTwoExact.insert(line >> 1);
    ++bloomTwoInsertions;
    ++pendingEvents.bloomTwoInsertions;
}

void
UpstreamUDP::insertFour(uint64_t line)
{
    if ((line & 3) != 0) {
        insertTwo(line);
        insertTwo(line + 2);
        return;
    }
    maybeClear(bloomFour, bloomFourExact, bloomFourInsertions,
               bloomFourCapacity, pendingEvents.bloomFourClears);
    bloomFour.insert(line >> 2);
    bloomFourExact.insert(line >> 2);
    ++bloomFourInsertions;
    ++pendingEvents.bloomFourInsertions;
}

void
UpstreamUDP::insertRun(uint64_t firstLine, unsigned count)
{
    uint64_t line = firstLine;
    while (count != 0) {
        if ((line & 3) == 0 && count >= 4) {
            insertFour(line);
            line += 4;
            count -= 4;
        } else if ((line & 1) == 0 && count >= 2) {
            insertTwo(line);
            line += 2;
            count -= 2;
        } else {
            insertOne(line);
            ++line;
            --count;
        }
    }
}

void
UpstreamUDP::flushStream(ThreadID tid)
{
    auto &stream = streamBuffers[tid];
    if (stream.count != 0) {
        insertRun(stream.firstLine, stream.count);
        stream.count = 0;
    }
}

UpstreamUDP::CommitResult
UpstreamUDP::notifyCommit(Addr instAddr, ThreadID tid, uint64_t cycle)
{
    checkThread(tid);
    advance(cycle);
    const uint64_t line = lineAddress(instAddr);
    CommitResult result;

    auto &candidates = seniorityFtq[tid];
    const auto candidate = std::find_if(
        candidates.begin(), candidates.end(),
        [line](const TimedLine &entry) { return entry.line == line; });
    if (candidate != candidates.end()) {
        result.seniorityHit = true;
        result.trained = trainLine(line, tid);
        // A candidate has one usefulness observation. Consume all duplicate
        // observations for the same line so every committed instruction in
        // the line cannot retrain the stream.
        candidates.erase(
            std::remove_if(candidates.begin(), candidates.end(),
                [line](const TimedLine &entry) { return entry.line == line; }),
            candidates.end());
    }

    auto &outstanding = outstandingPrefetches[tid];
    outstanding.erase(
        std::remove_if(outstanding.begin(), outstanding.end(),
            [line](const TimedLine &entry) { return entry.line == line; }),
        outstanding.end());
    return result;
}

void
UpstreamUDP::advance(uint64_t cycle)
{
    if (cycle - windowStartCycle > bloomClearPeriod) {
        windowStartCycle = cycle;
        windowEvictedPrefetches = 0;
        windowUnusefulEvictions = 0;
    }

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        auto &candidates = seniorityFtq[tid];
        while (!candidates.empty() &&
               cycle - candidates.front().cycle > seniorityHoldCycles) {
            candidates.pop_front();
            ++pendingEvents.seniorityExpired;
        }

        auto &outstanding = outstandingPrefetches[tid];
        while (!outstanding.empty() &&
               cycle - outstanding.front().cycle > seniorityHoldCycles) {
            outstanding.pop_front();
            ++pendingEvents.agedUnuseful;
        }
    }
}

UpstreamUDP::Snapshot
UpstreamUDP::snapshot() const
{
    Snapshot result{
        bloomOneInsertions,
        bloomTwoInsertions,
        bloomFourInsertions,
        bloomOne.occupancy(),
        bloomTwo.occupancy(),
        bloomFour.occupancy(),
        bloomOneExact.size(),
        bloomTwoExact.size(),
        bloomFourExact.size(),
    };
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        result.seniorityEntries += seniorityFtq[tid].size();
        result.outstandingPrefetches += outstandingPrefetches[tid].size();
    }
    return result;
}

UpstreamUDP::Events
UpstreamUDP::drainEvents()
{
    Events events = pendingEvents;
    pendingEvents = {};
    return events;
}

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
