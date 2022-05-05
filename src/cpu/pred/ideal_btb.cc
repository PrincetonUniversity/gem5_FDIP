/*
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
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

#include "cpu/pred/ideal_btb.hh"

#include "base/intmath.hh"
#include "base/trace.hh"
#include "debug/Fetch.hh"

namespace gem5
{

namespace branch_prediction
{

IdealBTB::IdealBTB(unsigned _numEntries,
                       unsigned _tagBits,
                       unsigned _instShiftAmt,
                       unsigned _num_threads)
    : numEntries(_numEntries),
      tagBits(_tagBits),
      // Nayana commented
      instShiftAmt(_instShiftAmt),
      //instShiftAmt(0),
      log2NumThreads(floorLog2(_num_threads))
{
    DPRINTF(Fetch, "BTB: Creating BTB object.\n");

    if (!isPowerOf2(numEntries)) {
        fatal("BTB entries is not a power of 2!");
    }

    //btb.resize(numEntries);

    //for (unsigned i = 0; i < numEntries; ++i) {
    //    btb[i].valid = false;
    //}

    idxMask = numEntries - 1;

    tagMask = (1ULL << tagBits) - 1;

    tagShiftAmt = instShiftAmt + floorLog2(numEntries);
}

void
IdealBTB::reset()
{
    //for (unsigned i = 0; i < numEntries; ++i) {
    //    btb[i].valid = false;
    //}
}

inline
uint64_t
IdealBTB::getIndex(Addr instPC, ThreadID tid)
{
    // Need to shift PC over by the word offset.
    return instPC;
    //return ((instPC >> instShiftAmt)
    //        ^ (tid << (tagShiftAmt - instShiftAmt - log2NumThreads)))
    //        & idxMask;
}

inline
Addr
IdealBTB::getTag(Addr instPC)
{
    DPRINTF(Fetch, "instPC: %#x BTB Tag: %#x tagShitfAmt: %d tagMask: %#x\n",instPC, (instPC >> tagShiftAmt) & tagMask, tagShiftAmt, tagMask);
    return instPC;
    //return (instPC >> tagShiftAmt) & tagMask;
}

bool
IdealBTB::type(Addr instPC, ThreadID tid)
{
    uint64_t btb_idx = getIndex(instPC, tid);

    Addr inst_tag = getTag(instPC);

    //assert(btb_idx < numEntries);

    if (btb[btb_idx].valid
        && inst_tag == btb[btb_idx].tag
        && btb[btb_idx].tid == tid) {
        return btb[btb_idx].uncond;
    } else {
        return false;
    }
}


bool
IdealBTB::valid(Addr instPC, ThreadID tid)
{
    uint64_t btb_idx = getIndex(instPC, tid);

    Addr inst_tag = getTag(instPC);

    //assert(btb_idx < numEntries);

    if (btb[btb_idx].valid
        && inst_tag == btb[btb_idx].tag
        && btb[btb_idx].tid == tid) {
        return true;
    } else {
        return false;
    }
}

int
IdealBTB::getBblIndex(Addr instPC, ThreadID tid){
    uint64_t btb_idx = getIndex(instPC, tid);


    Addr inst_tag = getTag(instPC);

    //assert(btb_idx < numEntries);
    unsigned MAX_COUNT = 64;
    
    for( unsigned i = 0; i < MAX_COUNT; i++ ){
        if (btb[btb_idx].valid
          && inst_tag == btb[btb_idx].tag
          && btb[btb_idx].tid == tid) {
            if (instPC < btb[btb_idx].branch.instAddr() 
              && (instPC >> 6)	== (btb[btb_idx].branch.instAddr() >>6)){
	          DPRINTF(Fetch, "Bgodala found btb_index:%d for instPC 0x%lx and BranchPC: 0x%lx\n",btb_idx, instPC, btb[btb_idx].branch.instAddr());
	          return btb_idx;
	      }
	}
        btb_idx = (btb_idx + 1)%numEntries;
    }
    return -1;
}

StaticInstPtr
IdealBTB::lookupBranchFromIndex(unsigned idx, ThreadID tid)
{
    //assert(idx < numEntries);

    if (btb[idx].valid
        && btb[idx].tid == tid) {
        return btb[idx].staticBranchInst;
    } else {
        return 0;
    }
}

TheISA::PCState
IdealBTB::lookupBranchPCFromIndex(unsigned idx, ThreadID tid)
{
    //assert(idx < numEntries);

    if (btb[idx].valid
        && btb[idx].tid == tid) {
        return btb[idx].branch;
    } else {
        return 0;
    }
}
// @todo Create some sort of return struct that has both whether or not the
// address is valid, and also the address.  For now will just use addr = 0 to
// represent invalid entry.
// Nayana: This returns branchPC of the basic block
StaticInstPtr
IdealBTB::lookupBranch(Addr instPC, ThreadID tid)
{
    uint64_t btb_idx = getIndex(instPC, tid);

    Addr inst_tag = getTag(instPC);

    //assert(btb_idx < numEntries);

    if (btb[btb_idx].valid
        && inst_tag == btb[btb_idx].tag
        && btb[btb_idx].tid == tid) {
        return btb[btb_idx].staticBranchInst;
    } else {
        return 0;
    }
}

TheISA::PCState
IdealBTB::lookupBranchPC(Addr instPC, ThreadID tid)
{
    uint64_t btb_idx = getIndex(instPC, tid);

    Addr inst_tag = getTag(instPC);

    //assert(btb_idx < numEntries);

    if (btb[btb_idx].valid
        && inst_tag == btb[btb_idx].tag
        && btb[btb_idx].tid == tid) {
        return btb[btb_idx].branch;
    } else {
        return 0;
    }
}

uint64_t
IdealBTB::lookupBblSize(Addr instPC, ThreadID tid)
{
    uint64_t btb_idx = getIndex(instPC, tid);

    Addr inst_tag = getTag(instPC);

    //assert(btb_idx < numEntries);

    if (btb[btb_idx].valid
        && inst_tag == btb[btb_idx].tag
        && btb[btb_idx].tid == tid) {
        return btb[btb_idx].bblSize;
    } else {
        return 0;
    }
}

TheISA::PCState
IdealBTB::lookup(Addr instPC, ThreadID tid)
{
    uint64_t btb_idx = getIndex(instPC, tid);

    Addr inst_tag = getTag(instPC);

    //assert(btb_idx < numEntries);

    if (btb[btb_idx].valid
        && inst_tag == btb[btb_idx].tag
        && btb[btb_idx].tid == tid) {
        return btb[btb_idx].target;
    } else {
        return 0;
    }
}

TheISA::PCState
IdealBTB::lookupFT(Addr instPC, ThreadID tid)
{
    uint64_t btb_idx = getIndex(instPC, tid);

    Addr inst_tag = getTag(instPC);

    //assert(btb_idx < numEntries);

    if (btb[btb_idx].valid
        && inst_tag == btb[btb_idx].tag
        && btb[btb_idx].tid == tid) {
        return btb[btb_idx].fallthrough;
    } else {
        return 0;
    }
}

void
IdealBTB::update(Addr instPC, const TheISA::PCState &target, ThreadID tid)
{
    uint64_t btb_idx = getIndex(instPC, tid);

    //assert(btb_idx < numEntries);

    btb[btb_idx].tid = tid;
    btb[btb_idx].valid = true;
    btb[btb_idx].target = target;
    btb[btb_idx].tag = getTag(instPC);
}

void
IdealBTB::update(Addr instPC, const StaticInstPtr &staticBranchInst, 
                   const TheISA::PCState &branch,
                   const uint64_t bblSize, const TheISA::PCState &target, 
                   const TheISA::PCState &ft, bool uncond, ThreadID tid)
{
    uint64_t btb_idx = getIndex(instPC, tid);
    
        DPRINTF(Fetch, "BTB update btb_index: %llu staticBrancInst 0x%lx target %s branchPC: %s\n", 
		    btb_idx, &*staticBranchInst, target, branch);

    //assert(btb_idx < numEntries);

    btb[btb_idx].tid = tid;
    btb[btb_idx].valid = true;
    btb[btb_idx].staticBranchInst = staticBranchInst;
    btb[btb_idx].branch = branch;
    btb[btb_idx].bblSize = bblSize;
    btb[btb_idx].target = target;
    btb[btb_idx].fallthrough = ft;
    btb[btb_idx].tag = getTag(instPC);
    btb[btb_idx].uncond = uncond;

    ////Bgodala
    ////Update the leader BTB Entry when ever update to any entry happens
    ////A leader entry is the first entry of a block
    //Addr block = (instPC >> 6) << 6;
    ////Do nothing if instPC is the leader
    //if ( block == instPC ){
    //  return;
    //}

    //uint64_t leader_btb_idx = getIndex(block, tid);

    ////If the leader entry is a valid entry then check if the branch PC it is pointing to
    ////is the closest one in the block
    //if (btb[leader_btb_idx].valid){
    //}else{
    //    btb[leader_btb_idx].tid = tid;
    //    btb[leader_btb_idx].valid = true;
    //    btb[leader_btb_idx].staticBranchInst = staticBranchInst;
    //    btb[leader_btb_idx].branch = instPC;
    //    btb[leader_btb_idx].bblSize = bblSize;
    //    btb[leader_btb_idx].target = target;
    //    btb[leader_btb_idx].fallthrough = ft;
    //    btb[leader_btb_idx].tag = getTag(block);
    //    btb[leader_btb_idx].uncond = uncond;
    //}
}
} // namespace branch_prediction
} // namespace gem5
