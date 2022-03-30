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

#include "params/LRUEmissaryRP.hh"
#include "sim/core.hh"
#include "base/trace.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(ReplacementPolicy, replacement_policy);
namespace replacement_policy
{

LRUEmissary::LRUEmissary(const Params &p)
    : Base(p), lru_ways(p.lru_ways),
    preserve_ways(p.preserve_ways)
{
}

void
LRUEmissary::invalidate(const std::shared_ptr<ReplacementData>& replacement_data)
{
    // Reset last touch timestamp
    std::static_pointer_cast<LRUEmissaryReplData>(
        replacement_data)->lastTouchTick = Tick(0);
}

void
LRUEmissary::touch(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    // Update last touch timestamp
    std::static_pointer_cast<LRUEmissaryReplData>(
        replacement_data)->lastTouchTick = curTick();
}

void
LRUEmissary::reset(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    // Set last touch timestamp
    std::static_pointer_cast<LRUEmissaryReplData>(
        replacement_data)->lastTouchTick = curTick();
}

ReplaceableEntry*
LRUEmissary::getVictim(const ReplacementCandidates& candidates) const
{
    // There must be at least one replacement candidate
    assert(candidates.size() > 0);

    // Visit all candidates to find victim
    ReplaceableEntry* victim = candidates[0];
    // Nayana added new replacement policy based on starvation bit
    ReplaceableEntry* victimNotSt = candidates[0];
    ReplaceableEntry* lruEntry = candidates[0];
    ReplaceableEntry* preservedEntry;
    uint8_t numNotPreserved = 0;
    uint8_t numlru = 0;
    uint8_t numpreserve = 0;

    // Find the LRU line among ones in LRU Mode
    for (const auto& candidate : candidates) {
        CacheBlk *blk = reinterpret_cast<CacheBlk*>(candidate);

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
                candidate_cost = candidate_cost/500;
                victim_cost = victim_cost/500;
                
                CacheBlk *victim_blk = reinterpret_cast<CacheBlk*>(preservedEntry);

                if((candidate_cost < victim_cost) && (curTick()/500 - candidate_cost > 500)){
                    //DPRINTFN("EMISSARY: very old entry candidate_cost:%llu victim_cost:%llu\n", candidate_cost, victim_cost);
                    preservedEntry = candidate;
                }else if(blk->getRefCount() < victim_blk->getRefCount()){
                    //DPRINTFN("EMISSARY: least freq entry candidate_ref_count:%d victim_ref_count:%d\n",blk->getRefCount(),victim_blk->getRefCount());
                    preservedEntry = candidate;
                }

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

    if(numpreserve > preserve_ways){
        CacheBlk *victim_blk = reinterpret_cast<CacheBlk*>(preservedEntry);
        //DPRINTFN("EMISSARY: preservedEntry: %s\n",victim_blk->print());
        CacheBlk *lru_blk = reinterpret_cast<CacheBlk*>(lruEntry);
        //DPRINTFN("EMISSARY: lruEntry: %s\n",lru_blk->print());
        //return preservedEntry;
        return lruEntry;
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
    return std::shared_ptr<ReplacementData>(new LRUEmissaryReplData());
}

} // namespace replacement_policy
} // namespace gem5
