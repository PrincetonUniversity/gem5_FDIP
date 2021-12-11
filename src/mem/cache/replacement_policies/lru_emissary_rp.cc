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

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(ReplacementPolicy, replacement_policy);
namespace replacement_policy
{

LRUEmissary::LRUEmissary(const Params &p)
    : Base(p)
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
    uint8_t numNotPreserved = 0;
    uint8_t numlru = 0;
    uint8_t numpreserve = 0;

    // Find the LRU line among ones in LRU Mode
    for (const auto& candidate : candidates) {
        CacheBlk *blk = reinterpret_cast<CacheBlk*>(candidate);
        if (!blk->rpMode) {
            numlru++;
            if (numlru == 1) {
                lruEntry = candidate;
            } else if (std::static_pointer_cast<LRUEmissaryReplData>(
                    candidate->replacementData)->lastTouchTick <
                std::static_pointer_cast<LRUEmissaryReplData>(
                    lruEntry->replacementData)->lastTouchTick) {
                lruEntry = candidate;
            }
        } else {
            numpreserve++;
            if (!blk->isPreserve()) {
                numNotPreserved++;
                if (numNotPreserved == 1) {
                    victimNotSt = candidate;
                } else if (std::static_pointer_cast<LRUEmissaryReplData>(
                        candidate->replacementData)->lastTouchTick <
                    std::static_pointer_cast<LRUEmissaryReplData>(
                        victimNotSt->replacementData)->lastTouchTick) {
                    victimNotSt = candidate;
                }
            }
            // Update victim entry if necessary
            if (numpreserve == 1) {
                victim = candidate;
            } else if (std::static_pointer_cast<LRUEmissaryReplData>(
                    candidate->replacementData)->lastTouchTick <
                std::static_pointer_cast<LRUEmissaryReplData>(
                    victim->replacementData)->lastTouchTick) {
                victim = candidate;
            }
        }
    }

    CacheBlk *lruBlk = reinterpret_cast<CacheBlk*>(lruEntry);
    // If there are no unpreserved entries in Preserve mode, return lru of lru mode
    if (!lruBlk->isPreserve() && (numNotPreserved==0 ||
                                 std::static_pointer_cast<LRUEmissaryReplData>(
                                 victimNotSt->replacementData)->lastTouchTick >
                                 std::static_pointer_cast<LRUEmissaryReplData>(
                                 lruEntry->replacementData)->lastTouchTick)) {
        return lruEntry;
    } else if (lruBlk->isPreserve() && (numNotPreserved==0 &&
                                 std::static_pointer_cast<LRUEmissaryReplData>(
                                 victim->replacementData)->lastTouchTick >
                                 std::static_pointer_cast<LRUEmissaryReplData>(
                                 lruEntry->replacementData)->lastTouchTick)) {
        return lruEntry;
    }

    lruBlk->rpMode = true;

    return (numNotPreserved>0) ? victimNotSt : victim;
}

std::shared_ptr<ReplacementData>
LRUEmissary::instantiateEntry()
{
    return std::shared_ptr<ReplacementData>(new LRUEmissaryReplData());
}

} // namespace replacement_policy
} // namespace gem5
