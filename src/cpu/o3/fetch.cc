/*
 * Copyright (c) 2010-2014 ARM Limited
 * Copyright (c) 2012-2013 AMD
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "cpu/o3/fetch.hh"

#include <algorithm>
#include <cstring>
#include <list>
#include <map>
#include <queue>

#include "arch/generic/tlb.hh"
#include "base/random.hh"
#include "base/types.hh"
#include "config/the_isa.hh"
#include "cpu/base.hh"
#include "cpu/exetrace.hh"
#include "cpu/nop_static_inst.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "debug/Activity.hh"
#include "debug/Drain.hh"
#include "debug/Fetch.hh"
#include "debug/O3CPU.hh"
#include "debug/O3PipeView.hh"
#include "mem/packet.hh"
#include "params/O3CPU.hh"
#include "sim/byteswap.hh"
#include "sim/core.hh"
#include "sim/eventq.hh"
#include "sim/full_system.hh"
#include "sim/system.hh"
#include "debug/Bgodala.hh"


using namespace std;

namespace gem5
{

namespace o3
{

// oracle stuff // 32KB / 64B / 8 = 64
#define SETS 64 // 1MB / 64B / 256 = 64 <-- if change this number, need to change set mask (ind) too
#define NUM_WAYS 8
#define CACHE_LINE_SIZE 64 // line size in bytes
#define CACHE_LISZE_SIZE_WIDTH 6 // number of bits
#define ICACHE_ACCESS_LATENCY 2 // in cycles
enum Repl {ORACLE, LRU, RANDOM, NONE};
//enum Repl REPL = ORACLE; // sets replacement policy
vector<vector<Addr> > oneMisses[SETS];
vector<Addr> in_cache[SETS];
long long repl_hits = 0;
long long repl_misses = 0;
long long repl_evicts = 0;
bool repl_init = true;
// end replacement policy data structure stuff

// cms11 for PipeView
std::map<Addr,Tick> memsent_ticks[10];
std::map<Addr,Tick> memrecv_ticks[10];
std::map<Addr,int> memlevels[10];
std::map<Addr,bool> buffer_cache[10];
std::map<Addr,bool> starve[10];
std::map<Addr,char> missSt[10];

/** Nayana: FDIP based Fetch Target Queue. */
std::deque<TheISA::PCState> prefetchQueue[FTQ_MAX_SIZE];
std::deque<int> prefetchQueueBblSize[FTQ_MAX_SIZE];
TheISA::PCState prevPC[FTQ_MAX_SIZE];
std::deque<InstSeqNum> prefetchQueueSeqNum[FTQ_MAX_SIZE];
std::deque<TheISA::PCState> prefetchQueueBr[FTQ_MAX_SIZE];


Fetch::IcachePort::IcachePort(Fetch *_fetch, CPU *_cpu) :
        RequestPort(_cpu->name() + ".icache_port", _cpu), fetch(_fetch)
{}


