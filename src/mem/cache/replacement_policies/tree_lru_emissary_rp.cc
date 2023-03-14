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

#include "mem/cache/replacement_policies/tree_lru_emissary_rp.hh"

#include <cassert>
#include <memory>
#include <map>

#include "params/TreeLRUEmissaryRP.hh"
#include "sim/core.hh"
#include "base/trace.hh"
#include "base/output.hh"


namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(ReplacementPolicy, replacement_policy);
namespace replacement_policy
{

/**
 * Get the index of the parent of the given indexed subtree.
 *
 * @param Index of the queried tree.
 * @return The index of the parent tree.
 */
static uint64_t
parentIndex(const uint64_t index)
{
    return std::floor((index-1)/2);
}

/**
 * Get index of the subtree on the left of the given indexed tree.
 *
 * @param index The index of the queried tree.
 * @return The index of the subtree to the left of the queried tree.
 */
static uint64_t
leftSubtreeIndex(const uint64_t index)
{
    return 2*index + 1;
}

/**
 * Get index of the subtree on the right of the given indexed tree.
 *
 * @param index The index of the queried tree.
 * @return The index of the subtree to the right of the queried tree.
 */
static uint64_t
rightSubtreeIndex(const uint64_t index)
{
    return 2*index + 2;
}

/**
 * Find out if the subtree at index corresponds to the right or left subtree
 * of its parent tree.
 *
 * @param index The index of the subtree.
 * @return True if it is a right subtree, false otherwise.
 */
static bool
isRightSubtree(const uint64_t index)
{
    return index%2 == 0;
}


TreeLRUEmissary::TreeLRUEmissary(const Params &p)
    : Base(p),
    numLeaves(p.num_leaves), count(0), 
    treeInstance(nullptr), 
    ptreeInstance(nullptr), 
    lru_ways(p.lru_ways),
    preserve_ways(p.preserve_ways),
    last_tick(0),
    flush_freq_in_cycles(p.flush_freq_in_cycles)
{
        registerExitCallback([this]() { dumpPreserveHist(); });
}

void
TreeLRUEmissary::invalidate(const std::shared_ptr<ReplacementData>& replacement_data)
{
    // Cast replacement data
    std::shared_ptr<TreeLRUEmissaryReplData> treePLRU_replacement_data =
        std::static_pointer_cast<TreeLRUEmissaryReplData>(replacement_data);
    PLRUTree* tree = treePLRU_replacement_data->tree.get();

    // Index of the tree entry we are currently checking
    // Make this entry the new LRU entry
    uint64_t tree_index = treePLRU_replacement_data->index;

    // Parse and update tree to make it point to the new LRU
    do {
        // Store whether we are coming from a left or right node
        const bool right = isRightSubtree(tree_index);

        // Go to the parent tree node
        tree_index = parentIndex(tree_index);

        // Update parent node to make it point to the node we just came from
        tree->at(tree_index) = right;
    } while (tree_index != 0);
}

void 
TreeLRUEmissary::promote(const std::shared_ptr<ReplacementData>&
        replacement_data) const
{
    std::shared_ptr<TreeLRUEmissaryReplData> treePLRU_replacement_data =
        std::static_pointer_cast<TreeLRUEmissaryReplData>(replacement_data);
    PLRUTree* ptree = treePLRU_replacement_data->ptree.get();

    // Index of the tree entry we are currently checking
    // Make this entry the MRU entry
    uint64_t tree_index = treePLRU_replacement_data->index;

    // Parse and update tree to make every bit point away from the new MRU
    do {
        // Store whether we are coming from a left or right node
        const bool right = isRightSubtree(tree_index);

        // Go to the parent tree node
        tree_index = parentIndex(tree_index);

        // Update node to not point to the touched leaf
        ptree->at(tree_index) = !right;
    } while (tree_index != 0);

    return;
}

void
TreeLRUEmissary::updateTree(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto *non_const_this = const_cast<TreeLRUEmissary*>(this);
    non_const_this->checkToFlushPreserveBits();

    auto repl_data = static_pointer_cast<TreeLRUEmissaryReplData>(replacement_data);
    CacheBlk *cur_blk = repl_data->blk;
    bool preserve = cur_blk->isPreserve();

    // Cast replacement data
    std::shared_ptr<TreeLRUEmissaryReplData> treePLRU_replacement_data =
        std::static_pointer_cast<TreeLRUEmissaryReplData>(replacement_data);
    PLRUTree* tree = treePLRU_replacement_data->tree.get();

    // Index of the tree entry we are currently checking
    // Make this entry the MRU entry
    uint64_t tree_index = treePLRU_replacement_data->index;

    // Parse and update tree to make every bit point away from the new MRU
    do {
        // Store whether we are coming from a left or right node
        const bool right = isRightSubtree(tree_index);

        // Go to the parent tree node
        tree_index = parentIndex(tree_index);

        // Update node to not point to the touched leaf
        tree->at(tree_index) = !right;
    } while (tree_index != 0);
}

void
TreeLRUEmissary::touch(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto *non_const_this = const_cast<TreeLRUEmissary*>(this);
    non_const_this->checkToFlushPreserveBits();

    auto repl_data = static_pointer_cast<TreeLRUEmissaryReplData>(replacement_data);
    CacheBlk *cur_blk = repl_data->blk;
    bool preserve = cur_blk->isPreserve();

    //TODO: Call promote
    //if(preserve){
    //    //promote(replacement_data);
    //    return;
    //}

    // Cast replacement data
    std::shared_ptr<TreeLRUEmissaryReplData> treePLRU_replacement_data =
        std::static_pointer_cast<TreeLRUEmissaryReplData>(replacement_data);
    PLRUTree* tree = treePLRU_replacement_data->tree.get();

    // Index of the tree entry we are currently checking
    // Make this entry the MRU entry
    uint64_t tree_index = treePLRU_replacement_data->index;

    // Parse and update tree to make every bit point away from the new MRU
    do {
        // Store whether we are coming from a left or right node
        const bool right = isRightSubtree(tree_index);

        // Go to the parent tree node
        tree_index = parentIndex(tree_index);

        // Update node to not point to the touched leaf
        tree->at(tree_index) = !right;
    } while (tree_index != 0);
}

void
TreeLRUEmissary::reset(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    // A reset has the same functionality of a touch
    touch(replacement_data);
}



ReplaceableEntry*
TreeLRUEmissary::getVictim(const ReplacementCandidates& candidates) const
{
    // There must be at least one replacement candidate
    assert(candidates.size() > 0);

    // Get tree
    const PLRUTree* tree = std::static_pointer_cast<TreeLRUEmissaryReplData>(
            candidates[0]->replacementData)->tree.get();
    const PLRUTree* ptree = std::static_pointer_cast<TreeLRUEmissaryReplData>(
            candidates[0]->replacementData)->ptree.get();
    ReplaceableEntry* victim;
    CacheBlk *victim_blk;

    // Index of the tree entry we are currently checking. Start with root.
    uint64_t tree_index = 0;

    uint8_t numpreserve = 0;

    bool resetPreservedWays = true;
    bool resetNonPreservedWays = true;

    // Find the LRU line among ones in LRU Mode
    for (const auto& candidate : candidates) {
        CacheBlk *blk = reinterpret_cast<CacheBlk*>(candidate);

        if(blk->isPreserve()){
            numpreserve++;
        }
    }

    if(numpreserve > preserve_ways){

        do{
            tree_index = 0;
            // Parse tree
            while (tree_index < tree->size()) {
                // Go to the next tree entry
                if (ptree->at(tree_index)) {
                    tree_index = rightSubtreeIndex(tree_index);
                } else {
                    tree_index = leftSubtreeIndex(tree_index);
                }
            }
            victim = candidates[tree_index - (numLeaves - 1)];
            victim_blk = reinterpret_cast<CacheBlk*>(victim);
            if(!victim_blk->isPreserve()){
                DPRINTF(EMISSARY,"P: victim block not preserved way %d\n", tree_index - (numLeaves - 1));
                auto candidate_repl_data = std::static_pointer_cast<TreeLRUEmissaryReplData>(victim->replacementData);
                promote(candidate_repl_data);
            }
        } while(!victim_blk->isPreserve());

        DPRINTF(EMISSARY,"P: victim set %d way %d\n", victim_blk->getSet(),  tree_index - (numLeaves - 1));
        return victim;
    }
     
    else{
        do{
            tree_index = 0;
            // Parse tree
            while (tree_index < tree->size()) {
                // Go to the next tree entry
                if (tree->at(tree_index)) {
                    tree_index = rightSubtreeIndex(tree_index);
                } else {
                    tree_index = leftSubtreeIndex(tree_index);
                }
            }
            victim = candidates[tree_index - (numLeaves - 1)];
            victim_blk = reinterpret_cast<CacheBlk*>(victim);
            if(victim_blk->isPreserve()){
                DPRINTF(EMISSARY,"Non-P: victim block preserved way %d\n", tree_index - (numLeaves - 1));
                auto candidate_repl_data = std::static_pointer_cast<TreeLRUEmissaryReplData>(victim->replacementData);
                updateTree(candidate_repl_data);
            }
        } while(victim_blk->isPreserve());

        // The tree index is currently at the leaf of the victim displaced by the
        // number of non-leaf nodes
        DPRINTF(EMISSARY,"Non-P: victim set %d way %d\n", victim_blk->getSet(),  tree_index - (numLeaves - 1));
        return victim;
    }
    
}

std::shared_ptr<ReplacementData>
TreeLRUEmissary::instantiateEntry()
{
    // Generate a tree instance every numLeaves created
    if (count % numLeaves == 0) {
        treeInstance = new PLRUTree(numLeaves - 1, false);
        ptreeInstance = new PLRUTree(numLeaves - 1, false);
    }

    // Create replacement data using current tree instance
    TreeLRUEmissaryReplData* treeLRUEmissaryReplData = new TreeLRUEmissaryReplData(NULL,
        (count % numLeaves) + numLeaves - 1,
        std::shared_ptr<PLRUTree>(treeInstance),
        std::shared_ptr<PLRUTree>(ptreeInstance));

    // Update instance counter
    count++;

    return std::shared_ptr<ReplacementData>(treeLRUEmissaryReplData);
}

std::shared_ptr<ReplacementData>
TreeLRUEmissary::instantiateEntry(CacheBlk *blk)
{
    // Generate a tree instance every numLeaves created
    if (count % numLeaves == 0) {
        treeInstance = new PLRUTree(numLeaves - 1, false);
        ptreeInstance = new PLRUTree(numLeaves - 1, false);
    }

    // Create replacement data using current tree instance
    TreeLRUEmissaryReplData* treeLRUEmissaryReplData = new TreeLRUEmissaryReplData(blk,
        (count % numLeaves) + numLeaves - 1,
        std::shared_ptr<PLRUTree>(treeInstance),
        std::shared_ptr<PLRUTree>(ptreeInstance));

    // Update instance counter
    count++;

    return std::shared_ptr<ReplacementData>(treeLRUEmissaryReplData);
}

void 
TreeLRUEmissary::checkToFlushPreserveBits(){

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
TreeLRUEmissary::dumpPreserveHist(){
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
