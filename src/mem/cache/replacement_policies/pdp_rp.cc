/**
 * Copyright (c) 2018-2020 Inria
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

#include "mem/cache/replacement_policies/pdp_rp.hh"

#include <cassert>
#include <memory>

#include "base/logging.hh" // For fatal_if
#include "base/random.hh"
#include "params/PDPRP.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(ReplacementPolicy, replacement_policy);
namespace replacement_policy
{

PDP::PDP(const Params &p)
  : Base(p), numRRPVBits(p.num_bits), maxRD(256), PD(0), 
    numWays(0), numSets(0), maxAccessDepth(p.max_access_depth)

{
    fatal_if(numRRPVBits <= 0, "There should be at least one bit per RRPV.\n");
}

void
PDP::invalidate(const std::shared_ptr<ReplacementData>& replacement_data)
{
    std::shared_ptr<PDPReplData> casted_replacement_data =
        std::static_pointer_cast<PDPReplData>(replacement_data);

    // Invalidate entry
    casted_replacement_data->valid = false;
    casted_replacement_data->rrpv.reset();
}

void
PDP::initializeVectors()
{
    for(int i=0; i< numSets; i++){
       perSetAccessQueue.push_back(std::vector<Addr>()); 
    }

    for(int i=0; i < maxRD; i++){
        RDCounter.push_back(0);
    }
}

void
PDP::updateAccess(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    //Age LRU Bits of a given set
    auto repl_data = std::static_pointer_cast<PDPReplData>(replacement_data);
    CacheBlk *cur_blk = repl_data->blk;
    auto set = cur_blk->getSet();

    //DPRINTFN("Age of set %d :", set);
    for(int way=0; way < numWays; way++){

        ReplaceableEntry *entry = indexingPolicy->getEntry(set, way);
        CacheBlk *blk = reinterpret_cast<CacheBlk*>(entry);
        auto candidate_repl_data = std::static_pointer_cast<PDPReplData>(blk->replacementData);
        candidate_repl_data->rrpv--;
        //DPRINTFNR(" %d",candidate_repl_data->rrpv);
    }
    //DPRINTFNR("\n");

    //if(allOnes){
    //    resetAll(entries, preserve);
    //    //DPRINTFN("All Ones set %d when updating way %d preserved %d\n", set, cur_blk->getWay(), preserve);
    //}
 
}


void
PDP::touch(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    std::shared_ptr<PDPReplData> casted_replacement_data =
        std::static_pointer_cast<PDPReplData>(replacement_data);

    casted_replacement_data->inserted = false;

    //TODO: Check here for access
    CacheBlk *blk = casted_replacement_data->blk;
    auto *non_const_this = const_cast<PDP*>(this);
    non_const_this->updateRD(blk->getSet(), blk->getTag());
    updateAccess(replacement_data);
}

void
PDP::reset(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    std::shared_ptr<PDPReplData> casted_replacement_data =
        std::static_pointer_cast<PDPReplData>(replacement_data);

    // Mark entry as ready to be used
    casted_replacement_data->valid = true;
    casted_replacement_data->inserted = true;

    //TODO: Check here for access
    CacheBlk *blk = casted_replacement_data->blk;
    auto *non_const_this = const_cast<PDP*>(this);
    non_const_this->updateRD(blk->getSet(), blk->getTag());

    updateAccess(replacement_data);
    casted_replacement_data->rrpv.saturate();
}

void 
PDP::updateRD(int set, Addr tag)
{
    auto &access_queue = perSetAccessQueue[set];
    
    std::vector<Addr> search_vec = {tag};
    auto it = std::find_end(access_queue.begin(), access_queue.end(), search_vec.begin(), search_vec.end());

    if(it != access_queue.end()){
        int rd = access_queue.size() - (access_queue.begin() - it);
        if(rd < maxRD){
            RDCounter[rd]++;
        }
    }

    access_queue.push_back(tag);
    
    if(access_queue.size() > maxAccessDepth){
        access_queue.erase(access_queue.begin());
    }

}


ReplaceableEntry*
PDP::getVictim(const ReplacementCandidates& candidates) const
{
    // There must be at least one replacement candidate
    assert(candidates.size() > 0);

    // Use first candidate as dummy victim
    ReplaceableEntry* victim = candidates[0];

    ReplaceableEntry* inserted_victim = NULL;
    int inserted_victim_RRPV = 0;

    // Store victim->rrpv in a variable to improve code readability
    int victim_RRPV = std::static_pointer_cast<PDPReplData>(
                        victim->replacementData)->rrpv;

    // Visit all candidates to find victim
    for (const auto& candidate : candidates) {
        std::shared_ptr<PDPReplData> candidate_repl_data =
            std::static_pointer_cast<PDPReplData>(
                candidate->replacementData);

        // Stop searching for victims if an invalid entry is found
        if (!candidate_repl_data->valid) {
            return candidate;
        }

        // Update victim entry if necessary
        int candidate_RRPV = candidate_repl_data->rrpv;
        if (candidate_RRPV > victim_RRPV) {
            victim = candidate;
            victim_RRPV = candidate_RRPV;
        }

        if(!inserted_victim || (inserted_victim && candidate_repl_data->inserted && candidate_RRPV > inserted_victim_RRPV)){
            inserted_victim = candidate;
            inserted_victim_RRPV = candidate_RRPV;
        }
    }

    if(inserted_victim){
        return inserted_victim;
    }


    // Get difference of victim's RRPV to the highest possible RRPV in
    // order to update the RRPV of all the other entries accordingly
    std::static_pointer_cast<PDPReplData>(
        victim->replacementData)->rrpv.saturate();

    return victim;
}

std::shared_ptr<ReplacementData>
PDP::instantiateEntry()
{
    return std::shared_ptr<ReplacementData>(new PDPReplData(numRRPVBits, NULL));
}

std::shared_ptr<ReplacementData>
PDP::instantiateEntry(CacheBlk *blk)
{
    return std::shared_ptr<ReplacementData>(new PDPReplData(numRRPVBits, blk));
}

} // namespace replacement_policy
} // namespace gem5
