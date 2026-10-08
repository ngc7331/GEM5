"""Opt-in hardware mapping for PDIP, ASPLOS 2024, Table 1 (page 852).

This maps capacities onto the existing RISC-V model, not the paper's x86
pipeline.  In particular, cache wrappers and the partitioned scheduler remain.
The same hardware profile can run FDIP alone or FDIP with upstream PDIP.
Instruction measurement limits are deliberately outside this profile.
"""

from pathlib import Path


TAGE_SETS = [1024, 1024, 2048, 2048, 2048, 2048, 2048, 2048]
ITTAGE_ENTRIES = [512, 512, 512, 1024, 1024]
IQ_SIZES = [10] * 13 + [11] * 4 + [20]


def validate_paper_align(args, parser):
    if not args.paper_align:
        return
    if args.bp_profile != "default":
        parser.error("--paper-align requires --bp-profile default")
    if getattr(args, "num_cpus", 1) != 1 or getattr(args, "smt", False):
        parser.error("--paper-align supports one CPU without SMT")
    if getattr(args, "bp_type", None) not in (None, "DecoupledBPUWithBTB"):
        parser.error("--paper-align requires --bp-type DecoupledBPUWithBTB")
    if getattr(args, "mem_type", "DRAMsim3") != "DRAMsim3":
        parser.error("--paper-align requires --mem-type DRAMsim3")
    incompatible = (
        "btb_tage_upper_bound", "standalone_sc", "no_pf", "no_l3cache",
        "ideal_cache", "ideal_kmhv3", "classic_l2", "xiangshan_ecore",
        "ruby", "enable_udp", "enable_pdip", "disable_fdip",
    )
    for option in incompatible:
        if getattr(args, option, False):
            parser.error("--paper-align cannot be combined with --" +
                         option.replace("_", "-"))
    if getattr(args, "param", []):
        parser.error("--paper-align does not allow late --param overrides")


def configure_paper_align_args(args):
    """Set construction-time geometry and avoid creating data PF hint links."""
    if not args.paper_align:
        return
    # Derive this from the selected configuration tree, including frozen copies.
    # Never fall back to a mutable source-tree DRAM configuration.
    dram_ini = (Path(__file__).resolve().parents[2] / "ext/dramsim3/"
                "xiangshan_configs/xiangshan_DDR4_8Gb_x8_3200_2ch.ini")
    if not dram_ini.is_file():
        raise FileNotFoundError(f"PDIP paper DRAM configuration missing: {dram_ini}")
    args.dramsim3_ini = str(dram_ini)
    args.cacheline_size = 64
    args.l1i_size, args.l1i_assoc = "32KiB", 8
    args.l1d_size, args.l1d_assoc = "64KiB", 16
    args.l2_size, args.l2_assoc, args.l2_slices = "1MiB", 16, 4
    args.l2_factor = 1
    args.l3_size, args.l3_assoc = "2MiB", 16
    args.l1d_hwp_type = None
    args.l2_hwp_type = None
    args.l2_wrapper_hwp_type = None
    args.l3_hwp_type = None
    args.l1_to_l2_pf_hint = False
    args.l2_to_l3_pf_hint = False
    args.fdip_static_distance = 24
    args.fdip_no_use_static_distance = False
    args.upstream_pdip_table_sets = 512
    args.upstream_pdip_table_assoc = 8
    args.upstream_pdip_targets_per_entry = 2
    args.upstream_pdip_prefetch_queue_size = 40
    args.upstream_pdip_queue_threshold = 2
    args.upstream_pdip_tag_bits = 10
    args.upstream_pdip_insertion_probability = 25
    # The existing detector uses >=; 11 cycles implements the paper's >10.
    args.upstream_pdip_min_stall_cycles = 11


def _cache_params(cache, size, assoc, latency, mshrs):
    cache.size = size
    cache.assoc = assoc
    cache.tag_latency = latency
    cache.data_latency = latency
    cache.sequential_access = False
    cache.response_latency = 0
    cache.mshrs = mshrs


