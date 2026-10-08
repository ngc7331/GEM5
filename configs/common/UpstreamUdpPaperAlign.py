"""Opt-in, best-effort UDP paper configuration (ISCA 2024, Table II).

This maps published resources onto the existing gem5 model. It does not
implement Scarab's predictor, unified scheduler, or dual-block prediction.
"""

import math
import os


def validate_paper_align(args, parser):
    if not args.paper_align:
        return
    if args.enable_udp or getattr(args, "param", []):
        parser.error("--paper-align cannot be combined with native UDP or late --param overrides")
    if any(getattr(args, name, False) for name in
           ("ruby", "ideal_kmhv3", "xiangshan_ecore", "smt")):
        parser.error("--paper-align requires the single-thread classic KMHV3 construction path")
    if args.mem_type != "DRAMsim3":
        parser.error("--paper-align requires DRAMsim3 for the paper DDR4-2400 profile")
    if args.bp_profile != "default" or args.standalone_sc or args.btb_tage_upper_bound:
        parser.error("--paper-align requires the default BP profile without standalone SC or upper-bound TAGE")
    if args.disable_fdip or args.disable_dp or args.no_pf or args.enable_pdip:
        parser.error("--paper-align requires FDIP and the paper data-prefetch proxy")
    if args.ideal_cache or args.no_l3cache or args.classic_l2 or args.num_cpus != 1:
        parser.error("--paper-align requires one CPU and the non-ideal four-slice L2/LLC hierarchy")


def _bloom_capacity(bits):
    # p = (1 - exp(-k*n/m))**k, solved for n at p=1%, k=6.
    return math.floor(-bits / 6 * math.log(1 - 0.01 ** (1 / 6)))


def prepare_paper_align(args):
    """Apply construction-time choices after kmhv3's argument defaults."""
    if not args.paper_align:
        return
    args.cpu_clock = "3GHz"
    args.mem_channels = 1
    tree_root = os.path.dirname(os.path.dirname(os.path.dirname(__file__)))
    args.dramsim3_ini = os.path.join(
        tree_root, "ext", "dramsim3", "xiangshan_configs",
        "paper_udp_DDR4_8Gb_x8_2400_1ch.ini")
    args.cacheline_size = 64
    args.caches = args.l2cache = args.l3cache = True
    args.l1i_size, args.l1i_assoc = "32KiB", 8
    args.l1d_size, args.l1d_assoc = "48KiB", 12
    args.l2_size, args.l2_assoc, args.l2_slices = "512KiB", 8, 4
    args.l2_factor = 1
    args.l3_size, args.l3_assoc = "2MiB", 16
    args.l1d_hwp_type = "XSCompositePrefetcher"
    args.l2_hwp_type = "PrefetcherForwarder"
    args.l2_wrapper_hwp_type = "L2CompositeWithWorkerPrefetcher"
    args.l3_hwp_type = "WorkerPrefetcher"
    args.fdip_static_distance = 32
    args.fdip_no_use_static_distance = False
    # Keep the paper default for ordinary aligned runs, but preserve an
    # explicitly requested CLI threshold for threshold-sensitivity runs.
    if not getattr(args, "_upstream_udp_off_path_threshold_explicit", False):
        args.upstream_udp_off_path_threshold = 300
    args.upstream_udp_bloom_one_bits = 16384
    args.upstream_udp_bloom_two_bits = 1024
    args.upstream_udp_bloom_four_bits = 1024
    args.upstream_udp_bloom_hashes = 6
    args.upstream_udp_bloom_one_entries = _bloom_capacity(16384)
    args.upstream_udp_bloom_two_entries = _bloom_capacity(1024)
    args.upstream_udp_bloom_four_entries = _bloom_capacity(1024)
    args.upstream_udp_bloom_clear_unuseful_permille = 750
    # The paper does not specify these two time windows; preserve the port's
    # defaults, rather than suggesting that they were recovered from Table II.
    args.upstream_udp_seniority_hold_cycles = 30000
    args.upstream_udp_bloom_clear_period = 10000


def _nominal_latency(cache, cycles):
    # Parallel tag/data latency. Wrapper, interconnect and CPU pipeline cycles
    # remain additional costs, so this is not an end-to-end hit-time claim.
    cache.tag_latency = cycles
    cache.data_latency = cycles
    cache.response_latency = 0
    cache.sequential_access = False
    cache.pipe_latency = 0