Fetch::Fetch(CPU *_cpu, const O3CPUParams &params)
    : fetchPolicy(params.smtFetchPolicy),
      cpu(_cpu),
      branchPred(nullptr),
      lastAddrFetched(0),
      lastProcessedLine(0),
      fallThroughPrefPC(0),
      decodeToFetchDelay(params.decodeToFetchDelay),
      renameToFetchDelay(params.renameToFetchDelay),
      iewToFetchDelay(params.iewToFetchDelay),
      commitToFetchDelay(params.commitToFetchDelay),
      fetchWidth(params.fetchWidth),
      decodeWidth(params.decodeWidth),
      retryPkt(NULL),
      retryTid(InvalidThreadID),
      cacheBlkSize(cpu->cacheLineSize()),
      fetchBufferSize(params.fetchBufferSize),
      fetchBufferMask(fetchBufferSize - 1),
      fetchQueueSize(params.fetchQueueSize),
      prefetchQueueSize(params.fetchQueueSize),
      numThreads(params.numThreads),
      numFetchingThreads(params.smtNumFetchingThreads),
      enablePerfectICache(params.enablePerfectICache),
      enableFDIP(params.enableFDIP),
      icachePort(this, _cpu),
      // cms11 oracle
      REPL(params.cache_repl),
      starveRandomness(params.starveRandomness),
      starveAtleast(params.starveAtleast),
      randomStarve(params.randomStarve),
      pureRandom(params.pureRandom),
      ftqSize(params.ftqSize),
      trackLastBlock(false),
      numSets(params.numSets),
      finishTranslationEvent(this), fetchStats(_cpu, this)
{
    if (numThreads > MaxThreads)
        fatal("numThreads (%d) is larger than compiled limit (%d),\n"
              "\tincrease MaxThreads in src/cpu/o3/limits.hh\n",
              numThreads, static_cast<int>(MaxThreads));
    if (fetchWidth > MaxWidth)
        fatal("fetchWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             fetchWidth, static_cast<int>(MaxWidth));
    if (fetchBufferSize > cacheBlkSize)
        fatal("fetch buffer size (%u bytes) is greater than the cache "
              "block size (%u bytes)\n", fetchBufferSize, cacheBlkSize);
    if (cacheBlkSize % fetchBufferSize)
        fatal("cache block (%u bytes) is not a multiple of the "
              "fetch buffer (%u bytes)\n", cacheBlkSize, fetchBufferSize);

    for (int i = 0; i < MaxThreads; i++) {
        fetchStatus[i] = Idle;
        decoder[i] = nullptr;
        preDecoder[i] = nullptr;
        pc[i] = 0;
        lastinst[i] = 0;
        fetchOffset[i] = 0;
        macroop[i] = nullptr;
        delayedCommit[i] = false;
        //memReq[i] = nullptr;
        memReq[i].clear();
        stalls[i] = {false, false};
        //fetchBuffer[i] = NULL;
        fetchBuffer[i].clear();
        //fetchBufferPC[i] = 0;
        fetchBufferPC[i].clear();
        fetchBufferReqPtr[i].clear();
        add_front = false;
        //fetchBufferValid[i] = false;
        fetchBufferValid[i].clear();
        lastIcacheStall[i] = 0;
        issuePipelinedIfetch[i] = false;
        seq[i] = 0;
        brseq[i] = 0;
    }

    branchPred = params.branchPred;

    for (ThreadID tid = 0; tid < numThreads; tid++) {
        decoder[tid] = new TheISA::Decoder(
                dynamic_cast<TheISA::ISA *>(params.isa[tid]));
        preDecoder[tid] = new TheISA::Decoder(
                dynamic_cast<TheISA::ISA *>(params.isa[tid]));
        // Create space to buffer the cache line data,
        // which may not hold the entire cache line.
        //fetchBuffer[tid] = new uint8_t[fetchBufferSize];
    }
    srand(123456);
    // Get the size of an instruction.
    instSize = decoder[0]->moreBytesSize();
    warn("ftqSize is %d\n",ftqSize);
}

std::string Fetch::name() const { return cpu->name() + ".fetch"; }

void
Fetch::regProbePoints()
{
    ppFetch = new ProbePointArg<DynInstPtr>(cpu->getProbeManager(), "Fetch");
    ppFetchRequestSent = new ProbePointArg<RequestPtr>(cpu->getProbeManager(),
                                                       "FetchRequest");

}

Fetch::FetchStatGroup::FetchStatGroup(CPU *cpu, Fetch *fetch)
    : statistics::Group(cpu, "fetch"),
    ADD_STAT(icacheStallCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch is stalled on an Icache miss"),
    ADD_STAT(insts, statistics::units::Count::get(),
             "Number of instructions fetch has processed"),
    ADD_STAT(branches, statistics::units::Count::get(),
             "Number of branches that fetch encountered"),
    ADD_STAT(predictedBranches, statistics::units::Count::get(),
             "Number of branches that fetch has predicted taken"),
    ADD_STAT(cycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has run and was not squashing or "
             "blocked"),
    ADD_STAT(squashCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent squashing"),
    ADD_STAT(tlbCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent waiting for tlb"),
    ADD_STAT(idleCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch was idle"),
    ADD_STAT(blockedCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent blocked"),
    ADD_STAT(miscStallCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent waiting on interrupts, or bad "
             "addresses, or out of MSHRs"),
    ADD_STAT(pendingDrainCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent waiting on pipes to drain"),
    ADD_STAT(noActiveThreadStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to no active thread to fetch from"),
    ADD_STAT(pendingTrapStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to pending traps"),
    ADD_STAT(pendingQuiesceStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to pending quiesce instructions"),
    ADD_STAT(icacheWaitRetryStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to full MSHR"),
    ADD_STAT(cacheLines, statistics::units::Count::get(),
             "Number of cache lines fetched"),
    ADD_STAT(icacheSquashes, statistics::units::Count::get(),
             "Number of outstanding Icache misses that were squashed"),
    ADD_STAT(tlbSquashes, statistics::units::Count::get(),
             "Number of outstanding ITLB misses that were squashed"),
    ADD_STAT(nisnDist, statistics::units::Count::get(),
             "Number of instructions fetched each cycle (Total)"),
    ADD_STAT(idleRate, statistics::units::Ratio::get(),
             "Ratio of cycles fetch was idle",
             idleCycles / cpu->baseStats.numCycles),
    ADD_STAT(branchRate, statistics::units::Ratio::get(),
             "Number of branch fetches per cycle",
             branches / cpu->baseStats.numCycles),
    ADD_STAT(rate, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
             "Number of inst fetches per cycle",
             insts / cpu->baseStats.numCycles)
{
        icacheStallCycles
            .prereq(icacheStallCycles);
        insts
            .prereq(insts);
        branches
            .prereq(branches);
        predictedBranches
            .prereq(predictedBranches);
        cycles
            .prereq(cycles);
        squashCycles
            .prereq(squashCycles);
        tlbCycles
            .prereq(tlbCycles);
        idleCycles
            .prereq(idleCycles);
        blockedCycles
            .prereq(blockedCycles);
        cacheLines
            .prereq(cacheLines);
        miscStallCycles
            .prereq(miscStallCycles);
        pendingDrainCycles
            .prereq(pendingDrainCycles);
        noActiveThreadStallCycles
            .prereq(noActiveThreadStallCycles);
        pendingTrapStallCycles
            .prereq(pendingTrapStallCycles);
        pendingQuiesceStallCycles
            .prereq(pendingQuiesceStallCycles);
        icacheWaitRetryStallCycles
            .prereq(icacheWaitRetryStallCycles);
        icacheSquashes
            .prereq(icacheSquashes);
        tlbSquashes
            .prereq(tlbSquashes);
        nisnDist
            .init(/* base value */ 0,
              /* last value */ fetch->fetchWidth,
              /* bucket size */ 1)
            .flags(statistics::pdf);
        idleRate
            .prereq(idleRate);
        branchRate
            .flags(statistics::total);
        rate
            .flags(statistics::total);
}
void
Fetch::setTimeBuffer(TimeBuffer<TimeStruct> *time_buffer)
{
    timeBuffer = time_buffer;

    // Create wires to get information from proper places in time buffer.
    fromDecode = timeBuffer->getWire(-decodeToFetchDelay);
    fromRename = timeBuffer->getWire(-renameToFetchDelay);
    fromIEW = timeBuffer->getWire(-iewToFetchDelay);
    fromCommit = timeBuffer->getWire(-commitToFetchDelay);
}

void
Fetch::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
}

void
Fetch::setFetchQueue(TimeBuffer<FetchStruct> *ftb_ptr)
{
    // Create wire to write information to proper place in fetch time buf.
    toDecode = ftb_ptr->getWire(0);
}

void
Fetch::startupStage()
{
    assert(priorityList.empty());
    resetStage();

    // Fetch needs to start fetching instructions at the very beginning,
    // so it must start up in active state.
    switchToActive();
}

void
Fetch::clearStates(ThreadID tid)
{
    fetchStatus[tid] = Running;
    pc[tid] = cpu->pcState(tid);
    prefPC[tid] = cpu->pcState(tid);
    lastPrefPC = cpu->pcState(tid);
    bblAddr[tid] = pc[tid].instAddr();
    bblSize[tid] = 0;
    seq[tid] = 0;
    brseq[tid] = 0;
    lastinst[tid] = 0;
    fetchOffset[tid] = 0;
    macroop[tid] = NULL;
    delayedCommit[tid] = false;
    //memReq[tid] = NULL;
    memReq[tid].clear();
    stalls[tid].decode = false;
    trackLastBlock = false;
    stalls[tid].drain = false;
    //fetchBufferPC[tid] = 0;
    //fetchBufferValid[tid] = false;
    fetchBuffer[tid].clear();
    fetchBufferPC[tid].clear();
    fetchBufferReqPtr[tid].clear();
    add_front = false;
    fetchBufferValid[tid].clear();
    fetchQueue[tid].clear();
    prefetchQueue[tid].clear();
    prefetchQueueBblSize[tid].clear();
    prefetchQueueSeqNum[tid].clear();
    prefetchQueueBr[tid].clear();
    prefetchBufferPC[tid].clear();
    lastProcessedLine = 0;
    lastAddrFetched = 0;
    // TODO not sure what to do with priorityList for now
    // priorityList.push_back(tid);
}

void
Fetch::resetStage()
{
    numInst = 0;
    interruptPending = false;
    cacheBlocked = false;

    priorityList.clear();

    // Setup PC and nextPC with initial state.
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        fetchStatus[tid] = Running;
        pc[tid] = cpu->pcState(tid);
        prefPC[tid] = cpu->pcState(tid);
        lastPrefPC = cpu->pcState(tid);
        bblAddr[tid] = pc[tid].instAddr();
        bblSize[tid] = 0;
        seq[tid] = 0;
        brseq[tid] = 0;
        lastinst[tid] = 0;
        fetchOffset[tid] = 0;
        macroop[tid] = NULL;

        delayedCommit[tid] = false;
        //memReq[tid] = NULL;
        memReq[tid].clear();

        stalls[tid].decode = false;
        trackLastBlock = false;
        stalls[tid].drain = false;

        //fetchBufferPC[tid] = 0;
        //fetchBufferValid[tid] = false;
        fetchBuffer[tid].clear();
        fetchBufferPC[tid].clear();
        fetchBufferReqPtr[tid].clear();
        add_front = false;
        fetchBufferValid[tid].clear();

        fetchQueue[tid].clear();
        prefetchQueue[tid].clear();
        prefetchQueueBblSize[tid].clear();
        prefetchQueueSeqNum[tid].clear();
        prefetchQueueBr[tid].clear();
        prefetchBufferPC[tid].clear();

        priorityList.push_back(tid);
    }
    lastProcessedLine = 0;
    lastAddrFetched = 0;

    wroteToTimeBuffer = false;
    _status = Inactive;
}

void
Fetch::processCacheCompletion(PacketPtr pkt)
{
    ThreadID tid = cpu->contextToThread(pkt->req->contextId());

    DPRINTF(Fetch, "[tid:%i] Waking up from cache miss vaddr %#x\n", tid, pkt->req->getVaddr());
    assert(!cpu->switchedOut());
    std::string level; 

    // cms11
    int memHierarchLevel = pkt->req->getAccessDepth();

    if (pkt->req->getAccessDepth()>2) {
        level = "unknown";
    } else if (pkt->req->getAccessDepth()>1) {
        level = "mem";
    } else if (pkt->req->getAccessDepth()>0) {
        level = "l2";
    } else {
        level = "l1";
    }
 
    memReqListIt memReq_it = memReq[tid].begin();

    while (memReq_it != memReq[tid].end() &&
           (*memReq_it)!=pkt->req) {
        DPRINTF(Fetch, "MemReqIter memReq : %#x\n",(*memReq_it)->getVaddr());
        ++memReq_it;
    }
    
    TheISA::PCState thisPC = pc[tid];
    Addr pcOffset = fetchOffset[tid];
    Addr fetchAddr = (thisPC.instAddr() + pcOffset) & decoder[tid]->pcMask();
    Addr fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);

    //if((fetchStatus[tid] == IcacheWaitResponse || fetchStatus[tid] == IcacheWaitRetry) && fetchBufferBlockPC == pkt->req->getVaddr()){
    if((fetchStatus[tid] == IcacheWaitResponse) && fetchBufferBlockPC == pkt->req->getVaddr()){
        DPRINTF(Fetch, "BGODALA: Wakingup CPU\n");
        cpu->wakeCPU();
        switchToActive();
        // Only switch to IcacheAccessComplete if we're not stalled as well.
        if (checkStall(tid)) {
            fetchStatus[tid] = Blocked;
        } else {
            fetchStatus[tid] = IcacheAccessComplete;
        }
    }

 
    // Only change the status if it's still waiting on the icache access
    // to return.
    //if (fetchStatus[tid] != IcacheWaitResponse ||
    //    pkt->req != memReq[tid]) {
    if (memReq_it==memReq[tid].end()) {
        ++fetchStats.icacheSquashes;
        delete pkt;
        DPRINTF(Fetch, "Returning because it is not present in memReq queue\n");
        return;
    }

    bufIt buf_it = fetchBuffer[tid].begin();
    pcIt pc_it = fetchBufferPC[tid].begin();
    validIt valid_it = fetchBufferValid[tid].begin();
    reqIt req_it = fetchBufferReqPtr[tid].begin();
 
    DPRINTF(Fetch, "Iterating though fetchBuffer\n");
    while (pc_it != fetchBufferPC[tid].end()) {
        DPRINTF(Fetch, "fetchBufferPC: %#x fetchBufferReqPtr: %#x memReq_it: %#x pkt->req: %#x\n",*pc_it, *req_it, *memReq_it, pkt->req);
        if ((*pc_it)==(*memReq_it)->getVaddr() && *memReq_it == *req_it) {
            //memcpy(fetchBuffer[tid], pkt->getConstPtr<uint8_t>(), fetchBufferSize);
            //fetchBufferValid[tid] = true;
            memcpy((*buf_it), pkt->getConstPtr<uint8_t>(), fetchBufferSize);
            assert(!*valid_it && "valid_it cannot be before initialzing it\n");
            (*valid_it) = true;
            break;
        }
        ++pc_it;
        ++buf_it;
        ++valid_it;
        ++req_it;
    }

    assert(req_it != fetchBufferReqPtr[tid].end() && "req_it cannot be end\n");
    DPRINTF(Fetch, "fetchBufferPC: %#x fetchBufferReqPtr: %#x memReq_it: %#x pkt->req: %#x\n",*pc_it, *req_it, *memReq_it, pkt->req);
    // cms11 edit
    memrecv_ticks[tid][(pkt->req->getVaddr())>>6] = curTick();
    memlevels[tid][(pkt->req->getVaddr())>>6] = memHierarchLevel;
    buffer_cache[tid][(pkt->req->getVaddr())>>6] = true;
    starve[tid][(pkt->req->getVaddr())>>6] = decodeIdle[tid];

    //Commenting this code since pc_it , buf_it and valid_it iterator is pointing
    //to the proper entry

    //buf_it = fetchBuffer[tid].begin();
    //pc_it = fetchBufferPC[tid].begin();
    //valid_it = fetchBufferValid[tid].begin();
    //req_it = fetchBufferReqPtr[tid].begin();
 
    //while (pc_it != fetchBufferPC[tid].end() &&
    //       (*pc_it)!=(*memReq_it)->getVaddr()) {
    //    ++pc_it;
    //    ++buf_it;
    //    ++valid_it;
    //}

    double random = double(rand()) * 100 / (double(RAND_MAX) + 1.0);
    bool didWeStarve = false;
    if(decodeIdle[tid] && *pc_it == fetchBufferBlockPC) {
        DPRINTF(Fetch, "%#x %s %d ", (*memReq_it)->getVaddr(), level, resteer);
        resteer = false;
        RequestPtr mem_req2 = std::make_shared<Request>(
            (*memReq_it)->getVaddr(), fetchBufferSize,
            Request::INST_FETCH, cpu->instRequestorId(), (*memReq_it)->getPC(),
            cpu->thread[tid]->contextId());
        mem_req2->setPaddr((*memReq_it)->getPaddr());
        mem_req2->taskId(cpu->taskId());
        PacketPtr data_pkt2 = new Packet(mem_req2, MemCmd::ReadReq);

        if (pkt->req->getAccessDepth()>0) {
            //fetchL1MissStarve++;
            //DPRINTFNR("T,%#x\n", (*memReq_it)->getVaddr());
            missSt[tid][(pkt->req->getVaddr())>>6] ='S';
            //fetchIcacheMissL1StDump++;
            int numStarves = 0;
            for(int i=0; i<8; i++) {
                if ((pkt->starveHistory>>i) & 1) {
                    numStarves++;
                }
            }

            if (!pureRandom && pkt->req->getAccessDepth()==1) {
                //fetchL2HitStarve++;
                didWeStarve = true;
                data_pkt2->setStarved(true);
                // Random preserve insertion OR
                // Insert always OR
                // 50% or more of misses led to starvation OR
                // Insert if starved atleast 1,2,4 etc number of times in last 8 accesses
                if ((randomStarve && random < starveRandomness) || 
                    (!randomStarve && starveAtleast==0) ||
		    (!randomStarve && starveAtleast<=8 && numStarves >= starveAtleast)) {
                    data_pkt2->setPreserve(true);
                } else {
                }
                icachePort.sendTimingStarvationReq(data_pkt2);
            } 
        } 
    } else if(pkt->req->getAccessDepth()>0) {
        //fetchL1MissNoStarve++;
        //DPRINTFNR("F,%#x\n", (*memReq_it)->getVaddr());
        missSt[tid][(pkt->req->getVaddr())>>6] ='N';
        //fetchIcacheMissL1NoStDump++;
    } else {
        //DPRINTFNR("H,%#x\n", (*memReq_it)->getVaddr());
        missSt[tid][(pkt->req->getVaddr())>>6] ='H';
    }

    decodeIdle[tid] = false;

    auto& tms = cpu->tmsMap[(*memReq_it)->getVaddr()];
    uint64_t &total = std::get<0>(tms);
    uint64_t &miss = std::get<1>(tms);
    uint64_t &starve = std::get<2>(tms);
    ++total;
    if (didWeStarve){
        ++miss;
        ++starve;
        //DPRINTFNR("T, %#x\n", (*memReq_it)->getVaddr());
    } else{ 
	if (pkt->req->getAccessDepth() > 0){
            ++miss;
	}
        //DPRINTFNR("F,%#x\n", (*memReq_it)->getVaddr());
    }

    if (pureRandom && random < starveRandomness) {
        RequestPtr mem_req2 = std::make_shared<Request>(
            (*memReq_it)->getVaddr(), fetchBufferSize,
            Request::INST_FETCH, cpu->instRequestorId(), (*memReq_it)->getPC(),
            cpu->thread[tid]->contextId());
        mem_req2->setPaddr((*memReq_it)->getPaddr());
        mem_req2->taskId(cpu->taskId());
        PacketPtr data_pkt2 = new Packet(mem_req2, MemCmd::ReadReq);
        data_pkt2->setPreserve(true);
        DPRINTF(Fetch, "Fetch marking the bit as starved\n");
        icachePort.sendTimingStarvationReq(data_pkt2);
    }
 
    DPRINTF(Fetch, "fetchBufferBlockPC: %#x and pc_it: %#x fetchAddr: %#x\n",
            fetchBufferBlockPC, *pc_it, fetchAddr);
    // Wake up the CPU (if it went to sleep and was waiting on
    // this completion event).
    int buf_ind = 0;
    for(auto &buf_it : fetchBufferPC[tid]){
      DPRINTF(Fetch, "fetchBufferPC[tid] at %d: %#x\n",buf_ind++, *(&buf_it));
    }
    if(*pc_it == fetchBufferBlockPC && pc_it == fetchBufferPC[tid].begin()) {
        cpu->wakeCPU();

        DPRINTF(Activity, "[tid:%i] Activating fetch due to cache completion\n",
                tid);

        switchToActive();

        // Only switch to IcacheAccessComplete if we're not stalled as well.
        if (checkStall(tid)) {
            fetchStatus[tid] = Blocked;
        } else {
            fetchStatus[tid] = IcacheAccessComplete;
        }
    }

    pkt->req->setAccessLatency();
    cpu->ppInstAccessComplete->notify(pkt);
    // Reset the mem req to NULL.
    delete pkt;
    memReq[tid].erase(memReq_it);
    //memReq[tid] = NULL;
}

void
Fetch::drainResume()
{
    for (ThreadID i = 0; i < numThreads; ++i) {
        stalls[i].decode = false;
        stalls[i].drain = false;
    }
}

void
Fetch::drainSanityCheck() const
{
    assert(isDrained());
    assert(retryPkt == NULL);
    assert(retryTid == InvalidThreadID);
    assert(!cacheBlocked);
    assert(!interruptPending);

    for (ThreadID i = 0; i < numThreads; ++i) {
        //assert(!memReq[i]);
        assert(memReq[i].empty());
        assert(fetchStatus[i] == Idle || stalls[i].drain);
    }

    branchPred->drainSanityCheck();
}

bool
Fetch::isDrained() const
{
    /* Make sure that threads are either idle of that the commit stage
     * has signaled that draining has completed by setting the drain
     * stall flag. This effectively forces the pipeline to be disabled
     * until the whole system is drained (simulation may continue to
     * drain other components).
     */
    for (ThreadID i = 0; i < numThreads; ++i) {
        // Verify fetch queues are drained
        if (!fetchQueue[i].empty())
            return false;

        // Return false if not idle or drain stalled
        if (fetchStatus[i] != Idle) {
            if (fetchStatus[i] == Blocked && stalls[i].drain)
                continue;
            else
                return false;
        }
    }

    /* The pipeline might start up again in the middle of the drain
     * cycle if the finish translation event is scheduled, so make
     * sure that's not the case.
     */
    return !finishTranslationEvent.scheduled();
}

void
Fetch::takeOverFrom()
{
    assert(cpu->getInstPort().isConnected());
    //assert(cpu->getInstBufferPort().isConnected());
    resetStage();

}

void
Fetch::drainStall(ThreadID tid)
{
    assert(cpu->isDraining());
    assert(!stalls[tid].drain);
    DPRINTF(Drain, "%i: Thread drained.\n", tid);
    stalls[tid].drain = true;
}

void
Fetch::wakeFromQuiesce()
{
    DPRINTF(Fetch, "Waking up from quiesce\n");
    // Hopefully this is safe
    // @todo: Allow other threads to wake from quiesce.
    fetchStatus[0] = Running;
}

void
Fetch::switchToActive()
{
    if (_status == Inactive) {
        DPRINTF(Activity, "Activating stage.\n");

        cpu->activateStage(CPU::FetchIdx);

        _status = Active;
    }
}

void
Fetch::switchToInactive()
{
    if (_status == Active) {
        DPRINTF(Activity, "Deactivating stage.\n");

        cpu->deactivateStage(CPU::FetchIdx);

        _status = Inactive;
    }
}

void
Fetch::deactivateThread(ThreadID tid)
{
    // Update priority list
    auto thread_it = std::find(priorityList.begin(), priorityList.end(), tid);
    if (thread_it != priorityList.end()) {
        priorityList.erase(thread_it);
    }
}

bool
Fetch::lookupAndUpdateNextPC(const DynInstPtr &inst, TheISA::PCState &nextPC)
{
    // Do branch prediction check here.
    // A bit of a misnomer...next_PC is actually the current PC until
    // this function updates it.
    //bool predict_taken;
    ThreadID tid = inst->threadNumber;
    TheISA::PCState ftPC = nextPC;
    inst->staticInst->advancePC(ftPC);

    // The next PC to access.

    if (!inst->isControl()) {
        //inst->staticInst->advancePC(nextPC);
        inst->setBblSize(bblSize[tid]);
        inst->setBblAddr(bblAddr[tid]);
        if(!inst->isMicroop() || inst->isLastMicroop())
            bblSize[tid] += nextPC.size();
        inst->staticInst->advancePC(nextPC);
        inst->setPredTarg(nextPC);
        inst->setPredTaken(false);
        DPRINTF(Fetch, "[tid:%i] [sn:%llu, %llu] isControl %d, PFQ Size: %d, "
                "Uncond: %d, Cond: %d, Direct: %d, Addr %#x, Target %#x\n", 
                tid, inst->seqNum, brseq[tid], inst->isControl(), 
                prefetchQueue[tid].size(), inst->isUncondCtrl(), 
                inst->isCondCtrl(), inst->isDirectCtrl(), 
                inst->pcState().instAddr(), //branchPC.instAddr(), 
                inst->isDirectCtrl() ? inst->branchTarget() : 0);
        return false;
    }
 
    DPRINTF(Fetch, "IsControl lookup: %d %d, %#x, %llu %llu\n", 
            inst->isControl(), prefetchQueue[tid].empty(), 
            inst->pcState().instAddr(), inst->seqNum, brseq[tid]);

    bool predict_taken;
    TheISA::PCState branchPC, tempPC;
    bool predictorInvoked = false;

    //Pre-decode branch instruction and update BTB
    if (inst->isDirectCtrl() && bblAddr[tid] != 0) {
        DPRINTF(Bgodala, "BBLInsert Inserting bblAddr[tid]: %#x instAddr: %#x branchTarget: %#x bblSize: %d diff: %d\n",
                bblAddr[tid], inst->pcState().instAddr(), inst->branchTarget(), bblSize[tid], inst->pcState().instAddr() - bblAddr[tid]);
        assert(((inst->pcState().instAddr() - bblAddr[tid]) == bblSize[tid]) && "BBLInsert Mismatch" );
        branchPred->BTBUpdate(bblAddr[tid],
                              inst->staticInst,
                              inst->pcState(),
                              bblSize[tid],
                              inst->branchTarget(),
                              ftPC,
                              inst->isUncondCtrl(),
                              tid);
    }
    else if (inst->isControl() && bblAddr[tid] != 0) {
        TheISA::PCState dummyBranchTarget = ftPC;
        //dummyBranchTarget.pc(-1);
        //dummyBranchTarget.npc(-1);

        DPRINTF(Bgodala, "BBLInsert Inserting Indirect ctrl bblAddr[tid]: %#x instAddr: %#x branchTarget: %#x bblSize: %d diff: %d\n",
                bblAddr[tid], inst->pcState().instAddr(), dummyBranchTarget, bblSize[tid], inst->pcState().instAddr() - bblAddr[tid]);
        assert(((inst->pcState().instAddr() - bblAddr[tid]) == bblSize[tid]) && "BBLInsert Mismatch" );
        branchPred->BTBUpdate(bblAddr[tid],
                              inst->staticInst,
                              inst->pcState(),
                              bblSize[tid],
                              dummyBranchTarget,
                              ftPC,
                              inst->isUncondCtrl(),
                              tid);
    }
    if (!prefetchQueue[tid].empty()) {
        for (auto it = prefetchQueue[tid].cbegin(); it != prefetchQueue[tid].cend(); ++it)
            DPRINTF(Fetch, "%s\n", *it);
    
        for (auto it = prefetchQueueBr[tid].cbegin(); it != prefetchQueueBr[tid].cend(); ++it)
            DPRINTF(Fetch, "%s\n", *it);

        tempPC = prefetchQueue[tid].front();
        prefetchQueue[tid].pop_front();
        prevPC[tid] = tempPC;
        prefetchQueueBblSize[tid].pop_front();
        brseq[tid] = prefetchQueueSeqNum[tid].front();
        prefetchQueueSeqNum[tid].pop_front();
        branchPC = prefetchQueueBr[tid].front();
        prefetchQueueBr[tid].pop_front();

        predict_taken = nextPC.npc() != tempPC.instAddr();

        if (branchPC.instAddr()!=inst->instAddr()){
            if (inst->isReturn()){
                warn("Return Inst: %llu\n", curTick());
            }
            // squash branch predictor state to remove stale entries
            branchPred->squash(brseq[tid]-1, tid);
            // Reset prefetch Queue
            prefetchQueue[tid].clear();
            prefetchQueueBblSize[tid].clear();
            prefetchQueueSeqNum[tid].clear();
            prefetchQueueBr[tid].clear();
            //prefetchBufferPC[tid].clear();
            tempPC = nextPC;
            predict_taken = branchPred->predict(inst->staticInst, seq[tid],
                                      bblAddr[tid], tempPC, tid);
            brseq[tid] = seq[tid];
            seq[tid]++;
            prefPC[tid] = tempPC;
            DPRINTF(Fetch, "Nayana mismatch %#x, %#x\n", branchPC.instAddr(), inst->instAddr());
            //DPRINTFN("Nayana mismatch %#x, %#x\n", branchPC.instAddr(), inst->instAddr());
            //if (prefetchQueue[tid].empty())
            //    TheISA::advancePC(tempPC, inst->staticInst);
            //else {
            //    tempPC = prefetchQueue[tid].front();
            //    prefetchQueue[tid].pop_front();
            //    prevPC[tid] = tempPC;
            //    prefetchQueueBblSize[tid].pop_front();
            //    brseq[tid] = prefetchQueueSeqNum[tid].front();
            //    prefetchQueueSeqNum[tid].pop_front();
            //    branchPC = prefetchQueueBr[tid].front();
            //    prefetchQueueBr[tid].pop_front();
            //}

            //assert(false && "branchpred called from fetch\n");
            if(enableFDIP){
                //prefetchBufferPC[tid].clear();
                //fetchBuffer[tid].clear();
                //fetchBufferPC[tid].clear();
                //fetchBufferReqPtr[tid].clear();
                //fetchBufferValid[tid].clear();
                predictorInvoked = true;
            }
        }

        DPRINTF(Fetch, "[tid:%i] [sn:%llu, %llu] Branch at PC %#x "
                "predicted to go to %s %d, %#x. Queue %d\n", tid, inst->seqNum, 
                brseq[tid], inst->pcState().instAddr(), tempPC, predict_taken, 
                branchPC.instAddr(), prefetchQueue[tid].size());
    } else {
        DPRINTF(Fetch, "Nayana lookupAndupdate\n");
        tempPC = nextPC;
	    DPRINTF(Fetch, "Bgodala tempPC is %#x\n", tempPC.instAddr());
        predict_taken = branchPred->predict(inst->staticInst, seq[tid],
                                      bblAddr[tid], tempPC, tid);
        brseq[tid] = seq[tid];
        seq[tid]++;
        //assert(false && "branchpred called from fetch\n");

        if(enableFDIP){
            //prefetchBufferPC[tid].clear();
            //fetchBuffer[tid].clear();
            //fetchBufferPC[tid].clear();
            //fetchBufferReqPtr[tid].clear();
            //fetchBufferValid[tid].clear();
            prefPC[tid] = tempPC;
            predictorInvoked = true;
        }
    }

    if(tempPC.instAddr()<0x10) {
        inst->staticInst->advancePC(nextPC);
        predict_taken = false;
    } else {
	    DPRINTF(Fetch, "Bgodala setting nextPC is %#x\n", tempPC.instAddr());
        nextPC = tempPC;
    }

    if (predict_taken) {
        DPRINTF(Fetch, "[tid:%i] [sn:%llu] Branch at PC %#x "
                "predicted to be taken to %s\n",
                tid, inst->seqNum, inst->pcState().instAddr(), nextPC);
    } else {
        DPRINTF(Fetch, "[tid:%i] [sn:%llu] Branch at PC %#x "
                "predicted to be not taken\n",
                tid, inst->seqNum, inst->pcState().instAddr());
    }

    DPRINTF(Fetch, "[tid:%i] [sn:%llu] Branch at PC %#x "
            "predicted to go to %s\n",
            tid, inst->seqNum, inst->pcState().instAddr(), nextPC);
    inst->setPredTarg(nextPC);
    inst->setPredTaken(predict_taken);
    inst->isBTBMiss = branchPred->isBTBMiss(brseq[tid], tid);

    ++fetchStats.branches;

    if (predict_taken) {
        ++fetchStats.predictedBranches;
    }

    inst->setBblSize(bblSize[tid]);
    inst->setBblAddr(bblAddr[tid]);

    bblAddr[tid] = nextPC.instAddr();
    bblSize[tid] = 0;

    if(enableFDIP && predictorInvoked){
        //warn("VERIFY: Self modifying corner case found! %llu\n",curTick());
        // When predictor is invoked reset prefetching
        prefPC[tid] = nextPC;
        lastPrefPC = 0; 
        prefetchQueue[tid].clear();
        prefetchQueueBblSize[tid].clear();
        prefetchQueueSeqNum[tid].clear();
        prefetchQueueBr[tid].clear();
        lastProcessedLine = 0;
        lastAddrFetched = 0;
        fallThroughPrefPC = 0;

        //Flush fetch and prefetch buffer only on taken branch
        //if(predict_taken){
        //    prefetchBufferPC[tid].clear();
        //    fetchBuffer[tid].clear();
        //    fetchBufferPC[tid].clear();
        //    fetchBufferReqPtr[tid].clear();
        //    fetchBufferValid[tid].clear();
        //    memReq[tid].clear();
        //}
        
    //    lastProcessedLine = 0;
    //    lastAddrFetched = 0;
    //    fallThroughPrefPC = 0;
    //    //if(predict_taken){
    //    //    prefetchBufferPC[tid].clear();
    //    //    fetchBuffer[tid].clear();
    //    //    fetchBufferPC[tid].clear();
    //    //    fetchBufferReqPtr[tid].clear();
    //    //    fetchBufferValid[tid].clear();
    //    //}
    }

    //if (prefetchQueue[0].size()==0){
    //    prefPC[0] = nextPC;
    //}
    return predict_taken;
}

void
Fetch::profileMispredict(const DynInstPtr &inst, bool taken)
{
    /*if (taken)
        mispredictedCFIAsFTActuallyTaken++;
    else
        mispredictedCFIAsTakenActuallyFT++;

    if(inst->isDirectCtrl() && !inst->isCall() && !inst->isReturn())
        mispredictedDirectBranches++;

    else if(inst->isIndirectCtrl() && !inst->isCall() && !inst->isReturn())
         mispredictedIndirectBranches++;

    else if(inst->isDirectCtrl() && inst->isCall())
        mispredictedDirectCalls++;

    else if(inst->isIndirectCtrl() && inst->isCall())
        mispredictedIndirectCalls++;
  
    else if(inst->isReturn())
        mispredictedReturns++;*/

}

bool
Fetch::fetchCacheLine(Addr vaddr, ThreadID tid, Addr pc)
{
    Fault fault = NoFault;

    assert(!cpu->switchedOut());

    // @todo: not sure if these should block translation.
    //AlphaDep
    if (cacheBlocked) {
        DPRINTF(Fetch, "[tid:%i] Can't fetch cache line, cache blocked\n",
                tid);
        return false;
    } else if (checkInterrupt(pc) && !delayedCommit[tid]) {
        // Hold off fetch from getting new instructions when:
        // Cache is blocked, or
        // while an interrupt is pending and we're not in PAL mode, or
        // fetch is switched out.
        DPRINTF(Fetch, "[tid:%i] Can't fetch cache line, interrupt pending\n",
                tid);
        return false;
    }

    // Align the fetch address to the start of a fetch buffer segment.
    Addr fetchBufferBlockPC = fetchBufferAlignPC(vaddr);

    DPRINTF(Fetch, "[tid:%i] Fetching cache line %#x for addr %#x\n",
            tid, fetchBufferBlockPC, vaddr);

    // Setup the memReq to do a read of the first instruction's address.
    // Set the appropriate read size and flags as well.
    // Build request here.
    RequestPtr mem_req = std::make_shared<Request>(
        fetchBufferBlockPC, fetchBufferSize,
        Request::INST_FETCH, cpu->instRequestorId(), pc,
        cpu->thread[tid]->contextId());

    mem_req->taskId(cpu->taskId());

    //memReq[tid] = mem_req;
    memReq[tid].push_back(mem_req);

    // Initiate translation of the icache block
    if(add_front)
        fetchStatus[tid] = ItlbWait;
    //TODO: Add check to not send multiple requests for the same
    //address
    if (add_front && !enableFDIP) {
        prefetchQueue[tid].clear();
        prefetchQueueBblSize[tid].clear();
        prefetchQueueSeqNum[tid].clear();
        prefetchQueueBr[tid].clear();
        prefetchBufferPC[tid].clear();
        fetchBuffer[tid].clear();
        fetchBufferPC[tid].clear();
        fetchBufferReqPtr[tid].clear();
        fetchBufferValid[tid].clear();
        fetchBufferPC[tid].push_front(fetchBufferBlockPC);
        fetchBufferValid[tid].push_front(false);
        fetchBuffer[tid].push_front(new uint8_t[fetchBufferSize]);
        prefetchBufferPC[tid].push_front(fetchBufferBlockPC);
        fetchBufferReqPtr[tid].push_front(mem_req);
    } else {
        fetchBufferPC[tid].push_back(fetchBufferBlockPC);
        fetchBufferValid[tid].push_back(false);
        fetchBuffer[tid].push_back(new uint8_t[fetchBufferSize]);
        fetchBufferReqPtr[tid].push_back(mem_req);
        DPRINTF(Fetch, "Sending fetchBufferPC:%#x fetchBufferReqPtr:%#x\n",fetchBufferBlockPC, mem_req);
    }

    add_front = false;

    FetchTranslation *trans = new FetchTranslation(this);
    cpu->mmu->translateTiming(mem_req, cpu->thread[tid]->getTC(),
                              trans, BaseMMU::Execute);

   return true;
}

void
Fetch::finishTranslation(const Fault &fault, const RequestPtr &mem_req)
{
    ThreadID tid = cpu->contextToThread(mem_req->contextId());
    Addr fetchBufferBlockPC = mem_req->getVaddr();

    assert(!cpu->switchedOut());

    // Wake up CPU if it was idle
    cpu->wakeCPU();

    ////if (fetchStatus[tid] != ItlbWait || mem_req != memReq[tid] ||
    //if ( memReq[tid].empty() || (mem_req->getVaddr() != ((pc[tid].instAddr() >> 6 ) << 6) &&  (mem_req != memReq[tid].back() ||
    //    //mem_req->getVaddr() != memReq[tid]->getVaddr()) {
    //    mem_req->getVaddr() != memReq[tid].back()->getVaddr()))) {

    bool foundPC = false;
    DPRINTF(Fetch, "fetchBufferPC size is %d memReq[tid] size is %d\n",fetchBufferPC[tid].size(), memReq[tid].size());
    reqIt mreq_it = fetchBufferReqPtr[tid].begin();

    for ( auto pc_it : fetchBufferPC[tid]){
        DPRINTF(Fetch, "pc_it: %#x and fetchBufferPC: %#x\n", pc_it, mem_req->getVaddr());
        if(pc_it == mem_req->getVaddr() && *mreq_it == mem_req){
            foundPC = true;
            break;
        }
        mreq_it++;
    }
    if(memReq[tid].empty() || !foundPC){
        DPRINTF(Fetch, "[tid:%i] Ignoring itlb completed after squash vaddr %#x pc[tid:%i] %#x memReq[tid].empty(): %d foundPC: %d\n",
                tid, mem_req->getVaddr(), tid, pc[tid].instAddr(), memReq[tid].empty(), foundPC);
        ++fetchStats.tlbSquashes;
        add_front = false;
        return;
    }


    // If translation was successful, attempt to read the icache block.
    if (fault == NoFault) {
        // Check that we're not going off into random memory
        // If we have, just wait around for commit to squash something and put
        // us on the right track
        if (!cpu->system->isMemAddr(mem_req->getPaddr())) {
            warn("Address %#x is outside of physical memory, stopping fetch\n",
                    mem_req->getPaddr());
            fetchStatus[tid] = NoGoodAddr;
            //memReq[tid] = NULL;
            memReq[tid].pop_back();
            add_front = false;
            return;
        }

        // Build packet here.
        PacketPtr data_pkt = new Packet(mem_req, MemCmd::ReadReq);
        data_pkt->dataDynamic(new uint8_t[fetchBufferSize]);

        //fetchBufferPC[tid] = fetchBufferBlockPC;
        //fetchBufferValid[tid] = false;
        DPRINTF(Fetch, "Fetch: Doing instruction read.\n");
        
        //if (add_front) {
        //    fetchBufferPC[tid].push_front(fetchBufferBlockPC);
        //    fetchBufferValid[tid].push_front(false);
        //    fetchBuffer[tid].push_front(new uint8_t[fetchBufferSize]);
        //} else {
        //    fetchBufferPC[tid].push_back(fetchBufferBlockPC);
        //    fetchBufferValid[tid].push_back(false);
        //    fetchBuffer[tid].push_back(new uint8_t[fetchBufferSize]);
        //}

        TheISA::PCState thisPC = pc[tid];
        Addr pcOffset = fetchOffset[tid];
        Addr fetchAddr = (thisPC.instAddr() + pcOffset) & decoder[tid]->pcMask();
        Addr fetchBufferExpectedPC = fetchBufferAlignPC(fetchAddr);


        fetchStats.cacheLines++;

        // Access the cache.
        if(enablePerfectICache){
            icachePort.sendFunctional(data_pkt);
            DPRINTF(Fetch, "[tid:%i] Doing Icache access.\n", tid);
            DPRINTF(Activity, "[tid:%i] Activity: Waiting on I-cache "
                    "response.\n", tid);
            lastIcacheStall[tid] = curTick();
            // cms11
            memsent_ticks[tid][(mem_req->getVaddr())>>6] = curTick();
            if(add_front  || (fetchBufferBlockPC==fetchBufferExpectedPC && fetchBufferPC[tid].size()==1))
                fetchStatus[tid] = IcacheWaitResponse;
            // Notify Fetch Request probe when a packet containing a fetch
            // request is successfully sent
            ppFetchRequestSent->notify(mem_req);
            //processCacheCompletion(data_pkt);
            cpu->schedule(new EventFunctionWrapper(
              [this,data_pkt]{ 
              processCacheCompletion(data_pkt);}, "BGODALA"), cpu->clockEdge(Cycles(ICACHE_ACCESS_LATENCY)));

            // Send this packet to model bandwidth utilization
            PacketPtr dummy_data_pkt = new Packet(mem_req, MemCmd::ReadReq);
            dummy_data_pkt->dataDynamic(new uint8_t[fetchBufferSize]);
            icachePort.sendTimingReq(dummy_data_pkt);
        }else{
            if (!icachePort.sendTimingReq(data_pkt)) {
                DPRINTF(Fetch, "SendTimingReq failed\n");
                //if (add_front  || (fetchBufferBlockPC==fetchBufferExpectedPC && fetchBufferPC[tid].size()==1)) {
                if (fetchBufferBlockPC==fetchBufferExpectedPC && !fetchBufferReqPtr[tid].empty() && fetchBufferReqPtr[tid].front() == *mreq_it) {

                    if (retryPkt == NULL){
                        assert(retryPkt == NULL);
                        assert(retryTid == InvalidThreadID);
                        DPRINTF(Fetch, "[tid:%i] Out of MSHRs!\n", tid);

                        fetchStatus[tid] = IcacheWaitRetry;
                        retryPkt = data_pkt;
                        retryTid = tid;
                    }
                } else {
                    //FIXME: pop only the request that failed
                    pcIt pc_it = fetchBufferPC[tid].begin();
                    validIt val_it = fetchBufferValid[tid].begin();
                    bufIt buf_it = fetchBuffer[tid].begin();
                    reqIt req_it = fetchBufferReqPtr[tid].begin();

                    while(req_it != fetchBufferReqPtr[tid].end()){

                        if(*req_it == mem_req) {
                             DPRINTF(Fetch, "Req failed erasing pkt with pc %#x\n", *pc_it);
                             fetchBufferPC[tid].erase(pc_it, fetchBufferPC[tid].end());
                             fetchBufferValid[tid].erase(val_it, fetchBufferValid[tid].end());
                             fetchBuffer[tid].erase(buf_it, fetchBuffer[tid].end());
                             fetchBufferReqPtr[tid].erase(req_it, fetchBufferReqPtr[tid].end());
                             memReqListIt memReq_it = std::find(memReq[tid].begin(),memReq[tid].end(), mem_req);
                             assert(memReq_it != memReq[tid].end() && "This element should exist in the memReq[tid] list");
                             memReq[tid].erase(memReq_it, memReq[tid].end());
                             break;
                        }
                        pc_it++;
                        buf_it++;
                        val_it++;
                        req_it++;
                    }
                }
                cacheBlocked = true;
            } else {
                DPRINTF(Fetch, "[tid:%i] Doing Icache access.\n", tid);
                DPRINTF(Activity, "[tid:%i] Activity: Waiting on I-cache "
                        "response.\n", tid);
                lastIcacheStall[tid] = curTick();
                // cms11
                memsent_ticks[tid][(mem_req->getVaddr())>>6] = curTick();
                if(add_front  || (fetchBufferBlockPC==fetchBufferExpectedPC && fetchBufferPC[tid].size()==1))
                    fetchStatus[tid] = IcacheWaitResponse;
                // Notify Fetch Request probe when a packet containing a fetch
                // request is successfully sent
                ppFetchRequestSent->notify(mem_req);
            }
        }
    } else {
        
        //DPRINTFN("Translation fault\n");
        TheISA::PCState thisPC = pc[tid];
        Addr pcOffset = fetchOffset[tid];
        Addr fetchAddr = (thisPC.instAddr() + pcOffset) & decoder[tid]->pcMask();
        Addr fetchBufferExpectedPC = fetchBufferAlignPC(fetchAddr);
        //if (!add_front && !fetchBufferPC[tid].empty()) {
        add_front = false;
        if (fetchBufferBlockPC!=fetchBufferExpectedPC) {
            DPRINTFN("Translation Faulted but not for head of buffer, so lets ignore for now %#x, %#x\n",
                    fetchBufferBlockPC, fetchBufferExpectedPC);
            //DPRINTF(Fetch, "Translation Faulted but not for head of buffer, so lets ignore for now %#x, %#x",
            //        fetchBufferBlockPC, fetchBufferExpectedPC);
            //memReq[tid].pop_back();
            //prefetchQueue[tid].clear();
            //prefetchQueueBblSize[tid].clear();
            //prefetchQueueSeqNum[tid].clear();
            //prefetchQueueBr[tid].clear();
            //fetchStatus[tid] = Running;
            //prefetchBufferPC[tid].clear();

            pcIt pc_it = fetchBufferPC[tid].begin();
            validIt val_it = fetchBufferValid[tid].begin();
            bufIt buf_it = fetchBuffer[tid].begin();
            reqIt req_it = fetchBufferReqPtr[tid].begin();
            pcIt pref_pc_it = prefetchBufferPC[tid].begin();

            while(req_it != fetchBufferReqPtr[tid].end()){

                assert(*pc_it == *pref_pc_it && "pc_it and pref_pc_it should be same\n");
                if(*req_it == mem_req) {
                     DPRINTFN("Translation Fault erasing pkt with pc %#x\n", *pc_it);
                     fetchBufferPC[tid].erase(pc_it, fetchBufferPC[tid].end());
                     fetchBufferValid[tid].erase(val_it, fetchBufferValid[tid].end());
                     fetchBuffer[tid].erase(buf_it, fetchBuffer[tid].end());
                     fetchBufferReqPtr[tid].erase(req_it, fetchBufferReqPtr[tid].end());
                     //empty prefetchBufferPC so that the request is not sent again
                     prefetchBufferPC[tid].erase(pref_pc_it,prefetchBufferPC[tid].end());
                     memReqListIt memReq_it = std::find(memReq[tid].begin(),memReq[tid].end(), mem_req);
                     assert(memReq_it != memReq[tid].end() && "This element should exist in the memReq[tid] list");
                     memReq[tid].erase(memReq_it, memReq[tid].end());
                     break;
                }
                pc_it++;
                buf_it++;
                val_it++;
                req_it++;
                pref_pc_it++;
            }

            //Stop prefetching
            prefPC[tid] = 0;
            lastPrefPC = 0;
            //while(!prefetchQueue[tid].empty()){
            //    Addr lastEntryAddr  = prefetchQueue[tid].back().instAddr();
            //    lastEntryAddr &=  decoder[tid]->pcMask();
            //    lastEntryAddr = fetchBufferAlignPC(lastEntryAddr);

            //    prefetchQueue[tid].pop_back();
            //    prefetchQueueBblSize[tid].pop_back();
            //    prefetchQueueSeqNum[tid].pop_back();
            //    prefetchQueueBr[tid].pop_back();
            //    if(lastEntryAddr == fetchBufferBlockPC ){
            //        break;
            //    }
            //}

            //if(!prefetchQueue[tid].empty()){
            //    DPRINTFN("prefetchQueue last element is %s\n", prefetchQueue[tid].back());
            //}else{
            //    DPRINTFN("prefetchQueue is empty\n"); 
            //}


            DPRINTFN("Stop prefetching on translation fault which is not the head of the buffer\n");
            //cpu->activityThisCycle();
            //fetchStatus[tid] = Running;
            return;
        }
        add_front = false;
        // Don't send an instruction to decode if we can't handle it.
        if (!(numInst < fetchWidth) ||
                !(fetchQueue[tid].size() < fetchQueueSize)) {
            //DPRINTF(Fetch, "Bgodala assert fail Translation fault 0x%lx fetchPC is 0x%lx\n",mem_req->getVaddr(), thisPC.instAddr());
            DPRINTFN("Bgodala assert fail Translation fault 0x%lx fetchPC is 0x%lx\n",mem_req->getVaddr(), thisPC.instAddr());
            //assert(!finishTranslationEvent.scheduled());
            if(finishTranslationEvent.scheduled()){
                return;
            }
            finishTranslationEvent.setFault(fault);
            finishTranslationEvent.setReq(mem_req);
            cpu->schedule(finishTranslationEvent,
                          cpu->clockEdge(Cycles(1)));
            return;
        }
        DPRINTF(Fetch, "TRAP: fetchBufferBlockPC: %#x and fetchBufferExpectedPC: %#x\n",
                fetchBufferBlockPC,
                fetchBufferExpectedPC);
        DPRINTF(Fetch,
                "[tid:%i] Got back req with addr %#x but expected %#x\n",
                //tid, mem_req->getVaddr(), memReq[tid]->getVaddr());
                tid, mem_req->getVaddr(), memReq[tid].back()->getVaddr());
        
        //DPRINTF(Fetch, "Translation Faulted but not for head of buffer, so lets ignore for now %#x, %#x",
        //        fetchBufferBlockPC, fetchBufferExpectedPC);
        //memReq[tid].pop_back();
        //prefetchQueue[tid].clear();
        //prefetchQueueBblSize[tid].clear();
        //prefetchQueueSeqNum[tid].clear();
        //prefetchQueueBr[tid].clear();
        //fetchStatus[tid] = Running;
        //return;

        // Translation faulted, icache request won't be sent.
        //memReq[tid] = NULL;
        memReq[tid].clear();
        //Stop prefetching too
        prevPC[tid]=0;

        // Send the fault to commit.  This thread will not do anything
        // until commit handles the fault.  The only other way it can
        // wake up is if a squash comes along and changes the PC.
        TheISA::PCState fetchPC = pc[tid];

        DPRINTF(Fetch, "[tid:%i] Translation faulted, building noop.\n", tid);
        // We will use a nop in order to carry the fault.
        DynInstPtr instruction = buildInst(tid, gem5::nopStaticInstPtr,
                                           NULL, fetchPC, fetchPC, false);
        instruction->setNotAnInst();
        instruction->setBrSeq(brseq[tid]);
        instruction->setBblSize(bblSize[tid]);
        instruction->setBblAddr(bblAddr[tid]);

        instruction->setPredTarg(fetchPC);
        instruction->fault = fault;
        wroteToTimeBuffer = true;

        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();

        fetchStatus[tid] = TrapPending;

        DPRINTF(Fetch, "[tid:%i] Blocked, need to handle the trap.\n", tid);
        DPRINTF(Fetch, "[tid:%i] fault (%s) detected @ PC %s.\n",
                tid, fault->name(), pc[tid]);
    }
    _status = updateFetchStatus();
    add_front = false;
}

void
Fetch::doSquash(const TheISA::PCState &newPC, const DynInstPtr squashInst,
        ThreadID tid)
{
    trackLastBlock = false;
    DPRINTF(Fetch, "[tid:%i] Squashing, setting PC to: %s.\n",
            tid, newPC);

    //DPRINTF(Fetch, "[tid:%i] prefetchQueue size: %d, %s.\n",
    //        tid, prefetchQueue[tid].size(), prefetchQueue[tid].front());
    //if (lastinst[tid]) {
        prefPC[tid] = newPC;
        lastPrefPC = newPC;
        //bblAddr[tid] = prefPC[tid].instAddr();
        prefetchQueue[tid].clear();
        prefetchQueueBblSize[tid].clear();
        prefetchQueueSeqNum[tid].clear();
        prefetchQueueBr[tid].clear();
        prefetchBufferPC[tid].clear();
        DPRINTF(Fetch, "[tid:%i] Squashing, prefetch Queue to size: %d.\n",
            tid, prefetchQueue[tid].size());
    //}
    lastProcessedLine = 0;
    lastAddrFetched = 0;
    fallThroughPrefPC = 0;

    pc[tid] = newPC;
    lastinst[tid] = squashInst;

    DPRINTF(Fetch, "[tid:%i] Squashing, Queue to size: %d %d %d.\n",
            tid, fetchBuffer[tid].size(), fetchBufferPC[tid].size(), fetchBufferValid[tid].size());
    fetchOffset[tid] = 0;
    if (squashInst && squashInst->pcState().instAddr() == newPC.instAddr()) {
        macroop[tid] = squashInst->macroop;
        macroop[tid] = NULL; 
        //for ( auto &buf_it : fetchBuffer[tid]){
        //    delete &*buf_it;
        //}
        fetchBuffer[tid].clear();
        fetchBufferPC[tid].clear();
        fetchBufferReqPtr[tid].clear();
        add_front = false;
        fetchBufferValid[tid].clear();
    } else {
        macroop[tid] = NULL;
        //for ( auto &buf_it : fetchBuffer[tid]){
        //    delete &*buf_it;
        //}
        fetchBuffer[tid].clear();
        fetchBufferPC[tid].clear();
        fetchBufferReqPtr[tid].clear();
        add_front = false;
        fetchBufferValid[tid].clear();
    }

    decoder[tid]->reset();

    // Clear the icache miss if it's outstanding.
    //if (fetchStatus[tid] == IcacheWaitResponse) {
    //    DPRINTF(Fetch, "[tid:%i] Squashing outstanding Icache miss.\n",
    //            tid);
    //    memReq[tid] = NULL;
        memReq[tid].clear();
        DPRINTF(Fetch, "[tid:%i] Squashing, Queue to size: %d %d %d.\n",
            tid, fetchBuffer[tid].size(), fetchBufferPC[tid].size(), fetchBufferValid[tid].size());
    //} else if (fetchStatus[tid] == ItlbWait) {
    //    DPRINTF(Fetch, "[tid:%i] Squashing outstanding ITLB miss.\n",
    //            tid);
    //    memReq[tid] = NULL;
    //}

    // Get rid of the retrying packet if it was from this thread.
    if (retryTid == tid) {
        assert(cacheBlocked);
        if (retryPkt) {
            delete retryPkt;
        }
        retryPkt = NULL;
        retryTid = InvalidThreadID;
    }

    fetchStatus[tid] = Squashing;

    // Empty fetch queue
    fetchQueue[tid].clear();

    // microops are being squashed, it is not known wheather the
    // youngest non-squashed microop was  marked delayed commit
    // or not. Setting the flag to true ensures that the
    // interrupts are not handled when they cannot be, though
    // some opportunities to handle interrupts may be missed.
    delayedCommit[tid] = true;

    ++fetchStats.squashCycles;
}

void
Fetch::squashFromDecode(const TheISA::PCState &newPC,
        const DynInstPtr squashInst, const InstSeqNum seq_num, ThreadID tid)
{
    DPRINTF(Fetch, "[tid:%i] Squashing from decode.\n", tid);

    doSquash(newPC, squashInst, tid);

    // Tell the CPU to remove any instructions that are in flight between
    // fetch and decode.
    cpu->removeInstsUntil(seq_num, tid);
}

bool
Fetch::checkStall(ThreadID tid) const
{
    bool ret_val = false;

    if (stalls[tid].drain) {
        assert(cpu->isDraining());
        DPRINTF(Fetch,"[tid:%i] Drain stall detected.\n",tid);
        ret_val = true;
    }

    return ret_val;
}

Fetch::FetchStatus
Fetch::updateFetchStatus()
{
    //Check Running
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (fetchStatus[tid] == Running ||
            fetchStatus[tid] == Squashing ||
            fetchStatus[tid] == IcacheAccessComplete) {

            if (_status == Inactive) {
                DPRINTF(Activity, "[tid:%i] Activating stage.\n",tid);

                if (fetchStatus[tid] == IcacheAccessComplete) {
                    DPRINTF(Activity, "[tid:%i] Activating fetch due to cache"
                            "completion\n",tid);
                }

                cpu->activateStage(CPU::FetchIdx);
            }

            return Active;
        }
    }

    // Stage is switching from active to inactive, notify CPU of it.
    if (_status == Active) {
        DPRINTF(Activity, "Deactivating stage.\n");

        cpu->deactivateStage(CPU::FetchIdx);
    }

    return Inactive;
}

void
Fetch::squash(const TheISA::PCState &newPC, const InstSeqNum seq_num,
        DynInstPtr squashInst, ThreadID tid)
{
    DPRINTF(Fetch, "[tid:%i] Squash from commit.\n", tid);

    doSquash(newPC, squashInst, tid);

    // Tell the CPU to remove any instructions that are not in the ROB.
    cpu->removeInstsNotInROB(tid);
}

void
Fetch::tick()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();
    bool status_change = false;

    wroteToTimeBuffer = false;

    for (ThreadID i = 0; i < numThreads; ++i) {
        issuePipelinedIfetch[i] = false;
    }

    while (threads != end) {
        ThreadID tid = *threads++;

        // Check the signals for each thread to determine the proper status
        // for each thread.
        bool updated_status = checkSignalsAndUpdate(tid);
        status_change =  status_change || updated_status;
    }

    DPRINTF(Fetch, "Running stage.\n");

    if (FullSystem) {
        if (fromCommit->commitInfo[0].interruptPending) {
            interruptPending = true;
        }

        if (fromCommit->commitInfo[0].clearInterrupt) {
            interruptPending = false;
        }
    }

    for (threadFetched = 0; threadFetched < numFetchingThreads;
         threadFetched++) {
        // Fetch each of the actively fetching threads.
        if(enableFDIP){
            addToFTQ();
        }
        fetch(status_change);
        //if (prefetchQueue[0].size()==0)
        //    prefPC[0] = pc[0];
    }

    // Record number of instructions fetched this cycle for distribution.
    fetchStats.nisnDist.sample(numInst);

    if (status_change) {
        // Change the fetch stage status if there was a status change.
        _status = updateFetchStatus();
    }

    // Issue the next I-cache request if possible.
    for (ThreadID i = 0; i < numThreads; ++i) {
        if (fromDecode->decodeIdle[i]) {
            decodeIdle[i] = true;
        } else {
            decodeIdle[i] = false;
        }
        //if (issuePipelinedIfetch[i]) {
        if(fetchStatus[i] != Squashing) {
            pipelineIcacheAccesses(i);
        } 
    }

    // Send instructions enqueued into the fetch queue to decode.
    // Limit rate by fetchWidth.  Stall if decode is stalled.
    unsigned insts_to_decode = 0;
    unsigned available_insts = 0;

    for (auto tid : *activeThreads) {
        if (!stalls[tid].decode) {
            available_insts += fetchQueue[tid].size();
        }
    }

    // Pick a random thread to start trying to grab instructions from
    auto tid_itr = activeThreads->begin();
    std::advance(tid_itr,
            random_mt.random<uint8_t>(0, activeThreads->size() - 1));

    while (available_insts != 0 && insts_to_decode < decodeWidth) {
        ThreadID tid = *tid_itr;
        if (!stalls[tid].decode && !fetchQueue[tid].empty()) {
            const auto& inst = fetchQueue[tid].front();
            if(trackLastBlock){
                inst->isStalled = true;
                trackLastBlock = false;
            }
            toDecode->insts[toDecode->size++] = inst;
            DPRINTF(Fetch, "[tid:%i] [sn:%llu] Sending instruction to decode "
                    "from fetch queue. Fetch queue size: %i.\n",
                    tid, inst->seqNum, fetchQueue[tid].size());

            wroteToTimeBuffer = true;
            fetchQueue[tid].pop_front();
            insts_to_decode++;
            available_insts--;
        }

        tid_itr++;
        // Wrap around if at end of active threads list
        if (tid_itr == activeThreads->end())
            tid_itr = activeThreads->begin();
    }

    // If there was activity this cycle, inform the CPU of it.
    if (wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();
    }

    // Reset the number of the instruction we've fetched.
    numInst = 0;
}

bool
Fetch::checkSignalsAndUpdate(ThreadID tid)
{
    if((fetchStatus[tid] == IcacheWaitResponse ||
        fetchStatus[tid] == IcacheAccessComplete) &&
       fetchBufferValid[tid].size()>0 &&
       fetchBufferValid[tid].front()) {
        fetchStatus[tid] = Running;
    }

    // Update the per thread stall statuses.
    if (fromDecode->decodeBlock[tid]) {
        stalls[tid].decode = true;
        trackLastBlock = true;
    }

    if (fromDecode->decodeUnblock[tid]) {
        assert(stalls[tid].decode);
        assert(!fromDecode->decodeBlock[tid]);
        stalls[tid].decode = false;
    }

    // Check squash signals from commit.
    if (fromCommit->commitInfo[tid].squash) {

        DPRINTF(Fetch, "[tid:%i] Squashing instructions due to squash "
                "from commit.\n",tid);
        // In any case, squash.
        squash(fromCommit->commitInfo[tid].pc,
               fromCommit->commitInfo[tid].doneSeqNum,
               fromCommit->commitInfo[tid].squashInst, tid);

        brseq[tid] = seq[tid];
        seq[tid]++;
        // If it was a branch mispredict on a control instruction, update the
        // branch predictor with that instruction, otherwise just kill the
        // invalid state we generated in after sequence number
        if (fromCommit->commitInfo[tid].mispredictInst &&
            fromCommit->commitInfo[tid].mispredictInst->isControl()) {
            bblAddr[tid] = fromCommit->commitInfo[tid].pc.instAddr();
            bblSize[tid] = 0;
            resteer = true;  
            profileMispredict(fromCommit->commitInfo[tid].mispredictInst, 
                              fromCommit->commitInfo[tid].branchTaken);

            TheISA::PCState ftPC;
            fromCommit->commitInfo[tid].mispredictInst->staticInst->advancePC(ftPC);
            uint64_t bblSz = (fromCommit->commitInfo[tid].mispredictInst->pcState().instAddr() - fromCommit->commitInfo[tid].bblAddr) / 4;
            DPRINTF(Fetch, "%s, %#x, %d\n", fromCommit->commitInfo[tid].mispredictInst->staticInst, ftPC.instAddr(), bblSz);

            branchPred->squash(//fromCommit->commitInfo[tid].doneSeqNum,
                              fromCommit->commitInfo[tid].doneBrSeqNum, 
                              fromCommit->commitInfo[tid].bblAddr,
                              fromCommit->commitInfo[tid].mispredictInst->staticInst,
                              fromCommit->commitInfo[tid].mispredictInst->pcState(),
                              bblSz,
                              fromCommit->commitInfo[tid].pc,
                              ftPC,
                              fromCommit->commitInfo[tid].branchTaken,
                              tid);
	    //if(fromCommit->commitInfo[tid].branchTaken){
	    //    DPRINTFN("Squash signal received from commit and branch is taken\n");
	    //    DPRINTFN("Inst is %s and target is %s SEQ is %llu\n", fromCommit->commitInfo[tid].mispredictInst->pcState(), fromCommit->commitInfo[tid].pc, fromCommit->commitInfo[tid].mispredictInst->seqNum);
	    //    if(fromCommit->commitInfo[tid].mispredictInst->readPredTarg().instAddr() == fromCommit->commitInfo[tid].pc.instAddr()){
	    //      DPRINTFN("FOUND EXCEPTION\n");
	    //    }
	    //}
	    if(fromCommit->commitInfo[tid].branchTaken && !fromCommit->commitInfo[tid].mispredictInst->isIndirectCtrl()){
                //branchPred->BTBUpdate(fromCommit->commitInfo[tid].bblAddr.front(),
                //DPRINTF(Trace, "BTB Update on Squash SEQ:%llu for branchPC: %#x targetPC: %#x\n",
                //        fromCommit->commitInfo[tid].mispredictInst->seqNum,
                //        fromCommit->commitInfo[tid].mispredictInst->pcState().instAddr(),
                //        fromCommit->commitInfo[tid].pc.instAddr());
                //bgodala BTBUpdate remove
                //branchPred->BTBUpdate(fromCommit->commitInfo[tid].mispredictInst->pcState().instAddr(),
                //                      fromCommit->commitInfo[tid].mispredictInst->staticInst,
                //                      fromCommit->commitInfo[tid].mispredictInst->pcState(),
                //                      fromCommit->commitInfo[tid].bblSize.front(),
                //                      fromCommit->commitInfo[tid].pc,
                //                      ftPC,
                //                      fromCommit->commitInfo[tid].mispredictInst->isUncondCtrl(),
                //                      tid);
	    }
            //if(fromCommit->commitInfo[tid].doneBrSeqNum<brseq[tid])
            //    brseq[tid] = fromCommit->commitInfo[tid].doneBrSeqNum;

        } else {
            if (fromCommit->commitInfo[tid].doneBrSeqNum>0) {
                branchPred->squash(//fromCommit->commitInfo[tid].doneSeqNum,
                                  fromCommit->commitInfo[tid].doneBrSeqNum ,
                                  tid);
            }
            //if(fromCommit->commitInfo[tid].doneBrSeqNum<brseq[tid])
            //    brseq[tid] = fromCommit->commitInfo[tid].doneBrSeqNum;
            if(fromCommit->commitInfo[tid].mispredictInst){
                DPRINTF(Bgodala,"Mispredict but on NON CONTROL instruction\n");
                bblAddr[tid] = fromCommit->commitInfo[tid].pc.instAddr();
                bblSize[tid] = 0;
            }else{
                DPRINTF(Bgodala,"Order violation from commit\n");
                DPRINTF(Bgodala,"bblAddr: %#x bblSize: %d\n",fromCommit->commitInfo[tid].bblAddr,fromCommit->commitInfo[tid].bblSize);
                bblAddr[tid] = 0; 
                bblSize[tid] = 0;
            }
        }

        return true;
    } else if (fromCommit->commitInfo[tid].doneSeqNum) {
        // Update the branch predictor if it wasn't a squashed instruction
        // that was broadcasted.
        while(fromCommit->commitInfo[tid].doneInst.size()>0) {
            DynInstPtr doneInst = fromCommit->commitInfo[tid].doneInst.front();
            //if (doneInst && doneInst->pcState().branching() && doneInst->readPredTarg().instAddr()!=doneInst->pcState().npc()) {
            //    TheISA::PCState ftPC;
            //    TheISA::advancePC(ftPC, doneInst->staticInst);
            //    DPRINTF(Fetch, "111\n");
            //    //branchPred->BTBUpdate(fromCommit->commitInfo[tid].bblAddr.front(),
            //    DPRINTF(Fetch, "BTB Update1 on done SEQ:%llu for branchPC: %#x targetPC: %#x\n",
            //            doneInst->seqNum, doneInst->pcState().instAddr(),doneInst->readPredTarg().instAddr());
            //    branchPred->BTBUpdate(doneInst->pcState().instAddr(),
            //                          doneInst->staticInst,
            //                          doneInst->pcState(),
            //                          fromCommit->commitInfo[tid].bblSize.front(),
            //                          doneInst->readPredTarg(),
            //                          ftPC,
            //                          doneInst->isUncondCtrl(),
            //                          tid);
            //} else if (doneInst && doneInst->isDirectCtrl() && doneInst->readPredTarg().instAddr()!=doneInst->pcState().npc()) {
            //    TheISA::PCState ftPC;
            //    TheISA::advancePC(ftPC, doneInst->staticInst);
            //    DPRINTF(Fetch, "222\n");
            //    //branchPred->BTBUpdate(fromCommit->commitInfo[tid].bblAddr.front(),
            //    DPRINTF(Fetch, "BTB Update2 on done SEQ:%llu for branchPC: %#x targetPC: %#x\n",
            //            doneInst->seqNum, doneInst->pcState().instAddr(),doneInst->readPredTarg().instAddr());
            //    branchPred->BTBUpdate(doneInst->pcState().instAddr(),
            //                          doneInst->staticInst,
            //                          doneInst->pcState(),
            //                          fromCommit->commitInfo[tid].bblSize.front(),
            //                          doneInst->branchTarget(),
            //                          ftPC,
            //                          doneInst->isUncondCtrl(),
            //                          tid);
            //}
            //fromCommit->commitInfo[tid].doneInst.pop_front();
            //fromCommit->commitInfo[tid].bblAddr.pop_front();
            //fromCommit->commitInfo[tid].bblSize.pop_front();
        }
        branchPred->update(//fromCommit->commitInfo[tid].doneSeqNum, tid);
                           fromCommit->commitInfo[tid].doneBrSeqNum, tid);
    }

    // Check squash signals from decode.
    if (fromDecode->decodeInfo[tid].squash) {
        DPRINTF(Fetch, "[tid:%i] Squashing instructions due to squash "
                "from decode.\n",tid);

        brseq[tid]= seq[tid];
        seq[tid]++;
        // Update the branch predictor.
        if (fromDecode->decodeInfo[tid].branchMispredict) {
            resteer = true;
            bblSize[tid] = 0;
            bblAddr[tid]=fromDecode->decodeInfo[tid].nextPC.instAddr();
            profileMispredict(fromDecode->decodeInfo[tid].mispredictInst,
                              fromDecode->decodeInfo[tid].branchTaken);

            TheISA::PCState ftPC;
            fromDecode->decodeInfo[tid].mispredictInst->staticInst->advancePC(ftPC);
            branchPred->squash(//fromDecode->decodeInfo[tid].doneSeqNum,
                              fromDecode->decodeInfo[tid].doneBrSeqNum, 
                              fromDecode->decodeInfo[tid].bblAddr,
                              fromDecode->decodeInfo[tid].mispredictInst->staticInst,
                              fromDecode->decodeInfo[tid].mispredictInst->pcState(),
                              1,
                              fromDecode->decodeInfo[tid].nextPC,
                              ftPC,
                              fromDecode->decodeInfo[tid].branchTaken,
                              tid);
            //branchPred->squash(fromDecode->decodeInfo[tid].doneBrSeqNum,
            //                  fromDecode->decodeInfo[tid].nextPC,
            //                  fromDecode->decodeInfo[tid].branchTaken,
            //                  tid);
            //bgodala BTBUpdate remove
	        //DPRINTF(Fetch, "Bgodala Updating BTB for inst %s\n",fromDecode->decodeInfo[tid].mispredictInst->pcState());
            //branchPred->BTBUpdate(fromDecode->decodeInfo[tid].mispredictInst->pcState().instAddr(),
            //                      fromDecode->decodeInfo[tid].mispredictInst->staticInst,
            //                      fromDecode->decodeInfo[tid].mispredictInst->pcState(),
            //                      fromDecode->decodeInfo[tid].bblSize,
            //                      fromDecode->decodeInfo[tid].nextPC,
            //                      ftPC,
            //                      fromDecode->decodeInfo[tid].mispredictInst->isUncondCtrl(),
			//	  tid);
        } else {
            bblAddr[tid] = 0;
            DPRINTF(Bgodala,"BGODALA No idea when this will happen\n");
            branchPred->squash(fromDecode->decodeInfo[tid].doneBrSeqNum,
                              tid);
        }
            //if(fromDecode->decodeInfo[tid].doneBrSeqNum<brseq[tid])
            //    brseq[tid] = fromDecode->decodeInfo[tid].doneBrSeqNum;

        if (fetchStatus[tid] != Squashing) {

            DPRINTF(Fetch, "Squashing from decode with PC = %s\n",
                fromDecode->decodeInfo[tid].nextPC);
            // Squash unless we're already squashing
            squashFromDecode(fromDecode->decodeInfo[tid].nextPC,
                             fromDecode->decodeInfo[tid].squashInst,
                             fromDecode->decodeInfo[tid].doneSeqNum,
                             tid);

            bblAddr[tid]=fromDecode->decodeInfo[tid].nextPC.instAddr();
            bblSize[tid]=0;
            return true;
        }
    }

    if (checkStall(tid) &&
        fetchStatus[tid] != IcacheWaitResponse &&
        memReq[tid].size()==0 && 
        fetchStatus[tid] != IcacheWaitRetry &&
        fetchStatus[tid] != ItlbWait &&
        fetchStatus[tid] != QuiescePending) {
        DPRINTF(Fetch, "[tid:%i] Setting to blocked\n",tid);

        fetchStatus[tid] = Blocked;

        return true;
    }

    if (fetchStatus[tid] == Blocked ||
        fetchStatus[tid] == Squashing) {
        // Switch status to running if fetch isn't being told to block or
        // squash this cycle.
        DPRINTF(Fetch, "[tid:%i] Done squashing, switching to running.\n",
                tid);

        fetchStatus[tid] = Running;

        return true;
    }

    // If we've reached this point, we have not gotten any signals that
    // cause fetch to change its status.  Fetch remains the same as before.
    return false;
}

DynInstPtr
Fetch::buildInst(ThreadID tid, StaticInstPtr staticInst,
        StaticInstPtr curMacroop, TheISA::PCState thisPC,
        TheISA::PCState nextPC, bool trace)
{
    // Get a sequence number.
    InstSeqNum seq = cpu->getAndIncrementInstSeq();

    // Create a new DynInst from the instruction fetched.
    DynInstPtr instruction =
        new DynInst(staticInst, curMacroop, thisPC, nextPC, seq, cpu);
    instruction->setTid(tid);

    instruction->setThreadState(cpu->thread[tid]);

    DPRINTF(Fetch, "[tid:%i] Instruction PC %#x (%d) created "
            "[sn:%lli].\n", tid, thisPC.instAddr(),
            thisPC.microPC(), seq);

    DPRINTF(Fetch, "[tid:%i] Instruction is: %s\n", tid,
            instruction->staticInst->
            disassemble(thisPC.instAddr()));

#if TRACING_ON
    if (trace) {
        instruction->traceData =
            cpu->getTracer()->getInstRecord(curTick(), cpu->tcBase(tid),
                    instruction->staticInst, thisPC, curMacroop);
    }
#else
    instruction->traceData = NULL;
#endif

    // Add instruction to the CPU's list of instructions.
    instruction->setInstListIt(cpu->addInst(instruction));

    // Write the instruction to the first slot in the queue
    // that heads to decode.
    assert(numInst < fetchWidth);
    fetchQueue[tid].push_back(instruction);
    assert(fetchQueue[tid].size() <= fetchQueueSize);
    DPRINTF(Fetch, "[tid:%i] Fetch queue entry created (%i/%i).\n",
            tid, fetchQueue[tid].size(), fetchQueueSize);
    //toDecode->insts[toDecode->size++] = instruction;

    // Keep track of if we can take an interrupt at this boundary
    delayedCommit[tid] = instruction->isDelayedCommit();

    return instruction;
}

TheISA::PCState
Fetch::predictNextBasicBlock(TheISA::PCState prefetchPc, TheISA::PCState &branchPC, ThreadID tid, bool &stopPrefetch)
{
    TheISA::PCState predictPC = prefetchPc;
    if (!branchPred->getBblValid(prefetchPc.instAddr(), tid))
        return 0;
    //if((prefetchPc.instAddr() & 0xff000000) != 0){
    //    return 0;
    //}
    //int btb_idx = branchPred->getBblIndex(prefetchPc.instAddr(), tid);
    //if (btb_idx < 0){
    //    return 0;
    //}

        DPRINTF(Bgodala, "Bgodala predictNextBasicBlock prefetchPC 0x%lx\n",prefetchPc.instAddr());
        // TODO: Multiple BBLs
        StaticInstPtr staticBranchInst = branchPred->getBranch(prefetchPc.instAddr(), tid);
        branchPC = branchPred->getBranchPC(prefetchPc.instAddr(), tid);
        
        if(branchPC.instAddr() < prefetchPc.instAddr()){
            DPRINTF(Fetch, "Fix this case later\n");
            return 0;
        }

        TheISA::PCState nextPC=branchPC;
        DPRINTF(Fetch,"nextPC instAddr is %#x\n",nextPC.instAddr());
        nextPC.npc(nextPC.pc() + nextPC.size());
        //TheISA::advancePC(nextPC, staticBranchInst);
        if(nextPC.npc()%4 != 0){
            DPRINTF(Fetch, "FOUND THUMB\n");
        }

	    //DPRINTF(Fetch, "branchPC.pc: %#x branchPC.npc: %#x comp: %d \n", branchPC.pc(),  branchPC.npc() ,  (branchPC.pc() + 4) ==  branchPC.npc());
	    //if((branchPC.npc() == (branchPC.pc() + 4 )) || (branchPC.npc() == (branchPC.pc() + 2))){
	    //	nextPC = branchPC;
	    //}

        DPRINTF(Fetch,"TESTING branchPC: %s nextPC: %s\n", branchPC, nextPC);
	    DPRINTF(Bgodala, "Bgodala Staticinst 0x%lx and BranchPC %s\n",staticBranchInst, branchPC);
        bool predict_taken = branchPred->predict(staticBranchInst, seq[tid],
                                      prefetchPc.instAddr(), nextPC, tid);

        if(nextPC.instAddr() < 0x1000){
            nextPC = branchPC;
            staticBranchInst->advancePC(nextPC);
            DPRINTF(Fetch,"Hack: Next pc is %#x\n",nextPC.instAddr());

            //assert(false && "next pc is < 0x1000\n");
        }

        DPRINTF(Bgodala, "prefetchPc: 0x%lx branchPC: 0x%lx nextPC: 0x%lx\n", prefetchPc.instAddr(), branchPC.instAddr(), nextPC.instAddr());
        if (predict_taken) {
            DPRINTF(Fetch, "[tid:%i] [sn:%llu] Branch at PC %#x "
                    "predicted to be taken to %s\n",
                    tid, seq[tid], branchPC.instAddr(), nextPC);
        } else {
            DPRINTF(Fetch, "[tid:%i] [sn:%llu] Branch at PC %#x "
                    "predicted to be not taken\n",
                    tid, seq[tid], branchPC.instAddr());
        }

       Addr branch = branchPC.instAddr();
       //TheISA::PCState predictWPPC = branchPred->getTaken(prefetchPc.instAddr(), tid);
       TheISA::PCState predictWPPC = branchPred->getTaken(branchPC.instAddr(), tid);
       if (predict_taken) {
           predictPC = predictWPPC;
           staticBranchInst->advancePC(predictWPPC);
       } else {
           predictPC = branchPC;
           staticBranchInst->advancePC(predictPC);
           DPRINTF(Fetch, "BGODALA VERIFY THIS!! AdvancePC: %#x\n",  predictPC.instAddr());
       }
       predictPC = nextPC;

       DPRINTF(Fetch, "Prefetch predict: %#x, %s, %#x, %#x\n", prefetchPc.instAddr(), staticBranchInst, branch, predictPC.instAddr());
       if(staticBranchInst->isUncondCtrl() && !predict_taken){
           DPRINTF(Fetch, "Stop prefetching on unconditional not taken branch\n");
           stopPrefetch = true;
       }
       auto& brConf = cpu->brConfMap[branch];
       uint64_t &total = std::get<0>(brConf);
       uint64_t &misPred = std::get<1>(brConf);

       if(total > 100 && misPred/total > 0.1){
           stopPrefetch = true;
       }

    return predictPC;
}

//Pre decode last line that is pre-fetched
void
Fetch::preDecode(){
    ThreadID tid = 0;
    TheISA::PCState bblPC = lastPrefPC;

    preDecoder[tid]->reset();

    // Avoid pre-decoding the same line again
    //if ( lastProcessedLine == lastAddrFetched ){
    //    return;
    //}

    //if (branchPred->getBblValid(thisPC.instAddr(), tid))
    //    return; 
    //Check if the buffer has valid last entry
    //If the line is same as the prefPC line then predcode it and update
    //BTB

    if(lastPrefPC == 0){
      return;
    }

    assert(fetchBufferValid[tid].size() == fetchBufferPC[tid].size() && "Check fetchBufferValid and fetchBufferPC sizes");
    assert(fetchBufferPC[tid].size() == fetchBuffer[tid].size() && "Check fetchBufferPC and fetchBuffer sizes");

    //if (fetchBufferValid[tid].size()>0 && fetchBufferValid[tid].back() && fetchBufferBlockPC == fetchBufferPC[tid].back()){
    if (fetchBufferValid[tid].size()>0 && fetchBufferValid[tid].back() && fetchBufferPC[tid].back() == prefetchBufferPC[tid].back()){

        TheISA::PCState thisPC = lastPrefPC;

        if( lastPrefPC.instAddr() <= fetchBufferPC[tid].back()){
            thisPC.pc(fetchBufferPC[tid].back());
            thisPC.npc(fetchBufferPC[tid].back() + 4);
        }

        thisPC.upc(0);
        thisPC.nupc(1);

        Addr fetchAddr = thisPC.instAddr() & decoder[tid]->pcMask();
        Addr fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);
        lastProcessedLine = fetchBufferPC[tid].back();
        //bool inRom = isRomMicroPC(thisPC.microPC());
        bool inRom = false;
        StaticInstPtr curMacroop = NULL; 
        StaticInstPtr staticInst = NULL; 
        TheISA::PCState nextPC = thisPC;;
        int pcOffset = 0;

        unsigned blkOffset = (fetchAddr - fetchBufferBlockPC) / instSize;
        Addr lastAddr = fetchBufferBlockPC + CACHE_LINE_SIZE;
        auto *dec_ptr = preDecoder[tid];

        DPRINTF(Fetch, "fetchBufferValid Size: %d fetchBufferPC Size: %d fetchBuffer Size: %d\n",fetchBufferValid[tid].size(),fetchBufferPC[tid].size(),fetchBuffer[tid].size());

        //bufIt buf_it = fetchBuffer[tid].begin();
        //pcIt pc_it = fetchBufferPC[tid].begin();
        //validIt valid_it = fetchBufferValid[tid].begin();
 
        //while(pc_it != fetchBufferPC[tid].end()){
        //    if(*valid_it){
        //        for(int i=0; i<16; i++){
        //            unsigned dec_data = 0;
        //            memcpy(&dec_data,
        //                    *buf_it + i * instSize, instSize);
        //            DPRINTF(Fetch, "fetchAddr: %#x data: %#x\n",*pc_it + i*4, dec_data);
        //        }
        //        DPRINTF(Fetch,"=================================================\n");
        //    }
        //    buf_it++;
        //    pc_it++;
        //    valid_it++;
        //}

        while(fetchAddr < lastAddr){
            unsigned dec_data = 0;
            DPRINTF(Fetch, "predecoder: fetchAddr is %#x fetchBufferBlockPC: %#x lastPrefPC[0]: %#x blkOffset: %d\n", fetchAddr, fetchBufferBlockPC, lastPrefPC.instAddr(), blkOffset);
            memcpy(&dec_data,
                    fetchBuffer[tid].back() + blkOffset * instSize, instSize);
            DPRINTF(Fetch, "predecoder: fetchAddr is %#x fetchBufferBlockPC: %#x lastPrefPC[0]: %#x blkOffset: %d Data: %#x\n", fetchAddr, fetchBufferBlockPC, lastPrefPC.instAddr(), blkOffset, dec_data);
            bool needMem = !inRom && !curMacroop && !dec_ptr->instReady();
            fetchAddr = (thisPC.instAddr() + pcOffset) & dec_ptr->pcMask();

            if(needMem){
                memcpy(dec_ptr->moreBytesPtr(),
                        fetchBuffer[tid].back() + blkOffset * instSize, instSize);
                preDecoder[tid]->moreBytes(thisPC, fetchAddr);

                if (dec_ptr->needMoreBytes()) {
                    blkOffset++;
                    fetchAddr += instSize;
                    pcOffset += instSize;
                }
            }
        
            do {
                DPRINTF(Fetch,"PREDECODER: thisPC: %s\n",thisPC);
                if (!(curMacroop || inRom)) {
                    if (dec_ptr->instReady()) {
                        staticInst = dec_ptr->decode(thisPC);

                        if (staticInst->isMacroop()) {
                            curMacroop = staticInst;
                        } else {
                        }
                    } else {
                        // We need more bytes for this instruction so blkOffset and
                        // pcOffset will be updated
                        break;
                    }
                }
                // Whether we're moving to a new macroop because we're at the
                // end of the current one, or the branch predictor incorrectly
                // thinks we are...
                bool newMacro = false;
                if (curMacroop || inRom) {
                    if (inRom) {
                        staticInst = dec_ptr->fetchRomMicroop(
                                thisPC.microPC(), curMacroop);
                    } else {
                        staticInst = curMacroop->fetchMicroop(thisPC.microPC());
                    }
                    newMacro |= staticInst->isLastMicroop();
                }
                nextPC = thisPC;
                staticInst->advancePC(nextPC);
                newMacro |= thisPC.instAddr() != nextPC.instAddr();
                inRom = isRomMicroPC(thisPC.microPC());

                if (staticInst->isDirectCtrl()) {
                    DPRINTF(Bgodala, "PREDECODE BBLInsert Inserting Direct ctrl bblAddr[tid]: %#x instAddr: %#x branchTarget: %#x bblSize: %d\n",
                            bblPC.instAddr(), thisPC.instAddr(), staticInst->branchTarget(thisPC), thisPC.instAddr() - bblPC.instAddr());
                    branchPred->BTBUpdate(bblPC.instAddr(),
                                          staticInst,
                                          thisPC,
                                          thisPC.instAddr() - bblPC.instAddr(),
                                          staticInst->branchTarget(thisPC),
                                          nextPC,
                                          staticInst->isUncondCtrl(),
                                          tid);
                    assert(thisPC.instAddr() >= bblPC.instAddr()  && "bblSize must be greater than 0");
                    bblPC = nextPC;
                } else if(staticInst->isControl()){
                    TheISA::PCState dummyBranchTarget = nextPC;
                    //dummyBranchTarget.pc(-1);
                    //dummyBranchTarget.npc(-1);

                    DPRINTF(Bgodala, "PREDECODE BBLInsert Inserting Indirect ctrl bblAddr[tid]: %#x instAddr: %#x branchTarget: %#x bblSize: %d\n",
                            bblPC.instAddr(), thisPC.instAddr(), dummyBranchTarget, thisPC.instAddr() - bblPC.instAddr());
                    branchPred->BTBUpdate(bblPC.instAddr(),
                                          staticInst,
                                          thisPC,
                                          thisPC.instAddr() - bblPC.instAddr(),
                                          dummyBranchTarget,
                                          nextPC,
                                          staticInst->isUncondCtrl(),
                                          tid);
                    assert(thisPC.instAddr() >= bblPC.instAddr()  && "bblSize must be greater than 0");
                    bblPC = nextPC;
                }

                //Move to next instruction
                thisPC = nextPC;
                if (newMacro) {
                    fetchAddr = thisPC.instAddr() & dec_ptr->pcMask(); 
                    //blkOffset = (fetchAddr - fetchBufferPC[tid]) / instSize;
                    blkOffset = (fetchAddr - fetchBufferPC[tid].back()) / instSize;
                    curMacroop = NULL;
                    pcOffset = 0;
                }


            } while(curMacroop || dec_ptr->instReady());
        }

    }
}

void
Fetch::addToFTQ()
{
    /////////////////////////////////////////////////
    // Add Prefetch entries to Fetch Target Queue
    /////////////////////////////////////////////////
    //ThreadID tid = getFetchingThread();

    assert(!cpu->switchedOut());

    ThreadID tid = 0;

    // Do not prefetch when status is TrapPending
    if ( fetchStatus[tid] == Squashing ||
         fetchStatus[tid] == TrapPending ||
         fetchStatus[tid] == QuiescePending) {
        issuePipelinedIfetch[tid] = false;
        return;
    }


    if (prefPC[tid] == 0){
        return;
    }
    preDecode();
    // The current Prefetch PC.
    TheISA::PCState thisPC = prefPC[tid];
    TheISA::PCState nextPC = thisPC;
    TheISA::PCState branchPC = thisPC;

    DPRINTF(Fetch, "Attempting to prefetch from [tid:%i] %i %#x\n", tid, prefetchQueue[tid].size(), thisPC);

    // Keep issuing while prefetchQueue is available
    while ( prefetchQueue[tid].size() < ftqSize ) {
        bool stopPrefetch = false;
        nextPC = predictNextBasicBlock(thisPC, branchPC, tid, stopPrefetch);

        if (nextPC.instAddr()>0x10) {
            TheISA::PCState prevPrefPC = prefPC[tid];;
            lastPrefPC = prefPC[tid];
            prefPC[tid] = nextPC;
            // Add to the prefetch queue
            prefetchQueue[tid].push_back(nextPC);
            prefetchQueueBblSize[tid].push_back(branchPC.instAddr() - thisPC.instAddr());
            if (branchPC.instAddr() < thisPC.instAddr()){
                DPRINTF(Fetch, "should not happen branchPC: %s thisPC: %s\n", branchPC, thisPC);
                assert(false && "Should not happen\n");
            }
            //prefetchQueueBblSize[tid].push_back(branchPred->getBblSize(thisPC.instAddr(), tid));
	        int tempBblSize = branchPred->getBblSize(thisPC.instAddr(), tid);
            if(tempBblSize != (branchPC.instAddr() - thisPC.instAddr())){
	            DPRINTF(Bgodala, "BGODALA bblSize Mismatch\n");
	            DPRINTF(Bgodala, "bblSize:%d diff is %d\n",tempBblSize, (branchPC.instAddr() - thisPC.instAddr()));
	            DPRINTF(Bgodala, "thisPC: %s and branchPC: %s", thisPC, branchPC);
                prefPC[tid] = 0;
                return;
                //assert(false && "Check BTB parameters\n");
	        }
            if(prefetchQueue[tid].size()==1) {
                prevPC[tid] = thisPC;
            } 
            prefetchQueueSeqNum[tid].push_back(seq[tid]);
            prefetchQueueBr[tid].push_back(branchPC);
            seq[tid]++;
            DPRINTF(Fetch, "[tid:%i] Prefetch queue entry created (%i/%i) %s %s.\n",
                    tid, prefetchQueue[tid].size(), prefetchQueueSize, prefetchQueue[tid].front(), nextPC);

            Addr curPCLine = (thisPC.instAddr() >> CACHE_LISZE_SIZE_WIDTH) << CACHE_LISZE_SIZE_WIDTH;
            Addr branchPCLine = (branchPC.instAddr() >> CACHE_LISZE_SIZE_WIDTH) << CACHE_LISZE_SIZE_WIDTH;

            curPCLine  &= decoder[tid]->pcMask();
            branchPCLine &= decoder[tid]->pcMask();

            //Do not add a line to prefetchBufferPC if the size does not match
            DPRINTF(Fetch, "prevPrefPC %#x and fallThroughPrefPC %#x\n",prevPrefPC.instAddr(), fallThroughPrefPC);
            DPRINTF(Fetch, "lastProcessedLine %#x and lastAddrFetched %#x\n",lastProcessedLine, lastAddrFetched);
            
            //When lastProcessedLine is different from lastAddrFetched
            if ( lastProcessedLine != lastAddrFetched  ){
                DPRINTF(Fetch, "setting lastProcessedLine to 0\n");
                lastProcessedLine = 0;
            }

            if ( prevPrefPC.instAddr() != fallThroughPrefPC) {
                if(tempBblSize == (branchPC.instAddr() - thisPC.instAddr())){
                    do{
                        if(lastAddrFetched != curPCLine){
                            if(!prefetchBufferPC[tid].empty() && curPCLine != prefetchBufferPC[tid].back()){
                                DPRINTF(Fetch, "Pushing curPCLine:%#x and branchPCLine:%#x\n",curPCLine, branchPCLine);
                                prefetchBufferPC[tid].push_back(curPCLine);
                                lastAddrFetched = curPCLine;
                            }else if(prefetchBufferPC[tid].empty()){
                                DPRINTF(Fetch, "EMPTY Pushing curPCLine:%#x and branchPCLine:%#x\n",curPCLine, branchPCLine);
                                prefetchBufferPC[tid].push_back(curPCLine);
                                lastAddrFetched = curPCLine;
                            }
                        }
                        curPCLine += CACHE_LINE_SIZE;
                        //if(curPCLine > branchPCLine){
                        //  break;
                        //}
                    }while(curPCLine <= branchPCLine);
                }
            }
            
            // OOO fetch
            Addr fetchAddr = nextPC.instAddr() & decoder[tid]->pcMask();
            Addr fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);

            pcIt pc_it = fetchBufferPC[tid].begin();
            while (pc_it != fetchBufferPC[tid].end() &&
                   (*pc_it)!=fetchBufferBlockPC) {
                ++pc_it;
            }

            issuePipelinedIfetch[tid] = pc_it == fetchBufferPC[tid].end() &&
                                       fetchStatus[tid] != IcacheWaitResponse &&
                                       fetchStatus[tid] != ItlbWait &&
                                       fetchStatus[tid] != IcacheWaitRetry &&
                                       fetchStatus[tid] != QuiescePending;
            fallThroughPrefPC = 0;
            if( branchPC.instAddr() + 4 != nextPC.instAddr()){
                DPRINTF(Fetch, "Taken branch found in addToFTQ\n");
                //FIXME: Special case if taken branch is in the same line then pre-decode it again next time

                //if (lastProcessedLine == lastAddrFetched && fetchBufferBlockPC == lastProcessedLine && 
                //        branchPCLine == fetchBufferBlockPC){
                if (lastAddrFetched == fetchBufferBlockPC){
                    DPRINTF(Fetch, "When branch target is within the same line\n");
                    lastPrefPC = prefPC[tid];

                    //Process a line again only if it is already processed
                    //if the line is not yet fetched then do not change lastProcessedLine
                    if(lastProcessedLine == lastAddrFetched){
                        lastProcessedLine = 0;
                    }
                    //fallThroughPrefPC = prefPC[tid].instAddr();
                }
                break;
            }
            if ( fetchBufferBlockPC == lastAddrFetched){
                DPRINTF(Fetch, "Branch Not taken\n");
                DPRINTF(Fetch, "target line same as lastline fetched\n");
                lastPrefPC = prefPC[tid];
                ////Process a line again only if it is already processed
                ////if the line is not yet fetched then do not change lastProcessedLine
                //if(lastProcessedLine == lastAddrFetched){
                //    lastProcessedLine = 0;
                //}
            }
            thisPC = nextPC;
            branchPC = thisPC;
            lastProcessedLine = 0;
            if(stopPrefetch){
                prefPC[tid] = 0;
                return;
            }
        } else{
        
            //FIXME: prefPC line here and set lastAddrFetched
            DPRINTF(Fetch, "lastProcessedLine %#x and lastAddrFetched %#x\n",lastProcessedLine, lastAddrFetched);
            
            Addr fetchAddr = prefPC[tid].instAddr() & decoder[tid]->pcMask();
            Addr fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);
            //if ( lastProcessedLine !=0 && lastProcessedLine != lastAddrFetched && lastAddrFetched == fetchBufferBlockPC){
            //    DPRINTF(Fetch, "Get Falthrough next time\n");
            //    fallThroughPrefPC = prefPC[tid].instAddr();
            //}

            if(lastProcessedLine == 0 && prefetchBufferPC[tid].empty()){
                lastProcessedLine = lastAddrFetched;
                DPRINTF(Fetch, "PREF BUF EMPTY for prefPC:%#x\n",prefPC[tid].instAddr());
                DPRINTF(Fetch, "Fixing lastProcessedLine %#x and lastAddrFetched %#x\n",lastProcessedLine, lastAddrFetched);
            }

            if ((lastProcessedLine !=0 && lastProcessedLine == lastAddrFetched)){
                DPRINTF(Fetch, "Last line\n");
                // if flag is set then use lastAddrFetched else prefPC
                Addr curPCLine = 0; 
                if(fallThroughPrefPC == prefPC[tid].instAddr()){
                    curPCLine = (lastAddrFetched>> CACHE_LISZE_SIZE_WIDTH) << CACHE_LISZE_SIZE_WIDTH;
                    curPCLine += CACHE_LINE_SIZE;
                }else{
                    fallThroughPrefPC = prefPC[tid].instAddr();
                    curPCLine = (prefPC[tid].instAddr() >> CACHE_LISZE_SIZE_WIDTH) << CACHE_LISZE_SIZE_WIDTH;
                    if(prefetchBufferPC[tid].empty() && curPCLine == lastAddrFetched && fetchBufferBlockPC == lastAddrFetched){
                        curPCLine += CACHE_LINE_SIZE;
                        DPRINTF(Fetch, "Go to next line if first line is already fetched and prefetchBuffer is empty\n");
                    }
                    //if(lastAddrFetched == prefPC[tid].instAddr()){
                    //    curPCLine += CACHE_LINE_SIZE;
                    //    DPRINTF(Fetch, "Already fetched prefPC %#x so fetching next line %#x\n",prefPC[tid].instAddr(),curPCLine);
                    //}
                    lastPrefPC = prefPC[tid];
                    
                    //pre-decode it in case the target of branch lies in the same line
                    //lastProcessedLine = 0;
                }
                Addr branchPCLine = curPCLine; 
                //Addr curPCLine = (thisPC.instAddr() >> CACHE_LISZE_SIZE_WIDTH) << CACHE_LISZE_SIZE_WIDTH;
                //Addr branchPCLine = curPCLine; 

                curPCLine  &= decoder[tid]->pcMask();
                branchPCLine &= decoder[tid]->pcMask();

                if(lastAddrFetched != curPCLine){
                    if(!prefetchBufferPC[tid].empty() && curPCLine != prefetchBufferPC[tid].back()){
                        DPRINTF(Fetch, "Pushing curPCLine:%#x and branchPCLine:%#x\n",curPCLine, branchPCLine);
                        prefetchBufferPC[tid].push_back(curPCLine);
                        lastAddrFetched = curPCLine;
                    }else if(prefetchBufferPC[tid].empty()){
                        DPRINTF(Fetch, "EMPTY Pushing curPCLine:%#x and branchPCLine:%#x\n",curPCLine, branchPCLine);
                        prefetchBufferPC[tid].push_back(curPCLine);
                        lastAddrFetched = curPCLine;
                    }
                }
                if(prefetchBufferPC[tid].empty()){
                    DPRINTFN("Corner case found %#x\n", lastAddrFetched);
                    assert(false && "Corner case found\n");
                }
            }
            break;
        }
    }

    // If prefetch buffer is empty then fetch head of the PC and memReq queue is empty
    //if (prefetchBufferPC[tid].empty() && prefetchQueue[tid].empty()){
    //    DPRINTF(Fetch,"addToFTQ pc[tid] is %#x\n", pc[tid].instAddr());
    //    //DPRINTFN("addToFTQ pc[tid] is %#x\n", pc[tid].instAddr());
    //    TheISA::PCState thisPC = pc[tid];
    //    Addr curPCLine = (thisPC.instAddr() >> CACHE_LISZE_SIZE_WIDTH) << CACHE_LISZE_SIZE_WIDTH;
    //    prefetchBufferPC[tid].push_back(curPCLine);
    //    lastAddrFetched = curPCLine;
    //    //lastPrefPC = thisPC;
    //    lastPrefPC = prefPC[tid];
    //    lastProcessedLine = 0;
    //}
}

void
Fetch::fetch(bool &status_change)
{
    //////////////////////////////////////////
    // Start actual fetch
    //////////////////////////////////////////
    ThreadID tid = getFetchingThread();

    assert(!cpu->switchedOut());

    if (tid == InvalidThreadID) {
        // Breaks looping condition in tick()
        threadFetched = numFetchingThreads;

        if (numThreads == 1) {  // @todo Per-thread stats
            profileStall(0);
        }

        return;
    }

    DPRINTF(Fetch, "Attempting to fetch from [tid:%i]\n", tid);
    DPRINTF(Fetch, "Status is fetchStatus[tid]: %d\n",fetchStatus[tid]);

    // The current PC.
    TheISA::PCState thisPC = pc[tid];

    Addr pcOffset = fetchOffset[tid];
    Addr fetchAddr = (thisPC.instAddr() + pcOffset) & decoder[tid]->pcMask();
    // Align the fetch PC so its at the start of a fetch buffer segment.

    //assert(fetchAddr == thisPC.instAddr() && "fetchAddr and thisPC are not same!");
    Addr fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);

    bool inRom = isRomMicroPC(thisPC.microPC());

    // If returning from the delay of a cache miss, then update the status
    // to running, otherwise do the cache access.  Possibly move this up
    // to tick() function.
    if (fetchStatus[tid] == IcacheAccessComplete) {
        DPRINTF(Fetch, "[tid:%i] Icache miss is complete.\n", tid);

        fetchStatus[tid] = Running;
        status_change = true;
        if(fetchBufferValid[tid].size() > 0 && !fetchBufferValid[tid].front()){
            return;
        }
    } else if (fetchStatus[tid] == Running) {
        // Align the fetch PC so its at the start of a fetch buffer segment.
        //Addr fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);

        // If buffer is no longer valid or fetchAddr has moved to point
        // to the next cache block, AND we have no remaining ucode
        // from a macro-op, then start fetch from icache.
        //if (!(fetchBufferValid[tid] &&
        //            fetchBufferBlockPC == fetchBufferPC[tid]) && !inRom &&
        //        !macroop[tid]) {
        if (!(fetchBufferValid[tid].size()>0 && fetchBufferValid[tid].front() && fetchBufferBlockPC == fetchBufferPC[tid].front())
            ) {

            //if(!macroop[tid])
            //    DPRINTF(Fetch, "bgodala: LOOK HERE for this case!!\n");
            
            assert(macroop[tid] == NULL && "Do not fetch new line when macroop is not null\n");
            assert(!inRom && "Do not fetch new line when inRom\n");

            if(fetchBufferValid[tid].size()>0 && fetchBufferBlockPC != fetchBufferPC[tid].front()){
                warn("This case should not happend fetchBufferBlockPC:%#x fetchBufferPC[tid]:%#x curTick:%llu\n", fetchBufferBlockPC, fetchBufferPC[tid].front(), curTick());
            }
            DPRINTF(Fetch, "[tid:%i] Attempting to translate and read "
                    "instruction, starting at PC %s fetchAddr: %#x fetchBufferBlockPC: %#x pcOffset: %d\n", tid, thisPC, fetchAddr, fetchBufferBlockPC, pcOffset);
            if (fetchBufferValid[tid].empty() || fetchBufferPC[tid].front()!=fetchBufferBlockPC || memReq[tid].empty()) {
                if(!enableFDIP){
                    add_front = true;
                    fetchCacheLine(fetchAddr, tid, thisPC.instAddr());
                }

                if (fetchStatus[tid] == IcacheWaitResponse)
                    ++fetchStats.icacheStallCycles;
                else if (fetchStatus[tid] == ItlbWait)
                    ++fetchStats.tlbCycles;
                else
                    ++fetchStats.miscStallCycles;
            }
            return;
        } else if (checkInterrupt(thisPC.instAddr()) && !delayedCommit[tid]) {
            // Stall CPU if an interrupt is posted and we're not issuing
            // an delayed commit micro-op currently (delayed commit
            // instructions are not interruptable by interrupts, only faults)
            ++fetchStats.miscStallCycles;
            DPRINTF(Fetch, "[tid:%i] Fetch is stalled!\n", tid);
            return;
        }
    } else if(fetchStatus[tid] == TrapPending || fetchStatus[tid] == QuiescePending){
        return;
    } else if (fetchBufferValid[tid].size()>0 && fetchBufferValid[tid].front() && fetchBufferBlockPC == fetchBufferPC[tid].front()) {
        DPRINTF(Fetch, "[tid:%i] Nayana added.\n", tid);

        fetchStatus[tid] = Running;
        status_change = true;
    } else {
        if (fetchStatus[tid] == Idle) {
            ++fetchStats.idleCycles;
            DPRINTF(Fetch, "[tid:%i] Fetch is idle!\n", tid);
        }

        // Status is Idle, so fetch should do nothing.
        return;
    }

    ++fetchStats.cycles;

    if(!(fetchBufferPC[tid].size() > 0)){
        return;
    }
    TheISA::PCState nextPC = thisPC;

    StaticInstPtr staticInst = NULL;
    StaticInstPtr curMacroop = macroop[tid];

    // If the read of the first instruction was successful, then grab the
    // instructions from the rest of the cache line and put them into the
    // queue heading to decode.

    DPRINTF(Fetch, "[tid:%i] Adding instructions to queue to "
            "decode.\n", tid);

    // Need to keep track of whether or not a predicted branch
    // ended this fetch block.
    bool predictedBranch = false;

    // Need to halt fetch if quiesce instruction detected
    bool quiesce = false;

    //TheISA::MachInst *cacheInsts =
    //    //reinterpret_cast<TheISA::MachInst *>(fetchBuffer[tid]);
    //    reinterpret_cast<TheISA::MachInst *>(fetchBuffer[tid].front());

    uint8_t* fetchBufferHead = fetchBuffer[tid].front();

    const unsigned numInsts = fetchBufferSize / instSize;
    //unsigned blkOffset = (fetchAddr - fetchBufferPC[tid]) / instSize;
    DPRINTF(Fetch, "fetchBufferPC size: %d, fetchBufferValid size: %d, fetchBuffer size: %d\n",
            fetchBufferPC[tid].size(), fetchBufferValid[tid].size(),fetchBuffer[tid].size());
    assert(fetchBufferPC[tid].size() > 0 && "fetchBufferPC[tid] size must be > 0\n");
    unsigned blkOffset = (fetchAddr - fetchBufferPC[tid].front()) / instSize;
    

    // cms11 oracle
    bool repl_first_inst = true;

    auto *dec_ptr = decoder[tid];
    const Addr pc_mask = dec_ptr->pcMask();

    // Loop through instruction memory from the cache.
    // Keep issuing while fetchWidth is available and branch is not
    // predicted taken
    while (numInst < fetchWidth && fetchQueue[tid].size() < fetchQueueSize
           && !predictedBranch && !quiesce) {
        // We need to process more memory if we aren't going to get a
        // StaticInst from the rom, the current macroop, or what's already
        // in the decoder.
        bool needMem = !inRom && !curMacroop && !dec_ptr->instReady();
        fetchAddr = (thisPC.instAddr() + pcOffset) & pc_mask;
        Addr fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);

        if (needMem) {
            // If buffer is no longer valid or fetchAddr has moved to point
            // to the next cache block then start fetch from icache.
            //if (!fetchBufferValid[tid] ||

            //if (!fetchBufferValid[tid].front() ||
            //    //fetchBufferBlockPC != fetchBufferPC[tid])
            //    fetchBufferBlockPC != fetchBufferPC[tid].front())
            //    break;

            if (blkOffset >= numInsts) {
                // We need to process more memory, but we've run out of the
                // current block.
                pcOffset = 0;
                break;
            }

            memcpy(dec_ptr->moreBytesPtr(),
                    fetchBufferHead + blkOffset * instSize, instSize);
            decoder[tid]->moreBytes(thisPC, fetchAddr);

            if (dec_ptr->needMoreBytes()) {
                blkOffset++;
                fetchAddr += instSize;
                pcOffset += instSize;
            }
        }

        // Extract as many instructions and/or microops as we can from
        // the memory we've processed so far.
        do {
            if (!(curMacroop || inRom)) {
                if (dec_ptr->instReady()) {
                    staticInst = dec_ptr->decode(thisPC);

                    // Increment stat of fetched instructions.
                    ++fetchStats.insts;

                    if (staticInst->isMacroop()) {
                        curMacroop = staticInst;
                    } else {
                        pcOffset = 0;
                    }
                } else {
                    // We need more bytes for this instruction so blkOffset and
                    // pcOffset will be updated
                    break;
                }
            }
            // Whether we're moving to a new macroop because we're at the
            // end of the current one, or the branch predictor incorrectly
            // thinks we are...
            bool newMacro = false;
            if (curMacroop || inRom) {
                if (inRom) {
                    staticInst = dec_ptr->fetchRomMicroop(
                            thisPC.microPC(), curMacroop);
                } else {
                    staticInst = curMacroop->fetchMicroop(thisPC.microPC());
                }
                newMacro |= staticInst->isLastMicroop();
            }

            DynInstPtr instruction =
                buildInst(tid, staticInst, curMacroop, thisPC, nextPC, true);

	    DPRINTF(Fetch,"Bgodala check thisPC %s\n", thisPC);
            ppFetch->notify(instruction);
            numInst++;

            ///////////////////////////////////////
            // cms11 start of oracle cache repl mod
            if( REPL == ORACLE){
                //warn("ORACLE WORKING numSets:%d", numSets);
                if (repl_first_inst) {
                    if (repl_init) {
                        repl_init = false;
                        srand(time(NULL));
                    }
                    Addr a = instruction->instAddr() >> 6;
                    //unsigned int ind = (unsigned)(a & 0x3f);
                    unsigned int ind = (unsigned)(a & ((unsigned)numSets - 1));
                    assert(ind < numSets);
                    assert(ind >= 0);
    
                    // check oneMisses
                    vector<Addr> new_evicts;
                    int possible_evicts = 1;
                    int last_size = 0;
                    for (int m = 0; m < oneMisses[ind].size(); m++) {
                        vector<Addr> *om = &(oneMisses[ind][m]);
                        possible_evicts = possible_evicts - 1 + (om->size() - last_size);
                        last_size = om->size();
                        //cout << "om #" << m << " possible evicts: " << possible_evicts << endl;
                        // evicts check
                        for (int e = 0; e < new_evicts.size(); e++) {
                            vector<Addr>::iterator in_e = find(om->begin(), om->begin() + om->size(), new_evicts[e]);
                            if (in_e != (om->begin() + om->size())) { // is in evicts
                                om->erase(in_e);
                                possible_evicts--;
                                last_size--;
                                if (om->size() == 1) {
                                    new_evicts.push_back((*om)[0]);
                                    oneMisses[ind].erase(oneMisses[ind].begin() + m);
                                    break;
                                }
                            }
                        }
                        // cross out check
                        vector<Addr>::iterator in_om = find(om->begin(), om->end(), a);
                        if (in_om != om->end()) { // is in
                            //cout << "crossing out!" << endl;
                            om->erase(in_om);
                            possible_evicts--;
                            last_size--;
                            //cout << "om #" << m << " possible evicts end: " << possible_evicts << endl;
                            if (possible_evicts == 1) {
                                for (int i = 0; i < om->size(); i++) {
                                    //cout << "adding " << (*om)[i] << " to new_evicts" << endl;
                                    new_evicts.push_back((*om)[i]);
                                }
                                // delete all om before and including
                                //cout << "deleting up to m: " << m << endl;
                                for (int i = 0; i <= m; i++) {
                                    oneMisses[ind].erase(oneMisses[ind].begin());
                                    //cout << "deleted" << endl;
                                }
                                m = -1;
                                last_size = 0;
                            }
                        }
                    }
                    //  synchronize in_cache with new evicts
                    if (REPL == ORACLE) {
                        for (int e = 0; e < new_evicts.size(); e++) {
                            vector<Addr>::iterator e_in_c = find(in_cache[ind].begin(), in_cache[ind].end(), new_evicts[e]);
                            if (e_in_c != in_cache[ind].end()) {
                                in_cache[ind].erase(e_in_c);
                            }
                        }
                    } else {
                        new_evicts = vector<Addr>();
                        vector<Addr>::iterator in_c = find(in_cache[ind].begin(), in_cache[ind].end(), a);
                        if (in_cache[ind].size() >= NUM_WAYS && in_c == in_cache[ind].end()) { // need to evict something
                            if (REPL == LRU) {
                                new_evicts.push_back(in_cache[ind][0]);
                                in_cache[ind].erase(in_cache[ind].begin());
                            } else if (REPL == RANDOM) {
                                int r = rand() % NUM_WAYS;
                                new_evicts.push_back(in_cache[ind][r]);
                                vector<Addr>::iterator it = in_cache[ind].begin();
                                advance(it, r);
                                in_cache[ind].erase(it);
                            }
                        } // if REPL == NONE, do nothing
                    }
                    // check in cache
                    bool add_om = false;
                    vector<Addr>::iterator in_c = find(in_cache[ind].begin(), in_cache[ind].end(), a);
                    if (in_c == in_cache[ind].end()) {
                        in_cache[ind].push_back(a);
                        add_om = true;
                        repl_misses++;
                    } else {
                        in_cache[ind].erase(in_c);
                        in_cache[ind].push_back(a);
                        repl_hits++;
                    }
                    // add oneMiss or not
                    if (REPL == ORACLE && add_om && in_cache[ind].size() > NUM_WAYS) {
                        // add oneMiss
                        vector<Addr> om(in_cache[ind].begin(), in_cache[ind].end() - 1);
                        oneMisses[ind].push_back(om);
                    }

                    // debug
                    //cout << "cache cap: " << dec << in_cache[ind].size() << endl;
                    //cout << "in_cache[" << dec << ind << "]: [";
                    assert(in_cache[ind].size() < 256);
                    for (int c = 0; c < in_cache[ind].size() - 1; c++) {
                        //cout << hex << in_cache[ind][c] << ", ";
                    }
                    //cout << hex << in_cache[ind].back() << "]" << endl;

                    for (vector<vector<Addr> >::iterator om_it = oneMisses[ind].begin(); om_it != oneMisses[ind].end(); om_it++) {
                        int lim = 0;
                        //cout << "[";
                        for (vector<Addr>::iterator it = om_it->begin(); it != om_it->end() - 1; it++) {
                            //cout << hex << *it << ", ";
                            if (lim > 30) {
                                //cout << "lim reached...";
                                break;
                            }
                            lim++;
                        }
                        //cout << hex << om_it->back() << "]" << endl;
                    }
                    if (new_evicts.size() > 0) {
                        //cout << "evicting: [";
                        for (int i = 0; i < new_evicts.size() - 1; i++) {
                            //cout << hex << new_evicts[i] << ", ";
                        } //cout << hex << new_evicts.back() << "]" << endl;
                    }
                    //cout << "hits = " << dec << repl_hits << endl;
                    //cout << "misses = " << dec << repl_misses << endl;
                    //cout << "evictions = " << dec << repl_evicts << endl;
                    //cout << "version: " << REPL << endl;
                    //cout << "---------------------------------------" << endl;

                    // evict blocks!
                    for (int e = 0; e < new_evicts.size(); e++) {
                        //cout << "evict addr: " << hex << (new_evicts[e] << 6) << endl;
                        Addr e_fetchBufferBlockPC = fetchBufferAlignPC(new_evicts[e] << 6);
                        Addr e_pc = e_fetchBufferBlockPC;
                        RequestPtr evict_mem_req = std::make_shared<Request>(
                            e_fetchBufferBlockPC, fetchBufferSize,
                            Request::INST_FETCH, cpu->instRequestorId(), e_pc,
                            cpu->thread[tid]->contextId());
                        evict_mem_req->taskId(cpu->taskId());
                        Fault e_fault = cpu->mmu->translateFunctional(evict_mem_req, cpu->thread[tid]->getTC(), BaseMMU::Execute);
                        //cout << "fault: " << e_fault << endl;
                        PacketPtr evict_pkt = new Packet(evict_mem_req, MemCmd::ReadReq);
                        //ibufferPort.sendTimingBufToCacheWrite(evict_pkt);
                        //ibufferPort.sendTimingWouldHaveStarved(evict_pkt);
                        icachePort.sendTimingWouldHaveStarved(evict_pkt);
                        repl_evicts++;
                    }
                    repl_first_inst = false;
                }
            }
            // end of oracle cache repl mod
            ////////////////////////////////


#if TRACING_ON
            if (debug::O3PipeView) {
                instruction->fetchTick = curTick();
            }
#endif
                instruction->fetchTick = curTick();
                instruction->mispred = 'F';
                instruction->squashedFromThisInst = false;
                // cms11 edit
                instruction->memsentTick = memsent_ticks[tid][(thisPC.instAddr())>>6];
                instruction->memrecvTick = memrecv_ticks[tid][(thisPC.instAddr())>>6];
                instruction->memlevel = memlevels[tid][(thisPC.instAddr())>>6];
                instruction->buf = buffer_cache[tid][(thisPC.instAddr())>>6];
                instruction->starve = starve[tid][(thisPC.instAddr())>>6];
                instruction->missSt = missSt[tid][(thisPC.instAddr())>>6];
                //instruction->fetchIcacheMissL1Dump = fetchIcacheMissL1Dump;
                //instruction->fetchIcacheMissL1StDump = fetchIcacheMissL1StDump;
                //instruction->fetchIcacheMissL1NoStDump = fetchIcacheMissL1NoStDump;

            nextPC = thisPC;

            // If we're branching after this instruction, quit fetching
            // from the same block.
            predictedBranch |= thisPC.branching();
            predictedBranch |=
                lookupAndUpdateNextPC(instruction, nextPC);
            instruction->setBrSeq(brseq[tid]);
            if (predictedBranch) {
                DPRINTF(Fetch, "Branch detected with PC = %s\n", thisPC);
            }

            newMacro |= thisPC.instAddr() != nextPC.instAddr();

            // Move to the next instruction, unless we have a branch.
            thisPC = nextPC;
            lastinst[tid] = instruction;
            inRom = isRomMicroPC(thisPC.microPC());

            if (newMacro) {
                fetchAddr = thisPC.instAddr() & pc_mask;
                //blkOffset = (fetchAddr - fetchBufferPC[tid]) / instSize;
                blkOffset = (fetchAddr - fetchBufferPC[tid].front()) / instSize;
                pcOffset = 0;
                curMacroop = NULL;
            }

            if (instruction->isQuiesce()) {
                DPRINTF(Fetch,
                        "Quiesce instruction encountered, halting fetch!\n");
                fetchStatus[tid] = QuiescePending;
                status_change = true;
                quiesce = true;
                break;
            }
        } while ((curMacroop || dec_ptr->instReady()) &&
                 numInst < fetchWidth &&
                 fetchQueue[tid].size() < fetchQueueSize);

        // Re-evaluate whether the next instruction to fetch is in micro-op ROM
        // or not.
        inRom = isRomMicroPC(thisPC.microPC());
    }

    pcOffset = 0;
    if (predictedBranch) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, predicted branch "
                "instruction encountered.\n", tid);
    } else if (numInst >= fetchWidth) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, reached fetch bandwidth "
                "for this cycle.\n", tid);
    } else if (blkOffset >= fetchBufferSize) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, reached the end of the"
                "fetch buffer.\n", tid);
    }

    macroop[tid] = curMacroop;
    fetchOffset[tid] = pcOffset;

    if (numInst > 0) {
        wroteToTimeBuffer = true;
    }

    pc[tid] = thisPC;

    // pipeline a fetch if we're crossing a fetch buffer boundary and not in
    // a state that would preclude fetching
    fetchAddr = (thisPC.instAddr() + pcOffset) & pc_mask;
    // Addr fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);
    fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);