def apply_paper_align(args, system):
    """Apply after kmhv3 defaults so those defaults cannot undo the mapping."""
    if not args.paper_align:
        return
    from m5.objects import NULL, XSDRRIPRP
    from common.LSQBankConflict import set_lsq_bank_conflict_cache_params

    for cpu in system.cpu:
        cpu.enableFdip = True
        cpu.enablePdip = False
        cpu.enableUpstreamPdip = bool(args.enable_upstream_pdip)
        cpu.decodeWidth = 12
        cpu.renameWidth = 12
        cpu.commitWidth = 12
        cpu.squashWidth = 12
        cpu.phyregReleaseWidth = 12
        cpu.numROBEntries = 512
        cpu.LQEntries = 144
        cpu.RARQEntries = 144
        cpu.SQEntries = 112
        cpu.numPhysIntRegs = 448
        # RISC-V has distinct scalar-FP and vector files; there is no shared
        # x86 FP/vector register pool in this model.
        cpu.numPhysFloatRegs = 400
        cpu.numPhysVecRegs = 400
        if len(cpu.scheduler.IQs) != len(IQ_SIZES):
            raise ValueError("PDIP paper profile requires the KMHV3 scheduler")
        for queue, size in zip(cpu.scheduler.IQs, IQ_SIZES):
            queue.size = size

        bp = cpu.branchPred
        bp.ftq_size = 24
        bp.fsq_size = 24
        bp.useStaticPrefetchDistance = True
        bp.staticPrefetchDistance = 24
        for name in ("ubtb", "abtb", "microtage", "mbtb", "tage",
                     "ittage", "mgsc", "ras"):
            getattr(bp, name).enabled = name in ("mbtb", "tage", "ittage")
        bp.mbtb.numEntries = 8192
        bp.mbtb.numWays = 4
        bp.mbtb.victimCacheSize = 0
        bp.tage.enableSC = False
        # The artifact's conventional TAGE uses global branch history.
        bp.tage.usePathHistory = False
        bp.tage.numPredictors = 8
        # tableSizes are sets, not entries. 28672 entries * 18 bits +
        # 128 * 7 alternate counters = 64624 bytes, excluding history/meta.
        bp.tage.tableSizes = TAGE_SETS
        bp.tage.numWays = [2] * 8
        bp.tage.TTagBitSizes = [13] * 8
        bp.tage.TTagPcShifts = [1] * 8
        bp.tage.histLengths = [4, 9, 17, 29, 56, 109, 211, 397]
        bp.tage.useAltOnNaSize = 128
        bp.tage.useAltOnNaWidth = 7
        bp.ittage.numPredictors = 5
        # Direct-mapped entries contain a full PC used by lookup in addition
        # to the target: 3584 * (64+64+9+2+1+1) = 505344 bits (61.6875 KiB).
        bp.ittage.tableSizes = ITTAGE_ENTRIES
        bp.ittage.TTagBitSizes = [9] * 5
        bp.ittage.TTagPcShifts = [1] * 5
        bp.ittage.histLengths = [4, 8, 13, 16, 32]

        _cache_params(cpu.icache, "32KiB", 8, 2, 16)
        cpu.icache.demand_mshr_reserve = 2
        # This model has a separate FDIP issue queue and PDIP candidate queue;
        # both receive the paper's 40-line limit, not a claim of one shared PQ.
        cpu.icache.prefetcher.numPrefetchMSHR = 40
        # The existing LSQ recognizes a cache hit only within one cycle and
        # models its own four-stage load pipeline. Preserve the supported
        # cache/refill timing; the paper's nominal two-cycle D$ is a model gap.
        _cache_params(cpu.dcache, "64KiB", 16, 1, 16)
        cpu.dcache.pipe_latency = 3
        cpu.dcache.wpu = NULL
        set_lsq_bank_conflict_cache_params(cpu, system)

    for wrapper in system.l2_wrappers:
        for cache_slice in wrapper.slices:
            cache = cache_slice.inner_cache
            _cache_params(cache, "256KiB", 16, 10, 8)
            cache.replacement_policy = XSDRRIPRP(mode=2, num_sets=256)
            cache.wpu = NULL
    _cache_params(system.l3, "2MiB", 16, 20, 64)
    mode = "upstream PDIP" if args.enable_upstream_pdip else "FDIP baseline"
    print(f"PDIP paper hardware alignment ({mode}): I$32KiB/8, BTB8192, TAGE~63KiB, "
          "ITTAGE~62KiB, FTQ24; RISC-V pipeline/wrapper approximations apply")