def apply_paper_align(args, system):
    """Final overlay, after setKmhV3Params has applied ordinary defaults."""
    if not args.paper_align:
        return
    from m5.objects import LRURP, NULL

    for cpu in system.cpu:
        cpu.fetchWidth = cpu.decodeWidth = cpu.renameWidth = 6
        cpu.commitWidth = cpu.phyregReleaseWidth = 6
        cpu.dispWidth = [6, 6, 6]
        cpu.numROBEntries = 352
        cpu.LQEntries = cpu.SQEntries = 64
        cpu.RARQEntries = 64
        # Preserve the ISA's supported FU classes and wakeup connections.
        # These partitioned queues total 125, but are not a unified RS.
        iq_sizes = [7, 7] + [6] * 11 + [7] * 4 + [17]
        if len(cpu.scheduler.IQs) != len(iq_sizes):
            raise ValueError("paper-align expects the 18-queue KMHV3 scheduler")
        for queue, size in zip(cpu.scheduler.IQs, iq_sizes):
            queue.size = size

        bp = cpu.branchPred
        # The paper-aligned FDIP baseline shares all resource settings with
        # UDP; only the upstream UDP mechanism is enabled in the UDP variant.
        cpu.enableFdip = True
        cpu.enablePdip = False
        cpu.enableUdp = bp.enableUdp = False
        cpu.enableUpstreamUdp = bp.enableUpstreamUdp = bool(args.enable_upstream_udp)
        bp.ftq_size = bp.fsq_size = 32
        bp.predictWidth = 32
        for name in ("ubtb", "abtb", "microtage", "ras"):
            getattr(bp, name).enabled = False
        for name in ("mbtb", "tage", "ittage", "mgsc"):
            getattr(bp, name).enabled = True
        bp.mbtb.numEntries = 8192
        bp.mbtb.numWays = 4
        bp.mbtb.victimCacheSize = 0
        bp.mbtb.blockSize = 32
        # 20,480 entries * (15 tag + 3 ctr + valid + useful) = 50KiB.
        # Active MGSC arrays add 13.5KiB. Auxiliary counters add ~0.314KiB.
        # Host PC/LRU and speculative snapshots are outside this logic budget.
        bp.tage.numPredictors = 8
        bp.tage.tableSizes = [1024] * 6 + [2048] * 2
        bp.tage.numWays = [2] * 8
        bp.tage.TTagBitSizes = [15] * 8
        bp.tage.TTagPcShifts = [1] * 8
        bp.tage.histLengths = [4, 9, 17, 29, 56, 109, 211, 397]
        bp.tage.blockSize = 32
        # MBTB uses a half-aligned window: a 32-byte prediction beginning in
        # the upper half of a 32-byte aligned block can cover the next block's
        # first 16 bytes. TAGE indexes from the 32-byte aligned start, so 16
        # halfword positions are insufficient (the offset can reach 23).
        bp.tage.maxBranchPositions = 32
        bp.tage.usePathHistory = False
        # Approximate the 2K indirect BTB using one small target table.
        # PathFoldedHist requires at least two history bits; this remains a
        # history-indexed, confidence-qualified proxy, not a literal I-BTB.
        bp.ittage.numPredictors = 1
        bp.ittage.tableSizes = [2048]
        bp.ittage.TTagBitSizes = [16]
        bp.ittage.TTagPcShifts = [1]
        bp.ittage.histLengths = [2]
        bp.ittage.numTablesToAlloc = 1
        bp.ittage.blockSize = 32
        bp.mgsc.forceUseSC = False
        bp.mgsc.allowMissingTageInfo = False

        cpu.icache.size, cpu.icache.assoc = "32KiB", 8
        cpu.dcache.size, cpu.dcache.assoc = "48KiB", 12
        _nominal_latency(cpu.icache, 3)
        # This LSQ samples L1D hits after exactly one cache cycle; a longer
        # response is classified as a miss and enters the miss replay path.
        # Preserve that interface and the
        # existing four-stage load pipeline instead of setting tag/data=4.
        _nominal_latency(cpu.dcache, 1)
        cpu.dcache.pipe_latency = 3
        # TreePLRU requires a power-of-two number of ways; use true LRU.
        cpu.icache.replacement_policy = LRURP()
        cpu.dcache.replacement_policy = LRURP()
        cpu.dcache.wpu = NULL
        pf = cpu.dcache.prefetcher
        for name in ("activepage", "sstride", "pht", "bop", "temporal",
                     "berti", "cplx", "spp", "opt"):
            setattr(pf, "enable_" + name, False)
        pf.enable_xsstream = True
        # The disabled stride component is still constructed; its historical
        # ten-entry default violates AssociativeSet/TreePLRU invariants.
        # MemorySize parameters are converted from strings by gem5's Python
        # parameter layer; an integer raises before the simulation starts.
        pf.sstride.stride_entries = "16"

    for wrapper in system.l2_wrappers:
        for slice_cache in wrapper.slices:
            cache = slice_cache.inner_cache
            cache.size, cache.assoc = "128KiB", 8
            _nominal_latency(cache, 13)
            cache.replacement_policy = LRURP()
            cache.wpu = NULL
        # Keep forwarding connections, disable non-paper request generators.
        pf = wrapper.prefetcher
        pf.enable_bop = pf.enable_cdp = pf.enable_cmc = False
        pf.enable_despacito_stream = False
    system.l3.size, system.l3.assoc = "2MiB", 16
    _nominal_latency(system.l3, 36)
    system.l3.replacement_policy = LRURP()
    mode = "UDP" if args.enable_upstream_udp else "FDIP baseline"
    print(f"Paper alignment: {mode} / ISCA 2024; TAGE+MGSC ~64KiB proxy, "
          "8K BTB, 2K single-table indirect proxy, FTQ 32x32B")