/*    issuePipelinedIfetch[tid] = fetchBufferBlockPC != fetchBufferPC[tid] &&
        fetchStatus[tid] != IcacheWaitResponse &&
        fetchStatus[tid] != ItlbWait &&
        fetchStatus[tid] != IcacheWaitRetry &&
        fetchStatus[tid] != QuiescePending &&
        !curMacroop;*/

    if (fetchBufferPC[tid].size()>0 && fetchBufferBlockPC != fetchBufferPC[tid].front()) {
        assert(!curMacroop && "Curmacroop should not be not null!");
        fetchBufferPC[tid].pop_front();
        fetchBuffer[tid].pop_front();
        fetchBufferValid[tid].pop_front();
        fetchBufferReqPtr[tid].pop_front();
        if(prefetchBufferPC[tid].size()>0){
          prefetchBufferPC[tid].pop_front();
        }
        DPRINTF(Fetch, "[tid:%i] Popping queue %d %d %d.\n", tid, fetchBuffer[tid].size(), fetchBufferPC[tid].size(), fetchBufferValid[tid].size());
        if(prefetchBufferPC[tid].size()>0 && fetchBufferBlockPC != prefetchBufferPC[tid].front() && !curMacroop){
        //    fetchBufferPC[tid].clear();
        //    fetchBuffer[tid].clear();
        //    fetchBufferValid[tid].clear();
            //DPRINTF(Fetch, "Front is still not same. fetchBufferBlockPC: %#x fetchBufferPC: %#x\n", fetchBufferBlockPC, fetchBufferPC[tid].front());

            //assert(false && "front mismatch\n");
            prefetchQueue[tid].clear();
            prefetchQueueBblSize[tid].clear();
            prefetchQueueSeqNum[tid].clear();
            prefetchQueueBr[tid].clear();
            prefetchBufferPC[tid].clear();
            fetchBuffer[tid].clear();
            fetchBufferPC[tid].clear();
            fetchBufferReqPtr[tid].clear();
            fetchBufferValid[tid].clear();
            memReq[tid].clear();
            //DPRINTF(Fetch, "Front is still not same. fetchBufferBlockPC: %#x fetchBufferPC: %#x\n", fetchBufferBlockPC, fetchBufferPC[tid].front());
            DPRINTFN("Front is still not same. fetchBufferBlockPC: %#x fetchBufferPC: %#x\n", fetchBufferBlockPC, fetchBufferPC[tid].front());
            lastProcessedLine = 0;
            lastAddrFetched = 0;
            fallThroughPrefPC = 0;
            lastPrefPC = 0;
            prefPC[tid] = thisPC;
        }
    }
}

