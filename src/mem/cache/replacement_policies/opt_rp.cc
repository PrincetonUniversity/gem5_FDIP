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

#include "mem/cache/replacement_policies/opt_rp.hh"

#include <cassert>
#include <memory>

#include "params/OPTRP.hh"
#include "sim/cur_tick.hh"

namespace gem5
{


GEM5_DEPRECATED_NAMESPACE(ReplacementPolicy, replacement_policy);
namespace replacement_policy
{

using namespace std;

OPT::OPT(const Params &p)
  : Base(p)
{
}

void
OPT::invalidate(const std::shared_ptr<ReplacementData>& replacement_data)
{
    // Reset last touch timestamp
    std::static_pointer_cast<OPTReplData>(
        replacement_data)->lastTouchTick = Tick(0);
}

void
OPT::touch(const std::shared_ptr<ReplacementData>& replacement_data) const 
{
   auto *non_const_this = const_cast<OPT*>(this);

   non_const_this->touch(replacement_data);
}


void
OPT::touch(const std::shared_ptr<ReplacementData>& replacement_data) 
{
    // Update last touch timestamp
    std::static_pointer_cast<OPTReplData>(
        replacement_data)->lastTouchTick = curTick();

    auto opt_repl_data = static_pointer_cast<OPTReplData>(replacement_data);

    CacheBlk *blk = opt_repl_data->blk;

    //warn("ORACLE WORKING numSets:%d", numSets);
    //Addr a = instruction->instAddr() >> 6;
    //unsigned int ind = (unsigned)(a & 0x3f);
    uint32_t ind = blk->getSet(); 
    Addr a = blk->getTag();
    //DPRINTFN("OPT:touch TAG is %#x\n",a);
    //DPRINTFN("OPT:touch set:%d and way:%d\n",blk->getSet(), blk->getWay());
    a = a << 6;
    a = a | (ind & 0x3f);
    // check oneMisses
    //DPRINTFN("OPT:touch addr is %#x\n", (a << 6));
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
            vector<Addr>::iterator in_e = std::find(om->begin(), om->begin() + om->size(), new_evicts[e]);
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
        vector<Addr>::iterator in_om = std::find(om->begin(), om->end(), a);
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
        for (int e = 0; e < new_evicts.size(); e++) {
            vector<Addr>::iterator e_in_c = std::find(in_cache[ind].begin(), in_cache[ind].end(), new_evicts[e]);
            if (e_in_c != in_cache[ind].end()) {
                in_cache[ind].erase(e_in_c);
            }
        }
    // check in cache
    bool add_om = false;
     vector<Addr>::iterator in_c = std::find(in_cache[ind].begin(), in_cache[ind].end(), a);
    if (in_c == in_cache[ind].end()) {
        in_cache[ind].push_back(a);
        add_om = true;
    //    repl_misses++;
    } else {
        in_cache[ind].erase(in_c);
        in_cache[ind].push_back(a);
    //    repl_hits++;
    }
    // add oneMiss or not
    if (add_om && in_cache[ind].size() > NUM_WAYS) {
        // add oneMiss
        vector<Addr> om(in_cache[ind].begin(), in_cache[ind].end() - 1);
        oneMisses[ind].push_back(om);
    }

    // debug
    //cout << "cache cap: " << dec << in_cache[ind].size() << endl;
    //cout << "in_cache[" << dec << ind << "]: [";
    assert(in_cache[ind].size() <= 256);
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
        Addr e_fetchBufferBlockPC = new_evicts[e] << 6;
        Addr e_pc = e_fetchBufferBlockPC;
        //DPRINTFN("OPT:touch evict addr is %#x\n",e_pc);

        if(new_evicts[e] == a){
            continue;
        }
        CacheBlk *blk = tags->findBlock(e_pc, false); 
        if(blk){
            //DPRINTFN("OPT:touch blk found set:%#x tag:%#x\n",blk->getSet(), blk->getTag());
            cache->invalidateBlock(blk);
        }
    }
    // end of oracle cache repl mod
    ////////////////////////////////

}

void
OPT::reset(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    // Set last touch timestamp
    std::static_pointer_cast<OPTReplData>(
        replacement_data)->lastTouchTick = curTick();

   auto *non_const_this = const_cast<OPT*>(this);

   non_const_this->touch(replacement_data);
}

void
OPT::cleanup(CacheBlk *blk){

    if(!blk || !blk->isValid())
        return;

    uint32_t ind = blk->getSet(); 
    Addr a = blk->getTag();
    a = a << 6;
    a = a | (ind & 0x3f);
    
    vector<Addr> new_evicts;
    new_evicts.push_back(a);

    for (int m = 0; m < oneMisses[ind].size(); m++) {
        vector<Addr> *om = &(oneMisses[ind][m]);
        //cout << "om #" << m << " possible evicts: " << possible_evicts << endl;
        // evicts check
        for (int e = 0; e < new_evicts.size(); e++) {
            vector<Addr>::iterator in_e = std::find(om->begin(), om->begin() + om->size(), new_evicts[e]);
            if (in_e != (om->begin() + om->size())) { // is in evicts
                om->erase(in_e);
                if (om->size() == 1) {
                    new_evicts.push_back((*om)[0]);
                    oneMisses[ind].erase(oneMisses[ind].begin() + m);
                    break;
                }
            }
        }
    }
 
    for (int e = 0; e < new_evicts.size(); e++) {
        vector<Addr>::iterator e_in_c = std::find(in_cache[ind].begin(), in_cache[ind].end(), new_evicts[e]);
        if (e_in_c != in_cache[ind].end()) {
            in_cache[ind].erase(e_in_c);
        }
    }
    
    //Remvove first element so that it is not invalidated
    //First element is the vctim so the invaildation is taken care by that
    new_evicts.erase(new_evicts.begin());
    
    // evict blocks!
    for (int e = 0; e < new_evicts.size(); e++) {
        //cout << "evict addr: " << hex << (new_evicts[e] << 6) << endl;
        Addr e_fetchBufferBlockPC = new_evicts[e] << 6;
        Addr e_pc = e_fetchBufferBlockPC;
        //DPRINTFN("OPT:touch evict addr is %#x\n",e_pc);

        CacheBlk *blk = tags->findBlock(e_pc, false); 
        if(blk){
            //DPRINTFN("OPT:touch blk found set:%#x tag:%#x\n",blk->getSet(), blk->getTag());
            cache->invalidateBlock(blk);
        }
    }

}

ReplaceableEntry*
OPT::getVictim(const ReplacementCandidates& candidates) const
{
    // There must be at least one replacement candidate
    assert(candidates.size() > 0);

    // Visit all candidates to find victim
    ReplaceableEntry* victim = candidates[0];
    for (const auto& candidate : candidates) {
        // Update victim entry if necessary
        if (std::static_pointer_cast<OPTReplData>(
                    candidate->replacementData)->lastTouchTick <
                std::static_pointer_cast<OPTReplData>(
                    victim->replacementData)->lastTouchTick) {
            victim = candidate;
        }
    }
    CacheBlk *blk = reinterpret_cast<CacheBlk*>(victim);
   auto *non_const_this = const_cast<OPT*>(this);
   non_const_this->cleanup(blk);

    return victim;
}

std::shared_ptr<ReplacementData>
OPT::instantiateEntry()
{
    return std::shared_ptr<ReplacementData>(new OPTReplData(NULL));
}

std::shared_ptr<ReplacementData>
OPT::instantiateEntry(CacheBlk *blk)
{
    return std::shared_ptr<ReplacementData>(new OPTReplData(blk));
}

} // namespace replacement_policy
} // namespace gem5
