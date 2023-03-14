/**
 * Copyright (c) 2018 Inria
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

#include "mem/cache/replacement_policies/lru_emissary_rp.hh"

#include <cassert>
#include <memory>
#include <map>

#include "params/LRUEmissaryRP.hh"
#include "sim/core.hh"
#include "base/trace.hh"
#include "base/output.hh"

#define MAX_VAL 32

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(ReplacementPolicy, replacement_policy);
namespace replacement_policy
{

LRUEmissary::LRUEmissary(const Params &p)
    : Base(p), lru_ways(p.lru_ways),
    preserve_ways(p.preserve_ways),
    last_tick(0),
    flush_freq_in_cycles(p.flush_freq_in_cycles),
    max_age(p.max_val)
{
        registerExitCallback([this]() { dumpPreserveHist(); });
}

void
LRUEmissary::invalidate(const std::shared_ptr<ReplacementData>& replacement_data)
{
    // Reset last touch timestamp
    std::static_pointer_cast<LRUEmissaryReplData>(
        replacement_data)->lastTouchTick = 0;
}

void 
LRUEmissary::promote(const std::shared_ptr<ReplacementData>&
        replacement_data) const
{
    return;
    if(std::static_pointer_cast<LRUEmissaryReplData>(
                           replacement_data)->lastTouchTick != 0){
        checkLRU(replacement_data);
    }
    std::static_pointer_cast<LRUEmissaryReplData>(
        replacement_data)->lastTouchTick = max_age; 
}

void
LRUEmissary::checkLRU(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    //Age LRU Bits of a given set
    auto repl_data = static_pointer_cast<LRUEmissaryReplData>(replacement_data);
    CacheBlk *cur_blk = repl_data->blk;
    auto set = cur_blk->getSet();

    bool preserve = cur_blk->isPreserve();

    std::vector<ReplaceableEntry*> entries; 

    bool allOnes = true;
    DPRINTF(EMISSARY, "Age of set %d :", set);
    for(int way=0; way < numWays; way++){

        ReplaceableEntry *entry = indexingPolicy->getEntry(set, way);
        CacheBlk *blk = reinterpret_cast<CacheBlk*>(entry);
        auto candidate_repl_data = std::static_pointer_cast<LRUEmissaryReplData>(blk->replacementData);
        if((blk->isPreserve() == preserve) && candidate_repl_data->lastTouchTick > 1){
            candidate_repl_data->lastTouchTick--;
            entries.push_back(entry);
        }
        DPRINTFR(EMISSARY, " %d",candidate_repl_data->lastTouchTick);
        if(blk->isPreserve()){
            DPRINTFR(EMISSARY, "(P)");
        }
        if(((preserve == blk->isPreserve())) && candidate_repl_data->lastTouchTick == 1){
            allOnes = false;
        }
    }
    DPRINTFR(EMISSARY, "\n");

    //if(allOnes){
    //    resetAll(entries, preserve);
    //    DPRINTF(EMISSARY, "All Ones set %d when updating way %d preserved %d\n", set, cur_blk->getWay(), preserve);
    //}
 
}

void
LRUEmissary::touch(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto *non_const_this = const_cast<LRUEmissary*>(this);
    non_const_this->checkToFlushPreserveBits();

    auto repl_data = static_pointer_cast<LRUEmissaryReplData>(replacement_data);
    CacheBlk *cur_blk = repl_data->blk;
    bool preserve = cur_blk->isPreserve();

    //if(preserve)
    //    return;

    // Update last touch timestamp
    //std::static_pointer_cast<LRUEmissaryReplData>(
    //    replacement_data)->lastTouchTick = Tick(500);
    //    //replacement_data)->lastTouchTick = curTick();
    //checkLRU(replacement_data);

    std::static_pointer_cast<LRUEmissaryReplData>(
        replacement_data)->lastTouchTick = max_age; 

}

void
LRUEmissary::reset(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto *non_const_this = const_cast<LRUEmissary*>(this);
    non_const_this->checkToFlushPreserveBits();
    //
    //Age LRU Bits
    checkLRU(replacement_data);
    // Set last touch timestamp
    std::static_pointer_cast<LRUEmissaryReplData>(
        replacement_data)->lastTouchTick = max_age; 
        //replacement_data)->lastTouchTick = curTick();
}

void
LRUEmissary::resetAll(const ReplacementCandidates& candidates, bool preservedWays) const
{
    return;
    for (const auto& candidate : candidates) {
        CacheBlk *blk = reinterpret_cast<CacheBlk*>(candidate);
        if((preservedWays == blk->isPreserve())){
            std::static_pointer_cast<LRUEmissaryReplData>(
                        candidate->replacementData)->lastTouchTick = 0; 
        }
    }

}

ReplaceableEntry*
LRUEmissary::getVictim(const ReplacementCandidates& candidates) const
{
    // There must be at least one replacement candidate
    assert(candidates.size() > 0);

    auto *non_const_this = const_cast<LRUEmissary*>(this);
    non_const_this->checkToFlushPreserveBits();

    // Visit all candidates to find victim
    ReplaceableEntry* victim = candidates[0];
    // Nayana added new replacement policy based on starvation bit
    ReplaceableEntry* victimNotSt = candidates[0];
    ReplaceableEntry* lruEntry = candidates[0];
    ReplaceableEntry* preservedEntry;
    uint8_t numNotPreserved = 0;
    uint8_t numlru = 0;
    uint8_t numpreserve = 0;

    bool resetPreservedWays = true;
    bool resetNonPreservedWays = true;
    // Find the LRU line among ones in LRU Mode
    for (const auto& candidate : candidates) {
        CacheBlk *blk = reinterpret_cast<CacheBlk*>(candidate);

        if(std::static_pointer_cast<LRUEmissaryReplData>(candidate->replacementData)->lastTouchTick == 0){
            return candidate;
        }

        if(blk->isPreserve()){
            numpreserve++;
            if(numpreserve==1){
                preservedEntry  = candidate;
            }else{
                auto candidate_repl_data = std::static_pointer_cast<LRUEmissaryReplData>(candidate->replacementData);
                auto victim_repl_data = std::static_pointer_cast<LRUEmissaryReplData>(preservedEntry->replacementData);
                uint64_t candidate_cost = candidate_repl_data->lastTouchTick;
                uint64_t victim_cost = victim_repl_data->lastTouchTick;
                
                //convert tick to time
                candidate_cost = candidate_cost;
                victim_cost = victim_cost;
                
                CacheBlk *victim_blk = reinterpret_cast<CacheBlk*>(preservedEntry);

                if((candidate_cost < victim_cost)){
                    //DPRINTFN("EMISSARY: very old entry candidate_cost:%llu victim_cost:%llu\n", candidate_cost, victim_cost);
                    preservedEntry = candidate;
                    resetPreservedWays = false;
                }
                //else if(blk->getRefCount() < victim_blk->getRefCount()){
                //    //DPRINTFN("EMISSARY: least freq entry candidate_ref_count:%d victim_ref_count:%d\n",blk->getRefCount(),victim_blk->getRefCount());
                //    preservedEntry = candidate;
                //    resetPreservedWays = false;
                //}

            }
        }else{
            // LRU of not preserved lines
            numNotPreserved++;
            if(numNotPreserved == 1){
                victimNotSt = candidate;
            } else if (std::static_pointer_cast<LRUEmissaryReplData>(
                    candidate->replacementData)->lastTouchTick <
                std::static_pointer_cast<LRUEmissaryReplData>(
                    victimNotSt->replacementData)->lastTouchTick) {
                victimNotSt = candidate;

                resetNonPreservedWays = false;

            }
        }
        //LRU of all
        if (std::static_pointer_cast<LRUEmissaryReplData>(
                candidate->replacementData)->lastTouchTick <
            std::static_pointer_cast<LRUEmissaryReplData>(
                lruEntry->replacementData)->lastTouchTick) {
            lruEntry = candidate;
        }
        //if (blk->isPreserve()) {
        //    numlru++;
        //    if (numlru == 1) {
        //        lruEntry = candidate;
        //    } else if (std::static_pointer_cast<LRUEmissaryReplData>(
        //            candidate->replacementData)->lastTouchTick <
        //        std::static_pointer_cast<LRUEmissaryReplData>(
        //            lruEntry->replacementData)->lastTouchTick) {
        //        lruEntry = candidate;
        //    }
        //} else {
        //    numpreserve++;
        //    if (!blk->isPreserve()) {
        //        numNotPreserved++;
        //        if (numNotPreserved == 1) {
        //            victimNotSt = candidate;
        //        } else if (std::static_pointer_cast<LRUEmissaryReplData>(
        //                candidate->replacementData)->lastTouchTick <
        //            std::static_pointer_cast<LRUEmissaryReplData>(
        //                victimNotSt->replacementData)->lastTouchTick) {
        //            victimNotSt = candidate;
        //        }
        //    }
        //    // Update victim entry if necessary
        //    if (numpreserve == 1) {
        //        victim = candidate;
        //    } else if (std::static_pointer_cast<LRUEmissaryReplData>(
        //            candidate->replacementData)->lastTouchTick <
        //        std::static_pointer_cast<LRUEmissaryReplData>(
        //            victim->replacementData)->lastTouchTick) {
        //        victim = candidate;
        //    }
        //}
    }

    //DPRINTFN("EMISSRY RP: numlru:%d numNotPreserved:%d numPreserved:%d\n",numlru, numNotPreserved, numpreserve);
    //assert(numpreserve <= preserve_ways && "numpreserve must be less than preserve_ways\n");

    if(resetPreservedWays){
        resetAll(candidates, true);
    }
    if(resetNonPreservedWays){
        resetAll(candidates, false);
    }

    if(numpreserve > preserve_ways){
        CacheBlk *victim_blk = reinterpret_cast<CacheBlk*>(preservedEntry);
        //DPRINTFN("EMISSARY: preservedEntry: %s\n",victim_blk->print());
        CacheBlk *lru_blk = reinterpret_cast<CacheBlk*>(lruEntry);
        //DPRINTFN("EMISSARY: lruEntry: %s\n",lru_blk->print());
        return preservedEntry;
        //return lruEntry;
    }else{
        return victimNotSt;
    }
    //CacheBlk *lruBlk = reinterpret_cast<CacheBlk*>(lruEntry);
    //// If there are no unpreserved entries in Preserve mode, return lru of lru mode
    //if (!lruBlk->isPreserve() && (numNotPreserved==0 ||
    //                             std::static_pointer_cast<LRUEmissaryReplData>(
    //                             victimNotSt->replacementData)->lastTouchTick >
    //                             std::static_pointer_cast<LRUEmissaryReplData>(
    //                             lruEntry->replacementData)->lastTouchTick)) {
    //    return lruEntry;
    //} else if (lruBlk->isPreserve() && (numNotPreserved==0 &&
    //                             std::static_pointer_cast<LRUEmissaryReplData>(
    //                             victim->replacementData)->lastTouchTick >
    //                             std::static_pointer_cast<LRUEmissaryReplData>(
    //                             lruEntry->replacementData)->lastTouchTick)) {
    //    return lruEntry;
    //}

    //lruBlk->rpMode = true;

    //return (numNotPreserved>0) ? victimNotSt : victim;
}

std::shared_ptr<ReplacementData>
LRUEmissary::instantiateEntry()
{
    return std::shared_ptr<ReplacementData>(new LRUEmissaryReplData(NULL));
}

std::shared_ptr<ReplacementData>
LRUEmissary::instantiateEntry(CacheBlk *blk)
{
    return std::shared_ptr<ReplacementData>(new LRUEmissaryReplData(blk));
}

void 
LRUEmissary::checkToFlushPreserveBits(){

    if (flush_freq_in_cycles == 0){
        return;
    }

    uint64_t cur_tick = curTick();

    uint64_t diff = (cur_tick - last_tick)/500;

    if ( diff >= flush_freq_in_cycles){
        dumpPreserveHist();
        last_tick = cur_tick;
    }
}

void
LRUEmissary::dumpPreserveHist(){
    ofstream histOut; 
    histOut.open(simout.directory()+"/set_hist.csv",fstream::app);

    histOut << curTick()/500 <<",";
    std::map<int,int> preserveCountHist;

    for(int i=0;i<numWays;i++){
        preserveCountHist[i] = 0;
    }

    for( int set=0; set < numSets; set++){
        
        int numPreserved=0;
        
        for(int way=0; way < numWays; way++){

            ReplaceableEntry *entry = indexingPolicy->getEntry(set, way);
            CacheBlk *blk = reinterpret_cast<CacheBlk*>(entry);
            if(blk->isPreserve()){
                numPreserved++;
            }

            if(!blk->isUsed()){
                blk->clearPreserve();
                //Erase preserve bit here
            }
            blk->clearUsed();
            //blk->clearPreserve();
            //Erase preserve bit here
        }

        if(numPreserved >= preserve_ways){
            preserveCountHist[preserve_ways]++;
            //histOut << preserve_ways <<",";
        }else{
            preserveCountHist[numPreserved]++;
            //histOut << numPreserved <<",";
        }

        ////Erase only saturated sets
        //if(numPreserved >= preserve_ways){
        //    for(int way=0; way < numWays; way++){

        //        ReplaceableEntry *entry = indexingPolicy->getEntry(set, way);
        //        CacheBlk *blk = reinterpret_cast<CacheBlk*>(entry);

        //        DPRINTFN("Ref Count is %d\n",blk->getRefCount());
        //        if(blk->getRefCount() <= 1){
        //            DPRINTFN("Clearing\n");
        //            blk->clearPreserve();
        //            //Erase preserve bit here
        //        }
        //    }
        //}
    }

    for(int i=0;i<numWays;i++){
        histOut << preserveCountHist[i] << ",";
    }

    histOut << "\n";

    histOut.close();
}

} // namespace replacement_policy
} // namespace gem5
