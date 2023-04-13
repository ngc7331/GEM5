#include "cpu/pred/ftb/decoupled_bpred.hh"

#include "base/output.hh"
#include "base/debug_helper.hh"
#include "cpu/pred/ftb/stream_common.hh"
#include "debug/DecoupleBPVerbose.hh"
#include "debug/DecoupleBPHist.hh"
#include "debug/Override.hh"
#include "debug/FTB.hh"
#include "debug/FTBITTAGE.hh"
#include "sim/core.hh"

namespace gem5
{
namespace branch_prediction
{
namespace ftb_pred
{

DecoupledBPUWithFTB::DecoupledBPUWithFTB(const DecoupledBPUWithFTBParams &p)
    : BPredUnit(p),
      enableLoopBuffer(p.enableLoopBuffer),
      enableLoopPredictor(p.enableLoopPredictor),
      fetchTargetQueue(p.ftq_size),
      fetchStreamQueueSize(p.fsq_size),
      numBr(p.numBr),
      historyBits(p.maxHistLen),
      uftb(p.uftb),
      ftb(p.ftb),
      tage(p.tage),
      ittage(p.ittage),
      ras(p.ras),
      enableDB(p.enableBPDB),
      numStages(p.numStages),
      historyManager(p.numBr),
      dbpFtbStats(this, p.numStages, p.fsq_size)
{
    if (enableDB) {
        bpdb.init_db();
        std::vector<std::pair<std::string, DataType>> fields_vec = {
            std::make_pair("startPC", UINT64),
            std::make_pair("controlPC", UINT64),
            std::make_pair("controlType", UINT64),
            std::make_pair("taken", UINT64),
            std::make_pair("mispred", UINT64),
            std::make_pair("fallThruPC", UINT64),
            std::make_pair("source", UINT64),
            std::make_pair("target", UINT64)
        };
        bptrace = bpdb.addAndGetTrace("BPTRACE", fields_vec);
        bptrace->init_table();
    }

    bpType = DecoupledFTBType;
    numStages = 3;
    // TODO: better impl (use vector to assign in python)
    // problem: ftb->getAndSetNewFTBEntry
    components.push_back(uftb);
    components.push_back(ftb);
    components.push_back(tage);
    components.push_back(ras);
    components.push_back(ittage);
    numComponents = components.size();
    for (int i = 0; i < numComponents; i++) {
        components[i]->setComponentIdx(i);
        if (enableDB) {
            components[i]->enableDB = true;
            components[i]->setDB(&bpdb);
            components[i]->setTrace();
        }
    }

    predsOfEachStage.resize(numStages);
    for (unsigned i = 0; i < numStages; i++) {
        predsOfEachStage[i].predSource = i;
        predsOfEachStage[i].condTakens.resize(numBr, false);
    }

    s0PC = 0x80000000;

    s0History.resize(historyBits, 0);
    fetchTargetQueue.setName(name());

    commitHistory.resize(historyBits, 0);
    squashing = true;

    lp = LoopPredictor(16, 4);

    if (!enableLoopPredictor && enableLoopBuffer) {
        fatal("loop buffer cannot be enabled without loop predictor\n");
    }

    registerExitCallback([this]() {
        auto out_handle = simout.create("topMisPredicts.txt", false, true);
        *out_handle->stream() << "startPC" << " " << "control pc" << " " << "count" << std::endl;
        std::vector<std::pair<std::pair<Addr, Addr>, int>> topMisPredPC;
        for (auto &it : topMispredicts) {
            topMisPredPC.push_back(it);
        }
        std::sort(topMisPredPC.begin(), topMisPredPC.end(), [](const std::pair<std::pair<Addr, Addr>, int> &a, const std::pair<std::pair<Addr, Addr>, int> &b) {
            return a.second > b.second;
        });
        for (auto& it : topMisPredPC) {
            *out_handle->stream() << std::hex << it.first.first << " " << it.first.second << " " << std::dec << it.second << std::endl;
        }
        simout.close(out_handle);

        out_handle = simout.create("topMisPredictHist.txt", false, true);
        // *out_handle->stream() << "use loop but invalid: " << useLoopButInvalid 
        //                       << " use loop and valid: " << useLoopAndValid 
        //                       << " not use loop: " << notUseLoop << std::endl;
        *out_handle->stream() << "Hist" << " " << "count" << std::endl;
        std::vector<std::pair<uint64_t, uint64_t>> topMisPredHistVec;
        for (const auto &entry: topMispredHist) {
            topMisPredHistVec.push_back(entry);
        }
        std::sort(topMisPredHistVec.begin(), topMisPredHistVec.end(),
                  [](const std::pair<uint64_t, uint64_t> &a,
                     const std::pair<uint64_t, uint64_t> &b) {
                      return a.second > b.second;
                  });
        for (const auto &entry: topMisPredHistVec) {
            *out_handle->stream() << std::hex << entry.first << " " << std::dec << entry.second << std::endl;
        }

        // if (dumpLoopPred) {
        //     out_handle = simout.create("misPredTripCount.txt", false, true);
        //     *out_handle->stream() << missCount << std::endl;
        //     for (const auto &entry : misPredTripCount) {
        //         *out_handle->stream()
        //             << entry.first << " " << entry.second << std::endl;
        //     }

        //     out_handle = simout.create("loopInfo.txt", false, true);
        //     for (const auto &entry : storedLoopStreams) {
        //         bool misPred = entry.second.squashType == SQUASH_CTRL;
        //         *out_handle->stream()
        //             << std::dec << "miss: " << misPred << " " << entry.first << " "
        //             << std::hex << entry.second.startPC << ", "
        //             << (misPred ? entry.second.exeBranchPC
        //                         : entry.second.predBranchPC)
        //             << "--->"
        //             << (misPred ? entry.second.exeTarget : entry.second.predTarget)
        //             << std::dec
        //             << " useLoopPred: " << entry.second.useLoopPrediction
        //             << " tripCount: " << entry.second.tripCount << std::endl;
        //     }
        // }

        // out_handle = simout.create("targets.txt", false, true);
        // for (const auto it : storeTargets) {
        //     *out_handle->stream() << std::hex << it << std::endl;
        // }

        out_handle = simout.create("misPredIndirectStream.txt", false, true);
        std::vector<std::pair<Addr, unsigned>> tempVec;
        for (auto &it : topMispredIndirect) {
            tempVec.push_back(std::make_pair(it.first, it.second));
        }
        std::sort(tempVec.begin(), tempVec.end(),
            [](const std::pair<Addr, unsigned> &a,
               const std::pair<Addr, unsigned> &b) {
                return a.second > b.second;
            });
        for (auto it : tempVec) {
            *out_handle->stream() << std::oct << it.second << " " << std::hex << it.first << std::endl;
        }

        simout.close(out_handle);
        if (enableDB) {
            bpdb.save_db("bp.db");
        }
    });
}

DecoupledBPUWithFTB::DBPFTBStats::DBPFTBStats(statistics::Group* parent, unsigned numStages, unsigned fsqSize):
    statistics::Group(parent),
    ADD_STAT(condNum, statistics::units::Count::get(), "the number of cond branches"),
    ADD_STAT(uncondNum, statistics::units::Count::get(), "the number of uncond branches"),
    ADD_STAT(returnNum, statistics::units::Count::get(), "the number of return branches"),
    ADD_STAT(otherNum, statistics::units::Count::get(), "the number of other branches"),
    ADD_STAT(condMiss, statistics::units::Count::get(), "the number of cond branch misses"),
    ADD_STAT(uncondMiss, statistics::units::Count::get(), "the number of uncond branch misses"),
    ADD_STAT(returnMiss, statistics::units::Count::get(), "the number of return branch misses"),
    ADD_STAT(otherMiss, statistics::units::Count::get(), "the number of other branch misses"),
    ADD_STAT(predsOfEachStage, statistics::units::Count::get(), "the number of preds of each stage that account for final pred"),
    ADD_STAT(commitPredsFromEachStage, statistics::units::Count::get(), "the number of preds of each stage that account for a committed stream"),
    ADD_STAT(fsqEntryDist, statistics::units::Count::get(), "the distribution of number of entries in fsq"),
    ADD_STAT(controlSquash, statistics::units::Count::get(), "the number of control squashes in bpu"),
    ADD_STAT(nonControlSquash, statistics::units::Count::get(), "the number of non-control squashes in bpu"),
    ADD_STAT(trapSquash, statistics::units::Count::get(), "the number of trap squashes in bpu"),
    ADD_STAT(ftqNotValid, statistics::units::Count::get(), "fetch needs ftq req but ftq not valid"),
    ADD_STAT(fsqNotValid, statistics::units::Count::get(), "ftq needs fsq req but fsq not valid"),
    ADD_STAT(fsqFullCannotEnq, statistics::units::Count::get(), "bpu has req but fsq full cannot enqueue"),
    ADD_STAT(ftbHit, statistics::units::Count::get(), "ftb hits (in predict block)"),
    ADD_STAT(ftbMiss, statistics::units::Count::get(), "ftb misses (in predict block)"),
    ADD_STAT(predFalseHit, statistics::units::Count::get(), "false hit detected at pred"),
    ADD_STAT(commitFalseHit, statistics::units::Count::get(), "false hit detected at commit")
{
    predsOfEachStage.init(numStages);
    commitPredsFromEachStage.init(numStages);
    fsqEntryDist.init(0, fsqSize, 1);
}

void
DecoupledBPUWithFTB::tick()
{
    dbpFtbStats.fsqEntryDist.sample(fetchStreamQueue.size(), 1);
    if (streamQueueFull()) {
        dbpFtbStats.fsqFullCannotEnq++;
    }

    if (!receivedPred && numOverrideBubbles == 0 && sentPCHist) {
        generateFinalPredAndCreateBubbles();
    }
    if (!squashing) {
        DPRINTF(DecoupleBP, "DecoupledBPUWithFTB::tick()\n");
        DPRINTF(Override, "DecoupledBPUWithFTB::tick()\n");
        tryEnqFetchTarget();
        tryEnqFetchStream();
    } else {
        receivedPred = false;
        DPRINTF(DecoupleBP, "Squashing, skip this cycle, receivedPred is %d.\n", receivedPred);
        DPRINTF(Override, "Squashing, skip this cycle, receivedPred is %d.\n", receivedPred);
    }

    if (numOverrideBubbles > 0) {
        numOverrideBubbles--;
    }

    sentPCHist = false;

    if (!receivedPred && !streamQueueFull()) {
        if (!enableLoopBuffer || (enableLoopBuffer && !lb.isActive())) {
            if (s0PC == ObservingPC) {
                DPRINTFV(true, "Predicting block %#lx, id: %lu\n", s0PC, fsqId);
            }   
            DPRINTF(DecoupleBP, "Requesting prediction for stream start=%#lx\n", s0PC);
            DPRINTF(Override, "Requesting prediction for stream start=%#lx\n", s0PC);
            // put startAddr in preds
            for (int i = 0; i < numStages; i++) {
                predsOfEachStage[i].bbStart = s0PC;
            }
            for (int i = 0; i < numComponents; i++) {
                components[i]->putPCHistory(s0PC, s0History, predsOfEachStage);
            }
        } else {
            DPRINTF(LoopBuffer, "Do not query bpu when loop buffer is active\n");
            DPRINTF(DecoupleBP, "Do not query bpu when loop buffer is active\n");
        }


        sentPCHist = true;
    }

    // query loop buffer with start pc
    if (enableLoopBuffer && !lb.isActive() &&
            lb.streamBeforeLoop.getTakenTarget() == lb.streamBeforeLoop.startPC) {
        lb.tryActivateLoop(s0PC);
    }

    DPRINTF(Override, "after putPCHistory\n");
    for (int i = 0; i < numStages; i++) {
        printFullFTBPrediction(predsOfEachStage[i]);
    }
    
    if (streamQueueFull()) {
        DPRINTF(DecoupleBP, "Stream queue is full, don't request prediction\n");
        DPRINTF(Override, "Stream queue is full, don't request prediction\n");
    }
    squashing = false;
}

// this function collects predictions from all stages and generate bubbles
// when loop buffer is active, predictions are from saved stream
void
DecoupledBPUWithFTB::generateFinalPredAndCreateBubbles()
{
    DPRINTF(Override, "In generateFinalPredAndCreateBubbles().\n");

    if (!enableLoopBuffer || (enableLoopBuffer && !lb.isActive())) {
        // predsOfEachStage should be ready now
        for (int i = 0; i < numStages; i++) {
            printFullFTBPrediction(predsOfEachStage[i]);
        }
        // choose the most accurate prediction
        FullFTBPrediction *chosen = &predsOfEachStage[0];

        for (int i = (int) numStages - 1; i >= 0; i--) {
            if (predsOfEachStage[i].valid) {
                chosen = &predsOfEachStage[i];
                DPRINTF(Override, "choose stage %d.\n", i);
                break;
            }
        }
        finalPred = *chosen;
        // calculate bubbles
        unsigned first_hit_stage = 0;
        while (first_hit_stage < numStages-1) {
            if (predsOfEachStage[first_hit_stage].match(*chosen)) {
                break;
            }
            first_hit_stage++;
        }
        // generate bubbles
        numOverrideBubbles = first_hit_stage;
        // assign pred source
        finalPred.predSource = first_hit_stage;
        receivedPred = true;

        printFullFTBPrediction(*chosen);
        dbpFtbStats.predsOfEachStage[first_hit_stage]++;
    } else {
        numOverrideBubbles = 0;
        receivedPred = true;
        DPRINTF(LoopBuffer, "Do not generate final pred when loop buffer is active\n");
        DPRINTF(DecoupleBP, "Do not generate final pred when loop buffer is active\n");
    }
    DPRINTF(Override, "Ends generateFinalPredAndCreateBubbles(), numOverrideBubbles is %d, receivedPred is set true.\n", numOverrideBubbles);

}

bool
DecoupledBPUWithFTB::trySupplyFetchWithTarget(Addr fetch_demand_pc, bool &fetch_target_in_loop)
{
    return fetchTargetQueue.trySupplyFetchWithTarget(fetch_demand_pc, fetch_target_in_loop);
}

std::pair<bool, bool>
DecoupledBPUWithFTB::decoupledPredict(const StaticInstPtr &inst,
                               const InstSeqNum &seqNum, PCStateBase &pc,
                               ThreadID tid)
{
    std::unique_ptr<PCStateBase> target(pc.clone());

    DPRINTF(DecoupleBP, "looking up pc %#lx\n", pc.instAddr());
    auto target_avail = fetchTargetQueue.fetchTargetAvailable();

    DPRINTF(DecoupleBP, "Supplying fetch with target ID %lu\n",
            fetchTargetQueue.getSupplyingTargetId());

    if (!target_avail) {
        DPRINTF(DecoupleBP,
                "No ftq entry to fetch, return dummy prediction\n");
        // todo pass these with reference
        // TODO: do we need to update PC if not taken?
        return std::make_pair(false, true);
    }

    const auto &target_to_fetch = fetchTargetQueue.getTarget();
    // found corresponding entry
    auto start = target_to_fetch.startPC;
    auto end = target_to_fetch.endPC;
    auto taken_pc = target_to_fetch.takenPC;
    auto in_loop = target_to_fetch.inLoop;
    auto loop_iter = target_to_fetch.iter;
    auto loop_exit = target_to_fetch.isExit;
    DPRINTF(DecoupleBP, "Responsing fetch with");
    printFetchTarget(target_to_fetch, "");

    auto current_loop_iter = fetchTargetQueue.getCurrentLoopIter();

    // supplying ftq entry might be taken before pc
    // because it might just be updated last cycle
    // but last cycle ftq tells fetch that this is a miss stream
    assert(pc.instAddr() < end && pc.instAddr() >= start);
    bool raw_taken = pc.instAddr() == taken_pc && target_to_fetch.taken;
    bool taken = raw_taken;
    bool run_out_of_this_entry = false;
    // an ftq entry may consists of multiple loop iterations,
    // so we need to check if we are at the end of this loop iteration,
    // since taken and not taken can both exist in the same ftq entry
    if (in_loop) {
        DPRINTF(LoopBuffer, "current loop iter %d, loop_iter %d, loop_exit %d\n",
            current_loop_iter, loop_iter, loop_exit);
        if (raw_taken) {
            if (current_loop_iter >= loop_iter - 1) {
                run_out_of_this_entry = true;
                if (loop_exit) {
                    taken = false;
                    DPRINTF(LoopBuffer, "modifying taken to false because of loop exit\n");
                }
            }
            fetchTargetQueue.incCurrentLoopIter(loop_iter);
        }
    } else {
        if (taken) {
            run_out_of_this_entry = true;
        }
    }

    if (taken) {
        auto &rtarget = target->as<GenericISA::PCStateWithNext>();
        rtarget.pc(target_to_fetch.target);
        // TODO: how about compressed?
        rtarget.npc(target_to_fetch.target + 4);
        rtarget.uReset();
        DPRINTF(DecoupleBP,
                "Predicted pc: %#lx, upc: %#lx, npc(meaningless): %#lx, instSeqNum: %d\n",
                target->instAddr(), rtarget.upc(), rtarget.npc(), seqNum);
        set(pc, *target);
    } else {
        inst->advancePC(*target);
        if (target->instAddr() >= end) {
            run_out_of_this_entry = true;
        }
    }
    DPRINTF(DecoupleBP, "Predict it %staken to %#lx\n", taken ? "" : "not ",
            target->instAddr());

    if (run_out_of_this_entry) {
        // dequeue the entry
        DPRINTF(DecoupleBP, "running out of ftq entry %lu\n",
                fetchTargetQueue.getSupplyingTargetId());
        fetchTargetQueue.finishCurrentFetchTarget();
    }

    return std::make_pair(taken, run_out_of_this_entry);
}

void
DecoupledBPUWithFTB::controlSquash(unsigned target_id, unsigned stream_id,
                            const PCStateBase &control_pc,
                            const PCStateBase &corr_target,
                            const StaticInstPtr &static_inst,
                            unsigned control_inst_size, bool actually_taken,
                            const InstSeqNum &seq, ThreadID tid)
{
    dbpFtbStats.controlSquash++;

    bool is_conditional = static_inst->isCondCtrl();
    bool is_indirect = static_inst->isIndirectCtrl();
    // bool is_call = static_inst->isCall() && !static_inst->isNonSpeculative();
    // bool is_return = static_inst->isReturn() && !static_inst->isNonSpeculative();




    squashing = true;

    // check sanity
    auto squashing_stream_it = fetchStreamQueue.find(stream_id);

    if (squashing_stream_it == fetchStreamQueue.end()) {
        assert(!fetchStreamQueue.empty());
        // assert(fetchStreamQueue.rbegin()->second.getNextStreamStart() == MaxAddr);
        DPRINTF(
            DecoupleBP || debugFlagOn,
            "The squashing stream is insane, ignore squash on it");
        return;
    }

    // recover pc
    s0PC = corr_target.instAddr();

    // get corresponding stream entry
    auto &stream = squashing_stream_it->second;

    auto pc = stream.startPC;
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (pc == ObservingPC) {
        debugFlagOn = true;
    }
    if (control_pc.instAddr() == ObservingPC || control_pc.instAddr() == ObservingPC2) {
        debugFlagOn = true;
    }

    DPRINTF(DecoupleBPHist,
            "stream start=%#lx, predict on hist: %s\n", stream.startPC,
            stream.history);

    DPRINTF(DecoupleBP || debugFlagOn,
            "Control squash: ftq_id=%lu, fsq_id=%lu,"
            " control_pc=%#lx, corr_target=%#lx, is_conditional=%u, "
            "is_indirect=%u, actually_taken=%u, branch seq: %lu\n",
            target_id, stream_id, control_pc.instAddr(),
            corr_target.instAddr(), is_conditional, is_indirect,
            actually_taken, seq);

    dumpFsq("Before control squash");

    // streamLoopPredictor->restoreLoopTable(stream.mruLoop);
    // streamLoopPredictor->controlSquash(stream_id, stream, control_pc.instAddr(), corr_target.instAddr());

    stream.squashType = SQUASH_CTRL;

    FetchTargetId ftq_demand_stream_id;


    stream.exeBranchInfo = BranchInfo(control_pc, corr_target, static_inst, control_inst_size);
    stream.exeTaken = actually_taken;
    stream.squashPC = control_pc.instAddr();

    squashStreamAfter(stream_id);

    stream.resolved = true;

    // recover history to the moment doing prediction
    DPRINTF(DecoupleBPHist,
             "Recover history %s\nto %s\n", s0History, stream.history);
    s0History = stream.history;
    
    // recover history info
    int real_shamt;
    bool real_taken;
    std::tie(real_shamt, real_taken) = stream.getHistInfoDuringSquash(control_pc.instAddr(), is_conditional, actually_taken, numBr);
    for (int i = 0; i < numComponents; ++i) {
        components[i]->recoverHist(s0History, stream, real_shamt, real_taken);
    }
    histShiftIn(real_shamt, real_taken, s0History);
    historyManager.squash(stream_id, real_shamt, real_taken, stream.exeBranchInfo);
    checkHistory(s0History);
    tage->checkFoldedHist(s0History, "control squash");

    DPRINTF(DecoupleBPHist,
                "Shift in history %s\n", s0History);

    if (enableLoopPredictor) {
        // recover loop predictor
        lp.recover(stream.loopRedirectInfo, actually_taken, control_pc.instAddr());
    }
    printStream(stream);

    
    // inc stream id because current stream ends
    // now stream always ends
    ftq_demand_stream_id = stream_id + 1;
    fsqId = stream_id + 1;

    dumpFsq("After control squash");

    fetchTargetQueue.squash(target_id + 1, ftq_demand_stream_id,
                            corr_target.instAddr());

    fetchTargetQueue.dump("After control squash");

    DPRINTFV(this->debugFlagOn || ::gem5::debug::DecoupleBP,
            "After squash, FSQ head Id=%lu, demand stream Id=%lu, Fetch "
            "demanded target Id=%lu\n",
            fsqId, fetchTargetQueue.getEnqState().streamId,
            fetchTargetQueue.getSupplyingTargetId());


}

void
DecoupledBPUWithFTB::nonControlSquash(unsigned target_id, unsigned stream_id,
                               const PCStateBase &inst_pc,
                               const InstSeqNum seq, ThreadID tid)
{
    dbpFtbStats.nonControlSquash++;
    DPRINTFV(this->debugFlagOn || ::gem5::debug::DecoupleBP,
            "non control squash: target id: %lu, stream id: %lu, inst_pc: %x, "
            "seq: %lu\n",
            target_id, stream_id, inst_pc.instAddr(), seq);
    squashing = true;

    dumpFsq("before non-control squash");

    // make sure the stream is in FSQ
    auto it = fetchStreamQueue.find(stream_id);
    assert(it != fetchStreamQueue.end());

    auto ftq_demand_stream_id = stream_id;

    squashStreamAfter(stream_id);

    auto &stream = it->second;

    stream.exeTaken = false;
    stream.resolved = true;
    stream.squashPC = inst_pc.instAddr();
    stream.squashType = SQUASH_OTHER;

    // recover history info
    s0History = it->second.history;
    int real_shamt;
    bool real_taken;
    std::tie(real_shamt, real_taken) = stream.getHistInfoDuringSquash(inst_pc.instAddr(), false, false, numBr);
    for (int i = 0; i < numComponents; ++i) {
        components[i]->recoverHist(s0History, stream, real_shamt, real_taken);
    }
    histShiftIn(real_shamt, real_taken, s0History);
    historyManager.squash(stream_id, real_shamt, real_taken, BranchInfo());
    checkHistory(s0History);
    tage->checkFoldedHist(s0History, "non control squash");
    // fetching from a new fsq entry
    auto pc = inst_pc.instAddr();
    fetchTargetQueue.squash(target_id + 1, ftq_demand_stream_id + 1, pc);

    s0PC = pc;
    fsqId = stream_id + 1;

    if (pc == ObservingPC) dumpFsq("after non-control squash");
    DPRINTFV(this->debugFlagOn || ::gem5::debug::DecoupleBP,
            "After squash, FSQ head Id=%lu, s0pc=%#lx, demand stream Id=%lu, "
            "Fetch demanded target Id=%lu\n",
            fsqId, s0PC, fetchTargetQueue.getEnqState().streamId,
            fetchTargetQueue.getSupplyingTargetId());
}

void
DecoupledBPUWithFTB::trapSquash(unsigned target_id, unsigned stream_id,
                         Addr last_committed_pc, const PCStateBase &inst_pc,
                         ThreadID tid)
{
    dbpFtbStats.trapSquash++;
    DPRINTF(DecoupleBP || debugFlagOn,
            "Trap squash: target id: %lu, stream id: %lu, inst_pc: %#lx\n",
            target_id, stream_id, inst_pc.instAddr());
    squashing = true;

    auto pc = inst_pc.instAddr();

    if (pc == ObservingPC) dumpFsq("before trap squash");

    auto it = fetchStreamQueue.find(stream_id);
    assert(it != fetchStreamQueue.end());
    auto &stream = it->second;

    stream.resolved = true;
    stream.exeTaken = false;
    stream.squashPC = inst_pc.instAddr();
    stream.squashType = SQUASH_TRAP;

    squashStreamAfter(stream_id);

    // recover history info
    s0History = stream.history;
    int real_shamt;
    bool real_taken;
    std::tie(real_shamt, real_taken) = stream.getHistInfoDuringSquash(inst_pc.instAddr(), false, false, numBr);
    for (int i = 0; i < numComponents; ++i) {
        components[i]->recoverHist(s0History, stream, real_shamt, real_taken);
    }
    histShiftIn(real_shamt, real_taken, s0History);
    historyManager.squash(stream_id, real_shamt, real_taken, BranchInfo());
    checkHistory(s0History);
    tage->checkFoldedHist(s0History, "trap squash");

    // inc stream id because current stream is disturbed
    auto ftq_demand_stream_id = stream_id + 1;
    fsqId = stream_id + 1;

    fetchTargetQueue.squash(target_id + 1, ftq_demand_stream_id,
                            inst_pc.instAddr());

    s0PC = inst_pc.instAddr();

    DPRINTF(DecoupleBP,
            "After trap squash, FSQ head Id=%lu, s0pc=%#lx, demand stream "
            "Id=%lu, Fetch demanded target Id=%lu\n",
            fsqId, s0PC, fetchTargetQueue.getEnqState().streamId,
            fetchTargetQueue.getSupplyingTargetId());
}

void DecoupledBPUWithFTB::update(unsigned stream_id, ThreadID tid)
{
    // aka, commit stream
    // commit controls in local prediction history buffer to committedSeq
    // mark all committed control instructions as correct
    // do not need to dequeue when empty
    if (fetchStreamQueue.empty())
        return;
    auto it = fetchStreamQueue.begin();
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    while (it != fetchStreamQueue.end() && stream_id >= it->first) {
        auto &stream = it->second;
        // dequeue
        DPRINTF(DecoupleBP, "dequeueing stream id: %lu, entry below:\n",
                it->first);
        bool miss_predicted = stream.squashType == SQUASH_CTRL;
        if (miss_predicted) {
            DPRINTF(FTBITTAGE || (stream.squashPC == 0x1e0eb6), "miss predicted stream.startAddr=%#lx\n", stream.startPC);
        }
        if (miss_predicted && stream.exeBranchInfo.isIndirect) {
            topMispredIndirect[stream.startPC]++;
        }
        // if (stream.startPC == ObservingPC) {
        //     debugFlagOn = true;
        // }
        // if (stream.exeBranchPC == ObservingPC2) {
        //     debugFlagOn = true;
        // }
        DPRINTF(DecoupleBP || debugFlagOn,
                "Commit stream start %#lx, which is %s predicted, "
                "final br addr: %#lx, final target: %#lx, pred br addr: %#lx, "
                "pred target: %#lx\n",
                stream.startPC, miss_predicted ? "miss" : "correctly",
                stream.exeBranchInfo.pc, stream.exeBranchInfo.target,
                stream.predBranchInfo.pc, stream.predBranchInfo.target);
        
        if (stream.isHit) {
            dbpFtbStats.ftbHit++;
        } else {
            if (stream.exeTaken) {
                dbpFtbStats.ftbMiss++;
                DPRINTF(FTB, "FTB miss detected when update, stream start %#lx, predTick %lu, printing branch info:\n", stream.startPC, stream.predTick);
                auto &slot = stream.exeBranchInfo;
                DPRINTF(FTB, "    pc:%#lx, size:%d, target:%#lx, cond:%d, indirect:%d, call:%d, return:%d\n",
                slot.pc, slot.size, slot.target, slot.isCond, slot.isIndirect, slot.isCall, slot.isReturn);
            }
            if (stream.falseHit) {
                dbpFtbStats.commitFalseHit++;
            }
        }
        dbpFtbStats.commitPredsFromEachStage[stream.predSource]++;


        if (stream.isHit || stream.exeTaken) {
            // generate new ftb entry first
            // each component will use info of this entry to update
            ftb->getAndSetNewFTBEntry(stream);
            for (int i = 0; i < numComponents; ++i) {
                components[i]->update(stream);
            }
        }

        // check loop predictor prediction
        auto lp_info = stream.loopRedirectInfo;
        DPRINTF(LoopPredictor, "at commit fsqid %d, real_branch_pc %#lx, squash type %d, loop predcition info: specCnt %d, tripCnt %d, conf %d, branch_pc %#lx, end_loop %d\n",
                it->first, stream.exeBranchInfo.pc, stream.squashType, lp_info.e.specCnt, lp_info.e.tripCnt, lp_info.e.conf, lp_info.branch_pc, lp_info.end_loop);

        if (stream.squashType == SQUASH_CTRL) {
            auto find_it = topMispredicts.find(std::make_pair(stream.startPC, stream.exeBranchInfo.pc));
            if (find_it == topMispredicts.end()) {
                topMispredicts[std::make_pair(stream.startPC, stream.exeBranchInfo.pc)] = 1;
            } else {
                find_it->second++;
            }

            // if (stream.isMiss /* && stream.exeBranchPC == ObservingPC */) {
            //     missCount++;
            // }

            // if (stream.exeBranchPC == ObservingPC) {
            //     debugFlagOn = true;
            //     auto misTripCount = misPredTripCount.find(stream.tripCount);
            //     if (misTripCount == misPredTripCount.end()) {
            //         misPredTripCount[stream.tripCount] = 1;
            //     } else {
            //         misPredTripCount[stream.tripCount]++;
            //     }
            //     DPRINTF(DecoupleBP || debugFlagOn, "commit mispredicted stream %lu\n", it->first);
            // }
        }

        if (/* stream.startPC == ObservingPC &&  */stream.squashType == SQUASH_CTRL) {
            auto hist(stream.history);
            hist.resize(18);
            uint64_t pattern = hist.to_ulong();
            auto find_it = topMispredHist.find(pattern);
            if (find_it == topMispredHist.end()) {
                topMispredHist[pattern] = 1;
            } else {
                find_it->second++;
            }
        }

        if (enableDB) {
            bptrace->write_record(BpTrace(stream));
        }

        if (enableLoopBuffer) {
            // if current stream is a short loop, try to peek loop buffer
            if (stream.startPC == lastCommittedStream.startPC &&
                lastCommittedStream.exeTaken && stream.exeTaken &&
                lastCommittedStream.exeBranchInfo.target == stream.exeBranchInfo.target &&
                lastCommittedStream.exeBranchInfo.pc == stream.exeBranchInfo.pc &&
                stream.exeBranchInfo.target == stream.startPC) {

                DPRINTF(DecoupleBP, "stream %lu is a loop, lastCommittedStream:\n", it->first);
                printStream(lastCommittedStream);
                DPRINTF(LoopBuffer, "commit peek loop buffer\n");
                lb.commitLoopPeek(stream.startPC, lastCommittedStream.exeBranchInfo.pc);
            }
            lastCommittedStream = stream;
        }

        it = fetchStreamQueue.erase(it);
    }
    DPRINTF(DecoupleBP, "after commit stream, fetchStreamQueue size: %lu\n",
            fetchStreamQueue.size());
    printStream(it->second);

    historyManager.commit(stream_id);
}

void
DecoupledBPUWithFTB::squashStreamAfter(unsigned squash_stream_id)
{
    auto erase_it = fetchStreamQueue.upper_bound(squash_stream_id);
    while (erase_it != fetchStreamQueue.end()) {
        DPRINTF(DecoupleBP || debugFlagOn || erase_it->second.startPC == ObservingPC,
                "Erasing stream %lu when squashing %lu\n", erase_it->first,
                squash_stream_id);
        printStream(erase_it->second);
        fetchStreamQueue.erase(erase_it++);
    }
}

void
DecoupledBPUWithFTB::dumpFsq(const char *when)
{
    DPRINTF(DecoupleBPProbe, "dumping fsq entries %s...\n", when);
    for (auto it = fetchStreamQueue.begin(); it != fetchStreamQueue.end();
         it++) {
        DPRINTFR(DecoupleBPProbe, "StreamID %lu, ", it->first);
        printStream(it->second);
    }
}

// this funtion use finalPred to enq fsq(ftq) and update s0PC
void
DecoupledBPUWithFTB::tryEnqFetchStream()
{
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (s0PC == ObservingPC) {
        debugFlagOn = true;
    }
    if (!receivedPred) {
        DPRINTF(DecoupleBP, "No received prediction, cannot enq fsq\n");
        DPRINTF(Override, "In tryEnqFetchStream(), received is false.\n");
        return;
    } else {
        DPRINTF(Override, "In tryEnqFetchStream(), received is true.\n");
    }
    if (s0PC == MaxAddr) {
        DPRINTF(DecoupleBP, "s0PC %#lx is insane, cannot make prediction\n", s0PC);
        return;
    }
    // prediction valid, but not ready to enq because of bubbles
    if (numOverrideBubbles > 0) {
        DPRINTF(DecoupleBP, "Waiting for bubble caused by overriding, bubbles rest: %u\n", numOverrideBubbles);
        DPRINTF(Override, "Waiting for bubble caused by overriding, bubbles rest: %u\n", numOverrideBubbles);
        return;
    }
    assert(!streamQueueFull());
    if (true) {
        bool should_create_new_stream = true;
        makeNewPrediction(should_create_new_stream);
    } else {
        DPRINTF(DecoupleBP || debugFlagOn, "FSQ is full: %lu\n",
                fetchStreamQueue.size());
    }
    for (int i = 0; i < numStages; i++) {
        predsOfEachStage[i].valid = false;
    }
    receivedPred = false;
    DPRINTF(Override, "In tryFetchEnqStream(), receivedPred reset to false.\n");
    DPRINTF(DecoupleBP || debugFlagOn, "fsqId=%lu\n", fsqId);
}

void
DecoupledBPUWithFTB::setTakenEntryWithStream(const FetchStream &stream_entry, FtqEntry &ftq_entry)
{
    ftq_entry.taken = true;
    ftq_entry.takenPC = stream_entry.getControlPC();
    ftq_entry.endPC = stream_entry.predEndPC;
    ftq_entry.target = stream_entry.getTakenTarget();
    ftq_entry.inLoop = stream_entry.fromLoopBuffer;
    ftq_entry.iter = stream_entry.isDouble ? 2 : stream_entry.fromLoopBuffer ? 1 : 0;
    ftq_entry.isExit = stream_entry.isExit;
    ftq_entry.loopEndPC = stream_entry.getBranchInfo().getEnd();
}

void
DecoupledBPUWithFTB::setNTEntryWithStream(FtqEntry &ftq_entry, Addr end_pc)
{
    ftq_entry.taken = false;
    ftq_entry.takenPC = 0;
    ftq_entry.target = 0;
    ftq_entry.endPC = end_pc;
    ftq_entry.inLoop = false;
    ftq_entry.iter = 0;
    ftq_entry.isExit = false;
    ftq_entry.loopEndPC = 0;
}

void
DecoupledBPUWithFTB::tryEnqFetchTarget()
{
    DPRINTF(DecoupleBP, "Try to enq fetch target\n");
    if (fetchTargetQueue.full()) {
        DPRINTF(DecoupleBP, "FTQ is full\n");
        return;
    }
    if (fetchStreamQueue.empty()) {
        dbpFtbStats.fsqNotValid++;
        // no stream that have not entered ftq
        DPRINTF(DecoupleBP, "No stream to enter ftq in fetchStreamQueue\n");
        return;
    }
    // ftq can accept new cache lines,
    // try to get cache lines from fetchStreamQueue
    // find current stream with ftqEnqfsqID in fetchStreamQueue
    auto &ftq_enq_state = fetchTargetQueue.getEnqState();
    auto it = fetchStreamQueue.find(ftq_enq_state.streamId);
    if (it == fetchStreamQueue.end()) {
        dbpFtbStats.fsqNotValid++;
        // desired stream not found in fsq
        DPRINTF(DecoupleBP, "FTQ enq desired Stream ID %u is not found\n",
                ftq_enq_state.streamId);
        return;
    }

    auto &stream_to_enq = it->second;
    Addr end = stream_to_enq.predEndPC;
    DPRINTF(DecoupleBP, "Serve enq PC: %#lx with stream %lu:\n",
            ftq_enq_state.pc, it->first);
    printStream(stream_to_enq);
    

    // We does let ftq to goes beyond fsq now
    if (ftq_enq_state.pc > end) {
        warn("FTQ enq PC %#lx is beyond fsq end %#lx\n",
         ftq_enq_state.pc, end);
    }
    
    assert(ftq_enq_state.pc <= end || (end < 0x20 && (ftq_enq_state.pc + 0x20 < 0x20)));

    // create a new target entry
    FtqEntry ftq_entry;
    ftq_entry.startPC = ftq_enq_state.pc;
    ftq_entry.fsqID = ftq_enq_state.streamId;

    // set prediction results to ftq entry
    bool taken = stream_to_enq.predTaken;
    bool inLoop = stream_to_enq.fromLoopBuffer;
    bool loopExit = stream_to_enq.isExit;
    Addr loopEndPC = stream_to_enq.getBranchInfo().getEnd();
    if (taken) {
        setTakenEntryWithStream(stream_to_enq, ftq_entry);
    } else {
        setNTEntryWithStream(ftq_entry, end);
    }

    // update ftq_enq_state
    // if in loop, next pc will either be loop exit or loop start
    ftq_enq_state.pc = inLoop ?
        loopExit ? loopEndPC : stream_to_enq.predBranchInfo.target :
        taken ? stream_to_enq.predBranchInfo.target : end;
    ftq_enq_state.streamId++;
    DPRINTF(DecoupleBP,
            "Update ftqEnqPC to %#lx, FTQ demand stream ID to %lu\n",
            ftq_enq_state.pc, ftq_enq_state.streamId);

    fetchTargetQueue.enqueue(ftq_entry);

    assert(ftq_enq_state.streamId <= fsqId + 1);

    // DPRINTF(DecoupleBP, "a%s stream, next enqueue target: %lu\n",
    //         stream_to_enq.getEnded() ? "n ended" : " miss", ftq_enq_state.nextEnqTargetId);
    printFetchTarget(ftq_entry, "Insert to FTQ");
    fetchTargetQueue.dump("After insert new entry");
}

void
DecoupledBPUWithFTB::histShiftIn(int shamt, bool taken, boost::dynamic_bitset<> &history)
{
    if (shamt == 0) {
        return;
    }
    history <<= shamt;
    history[0] = taken;
}

// this function enqueues fsq and update s0PC and s0History
// use loop predictor and loop buffer here
void
DecoupledBPUWithFTB::makeNewPrediction(bool create_new_stream)
{
    DPRINTF(DecoupleBP, "Try to make new prediction\n");
    FetchStream entry_new;
    auto &entry = entry_new;
    entry.startPC = s0PC;
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (s0PC == ObservingPC) {
        debugFlagOn = true;
    }
    if (finalPred.controlAddr() == ObservingPC || finalPred.controlAddr() == ObservingPC2) {
        debugFlagOn = true;
    }
    DPRINTF(DecoupleBP || debugFlagOn, "Make pred with %s, pred valid: %i, taken: %i\n",
             create_new_stream ? "new stream" : "last missing stream",
             finalPred.valid, finalPred.isTaken());

    // if loop buffer is not activated, use normal prediction from branch predictors
    bool endLoop, isDouble;
    LoopRedirectInfo lpRedirectInfo;
    if (!enableLoopBuffer || (enableLoopBuffer && !lb.isActive())) {
        entry.fromLoopBuffer = false;
        entry.isDouble = false;
        entry.isExit = false;

        bool taken = finalPred.isTaken();
        bool predReasonable = finalPred.isReasonable();
        if (predReasonable) {
            if (enableLoopPredictor) {
                // query loop predictor and modify taken result
                // TODO: What if loop branch is predicted not taken?
                Addr branch_addr = finalPred.controlAddr();
                std::tie(endLoop, lpRedirectInfo, isDouble) = lp.shouldEndLoop(taken, branch_addr, false);
                entry.loopRedirectInfo = lpRedirectInfo; // record loop info for redirect recover
                if (endLoop) {
                    // we should only modify the direction of the loop branch, because
                    // a latter branch (outside loop branch) may be taken
                    DPRINTF(DecoupleBP || debugFlagOn, "Loop predictor says end loop at %#lx\n", branch_addr);
                    int takenIdx = finalPred.getTakenBranchIdx();
                    finalPred.condTakens[takenIdx] = false;
                    taken = finalPred.isTaken();
                }
                entry.isExit = endLoop;
            }
            Addr fallThroughAddr = finalPred.getFallThrough();
            entry.isHit = finalPred.valid;
            entry.falseHit = false;
            entry.predFTBEntry = finalPred.ftbEntry;
            entry.predTaken = taken;
            entry.predEndPC = fallThroughAddr;
            // update s0PC
            Addr nextPC = finalPred.getTarget();
            if (taken) {
                entry.predBranchInfo = finalPred.getTakenSlot().getBranchInfo();
                entry.predBranchInfo.target = nextPC; // use the final target which may be not from ftb
            }
            s0PC = nextPC;
        } else {
            DPRINTF(DecoupleBP || debugFlagOn, "Prediction is not reasonable, printing ftb entry\n");
            ftb->printFTBEntry(finalPred.ftbEntry);
            dbpFtbStats.predFalseHit++;
            // prediction is not reasonable, use fall through
            entry.isHit = false;
            entry.falseHit = true;
            entry.predTaken = false;
            entry.predEndPC = entry.startPC + 32;
            entry.predFTBEntry = FTBEntry();
            s0PC = entry.startPC + 32; // TODO: parameterize
            // TODO: when false hit, act like a miss, do not update history
        }

        entry.history = s0History;
        entry.predTick = finalPred.predTick;
        entry.predSource = finalPred.predSource;

        // update (folded) histories for components
        for (int i = 0; i < numComponents; i++) {
            components[i]->specUpdateHist(s0History, finalPred);
            entry.predMetas[i] = components[i]->getPredictionMeta();
        }
        // update ghr
        int shamt;
        std::tie(shamt, taken) = finalPred.getHistInfo();
        boost::to_string(s0History, buf1);
        histShiftIn(shamt, taken, s0History);
        boost::to_string(s0History, buf2);

        historyManager.addSpeculativeHist(entry.startPC, shamt, taken, entry.predBranchInfo, fsqId);
        tage->checkFoldedHist(s0History, "speculative update");

        
        entry.setDefaultResolve();


    } else {
        assert(enableLoopPredictor);
        // loop buffer is activated, use loop buffer to make prediction
        // determine whether this stream entry has double iterations
        std::tie(endLoop, lpRedirectInfo, isDouble) = lp.shouldEndLoop(
            true, lb.getActiveLoopBranch(), lb.activeLoopMayBeDouble()
        );
        entry = lb.streamBeforeLoop;
        entry.startPC = s0PC;
        bool conf = lpRedirectInfo.e.conf == 7;
        bool confExit = conf && endLoop;
        entry.fromLoopBuffer = true;
        entry.isDouble = isDouble;
        entry.isExit = confExit;
        entry.loopRedirectInfo = lpRedirectInfo;

        // redirect to fall through of loop branch if loop is ended
        if (confExit) {
            s0PC = lb.streamBeforeLoop.predBranchInfo.getEnd();
            lb.deactivate(false);
        }
        DPRINTF(LoopBuffer, "stream before loop:\n");
        printStream(lb.streamBeforeLoop);
    }

    if (enableLoopBuffer && !lb.isActive()) {
        lb.recordNewestStreamOutsideLoop(entry);
    }




    auto [insert_it, inserted] = fetchStreamQueue.emplace(fsqId, entry);
    assert(inserted);

    dumpFsq("after insert new stream");
    DPRINTF(DecoupleBP || debugFlagOn, "Insert fetch stream %lu\n", fsqId);

    fsqId++;
    printStream(entry);
}

void
DecoupledBPUWithFTB::checkHistory(const boost::dynamic_bitset<> &history)
{/*
    unsigned ideal_size = 0;
    boost::dynamic_bitset<> ideal_hash_hist(historyBits, 0);
    int ideal_sp = ras->getNonSpecSp();
    std::vector<RAS::RASEntry> ideal_stack(ras->getNonSpecStack());
    for (const auto entry: historyManager.getSpeculativeHist()) {
        if (entry.shamt == 0 && !entry.is_call && !entry.is_return) {
            continue;
        }
        if (entry.shamt != 0) {
            ideal_size += entry.shamt;
            DPRINTF(DecoupleBPVerbose, "pc: %#lx, shamt: %d, cond_taken: %d\n", entry.pc,
                    entry.shamt, entry.cond_taken);
            ideal_hash_hist <<= entry.shamt;
            ideal_hash_hist[0] = entry.cond_taken;
        }
        if (entry.is_call || entry.is_return) {
            DPRINTF(DecoupleBPVerbose, "pc: %#lx, is_call: %d, is_return: %d, retAddr: %#lx\n",
                entry.pc, entry.is_call, entry.is_return, entry.retAddr);
            if (entry.is_call) {
                ras->push(entry.retAddr, ideal_stack, ideal_sp);
            }
            if (entry.is_return) {
                ras->pop(ideal_stack, ideal_sp);
            }
        }
    }
    unsigned comparable_size = std::min(ideal_size, historyBits);
    boost::dynamic_bitset<> sized_real_hist(history);
    ideal_hash_hist.resize(comparable_size);
    sized_real_hist.resize(comparable_size);

    boost::to_string(ideal_hash_hist, buf1);
    boost::to_string(sized_real_hist, buf2);
    DPRINTF(DecoupleBP,
            "Ideal size:\t%u, real history size:\t%u, comparable size:\t%u\n",
            ideal_size, historyBits, comparable_size);
    DPRINTF(DecoupleBP, "Ideal history:\t%s\nreal history:\t%s\n",
            buf1.c_str(), buf2.c_str());
    int sp = ras->getSp();
    DPRINTF(DecoupleBP, "ideal sp:\t%d, real sp:\t%d\n", ideal_sp, sp);
    assert(ideal_hash_hist == sized_real_hist);
    // assert(ideal_sp == sp);
    */
}

void
DecoupledBPUWithFTB::resetPC(Addr new_pc)
{
    s0PC = new_pc;
    fetchTargetQueue.resetPC(new_pc);
}

}  // namespace ftb_pred

}  // namespace branch_prediction

}  // namespace gem5
