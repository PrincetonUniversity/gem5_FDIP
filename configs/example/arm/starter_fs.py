# Copyright (c) 2016-2017, 2020 ARM Limited
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""This script is the full system example script from the ARM
Research Starter Kit on System Modeling. More information can be found
at: http://www.arm.com/ResearchEnablement/SystemModeling
"""

import os
import m5
from m5.util import addToPath
from m5.objects import *
from m5.options import *
import argparse

m5.util.addToPath('../..')

from common import Options
from common import Simulation
from common import SysPaths
from common import ObjectList
from common import CacheConfig
from common import MemConfig
from common.cores.arm import HPI

import devices


default_kernel = 'vmlinux.arm64'
default_disk = 'linaro-minimal-aarch64.img'
default_root_device = '/dev/vda'


# Pre-defined CPU configurations. Each tuple must be ordered as : (cpu_class,
# l1_icache_class, l1_dcache_class, walk_cache_class, l2_Cache_class). Any of
# the cache class may be 'None' if the particular cache is not present.
cpu_types = {

    #"atomic" : ( AtomicSimpleCPU, None, None, None, None),
    #"atomic" : ( AtomicSimpleCPU, None, None, None, None),
    "atomic" : ( AtomicSimpleCPU,
               devices.L1I, devices.L1D,
               devices.WalkCache,
               devices.L2),
    "minor" : (MinorCPU,
               devices.L1I, devices.L1D,
               devices.WalkCache,
               devices.L2),
    "hpi" : ( HPI.HPI,
              HPI.HPI_ICache, HPI.HPI_DCache,
              HPI.HPI_WalkCache,
              HPI.HPI_L2),
    "ooo" : ( O3CPU,
               devices.L1I, devices.L1D,
               devices.WalkCache,
               devices.L2)
}

def create_cow_image(name):
    """Helper function to create a Copy-on-Write disk image"""
    image = CowDiskImage()
    image.child.image_file = SysPaths.disk(name)

    return image;


def create(args):
    ''' Create and configure the system object. '''

    if args.script and not os.path.isfile(args.script):
        print("Error: Bootscript %s does not exist" % args.script)
        sys.exit(1)

    want_caches = False
    if args.cpu_type == "O3CPU":
        (CPUClass, test_mem_mode, FutureClass) = Simulation.setCPUClass(args)
        print(test_mem_mode)
        cpu_class = CPUClass
        mem_mode = test_mem_mode
        args.cpu = "ooo"
        want_caches = True
    else:
        cpu_class = cpu_types[args.cpu][0]
        mem_mode = cpu_class.memory_mode()
        #mem_mode = "timing"
        want_caches = True
        #want_caches = True if mem_mode == "timing" else False

    # Only simulate caches when using a timing CPU (e.g., the HPI model)
    #want_caches = True

    system = devices.SimpleSystem(caches=want_caches,
                                  mem_size = args.mem_size,
                                  mem_mode=mem_mode,
                                  workload=ArmFsLinux(
                                      object_file=
                                      SysPaths.binary(args.kernel)),
                                  readfile=args.script,
                                  m1=args.m1)

    #CacheConfig.config_cache(args, system)
    MemConfig.config_mem(args, system)

    system.realview.vio[0].vio=VirtIOBlock(image=create_cow_image(args.disk_image))
    # Add the PCI devices we need for this system. The base system
    # doesn't have any PCI devices by default since they are assumed
    # to be added by the configuration scripts needing them.
    #system.pci_devices = [
    #    # Create a VirtIO block device for the system's boot
    #    # disk. Attach the disk image using gem5's Copy-on-Write
    #    # functionality to avoid writing changes to the stored copy of
    #    # the disk image.
    #    PciVirtIO(vio=VirtIOBlock(image=create_cow_image(args.disk_image))),
    #]

    ## Attach the PCI devices to the system. The helper method in the
    ## system assigns a unique PCI bus ID to each of the devices and
    ## connects them to the IO bus.
    #for dev in system.pci_devices:
    #    system.attach_pci(dev)

    # Wire up the system's memory system
    system.connect()

    # Add CPU clusters to the system
    system.cpu_cluster = [
        devices.CpuCluster(system,
                           args.num_cores,
                           args.cpu_freq, "1.0V",
                           *cpu_types[args.cpu],
                           l1i_rp=args.l1i_rp,
                           l2_rp=args.l2_rp,
                           preserve_ways=args.preserve_ways,
                           args=args),
    ]

    # Create a cache hierarchy for the cluster. We are assuming that
    # clusters have core-private L1 caches and an L2 that's shared
    # within the cluster.
    system.addCaches(want_caches, last_cache_level=3)

    # Setup gem5's minimal Linux boot loader.
    system.realview.setupBootLoader(system, SysPaths.binary, args.bootloader)
    #system.realview.setupBootLoader(system, SysPaths.binary)

    if args.dtb:
        system.workload.dtb_filename = args.dtb
    else:
        # No DTB specified: autogenerate DTB
        system.workload.dtb_filename = \
            os.path.join(m5.options.outdir, 'system.dtb')
        system.generateDtb(system.workload.dtb_filename)

    # Linux boot command flags
    kernel_cmd = [
        # Tell Linux to use the simulated serial port as a console
        "console=ttyAMA0",
        # Hard-code timi
        "lpj=19988480",
        # Disable address space randomisation to get a consistent
        # memory layout.
        "norandmaps",
        # Tell Linux where to find the root disk image.
        "root=%s" % args.root_device,
        # Mount the root disk read-write by default.
        "rw",
        # Tell Linux about the amount of physical memory present.
        "mem=%s" % args.mem_size,
    ]
    system.workload.command_line = " ".join(kernel_cmd)

    return system

def parse_stats(args):
    stats_file = os.path.join(m5.options.outdir,'stats.txt')
    stats_file_handle = open(stats_file,'r')
    found = False

    warmup_instcount = 5000000
    if args.warmup_insts:
        warmup_instcount = args.warmup_insts
    for line in stats_file_handle:
        if "simInsts" in line:
            toks = line.split()
            inst_count = int(toks[1])
            #print(toks)
            #print(inst_count)
            if(inst_count >= warmup_instcount):
                found = True
                break
    stats_file_handle.close()
    #os.remove(stats_file)
    #with open(stats_file,'w') as fp:
    #    pass
    return found

def run(args, switch_cpu_list=None, root=None):
    cptdir = m5.options.outdir
    if args.checkpoint:
        print("Checkpoint directory: %s" % cptdir)

    #while True:
    #    event = m5.simulate(5000000)
    #    exit_msg = event.getCause()
    #    m5.stats.dump()

    #    if "max instruction" in exit_msg:
    #        break
    #while True:
    #    event = m5.simulate()

    if not switch_cpu_list is None:
        event = m5.simulate()
        m5.switchCpus(root.system, switch_cpu_list)

        tmp_cpu_list = []
        for old_cpu, new_cpu in switch_cpu_list:
            tmp_cpu_list.append((new_cpu, old_cpu))
        switch_cpu_list = tmp_cpu_list

        print("REAL SIMULATION")
        m5.stats.dump()
        m5.stats.reset()
        event = m5.simulate()
        exit_msg = event.getCause()
        print(exit_msg, " @ ", m5.curTick())

    else:
        if args.warmup_insts:
            while True:
                event = m5.simulate(250000000)
                m5.stats.dump()
                if not switch_cpu_list is None:
                    m5.switchCpus(root.system, switch_cpu_list)
                    tmp_cpu_list = []
                    for old_cpu, new_cpu in switch_cpu_list:
                        tmp_cpu_list.append((new_cpu, old_cpu))
                    switch_cpu_list = tmp_cpu_list

                if(parse_stats(args)):
                    break

            # Reset stats and prepare to get final stats
            m5.stats.reset()
            m5.stats.outputList.clear()
            m5.stats.addStatVisitor("stats_final.txt")

        while True:
            event = m5.simulate()
            exit_msg = event.getCause()
            if exit_msg == "checkpoint":
                print("Dropping checkpoint at tick %d" % m5.curTick())
                cpt_dir = os.path.join(m5.options.outdir, "cpt.%d" % m5.curTick())
                m5.checkpoint(os.path.join(cpt_dir))
                print("Checkpoint done.")
            else:
                print(exit_msg, " @ ", m5.curTick())
                break

        m5.stats.dump()

    sys.exit(event.getCode())

def setup_switch_cpus(args, switch_cpus, cpu_cluster):

    for cpu,old_cpu in zip(switch_cpus, cpu_cluster.cpus):

        print(type(cpu))
        if type(cpu) == O3CPU:
            if args.fdip:
                cpu.enableFDIP = args.fdip

            if args.perfectICache:
                cpu.enablePerfectICache = args.perfectICache

            if args.starveAtleast:
                cpu.starveAtleast = args.starveAtleast

            if args.randomStarve:
                cpu.randomStarve = args.randomStarve

            if args.starveRandomness:
                cpu.starveRandomness = args.starveRandomness

            if args.pureRandom:
                cpu.pureRandom = args.pureRandom

            if args.dump_tms:
                cpu.dumpTms = args.dump_tms

            if args.dump_btbconf:
                cpu.dumpBTBConf = args.dump_btbconf

            if args.btbConfThreshold:
                cpu.btbConfThreshold = args.btbConfThreshold

            if args.btbConfMinInst:
                cpu.btbConfMinInst = args.btbConfMinInst

            if args.histRandom:
                cpu.histRandom = args.histRandom

            if args.fetchQSize:
                cpu.fetchQueueSize = args.fetchQSize

            if args.ftqSize >=0:
                cpu.ftqSize = args.ftqSize

            if args.ftqInst >0:
                cpu.ftqInst = args.ftqInst

            if args.oracleEMISSARY:
                cpu.oracleEMISSARY = args.oracleEMISSARY

            if args.oracleStarvationsFileName:
                cpu.oracleStarvationsFileName = args.oracleStarvationsFileName

            if args.oracleStarvationCountThreshold:
                cpu.oracleStarvationCountThreshold = args.oracleStarvationCountThreshold


            #if cpu_cluster._l1i_rp == "LRUEmissary" or cpu_cluster._l2_rp =="LRUEmissary":
            #    cpu.enableStarvationEMISSARY = True
            #cpu.enableStarvationEMISSARY = True
            cpu.enableEmissaryRetirement = True

            cpu.emissaryEnableIQEmpty = True

            if args.totalSimInsts:
                cpu.totalSimInsts = args.totalSimInsts
                #if args.warmup_insts:
                #    cpu.totalSimInsts += args.warmup_insts

            # 0: ORACLE
            # 1: LRU
            # 2: RANDOM
            # 3: NONE
            if args.opt:
                cpu.cache_repl = 0

            if args.numSets:
                cpu.numSets = args.numSets

            #cpu.mmu = old_cpu.mmu
            #cpu.icache_port = old_cpu.icache_port
            #cpu.dcache_port = old_cpu.dcache_port

        if args.maxinsts:
            cpu.max_insts_any_thread = args.maxinsts
            #if args.warmup_insts:
            #    cpu.max_insts_any_thread += args.warmup_insts


        cpu.branchPred = old_cpu.branchPred
        #cpu.brancPred.indirectBranchPred = old_cpu.brancPred.indirectBranchPred
        #if args.bp_type:
        #    bpClass = ObjectList.bp_list.get(args.bp_type)
        #    cpu.branchPred = bpClass()
        #    if args.btb_entries:
        #        cpu.branchPred.BTBEntries = args.btb_entries
        #        #cpu.branchPred.BTBEntries = 64*1024*1024
        #        cpu.branchPred.BTBTagSize = 64 - (math.log(cpu.branchPred.BTBEntries,2) + 2)
        #    #indirectBPClass = ObjectList.indirect_bp_list.get('SimpleIndirectPredictor')
        #    indirectBPClass = ObjectList.indirect_bp_list.get('ITTAGE')
        #    cpu.branchPred.indirectBranchPred = indirectBPClass()



def main():
    parser = argparse.ArgumentParser(epilog=__doc__)

    parser.add_argument("--dtb", type=str, default=None,
                        help="DTB file to load")
    parser.add_argument("--kernel", type=str, default=default_kernel,
                        help="Linux kernel")
    parser.add_argument("--disk-image", type=str,
                        default=default_disk,
                        help="Disk to instantiate")
    parser.add_argument("--bootloader", type=str,
                        default="boot.arm64",
                        help="bootloader")
    parser.add_argument("--root-device", type=str,
                        default=default_root_device,
                        help="OS device name for root partition (default: {})"
                             .format(default_root_device))
    parser.add_argument("--script", type=str, default="",
                        help = "Linux bootscript")
    parser.add_argument("--cpu", type=str, choices=list(cpu_types.keys()),
                        default="atomic",
                        help="CPU model to use")
    parser.add_argument("--cpu-freq", type=str, default="2GHz")
    parser.add_argument("--m1", default=False, action="store_true")
    parser.add_argument("--opt", default=False, action="store_true")
    parser.add_argument("--numSets", type=int, default=64,
                        help="Number of L1i sets")
    parser.add_argument("--num-cores", type=int, default=1,
                        help="Number of CPU cores")
    #parser.add_argument("--mem-type", default="DDR3_1600_8x8",
    #                    choices=ObjectList.mem_list.get_names(),
    #                    help = "type of memory to use")
    #parser.add_argument("--mem-channels", type=int, default=1,
    #                    help = "number of memory channels")
    #parser.add_argument("--mem-ranks", type=int, default=None,
    #                    help = "number of memory ranks per channel")
    #parser.add_argument("--mem-size", action="store", type=str,
    #                    default="1GB",
    #                    help="Specify the physical memory size")
    parser.add_argument("--checkpoint", action="store_true")
    parser.add_argument("--restore", type=str, default=None)


    Options.addCommonOptions(parser)
    args = parser.parse_args()
    print(args)
    root = Root(full_system=True)
    root.system = create(args)


    switch_cpus = [ O3CPU(switched_out=True, cpu_id=idx)
                  for idx in range(args.num_cores) ]

    #switch_cpus = root.system.cpu_cluster[0].switch_cpus
    root.system.switch_cpus = switch_cpus
    #root.system.cpus = root.system.cpu_cluster[0].cpus

    switch_cpu_list = [(root.system.cpu_cluster[0].cpus[i], switch_cpus[i]) for i in range(args.num_cores)]

    for i in range(args.num_cores):
        switch_cpus[i].isa = root.system.cpu_cluster[0].cpus[i].isa
        switch_cpus[i].system = root.system
        switch_cpus[i].clk_domain = root.system.cpu_cluster[0].cpus[i].clk_domain

    setup_switch_cpus(args, switch_cpus,root.system.cpu_cluster[0])

    if args.restore is not None:
        m5.instantiate(args.restore)
    else:
        m5.instantiate()

    #(TestCPUClass, test_mem_mode, FutureClass) = Simulation.setCPUClass(args)
    #print(root.system.cpu_cluster[0].num_cpus)
    run(args, switch_cpu_list, root)
    #run(args)
    #Simulation.run(args, root, root.system, FutureClass)


if __name__ == "__m5_main__":
    main()
