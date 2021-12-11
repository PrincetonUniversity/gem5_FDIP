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

#include "mem/cache/replacement_policies/mlp_lin_rp.hh"

#include <cassert>
#include <memory>

#include "params/MLPLINRP.hh"
#include "sim/core.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(ReplacementPolicy, replacement_policy);
namespace replacement_policy
{

MLPLIN::MLPLIN(const Params &p)
    : Base(p)
{
}

void
MLPLIN::invalidate(const std::shared_ptr<ReplacementData>& replacement_data)
{
    // Reset last touch timestamp
    std::static_pointer_cast<MLPLINReplData>(
        replacement_data)->lastTouchTick = Tick(0);
}

void
MLPLIN::touch(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    // Update last touch timestamp
    std::static_pointer_cast<MLPLINReplData>(
        replacement_data)->lastTouchTick = curTick();
}

void
MLPLIN::reset(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    // Set last touch timestamp
    std::static_pointer_cast<MLPLINReplData>(
        replacement_data)->lastTouchTick = curTick();
}

ReplaceableEntry*
MLPLIN::getVictim(const ReplacementCandidates& candidates) const
{
    // There must be at least one replacement candidate
    assert(candidates.size() > 0);

    ReplaceableEntry* lru = candidates[0];
    CacheBlk *lru_blk = reinterpret_cast<CacheBlk*>(lru);

    for (const auto& candidate : candidates) {
        CacheBlk *candidate_blk = reinterpret_cast<CacheBlk*>(candidate);
        if (std::static_pointer_cast<MLPLINReplData>(
                    candidate->replacementData)->lastTouchTick <
                std::static_pointer_cast<MLPLINReplData>(
      		    lru->replacementData)->lastTouchTick) {
            lru = candidate;
            lru_blk = candidate_blk;
        } 
    }
    lru_blk->recency_stack = 0;
	    
    for (int i=1; i<8; i++) {
	ReplaceableEntry* temp = candidates[0];
	CacheBlk *temp_blk = reinterpret_cast<CacheBlk*>(temp);
	bool found = false;

        for (const auto& candidate : candidates) {
	    CacheBlk *candidate_blk = reinterpret_cast<CacheBlk*>(candidate);
            if ((std::static_pointer_cast<MLPLINReplData>(candidate->replacementData)->lastTouchTick >
                 std::static_pointer_cast<MLPLINReplData>(lru->replacementData)->lastTouchTick) && 
		(!found || std::static_pointer_cast<MLPLINReplData>(candidate->replacementData)->lastTouchTick <
		           std::static_pointer_cast<MLPLINReplData>(temp->replacementData)->lastTouchTick)) {
		    temp = candidate;
		    temp_blk = candidate_blk;
		    found = true;
	    }
	}
	temp_blk->recency_stack = i;
	lru = temp;
    }

    // Visit all candidates to find victim
    ReplaceableEntry* victim = candidates[0];
    CacheBlk *victim_blk = reinterpret_cast<CacheBlk*>(victim);
    unsigned lambda = 4;

    for (const auto& candidate : candidates) {
        // Update victim entry if necessary
        CacheBlk *candidate_blk = reinterpret_cast<CacheBlk*>(candidate);
        if ((candidate_blk->recency_stack + (lambda * candidate_blk->miss_cost)) <
            (victim_blk->recency_stack + (lambda * victim_blk->miss_cost))) {
            victim = candidate;
            victim_blk = candidate_blk;
        }
    }

    return victim;
}

std::shared_ptr<ReplacementData>
MLPLIN::instantiateEntry()
{
    return std::shared_ptr<ReplacementData>(new MLPLINReplData());
}

} // namespace replacement_policy
} // namespace gem5