void
Fetch::recvReqRetry()
{
    if (retryPkt != NULL && fetchStatus[retryTid] == IcacheWaitRetry) {
        assert(cacheBlocked);
        assert(retryTid != InvalidThreadID);
        assert(fetchStatus[retryTid] == IcacheWaitRetry);

        if (icachePort.sendTimingReq(retryPkt)) {
            if(retryPkt->req->getVaddr()==fetchBufferPC[retryTid].front())
                fetchStatus[retryTid] = IcacheWaitResponse;
            // Notify Fetch Request probe when a retryPkt is successfully sent.
            // Note that notify must be called before retryPkt is set to NULL.
            ppFetchRequestSent->notify(retryPkt->req);
            retryPkt = NULL;
            retryTid = InvalidThreadID;
            cacheBlocked = false;
        }
    } else {
        // Nayana commented
        //assert(retryTid == InvalidThreadID);
        // Access has been squashed since it was sent out.  Just clear
        // the cache being blocked.
        if (retryPkt) {
            delete retryPkt;
        }
        retryPkt = NULL;
        retryTid = InvalidThreadID;
        cacheBlocked = false;
    }
}

///////////////////////////////////////
//                                   //
//  SMT FETCH POLICY MAINTAINED HERE //
//                                   //
///////////////////////////////////////
ThreadID
Fetch::getFetchingThread()
{
    if (numThreads > 1) {
        switch (fetchPolicy) {
          case SMTFetchPolicy::RoundRobin:
            return roundRobin();
          case SMTFetchPolicy::IQCount:
            return iqCount();
          case SMTFetchPolicy::LSQCount:
            return lsqCount();
          case SMTFetchPolicy::Branch:
            return branchCount();
          default:
            return InvalidThreadID;
        }
    } else {
        std::list<ThreadID>::iterator thread = activeThreads->begin();
        if (thread == activeThreads->end()) {
            return InvalidThreadID;
        }

        ThreadID tid = *thread;

        if (fetchStatus[tid] == Running ||
            fetchStatus[tid] == IcacheAccessComplete ||
            fetchStatus[tid] == Idle ||
            fetchBufferValid[tid].front()) {
            return tid;
        } else {
            return InvalidThreadID;
        }
    }
}


