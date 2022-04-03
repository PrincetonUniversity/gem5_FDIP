#ifndef __CPU_PRED_ITTAGE
#define __CPU_PRED_ITTAGE

#include "base/types.hh"
#include "config/the_isa.hh"
#include "cpu/pred/indirect.hh"
#include "params/ITTAGE.hh"

namespace gem5
{

namespace branch_prediction
{

class ITTAGE: public IndirectPredictor
{
#define STEP1 3
#define STEP2 11
#define HISTBUFFERLENGTH 4096 // Size of the history circular buffer
#define LOGG 12
#define LOGTICK 6 // For management of the reset of useful counters
#define LOGSPEC 6

  private:

    // ITTAGE global table entry
    class ITTageEntry
    {
      public:
        int8_t ctr;
        uint64_t tag;
        uint8_t u;
        uint64_t target; // 25 bits (18 offset + 7-bit region pointer)
        ITTageEntry() : ctr(0), tag(0), u(0), target(0) {}
    };

    class FoldedHistorytmp
    {
        // This is the cyclic shift register for folding
        // a long global history into a smaller number of bits;
        // see P. Michaud's PPM-like predictor at CBP-1
      public:
        unsigned comp;
        int compLength;
        int origLength;
        int outpoint;

        FoldedHistorytmp()
        {
        }

        void
        init(int original_length, int compressed_length)
        {
            comp = 0;
            origLength = original_length;
            compLength = compressed_length;
            outpoint = origLength % compLength;
        }

        void
        update(uint8_t * h, int pt)
        {
            comp = (comp << 1) | h[pt & (HISTBUFFERLENGTH - 1)];
            comp ^= h[(pt + origLength) & (HISTBUFFERLENGTH - 1)] << outpoint;
            comp ^= (comp >> compLength);
            comp &= (1 << compLength) - 1;
        }
    };

    int tick; // Control counter for the resetting of useful bits

    // Class for storing speculative predictions: i.e. provided by a table
    // entry that has already provided a still speculative prediction
    struct IUMEntry {
      public:
        uint64_t tag;
        Addr pred;
        IUMEntry() {}
    };

    class RegionEntry // ITTAGE target region table entry
    {
      public:
        uint64_t region; // 14 bits (not any more, now 46 bits)
        int8_t u; // 1 bit
        RegionEntry() : region(0), u(0) {}
    };

    struct HistoryEntry {
        HistoryEntry(Addr br_addr, Addr tgt_addr, InstSeqNum seq_num)
            : pcAddr(br_addr), targetAddr(tgt_addr), seqNum(seq_num) { }
        Addr pcAddr;
        Addr targetAddr;
        InstSeqNum seqNum;
    };

    // Keep per-thread histories to
    // support SMT.
    class ThreadHistory
    {
      public:
        // Speculative branc history (circular buffer)
        uint8_t ghist[HISTBUFFERLENGTH];

        // For management at fetch time
        int fetchPtGhist;
        FoldedHistorytmp * fetchComputeIndices;
        FoldedHistorytmp * fetchComputeTags[2];

        // For management at retire time
        int retirePtGhist;
        FoldedHistorytmp * retireComputeIndices;
        FoldedHistorytmp * retireComputeTags[2];

        std::deque<HistoryEntry> pathHist;
        unsigned headHistEntry;
    };

    std::vector<ThreadHistory> threadHistory;

    struct ITTageBranchInfo {
        Addr predTarget; // Prediction
        Addr altTarget;  // Alternate prediction
        Addr pred;       // LTTAGE prediction
        Addr longestMatchPredTarget;
        int hitBank;
        int altBank;
        Addr branchPC;
        bool taken;
        bool condBranch;

        // Pointer to dynamically allocated storage
        // to save table indices and folded histories.
        // To do one call to new instead of 5.
        int * storage;

        // Pointers to actual saved array within the dynamically
        // allocated storage.
        int * tableIndices;
        int * tableTags;
        int * ci;
        int * ct0;
        int * ct1;

        ITTageBranchInfo(int sz)
            : predTarget(0),
              altTarget(0),
              pred(0),
              hitBank(0),
              altBank(0),
              branchPC(0),
              taken(false),
              condBranch(false)
        {
            storage = new int [sz * 5];
            tableIndices = storage;
            tableTags = storage + sz;
            ci = tableTags + sz;
            ct0 = ci + sz;
            ct1 = ct0 + sz;
        }
    };

    // "Use alternate prediction on weak predictions": a 4-bit counter to
    // determine whether the newly allocated entries should be considered
    // as valid or not for delivering the prediction
    int8_t useAltOnNA;

    // For the pseudo-random number generator
    int seed;

    // For the IUM
    int ptIumRetire;
    int ptIumFetch;
    IUMEntry * IUMTable;

    // Target region tables
    RegionEntry * regionTable;

    const unsigned nHistoryTables;
    std::vector<unsigned> tagTableTagWidths;
    std::vector<int> logTagTableSizes;
    ITTageEntry ** gtable;
    std::vector<int> histLengths;
    int * tableIndices;
    int * tableTags;

  public:

    ITTAGE(const ITTAGEParams &params);

    void updateDirectionInfo(ThreadID tid, bool actually_taken, void * &indirect_history);

    bool lookup(Addr br_addr, TheISA::PCState& br_target, ThreadID tid,
                void *& bp_history);
    void recordIndirect(Addr br_addr, Addr tgt_addr, InstSeqNum seq_num,
                        ThreadID tid);
    void commit(InstSeqNum seq_num, ThreadID tidi, void * indirect_history);
    void squash(InstSeqNum seq_num, ThreadID tid);
    void recordTarget(InstSeqNum seq_num,  void * indirect_history, const TheISA::PCState & target,
                      ThreadID tid);
    void genIndirectInfo(ThreadID tid, void* & indirect_history);
    void deleteIndirectInfo(ThreadID tid, void * indirect_history);
    void historyUpdate(ThreadID tid, Addr branch_pc, bool taken,
                       void * bp_history, const StaticInstPtr & inst,
                       Addr target);

    void
    changeDirectionPrediction(ThreadID tid,
                              void * indirect_history,
                              bool actually_taken)
    {}
    void
    deleteDirectionInfo(ThreadID tid,
                        void * indirect_history)
    {}

  private:

    int getRandom() const;

    // gindex computes a full hash of pc and ghist
    int gindex(ThreadID tid, Addr pc, int bank, bool at_fetch);

    // Tag computation
    uint16_t gtag(ThreadID tid, unsigned int pc, int bank, bool at_fetch);

    void tagePredict(ThreadID tid, Addr branch_pc, ITTageBranchInfo * bi);

    void calculateIndicesAndTags(ThreadID tid, Addr branch_pc,
                                 ITTageBranchInfo * bi, bool at_fetch);

    Addr predict(ThreadID tid, Addr branch_pc, void *& b);

    Addr predictIUM(ITTageBranchInfo * bi);

    void IUMUpdate(Addr target, void * b);

    // Update fetch histories
    //void fetchHistoryUpdate(Addr pc, uint16_t br_type, bool taken,
    //                        Addr target, ThreadID tid);
    void historyUpdate(ThreadID tid, Addr branch_pc, bool taken, void * b,
                       Addr target, bool at_fetch);

    // Predictor update
    void updateBrIndirect(Addr pc, uint16_t br_type, bool taken, Addr target,
                          ThreadID tid, void * indirect_history);

};
} // namespace branch_prediction
} // namespace gem5
#endif // __CPU_PRED_ITTAGE
