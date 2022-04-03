#include "cpu/pred/ittage.hh"

#include "base/random.hh"
#include "cpu/static_inst.hh"

namespace gem5
{

namespace branch_prediction
{

ITTAGE::ITTAGE(const ITTAGEParams &params)
    : IndirectPredictor(params),
      threadHistory(params.numThreads),
      useAltOnNA(0),
      seed(0),
      ptIumRetire(0),
      ptIumFetch(0),
      nHistoryTables(params.nHistoryTables),
      histLengths(params.histLengths)
{
    for (auto & history : threadHistory) {
        for (int i = 0; i < HISTBUFFERLENGTH; i++) {
            history.ghist[0] = 0;
        }
    }

    logTagTableSizes.resize(nHistoryTables + 1);
    logTagTableSizes[0] = LOGG;
    logTagTableSizes[1] = LOGG;
    logTagTableSizes[STEP1] = LOGG;
    logTagTableSizes[STEP2] = LOGG - 1;

    // Grouped together with T1 4K entries
    for (int i = 2; i <= STEP1 - 1; i++) {
        logTagTableSizes[i] = logTagTableSizes[1] - 3;
    }

    // Grouped together with T4 4K entries
    for (int i = STEP1 + 1; i <= STEP2 - 1; i++) {
        logTagTableSizes[i] = logTagTableSizes[STEP1] - 3;
    }

    // Grouped together with T12 2K entries
    for (int i = STEP2 + 1; i <= nHistoryTables; i++) {
        logTagTableSizes[i] = logTagTableSizes[STEP2] - 3;
    }

    gtable = new ITTageEntry * [nHistoryTables + 1];
    gtable[0] = new ITTageEntry[1 << logTagTableSizes[0]];
    gtable[1] = new ITTageEntry[1 << logTagTableSizes[1]];
    gtable[STEP1] = new ITTageEntry[1 << logTagTableSizes[STEP1]];
    gtable[STEP2] = new ITTageEntry[1 << logTagTableSizes[STEP2]];
    tagTableTagWidths.resize(nHistoryTables + 1);
    tagTableTagWidths[0] = 0; // T0 is tagless

    for (int i = 1; i <= STEP1 - 1; i++) {
        gtable[i] = gtable[1];
        tagTableTagWidths[i] = 9;
    }

    for (int i = STEP1; i <= STEP2 - 1; i++) {
        gtable[i] = gtable[STEP1];
        tagTableTagWidths[i] = 13;
    }

    for (int i = STEP2; i <= nHistoryTables; i++) {
        gtable[i] = gtable[STEP2];
        tagTableTagWidths[i] = 15;
    }

    for (auto & history : threadHistory) {
        // Initialisation of index and tag computation functions
        history.fetchComputeIndices = new FoldedHistorytmp[nHistoryTables + 1];
        history.fetchComputeTags[0] = new FoldedHistorytmp[nHistoryTables + 1];
        history.fetchComputeTags[1] = new FoldedHistorytmp[nHistoryTables + 1];

        for (int i = 1; i <= nHistoryTables; i++) {
            history.fetchComputeIndices[i].init(histLengths[i],
                                                logTagTableSizes[i]);
            history.fetchComputeTags[0][i].init(
                history.fetchComputeIndices[i].origLength,
                tagTableTagWidths[i]);
            history.fetchComputeTags[1][i].init(
                history.fetchComputeIndices[i].origLength,
                tagTableTagWidths[i] - 1);
        }

        history.fetchPtGhist = 0;
        history.retireComputeIndices =
            new FoldedHistorytmp[nHistoryTables + 1];
        history.retireComputeTags[0] =
            new FoldedHistorytmp[nHistoryTables + 1];
        history.retireComputeTags[1] =
            new FoldedHistorytmp[nHistoryTables + 1];

        for (int i = 1; i <= nHistoryTables; i++) {
            history.retireComputeIndices[i].init(histLengths[i],
                                                 logTagTableSizes[i]);
            history.retireComputeTags[0][i].init(
                history.retireComputeIndices[i].origLength,
                tagTableTagWidths[i]);
            history.retireComputeTags[1][i].init(
                history.retireComputeIndices[i].origLength,
                tagTableTagWidths[i] - 1);
        }

        history.retirePtGhist = 0;
    }

    regionTable = new RegionEntry[128];
    IUMTable = new IUMEntry[1 << LOGSPEC];
    tableIndices = new int [nHistoryTables + 1];
    tableTags = new int [nHistoryTables + 1];
}
void
ITTAGE::genIndirectInfo(ThreadID tid,
        void* & indirect_history)
{
    // record the GHR as it was before this prediction
    // It will be used to recover the history in case this prediction is
    // wrong or belongs to bad path
    ITTageBranchInfo * bi = new ITTageBranchInfo(nHistoryTables + 1);
    indirect_history = (void *)(bi);
}

void
ITTAGE::updateDirectionInfo(ThreadID tid, bool actually_taken, void * &indirect_history)
{
    //ITTageBranchInfo * bi = new ITTageBranchInfo(nHistoryTables + 1);
    //indirect_history = (void *)(bi);
    ITTageBranchInfo * bi = static_cast<ITTageBranchInfo *>(indirect_history);
    bi->taken = actually_taken;
}

bool
ITTAGE::lookup(Addr br_addr, TheISA::PCState& br_target, ThreadID tid,
               void *& bp_history)
{
    br_target = predict(tid, br_addr, bp_history);
    return br_target != 0;
}

Addr
ITTAGE::predict(ThreadID tid, Addr branch_pc, void *& b)
{
    //ITTageBranchInfo * bi = new ITTageBranchInfo(nHistoryTables + 1);
    //b = (void *)(bi);
    ITTageBranchInfo * bi = static_cast<ITTageBranchInfo *>(b);
    calculateIndicesAndTags(tid, branch_pc, bi, true);
    tagePredict(tid, branch_pc, bi);
    // Check IUM table
    Addr pred = predictIUM(bi);
    return pred;
}

void
ITTAGE::tagePredict(ThreadID tid, Addr branch_pc, ITTageBranchInfo * bi)
{
    // Look for the bank with longest matching history
    for (int i = nHistoryTables; i >= 0; i--) {
        if (gtable[i][tableIndices[i]].tag == tableTags[i]) {
            bi->hitBank = i;
            break;
        }
    }

    // Look for the alternate bank
    for (int i = bi->hitBank - 1; i >= 0; i--) {
        if (gtable[i][tableIndices[i]].tag == tableTags[i]) {
            bi->altBank = i;
            break;
        }
    }

    // Computes the prediction and the alternate prediction
    if (bi->altBank >= 0) {
        ITTageEntry * table = static_cast<ITTageEntry *>(gtable[bi->altBank]);
        bi->altTarget = table[tableIndices[bi->altBank]].target;
    }

    ITTageEntry * table = static_cast<ITTageEntry *>(gtable[bi->hitBank]);
    bi->predTarget = table[tableIndices[bi->hitBank]].target;
    // Proceed with the indirection through the target region table
    bi->altTarget = (bi->altTarget & ((1 << 18) - 1)) +
                    (regionTable[(bi->altTarget >> 18) & 127].region << 18);
    bi->predTarget = (bi->predTarget & ((1 << 18) - 1)) +
                     (regionTable[(bi->predTarget >> 18) & 127].region << 18);
    bi->longestMatchPredTarget = bi->predTarget;

    // If the entry is recognized as a newly allocated entry and useAltOnNA
    // is positive use the alternate prediction
    if (bi->altBank >= 0)
        if ((useAltOnNA >= 0) && (table[tableIndices[bi->hitBank]].ctr == 0)) {
            bi->predTarget = bi->altTarget;
        }

    bi->branchPC = branch_pc;
}

void
ITTAGE::calculateIndicesAndTags(ThreadID tid, Addr branch_pc,
                                ITTageBranchInfo * bi, bool at_fetch)
{
    // Computes the table addresses and the partial tags
    for (int i = 0; i <= nHistoryTables; i++) {
        tableIndices[i] = gindex(tid, branch_pc, i, at_fetch);
        tableTags[i] = gtag(tid, branch_pc, i, at_fetch);
    }

    for (int i = 2; i <= STEP1 - 1; i++) {
        tableIndices[i] = ((tableIndices[1] & 7) ^ (i - 1)) +
                          (tableIndices[i] << 3);
    }

    for (int i = STEP1 + 1; i <= STEP2 - 1; i++) {
        tableIndices[i] = ((tableIndices[STEP1] & 7) ^ (i - STEP1)) +
                          (tableIndices[i] << 3);
    }

    for (int i = STEP2 + 1; i <= nHistoryTables; i++) {
        tableIndices[i] = ((tableIndices[STEP2] & 7) ^ (i - STEP2)) +
                          (tableIndices[i] << 3);
    }

    tableTags[0] = 0;
    tableIndices[0] = branch_pc & ((1 << logTagTableSizes[0]) - 1);

    for (int i = 0; i <= nHistoryTables; i++) {
        bi->tableIndices[i] = tableIndices[i];
        bi->tableTags[i] = tableTags[i];
    }
}

Addr
ITTAGE::predictIUM(ITTageBranchInfo * bi)
{
    int ium_tag = (bi->hitBank) + (tableIndices[bi->hitBank] << 4);
    int min = (ptIumRetire > ptIumFetch + 8) ? ptIumFetch + 8 : ptIumRetire;

    for (int i = ptIumFetch; i < min; i++) {
        if (IUMTable[i & ((1 << LOGSPEC) - 1)].tag == ium_tag) {
            return IUMTable[i & ((1 << LOGSPEC) - 1)].pred;
        }
    }

    return bi->predTarget;
}

void
ITTAGE::recordIndirect(Addr br_addr, Addr tgt_addr, InstSeqNum seq_num,
                       ThreadID tid)
{
    //HistoryEntry entry(br_addr, tgt_addr, seq_num);
    //threadHistory[tid].pathHist.push_back(entry);
}

void
ITTAGE::commit(InstSeqNum seq_num, ThreadID tid, void * indirect_history)
{
    if(!indirect_history){
      return;
    }
    ITTageBranchInfo * bi = static_cast<ITTageBranchInfo *>(indirect_history);
    updateBrIndirect(bi->branchPC, 0, bi->taken, bi->pred, tid,
                     indirect_history);
    delete bi;
}

void
ITTAGE::historyUpdate(ThreadID tid, Addr branch_pc, bool taken,
                      void * indirect_history, const StaticInstPtr & inst,
                      Addr target)
{
    IUMUpdate(target, indirect_history);
    ITTageBranchInfo * bi = static_cast<ITTageBranchInfo *>(indirect_history);
    bi->condBranch = inst->isCondCtrl();
    // Update fetch histories
    historyUpdate(tid, branch_pc, taken, indirect_history, target, true);
}

void
ITTAGE::IUMUpdate(Addr target, void * b)
{
    ITTageBranchInfo * bi = (ITTageBranchInfo *)(b);
    int ium_tag = (bi->hitBank) + (bi->tableIndices[bi->hitBank] << 4);
    ptIumFetch--;
    IUMTable[ptIumFetch & ((1 << LOGSPEC) - 1)].tag = ium_tag;
    IUMTable[ptIumFetch & ((1 << LOGSPEC) - 1)].pred = target;
}

void
ITTAGE::historyUpdate(ThreadID tid, Addr branch_pc, bool taken,
                      void * b, Addr target, bool at_fetch)
{
    ITTageBranchInfo * bi = (ITTageBranchInfo *)(b);
    bi->taken = taken;
    ThreadHistory & thread_history = threadHistory[tid];
    int Y;
    FoldedHistorytmp * H;
    FoldedHistorytmp * G;
    FoldedHistorytmp * J;

    if (at_fetch) {
        Y = thread_history.fetchPtGhist;
        H = thread_history.fetchComputeIndices;
        G = thread_history.fetchComputeTags[0];
        J = thread_history.fetchComputeTags[1];
    } else {
        Y = thread_history.retirePtGhist;
        H = thread_history.retireComputeIndices;
        G = thread_history.retireComputeTags[0];
        J = thread_history.retireComputeTags[1];
    }

    Addr path = ((target ^ (target >> 3) ^ branch_pc));

    if (bi->condBranch) {
        path = taken;
    }

    int maxt = 10;

    for (int t = 0; t < maxt; t++) {
        bool P = (path & 1);
        path >>= 1;
        // Update history
        Y--;
        thread_history.ghist[Y & (HISTBUFFERLENGTH - 1)] = P;

        // Prepare next index and tag computations for user branchs
        for (int i = 1; i <= nHistoryTables; i++) {
            if (at_fetch) {
                bi->ci[i]  = H[i].comp;
                bi->ct0[i] = G[i].comp;
                bi->ct0[i] = J[i].comp;
            }

            H[i].update(thread_history.ghist, Y);
            G[i].update(thread_history.ghist, Y);
            J[i].update(thread_history.ghist, Y);
        }
    }
}

// gindex computes a full hash of pc and ghist
int
ITTAGE::gindex(ThreadID tid, Addr pc, int bank, bool at_fetch)
{
    FoldedHistorytmp * compute_indices =
        at_fetch ?
        threadHistory[tid].fetchComputeIndices :
        threadHistory[tid].retireComputeIndices;
    int index = pc ^ (pc >> (abs(logTagTableSizes[bank] - bank) + 1)) ^
                compute_indices[bank].comp;
    return (index & ((1 << (logTagTableSizes[bank])) - 1));
}

// Tag computation
uint16_t
ITTAGE::gtag(ThreadID tid, unsigned int pc, int bank, bool at_fetch)
{
    FoldedHistorytmp * compute_tags[2];

    if (at_fetch) {
        compute_tags[0] = threadHistory[tid].fetchComputeTags[0];
        compute_tags[1] = threadHistory[tid].fetchComputeTags[1];
    } else {
        compute_tags[0] = threadHistory[tid].retireComputeTags[0];
        compute_tags[1] = threadHistory[tid].retireComputeTags[1];
    }

    int tag = pc ^ compute_tags[0][bank].comp ^
              (compute_tags[1][bank].comp << 1);
    return (tag & ((1 << tagTableTagWidths[bank]) - 1));
}

int
ITTAGE::getRandom() const
{
    //return 1; // TODO - REMOVE ME after correlation...
    return random_mt.random<int>();
}

// Predictor update
void
ITTAGE::updateBrIndirect(Addr branch_pc, uint16_t br_type, bool taken,
                         Addr target, ThreadID tid, void * indirect_history)
{
    //ITTageBranchInfo * bi2 = (ITTageBranchInfo *)(indirect_history);
    int nrand = getRandom();
    ptIumRetire--;
    // Recompute the prediction by the ITTAGE predictor
    ITTageBranchInfo * bi = new ITTageBranchInfo(nHistoryTables + 1);
    calculateIndicesAndTags(tid, branch_pc, bi, false);
    tagePredict(tid, branch_pc, bi);
    // Allocation if the Longest Matching entry does not provide the correct
    // entry
    bool alloc = (bi->longestMatchPredTarget != target);

    if ((bi->hitBank > 0) & (bi->altBank >= 0)) {
        // Manage the selection between longest matching and alternate
        // matching for "pseudo"-newly allocated longest matching entry
        bool pseudoNewAlloc =
            (gtable[bi->hitBank][tableIndices[bi->hitBank]].ctr == 0);

        if (pseudoNewAlloc) {
            if (bi->altTarget) {
                if (bi->longestMatchPredTarget != bi->altTarget) {
                    if ((bi->altTarget == target) ||
                            (bi->longestMatchPredTarget == target)) {
                        if (bi->altTarget == target) {
                            if (useAltOnNA < 7) {
                                useAltOnNA++;
                            }
                        } else {
                            if (useAltOnNA >= -8) {
                                useAltOnNA--;
                            }
                        }
                    }
                }
            }
        }
    }

    if (alloc) {
        // Need to compute the target field (Offset + Region pointer)
        uint64_t region = (target >> 18);
        int ptRegion = -1;

        // Associative search on the region table
        for (int i = 0; i < 128; i++)
            if (regionTable[i].region == region) {
                ptRegion = i;
                break;
            }

        if (ptRegion == -1) {
            // Miss in the target region table, allocate a free entry
            for (int i = 0; i < 128; i++)
                if (regionTable[i].u == 0) {
                    ptRegion = i;
                    regionTable[i].region = region;
                    regionTable[i].u = 1;
                    break;
                }

            // A very simple replacement policy
            if (ptRegion == -1) {
                for (int i = 0; i < 128; i++) {
                    regionTable[i].u = 0;
                }

                ptRegion = 0;
                regionTable[0].region = region;
                regionTable[0].u = 1;
            }
        }

        int indTarget = (target & ((1 << 18) - 1)) + (ptRegion << 18);
        // We allocate an entry with a longer history to avoid ping-pong,
        // we do not choose systematically the next entry, but among the 2
        // next entries
        int Y = nrand;
        int X = bi->hitBank + 1;

        if ((Y & 31) == 0) {
            X++;
        }

        int T = 2;

        // Allocating 3 entries on a misprediction just work a little bit
        // better than a single allocation
        for (int i = X; i <= nHistoryTables; i += 1) {
            ITTageEntry * table = static_cast<ITTageEntry *>(gtable[i]);

            if (table[tableIndices[i]].u == 0) {
                table[tableIndices[i]].tag = tableTags[i];
                table[tableIndices[i]].target = indTarget;
                table[tableIndices[i]].ctr = 0;
                table[tableIndices[i]].u = 0;

                if (tick > 0) {
                    tick--;
                }

                if (T == 0) {
                    break;
                }

                T--;
                i += 1;
            } else {
                tick++;
            }
        }
    }

    if ((tick >= (1 << LOGTICK))) {
        tick = 0;

        // Reset the useful bit
        for (int i = 0; i <= nHistoryTables; i++) {
            for (int j = 0; j < (1 << logTagTableSizes[i]); j++) {
                gtable[i][j].u = 0;
            }
        }
    }

    if (bi->longestMatchPredTarget == target) {
        if (gtable[bi->hitBank][tableIndices[bi->hitBank]].ctr < 3) {
            gtable[bi->hitBank][tableIndices[bi->hitBank]].ctr++;
        }
    } else {
        if (gtable[bi->hitBank][tableIndices[bi->hitBank]].ctr > 0) {
            gtable[bi->hitBank][tableIndices[bi->hitBank]].ctr--;
        } else {
            // Replace the target field by the new target
            // Need to compute the target field :-)
            uint64_t region = (target >> 18);
            int ptRegion = -1;

            for (int i = 0; i < 128; i++) {
                if (regionTable[i].region == region) {
                    ptRegion = i;
                    break;
                }
            }

            if (ptRegion == -1) {
                for (int i = 0; i < 128; i++) {
                    if (regionTable[i].u == 0) {
                        ptRegion = i;
                        regionTable[i].region = region;
                        regionTable[i].u = 1;
                        break;
                    }
                }

                // A very simple replacement policy
                if (ptRegion == -1) {
                    for (int i = 0; i < 128; i++) {
                        regionTable[i].u = 0;
                    }

                    ptRegion = 0;
                    regionTable[0].region = region;
                    regionTable[0].u = 1;
                }
            }

            int indTarget = (target & ((1 << 18) - 1)) + (ptRegion << 18);
            ITTageEntry * table =
                static_cast<ITTageEntry *>(gtable[bi->hitBank]);
            table[tableIndices[bi->hitBank]].target = indTarget;
        }
    }

    // Update the u bit
    if (bi->hitBank != 0) {
        if (bi->longestMatchPredTarget != bi->altTarget) {
            if (bi->longestMatchPredTarget == target) {
                gtable[bi->hitBank][tableIndices[bi->hitBank]].u = 1;
            }
        }
    }

    // Update retire history path
    historyUpdate(tid, branch_pc, taken, indirect_history, target, false);

    delete bi;
}

void 
ITTAGE::deleteIndirectInfo(ThreadID tid, void * indirect_history){
    ITTageBranchInfo * bi = static_cast<ITTageBranchInfo *>(indirect_history);
    delete bi;
}

// TODO
void
ITTAGE::squash(InstSeqNum seq_num, ThreadID tid)
{
}

// TODO
void
ITTAGE::recordTarget(InstSeqNum seq_num,  void * indirect_history,
                     const TheISA::PCState & target, ThreadID tid)
{
    ITTageBranchInfo * bi = static_cast<ITTageBranchInfo *>(indirect_history);
    bi->pred = target.instAddr();  
}

} // namespace branch_prediction

//gem5::branch_prediction::ITTAGE *
//ITTAGEParams::create() const
//{
//    return new gem5::branch_prediction::ITTAGE(this);
//}

} // namespace gem5