ThreadID
Fetch::roundRobin()
{
    std::list<ThreadID>::iterator pri_iter = priorityList.begin();
    std::list<ThreadID>::iterator end      = priorityList.end();

    ThreadID high_pri;

    while (pri_iter != end) {
        high_pri = *pri_iter;

        assert(high_pri <= numThreads);

        if (fetchStatus[high_pri] == Running ||
            fetchStatus[high_pri] == IcacheAccessComplete ||
            fetchStatus[high_pri] == Idle) {

            priorityList.erase(pri_iter);
            priorityList.push_back(high_pri);

            return high_pri;
        }

        pri_iter++;
    }

    return InvalidThreadID;
}

ThreadID
Fetch::iqCount()
{
    //sorted from lowest->highest
    std::priority_queue<unsigned, std::vector<unsigned>,
                        std::greater<unsigned> > PQ;
    std::map<unsigned, ThreadID> threadMap;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;
        unsigned iqCount = fromIEW->iewInfo[tid].iqCount;

        //we can potentially get tid collisions if two threads
        //have the same iqCount, but this should be rare.
        PQ.push(iqCount);
        threadMap[iqCount] = tid;
    }

    while (!PQ.empty()) {
        ThreadID high_pri = threadMap[PQ.top()];

        if (fetchStatus[high_pri] == Running ||
            fetchStatus[high_pri] == IcacheAccessComplete ||
            fetchStatus[high_pri] == Idle)
            return high_pri;
        else
            PQ.pop();

    }

    return InvalidThreadID;
}

ThreadID
Fetch::lsqCount()
{
    //sorted from lowest->highest
    std::priority_queue<unsigned, std::vector<unsigned>,
                        std::greater<unsigned> > PQ;
    std::map<unsigned, ThreadID> threadMap;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;
        unsigned ldstqCount = fromIEW->iewInfo[tid].ldstqCount;

        //we can potentially get tid collisions if two threads
        //have the same iqCount, but this should be rare.
        PQ.push(ldstqCount);
        threadMap[ldstqCount] = tid;
    }

    while (!PQ.empty()) {
        ThreadID high_pri = threadMap[PQ.top()];

        if (fetchStatus[high_pri] == Running ||
            fetchStatus[high_pri] == IcacheAccessComplete ||
            fetchStatus[high_pri] == Idle)
            return high_pri;
        else
            PQ.pop();
    }

    return InvalidThreadID;
}

ThreadID
Fetch::branchCount()
{
    panic("Branch Count Fetch policy unimplemented\n");
    return InvalidThreadID;
}

void
Fetch::pipelineIcacheAccesses(ThreadID tid)
{
    //if (!issuePipelinedIfetch[tid]) {
    //if (fetchStatus[tid] == ItlbWait
    //    || fetchStatus[tid] == IcacheWaitResponse
    //    || fetchStatus[tid] == IcacheWaitRetry){
    //    return;
    //}
    
    if(!enableFDIP)
        return;

    if (fetchStatus[tid] == IcacheWaitRetry
        || fetchStatus[tid] == TrapPending
        || fetchStatus[tid] == QuiescePending){
        return;
    }

    //if (prefetchQueue[tid].empty()) {
    //    return;
    //}
    // If prefetch buffer is empty then fetch head of the PC and memReq queue is empty
    if (prefetchBufferPC[tid].empty() && prefetchQueue[tid].empty()){
        DPRINTF(Fetch,"pipelineIcache addToFTQ pc[tid] is %#x\n", pc[tid].instAddr());
        //DPRINTFN("addToFTQ pc[tid] is %#x\n", pc[tid].instAddr());
        TheISA::PCState thisPC = pc[tid];
        Addr curPCLine = (thisPC.instAddr() >> CACHE_LISZE_SIZE_WIDTH) << CACHE_LISZE_SIZE_WIDTH;
        prefetchBufferPC[tid].push_back(curPCLine);
        lastAddrFetched = curPCLine;
        //lastPrefPC = thisPC;
        lastPrefPC = prefPC[tid]; 
        lastProcessedLine = 0;
        //prefPC[tid] = pc[tid];
    }

    if (prefetchBufferPC[tid].empty()) {
        return;
    }


    std::deque<TheISA::PCState>::iterator it = prefetchQueue[tid].begin();
    std::deque<int>::iterator it_sz = prefetchQueueBblSize[tid].begin();

    while (it!= prefetchQueue[tid].end()) {
        DPRINTF(Fetch, "PFQ %#x\n", (*it).instAddr());
        it++;
    }
    
    while (it_sz!= prefetchQueueBblSize[tid].end()) {
        DPRINTF(Fetch, "PFQBblSz %d\n", *it_sz);
        it_sz++;
    }

    for ( auto pc_it : fetchBufferPC[tid]){
        DPRINTF(Fetch, "fetchBufferPC %#x\n", pc_it);
    }

    for ( auto pref_pc_it : prefetchBufferPC[tid]){
        DPRINTF(Fetch, "prefetchBufferPC %#x\n", pref_pc_it);
    }

    pcIt pc_it = fetchBufferPC[tid].begin();
    pcIt pref_pc_it = prefetchBufferPC[tid].begin();

    DPRINTF(Fetch, "Iterating through prefetchBufferPC\n");
    while(pc_it != fetchBufferPC[tid].end() && 
            pref_pc_it != prefetchBufferPC[tid].end() && 
            (*pref_pc_it) == (*pc_it)){
        DPRINTF(Fetch, "%#x\n", *pc_it);
        pref_pc_it++;
        pc_it++;
    }

    if(pc_it != fetchBufferPC[tid].end()){
        DPRINTF(Fetch, "Something is wrong\n");
        assert(false && "pc_it is not the end of the fetchBufferPC\n");
        return;
    }

    if(pref_pc_it != prefetchBufferPC[tid].end()){
        assert(pc_it == fetchBufferPC[tid].end() && " pc_it is not the end of the fetchBufferPC\n");
        DPRINTF(Fetch, "Issuing a pipelined access %#x\n", *pref_pc_it); 
        fetchCacheLine(*pref_pc_it, tid, *pref_pc_it);
    }

    //it = prefetchQueue[tid].begin();
    //it_sz = prefetchQueueBblSize[tid].begin();

    //if (prefetchQueue[tid].size()==1) {
    //    Addr prevAddr = prevPC[tid].instAddr();
    //    int bblsz = (it_sz!=prefetchQueueBblSize[tid].end()) ? *it_sz : 0;
    //    Addr lastAddr = prevAddr + bblsz;
    //    if(bblsz>1 && prevAddr>0x10 && (prevAddr>>6)<(lastAddr>>6)) {
    //        Addr fetchAddr = (prevAddr+64) & decoder[tid]->pcMask();
    //        Addr fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);
    //        pcIt pc_it = fetchBufferPC[tid].begin();
    //        validIt valid_it = fetchBufferValid[tid].begin();

    //        DPRINTF(Fetch, "[tid:%i] Issuing a pipelined I-cache access, "
    //                "starting at PC 20 %d %d %d.\n", tid, fetchBufferPC[tid].size(), fetchBufferValid[tid].size(), bblsz);
    //        while (pc_it != fetchBufferPC[tid].end() &&
    //               ((*pc_it)!=fetchBufferBlockPC)) {
    //            DPRINTF(Fetch, "%#x %#x\n", *pc_it, fetchBufferBlockPC);
    //            ++pc_it;
    //            ++valid_it;
    //        }
    //        // Unless buffer already got the block, fetch it from icache.
    //        if (pc_it == fetchBufferPC[tid].end()) {
    //            DPRINTF(Fetch, "[tid:%i] Issuing a pipelined I-cache access, "
    //                    "starting at PC %s.\n", tid, prevPC[tid]);
    //            fetchCacheLine(fetchBufferBlockPC, tid, prevPC[tid].instAddr());
    //            return;
    //        }
    //    }
    //} 
    //it_sz++;
    //
    //while (it!= prefetchQueue[tid].end()) {
    //    // The next PC to access.
    //    TheISA::PCState thisPC = *it++;
    //    int bblsz = (it_sz!=prefetchQueueBblSize[tid].end()) ? *it_sz++ : 0;
    //    Addr lastAddr = thisPC.instAddr() + bblsz;

    //    Addr fetchAddr = thisPC.instAddr() & decoder[tid]->pcMask();
    //    // Align the fetch PC so its at the start of a fetch buffer segment.
    //    Addr fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);
    //    DPRINTF(Fetch, "[tid:%i] Issuing a pipelined I-cache access outer, "
    //                "starting at PC %d %d %d.\n", tid, fetchBufferPC[tid].size(), fetchBufferValid[tid].size(), bblsz);
    //    if (fetchBufferPC[tid].empty()) {
    //        DPRINTF(Fetch, "[tid:%i] Issuing a pipelined I-cache access, "
    //                "starting at PC %s.\n", tid, thisPC);

    //        fetchCacheLine(fetchBufferBlockPC, tid, thisPC.instAddr());
    //        return;
    //    } 
    //    int i = 1;
    //    do {
    //        pcIt pc_it = fetchBufferPC[tid].begin();
    //        validIt valid_it = fetchBufferValid[tid].begin();

    //        DPRINTF(Fetch, "[tid:%i] Issuing a pipelined I-cache access, "
    //                "starting at PC 10 %d %d %d.\n", tid, fetchBufferPC[tid].size(), fetchBufferValid[tid].size(), bblsz);
    //        while (pc_it != fetchBufferPC[tid].end() &&
    //               ((*pc_it)!=fetchBufferBlockPC)) {
    //            DPRINTF(Fetch, "%#x %#x %#x\n", *pc_it, fetchBufferBlockPC, lastAddr);
    //            ++pc_it;
    //            ++valid_it;
    //        }
    //        // Unless buffer already got the block, fetch it from icache.
    //        if (pc_it == fetchBufferPC[tid].end()) {
    //            DPRINTF(Fetch, "[tid:%i] Issuing a pipelined I-cache access, "
    //                    "starting at PC %s.\n", tid, thisPC);
    //            fetchCacheLine(fetchBufferBlockPC, tid, thisPC.instAddr());
    //            return;
    //        }
    //        fetchAddr = (thisPC.instAddr()+(i*64)) & decoder[tid]->pcMask(); 
    //        i++;
    //        fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);
    //    } while(bblsz>1 && i<3 && (fetchBufferBlockPC>>6)<(lastAddr>>6)); 
    //    if((fetchBufferBlockPC>>6)<(lastAddr>>6)) {
    //        DPRINTF(Fetch, "Nayana please check me %#x, %#x %s, %d", fetchBufferBlockPC, lastAddr, thisPC, bblsz);
    //    }
    //}
    //// The next PC to access.
    //TheISA::PCState thisPC = pc[tid];

    //if (isRomMicroPC(thisPC.microPC())) {
    //    return;
    //}

    //Addr pcOffset = fetchOffset[tid];
    //Addr fetchAddr = (thisPC.instAddr() + pcOffset) & decoder[tid]->pcMask();

    //// Align the fetch PC so its at the start of a fetch buffer segment.
    //Addr fetchBufferBlockPC = fetchBufferAlignPC(fetchAddr);

    //// Unless buffer already got the block, fetch it from icache.
    //if (!(fetchBufferValid[tid] && fetchBufferBlockPC == fetchBufferPC[tid])) {
    //    DPRINTF(Fetch, "[tid:%i] Issuing a pipelined I-cache access, "
    //            "starting at PC %s.\n", tid, thisPC);

    //    fetchCacheLine(fetchAddr, tid, thisPC.instAddr());
    //}
}

void
Fetch::profileStall(ThreadID tid)
{
    DPRINTF(Fetch,"There are no more threads available to fetch from.\n");

    // @todo Per-thread stats

    if (stalls[tid].drain) {
        ++fetchStats.pendingDrainCycles;
        DPRINTF(Fetch, "Fetch is waiting for a drain!\n");
    } else if (activeThreads->empty()) {
        ++fetchStats.noActiveThreadStallCycles;
        DPRINTF(Fetch, "Fetch has no active thread!\n");
    } else if (fetchStatus[tid] == Blocked) {
        ++fetchStats.blockedCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is blocked!\n", tid);
    } else if (fetchStatus[tid] == Squashing) {
        ++fetchStats.squashCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is squashing!\n", tid);
    } else if (fetchStatus[tid] == IcacheWaitResponse) {
        ++fetchStats.icacheStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting cache response!\n",
                tid);
    } else if (fetchStatus[tid] == ItlbWait) {
        ++fetchStats.tlbCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting ITLB walk to "
                "finish!\n", tid);
    } else if (fetchStatus[tid] == TrapPending) {
        ++fetchStats.pendingTrapStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting for a pending trap!\n",
                tid);
    } else if (fetchStatus[tid] == QuiescePending) {
        ++fetchStats.pendingQuiesceStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting for a pending quiesce "
                "instruction!\n", tid);
    } else if (fetchStatus[tid] == IcacheWaitRetry) {
        ++fetchStats.icacheWaitRetryStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting for an I-cache retry!\n",
                tid);
    } else if (fetchStatus[tid] == NoGoodAddr) {
            DPRINTF(Fetch, "[tid:%i] Fetch predicted non-executable address\n",
                    tid);
    } else {
        DPRINTF(Fetch, "[tid:%i] Unexpected fetch stall reason "
            "(Status: %i)\n",
            tid, fetchStatus[tid]);
    }
}

bool
Fetch::IcachePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(O3CPU, "Fetch unit received timing\n");
    // We shouldn't ever get a cacheable block in Modified state
    assert(pkt->req->isUncacheable() ||
           !(pkt->cacheResponding() && !pkt->hasSharers()));
    
    //Do not execute processCacheCompletion when perfect ICache
    //is enabled. Timing requests are still sent to model
    //memory bandwidth
    if(!fetch->enablePerfectICache){
        fetch->processCacheCompletion(pkt);
    }else{
        delete pkt;
    }



    return true;
}

void
Fetch::IcachePort::recvReqRetry()
{
    fetch->recvReqRetry();
}

} // namespace o3
} // namespace gem5
