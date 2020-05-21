#!/usr/bin/env python

# Copyright (c) 2020, Arm Limited and Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Collect statistics with performance counters (PAPI)

import logging
import os
import stat
import subprocess
import platform
import sys
import argparse
import re
import json
from datetime import date
from itertools import product
from collections import defaultdict
import numpy as np

PAPI_rounds_x86_64 = [
    'PAPI_TOT_INS,PAPI_TOT_CYC',   # CPI, InsMIX
    'PAPI_TOT_INS,PAPI_L1_DCM,PAPI_L1_ICM,PAPI_L1_TCM,PAPI_L1_LDM,PAPI_L1_STM',  # L1 cache
    'PAPI_TOT_INS,PAPI_L2_DCM,PAPI_L2_ICM,PAPI_L2_TCM,PAPI_L2_STM,PAPI_L2_DCA',  # L2 cache
]

PAPI_rounds_aarch64 = [
    'PAPI_TOT_INS,PAPI_TOT_CYC,PAPI_L1_DCA,PAPI_L1_DCM',
    'PAPI_TOT_INS,PAPI_L1_ICM,PAPI_L1_ICA,PAPI_L2_DCM',
]


class StatsPerfCounters(object):

    p_PAPICounter = (
        r"PAPI_(?P<stat>[\w\d]+_[\w\d]+)\[(?P<tid>\d+)\]\[(?P<pid>\d+)\]\[(?P<bid>\d+)\]=(?P<value>\d+)")
    # E.g., INST_RETIRED[1][6260][3]=12557713 ==> stat[tid][pid][bid]=value

    def __init__(self, shfileN, iterations, nthreads, platform, barriers):

        self.logger = logging.getLogger('Perfcntrs.StatsPerfCounters')
        self.iterations = iterations
        self.nthreads = nthreads
        self.runningOn = platform  # Currently supports aarch64 or x86_64
        self.barriers = barriers  # if True we are collecting counters per parallel region
        self.filePrefix = shfileN[:-3]  # .sh extension out
        # repetitions out, only prefix remains
        self.filePrefix = self.filePrefix[:-len(str(self.iterations))]

        # The main class structure, the statistics is a list of dictionaries, one per thread:
        # stats per barrier; each barrier has a [defaultdict(list) for _ in range(nthreads)]]
        self.STATS = []
        # Each dictionary will be indexed by the event (e.g., CPU_CYCLES, INST_RETIRED, etc);
        # each element will be a list with an entry per iteration (iteration = execution).
        # Some counters are sampled several times (e.g., when gathering cache stats).
        # Thus, we keep tuples to compute metrics in a more accurate way.
        # The tuples always follow the format: TOT_INS, OTHER_STAT

        # we will parse each line of the file, compile to be faster
        self.p = re.compile(self.p_PAPICounter)

        _t_inst = []  # this will help us to construct the tuples

        # Parse all the files
        for i in range(self.iterations):

            try:
                _fni = '{}{}.papi'.format(self.filePrefix, i)
                _fi = open(_fni, 'r')
                self.logger.debug('Parsing file: {}'.format(_fni))

                for line in _fi:
                    m = re.search(self.p, line)
                    if m:
                        tid, pid, bid = int(m.group("tid")), int(
                            m.group("pid")), int(m.group("bid"))
                        stat, value = m.group("stat"), int(m.group("value"))
                        # self.logger.debug('Parsed: {}, {}, {}, {}, {}'.format(stat, tid, pid, bid, value))

                        if len(self.STATS) < (bid + 1):  # increase the list of barriers
                            # self.logger.debug('prev_len(STATS) %d', len(self.STATS))
                            self.STATS.append([defaultdict(list)
                                               for _ in range(nthreads)])
                            # self.logger.debug('len(STATS) %d', len(self.STATS))
                            _t_inst.append([0 for _ in range(self.nthreads)])

                        if stat == 'TOT_INS':
                            _t_inst[bid][tid] = value

                        # self.logger.info('bid %d tid %d stat %s', bid, tid, stat)
                        self.STATS[bid][tid][stat].append(
                            (_t_inst[bid][tid], value))

                    else:
                        self.logger.warn('Error parsing line: {}'.format(line))

                _fi.close()

            except OSError as err:
                self.logger.error('{}  _fni: {}'.format(err, _fni))
                continue

    def getNumberBarriers(self):
        # Returns the number of barriers parsed
        return len(self.STATS)

    # Obtain statistics
    def getTotalCycles(self, threadID, iteration=0, barrierID=0):
        # For a given iteration, return the cpu cycles for threadID and barrierID.
        # If barrierID is < 0, return the total number of cycles for threadID
        # across all barriers
        STAT = 'TOT_CYC'
        if barrierID < 0:
            return sum(map(lambda x: x[threadID][STAT][iteration][1], self.STATS))
        else:
            return self.STATS[barrierID][threadID][STAT][iteration][1]

    def getAvgCycles(self, threadID, barrierID=0, std=0):
        # For a given threadID and barrierID, return the average number of cycles across iterations
        STAT = 'TOT_CYC'
        cycles = list(
            map(lambda x: x[1], self.STATS[barrierID][threadID][STAT]))
        if std:
            return np.std(cycles, ddof=1)
        else:
            return np.mean(cycles)

    def getTotalInstructions(self, threadID, iteration=0, barrierID=0):
        # For a given iteration, return the number of instructions executed
        # for threadID and barrierID.
        # If barrierID is < 0, return the total number of executed instructions
        # for threadID across all barriers.
        STAT = 'TOT_INS'
        if barrierID < 0:
            return sum(map(lambda x: x[threadID][STAT][iteration][1], self.STATS))
        else:
            return self.STATS[barrierID][threadID][STAT][iteration][1]

    def getAvgInstructions(self, threadID, barrierID=0, std=0):
        # For a given threadID and barrierID, return the average number of
        # instructions across iterations
        STAT = 'TOT_INS'
        insts = list(
            map(lambda x: x[1], self.STATS[barrierID][threadID][STAT]))
        if std:
            return np.std(insts, ddof=1)
        else:
            return np.mean(insts)

    def getTotalL1DataMisses(self, threadID, iteration=0, barrierID=0):
        # For a given iteration, return the number of L1 data misses for threadID and barrierID.
        # If barrierID < 0, it returns the total number of L1 data cache misses for threadID across all barriers
        STATd = 'L1_DCM'
        if barrierID < 0:
            return sum(map(lambda x: x[threadID][STATd][iteration][1], self.STATS))
        else:
            return self.STATS[barrierID][threadID][STATd][iteration][1]

    def getAvgL1DataMisses(self, threadID, barrierID=0, std=0):
        # For a given threadID and barrierID, return the average number of
        # L1 data misses across iterations
        STATd = 'L1_DCM'
        misses = list(
            map(lambda x: x[1], self.STATS[barrierID][threadID][STATd]))
        if std:
            return np.std(misses, ddof=1)
        else:
            return np.mean(misses)

    def getTotalL1InstMisses(self, threadID, iteration=0, barrierID=0):
        # For a given iteration, return the number of L1 inst misses for threadID and barrierID.
        # If barrierID < 0, return the total number of L1 inst cache misses
        # for threadID across all barriers
        STATi = 'L1_ICM'
        if barrierID < 0:
            return sum(map(lambda x: x[threadID][STATi][iteration][1], self.STATS))
        else:
            return self.STATS[barrierID][threadID][STATi][iteration][1]

    def getAvgL1InstMisses(self, threadID, barrierID=0, std=0):
        # For a given threadID and barrierID, return the average number of
        # L1 instruction misses across iterations
        STATi = 'L1_ICM'
        misses = list(
            map(lambda x: x[1], self.STATS[barrierID][threadID][STATi]))
        if std:
            return np.std(misses, ddof=1)
        else:
            return np.mean(misses)

    def getTotalL1DataAccesses(self, threadID, iteration=0, barrierID=0):
        # For a given iteration, return the number of L1 data cache accesses
        # for threadID and barrierID
        # If barrierID < 0, return the total number of L1 data cache accesses
        # for threadID across all barriers
        # Only for aarch64
        if self.runningOn != 'aarch64':
            return 0.0
        STATd = 'L1_DCA'
        if barrierID < 0:
            return sum(map(lambda x: x[threadID][STATd][iteration][1], self.STATS))
        else:
            return self.STATS[barrierID][threadID][STATd][iteration][1]

    def getAvgL1DataAccesses(self, threadID, barrierID=0, std=0):
        # For a given threadID and barrierID, return the average number of
        # L1 data accesses across iterations
        # Only for aarch64
        if self.runningOn != 'aarch64':
            return 0.0
        STATd = 'L1_DCA'
        accesses = list(
            map(lambda x: x[1], self.STATS[barrierID][threadID][STATd]))
        if std:
            return np.std(accesses, ddof=1)
        else:
            return np.mean(accesses)

    def getTotalL1InstAccesses(self, threadID, iteration=0, barrierID=0):
        # For a given iteration, return the number of L1 inst cache accesses
        # for threadID and barrierID
        # If barrierID < 0, return the total number of L1 inst cache accesses
        # for threadID across all barriers
        # Only for aarch64
        if self.runningOn != 'aarch64':
            return 0.0
        STATi = 'L1_ICA'
        if barrierID < 0:
            return sum(map(lambda x: x[threadID][STATi][iteration][1], self.STATS))
        else:
            return self.STATS[barrierID][threadID][STATi][iteration][1]

    def getAvgL1InstAccesses(self, threadID, barrierID=0, std=0):
        # For a given threadID and barrierID, return the average number of
        # L1 inst accesses across iterations
        # Only for aarch64
        if self.runningOn != 'aarch64':
            return 0.0
        STATi = 'L1_ICA'
        accesses = list(
            map(lambda x: x[1], self.STATS[barrierID][threadID][STATi]))
        if std:
            return np.std(accesses, ddof=1)
        else:
            return np.mean(accesses)

    def getTotalL2DataMisses(self, threadID, iteration=0, barrierID=0):
        # For a given iteration, return the number of L2 data cache misses
        # for threadID and barrierID
        # If barrierID < 0, return the total number of L2 data cache misses for
        # threadID across all barriers
        STATd = 'L2_DCM'
        if barrierID < 0:
            return sum(map(lambda x: x[threadID][STATd][iteration][1], self.STATS))
        else:
            error = False
            if barrierID >= len(self.STATS):
                self.logger.error(
                    "GetTotalL2DataMisses: number of barriers does not match")
                error = True
            if threadID >= len(self.STATS[barrierID]):
                self.logger.error(
                    "GetTotalL2DataMisses: number of threads does not match")
                error = True
            if STATd not in self.STATS[barrierID][threadID]:
                self.logger.error("GetTotalL2DataMisses: " +
                                  STATd + " not in stats")
                error = True
            if iteration >= len(self.STATS[barrierID][threadID][STATd]):
                self.logger.error(
                    "GetTotalL2DataMisses: Iterations do not match")
                error = True

            if error:
                return 0
            else:
                return self.STATS[barrierID][threadID][STATd][iteration][1]

    def getAvgL2DataMisses(self, threadID, barrierID=0, std=0):
        # For a given threadID and barrierID, return the average number of
        # L2 data misses across iterations
        STATd = 'L2_DCM'
        misses = list(
            map(lambda x: x[1], self.STATS[barrierID][threadID][STATd]))
        if std:
            return np.std(misses, ddof=1)
        else:
            return np.mean(misses)

    def getTotalL2DataAccesses(self, threadID, iteration=0, barrierID=0):
        # For a given iteration, return the number of L2 data cache accesses for
        # threadID and barrierID
        # If barrierID < 0, return the total number of L2 data cache accesses for
        # threadID across all barriers
        if self.runningOn == 'aarch64':
            return 0.0
        STATd = 'L2_DCA'
        if barrierID < 0:
            return sum(map(lambda x: x[threadID][STATd][iteration][1], self.STATS))
        else:
            return self.STATS[barrierID][threadID][STATd][iteration][1]

    def getAvgL2DataAccesses(self, threadID, barrierID=0, std=0):
        # For a given threadID and barrierID, return the average number of
        # L2 data accesses across iterations
        if self.runningOn == 'aarch64':
            return 0.0
        STATd = 'L2_DCA'
        accesses = list(
            map(lambda x: x[1], self.STATS[barrierID][threadID][STATd]))
        if std:
            return np.std(accesses, ddof=1)
        else:
            return np.mean(accesses)


class Experiment(object):
    # Creates and runs experiment files to gather statistics based on performance counters
    def __init__(self, miniApp, nthreads, barriers, appParams, iterations=1, bpdir='.', \
                 outpath='./tmp', out_suffix='', libpath='./barrierpoint-libs', \
                 debug=False, dryRun=False, benchpath='./benchmarks'):

        # Define your own Logger flavour
        self.logger = logging.getLogger('Perfcntrs')
        self.debug = debug
        self.dryRun = dryRun

        # Attributes & properties
        self.miniApp = miniApp          # miniApp name
        self.nthreads = nthreads        # number of threads
        self.barriers = barriers        # running the analysis per parallel region
        self.iterations = iterations    # How many times the application is executed
        self.benchpath = benchpath  # benchamark path
        self.libpath = libpath  # BarrierPoint library parth
        self.out_suffix = out_suffix  # Suffix for output folders

        self.bpdir = bpdir  # BP methodology directory
        self.appParams = self.GetBenchParams(appParams, nthreads)

        # path to store the output files (PAPI results)
        if out_suffix:
            self._toolOutPath = "{}/{}-{}.{}t".format(
                outpath, miniApp, out_suffix, nthreads)
        else:
            self._toolOutPath = "{}/{}.{}t".format(outpath, miniApp, nthreads)

        # Get the running platform (aarch64 or x86 are supported)
        _plf = platform.uname()
        if _plf[-2] == 'aarch64':  # running on Arm
            self.runningOn = 'aarch64'
        elif _plf[-2] == 'x86_64':  # running on x86
            self.runningOn = 'x86_64'
        else:
            self.runningOn = 'Unknown'
        self.logger.info('Runnning on {} machine'.format(self.runningOn))

    @property
    def toolOutPath(self):
        return self._toolOutPath

    @toolOutPath.setter
    def toolOutPath(self, toolOutPath):
        self._toolOutPath = toolOutPath

    def GetBenchPath(self, benchpath, miniApp):
        if os.path.exists(benchpath + "/" + miniApp):
            return benchpath + "/" + miniApp
        else:
            self.logger.error(
                "App \"{}\" not found in benchpath {}".format(miniApp, benchpath))
            return None

    def GetBenchParams(self, appParams, nthreads):
        if "{}" in appParams:  # Substitute {} in app parameters for number of threads
            return appParams.format(nthreads)
        else:
            return appParams

    def CreateExperimentBatch(self):
        # Creates an experiment batch (.sh file) with the application/threads
        # to obtain performance counters for the parallel region (ROI)
        self.logger.info("Creating experiments bash scripts for {}.{}t".format(
            self.miniApp, self.nthreads))

        # Get the platform specific PAPI counters (aarch64 or x86 are supported)
        if self.runningOn == 'x86_64':
            self.PAPI_rounds = PAPI_rounds_x86_64
        elif self.runningOn == 'aarch64':
            self.PAPI_rounds = PAPI_rounds_aarch64
        else:
            self.logger.error('Unknown platform')
            return False

        # Try to create the ouput folder, if it does not exist
        if not os.path.isdir(self.toolOutPath):
            try:
                os.makedirs(self.toolOutPath)
            except OSError:
                self.logger.warn(
                    'Could not create output path {}. Storing results in root folder'.format(self.toolOutPath))
                self.toolOutPath = '.'

        # Create a .sh file with the experiment commands
        try:
            # Analysis per parallel region
            if self.barriers:
                if self.out_suffix:
                    self.shfileN = '{}/{}-{}.{}t.BP.rep{}.sh'.format(
                        self.toolOutPath, self.miniApp, self.out_suffix, self.nthreads, self.iterations)
                else:
                    self.shfileN = '{}/{}.{}t.BP.rep{}.sh'.format(
                        self.toolOutPath, self.miniApp, self.nthreads,
                        self.iterations)
            # Analysis of whole application
            else:
                if self.out_suffix:
                    self.shfileN = '{}/{}-{}.{}t.rep{}.sh'.format(
                        self.toolOutPath, self.miniApp, self.out_suffix, self.nthreads, self.iterations)
                else:
                    self.shfileN = '{}/{}.{}t.rep{}.sh'.format(
                        self.toolOutPath, self.miniApp, self.nthreads,
                        self.iterations)
            self.shfile = open(self.shfileN, 'w')
            self.logger.debug('Creating sh file: {}'.format(self.shfileN))

        except OSError as err:
            self.logger.error('{}  shfileN: {}'.format(err, self.shfileN))
            return False

        # Header of .sh file
        self.shfile.write('#!/bin/sh\n\n')
        self.shfile.write('# {} - {} threads - {} iteration(s)\n\n'
                          .format(self.miniApp, self.nthreads, self.iterations))

        self.shfile.write('# Exports\n')
        self.shfile.write('export BP_PERFCNTRS=1\n')
        self.shfile.write(
            'export BP_PERFCNTRS_VERBOSE={}\n'.format(1 if self.debug else 0))
        self.shfile.write('export BP_PERFCNTRS_OMPPARALLEL={}\n'.format(
            1 if self.barriers else 0))
        self.shfile.write('export BP_PERFCNTRS_SAMPLING={}\n'.format(
            0 if self.barriers else 1))
        self.shfile.write('export OMP_NUM_THREADS={}\n'.format(self.nthreads))
        self.shfile.write(
            'export GOMP_CPU_AFFINITY=\"0-{}\"\n\n'.format(self.nthreads - 1))

        self.shfile.write('# Remove old files\n')
        # Remove old CSV files
        if self.barriers:
            if self.out_suffix:
                self.shfile.write('rm -f {}/{}-{}.{}t.BP.*.csv\n'
                                  .format(self.toolOutPath, self.miniApp, self.out_suffix,
                                          self.nthreads))
            else:
                self.shfile.write('rm -f {}/{}.{}t.BP.*.csv\n'
                                  .format(self.toolOutPath, self.miniApp,
                                          self.nthreads))
        else:
            if self.out_suffix:
                self.shfile.write('rm -f {}/{}-{}.{}t.*.csv\n'
                                  .format(self.toolOutPath, self.miniApp, self.out_suffix,
                                          self.nthreads))
            else:
                self.shfile.write('rm -f {}/{}.{}t.*.csv\n'
                                  .format(self.toolOutPath, self.miniApp,
                                          self.nthreads))

        # Remove old .papi files
        self.shfile.write(
            'for i in $(seq 0 {})\ndo\n'.format(self.iterations - 1))
        # Per parallel region
        if self.barriers:
            if self.out_suffix:
                self.shfile.write('\trm -f {}/{}-{}.{}t.BP.rep$((i)).papi\n'
                                  .format(self.toolOutPath, self.miniApp, self.out_suffix,
                                          self.nthreads))
            else:
                self.shfile.write('\trm -f {}/{}.{}t.BP.rep$((i)).papi\n'
                                  .format(self.toolOutPath, self.miniApp, self.nthreads))
        # Whole application
        else:
            if self.out_suffix:
                self.shfile.write('\trm -f {}/{}-{}.{}t.rep$((i)).papi\n'
                                  .format(self.toolOutPath, self.miniApp, self.out_suffix,
                                          self.nthreads))
            else:
                self.shfile.write('\trm -f {}/{}.{}t.rep$((i)).papi\n'
                                  .format(self.toolOutPath, self.miniApp, self.nthreads))
        self.shfile.write('done\n\n')

        # Run experiments command
        self.shfile.write(
            'for i in $(seq 0 {})\ndo\n'.format(self.iterations - 1))

        # The PAPI output file will be open in "append" mode to add
        # the different iterations at the end of the file
        # We follow the naming convention:
        # <app>.<nthreads>.<date>.<arch>.<rep><id>
        # E.g. rsbench.4t.2020_03_20.aarch64.rep1.papi

        # Per parallel region
        if self.barriers:
            if self.out_suffix:
                self.shfile.write('\texport BP_PERFCNTRS_OUTPUT_FILE=\"{}/{}-{}.{}t.BP.rep$((i)).papi"\n\n'
                                  .format(self.toolOutPath, self.miniApp, self.out_suffix,
                                          self.nthreads))
            else:
                self.shfile.write('\texport BP_PERFCNTRS_OUTPUT_FILE=\"{}/{}.{}t.BP.rep$((i)).papi"\n\n'
                                  .format(self.toolOutPath, self.miniApp, self.nthreads))
        # Whole application
        else:
            if self.out_suffix:
                self.shfile.write('\texport BP_PERFCNTRS_OUTPUT_FILE=\"{}/{}-{}.{}t.rep$((i)).papi\"\n\n'
                                  .format(self.toolOutPath, self.miniApp, self.out_suffix,
                                          self.nthreads))
            else:
                self.shfile.write('\texport BP_PERFCNTRS_OUTPUT_FILE=\"{}/{}.{}t.rep$((i)).papi\"\n\n'
                                  .format(self.toolOutPath, self.miniApp, self.nthreads))

        for _PAPI_set in self.PAPI_rounds:
            self.shfile.write('\texport BP_PERFCNTRS_EVENTS=\"{}\"\n'.
                              format(_PAPI_set))
            self.shfile.write('\texport LD_PRELOAD=\"{}\"\n'.
                              format(self.bpdir + "/" + self.libpath + "/libbp_perfcntrs.so"))
            self.shfile.write('\t{} {}\n\n'.format(self.GetBenchPath(
                self.benchpath, self.miniApp), self.appParams))

        self.shfile.write('done\n\n')
        self.shfile.close()

        self.logger.info("Finishing creating the bash scripts for {}.{}t: {}".format(
            self.miniApp, self.nthreads, self.shfileN))

        return True

    def RunExperimentBatch(self):
        # Runs the experiments and parses the results

        if not os.path.isfile(self.shfileN):
            self.logger.error(
                'Could not locate sh file {}'.format(self.shfileN))
            return False

        self.logger.info(
            "Running perfcntrs experiments batch: {}".format(self.shfileN))

        cmd = self.shfileN
        if not self.dryRun:
            self.logger.debug('Executing perfcntrs command: {}'.format(cmd))
            # add exec permissions to sh file
            os.chmod(self.shfileN, os.stat(
                self.shfileN).st_mode | stat.S_IEXEC)
            proc = subprocess.call(cmd, shell=True)
            # proc = subprocess.check_call(cmd, shell = True)
            # Instead of raising our own error, we could use check_call above for a traceback
            if proc != 0:
                self.logger.error("Unsuccessful command: {}")
                sys.exit()

        else:
            self.logger.info('Dry run for: {}'.format(cmd))

        # Parsing experiments
        self.stats = StatsPerfCounters(
            self.shfileN, self.iterations, self.nthreads, self.runningOn, self.barriers)

        # Create CSV file as output with all the metrics
        try:
            self.outfileN = '{}.check.csv'.format(self.shfileN[:-3])
            self.outfile = open(self.outfileN, 'w')
            self.logger.debug('Creating csv file: {}'.format(self.outfileN))
            self.outfile.write('Benchmark,Thread,Iteration,Barrier,'
                               'Cycles,Instructions,'
                               'L1DAccesses,L1DMisses,L1IAccesses,L1IMisses,'
                               'L2DAccesses,L2DMisses\n')

            for i in range(self.nthreads):
                for j in range(self.iterations):
                    for k in range(self.stats.getNumberBarriers()):
                        self.outfile.write('{},{},{},{},{},{},{},{},{},{},{},{}\n'
                                           .format(self.miniApp, i, j, k,
                                                   self.stats.getTotalCycles(
                                                       i, j, k), self.stats.getTotalInstructions(i, j, k),
                                                   self.stats.getTotalL1DataAccesses(
                                                       i, j, k), self.stats.getTotalL1DataMisses(i, j, k),
                                                   self.stats.getTotalL1InstAccesses(
                                                       i, j, k), self.stats.getTotalL1InstMisses(i, j, k),
                                                   self.stats.getTotalL2DataAccesses(i, j, k), self.stats.getTotalL2DataMisses(i, j, k)))
            self.outfile.close()

        except OSError as err:
            self.logger.error('{}  outfileN: {}'.format(err, self.outfileN))
            return False

        try:
            self.outfileN = '{}.csv'.format(self.shfileN[:-3])
            self.outfile = open(self.outfileN, 'w')
            self.logger.debug('Creating csv file: {}'.format(self.outfileN))
            self.outfile.write('Benchmark,Thread,Barrier,Cycles,Instructions,'
                               'L1DAccesses,L1DMisses,L1IAccesses,L1IMisses,'
                               'L2DAccesses,L2DMisses\n')

            for i in range(self.nthreads):
                for j in range(self.stats.getNumberBarriers()):
                    self.outfile.write('{},{},{},{},{},{},{},{},{},{},{}\n'
                                       .format(self.miniApp, i, j,
                                               self.stats.getAvgCycles(i, j),
                                               self.stats.getAvgInstructions(
                                                   i, j),
                                               self.stats.getAvgL1DataAccesses(
                                                   i, j), self.stats.getAvgL1DataMisses(i, j),
                                               self.stats.getAvgL1InstAccesses(
                                                   i, j), self.stats.getAvgL1InstMisses(i, j),
                                               self.stats.getAvgL2DataAccesses(i, j), self.stats.getAvgL2DataMisses(i, j)))
            self.outfile.close()

        except OSError as err:
            self.logger.error('{}  outfileN: {}'.format(err, self.outfileN))
            return False

        self.logger.info("Finished perfcntrs experiments batch for {}.{}t".format(
            self.miniApp, self.nthreads))

        return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-c', '--config_file',
                        required=True, help='JSON config file')
    args = parser.parse_args()

    # Decode JSON configuration file
    with open(args.config_file, 'r') as f:
        json_data = json.load(f)

    rootdir = json_data["paths"]["rootdir"]
    benchpath = json_data["paths"]["benchpath"]
    libspath = json_data["paths"]["libspath"]
    dry_run = json_data["dry_run"]
    debug_mode = json_data["Debug"]
    suffix = json_data["Suffix"]
    outpath = json_data["paths"]["outpath"]
    nthreads = json_data["threads"]
    perf_iters = json_data["Perfcntrs_iterations"]
    # Captures perfcntr execution for the whole app or for each BP
    barrier_mode = [0, 1]

    # Define the logger
    formatter = logging.Formatter('[%(asctime)s] [%(levelname)s] - (%(name)s) - %(message)s')

    # File handler for debug info
    # If both DR instrumentation and performance analysis are executed,
    # the debug file will be appended to the previous existent one from the DR instrumentation.
    if json_data["execution"]["bp_identification"]:
        handler = logging.FileHandler("debug-barrierpoint.log")  # Append to file
    else:
        handler = logging.FileHandler("debug-barrierpoint.log", 'w')

    handler.setFormatter(formatter)

    # Console handler (only INFO level)
    ch = logging.StreamHandler()
    ch.setLevel(logging.INFO)
    ch.setFormatter(formatter)

    logger = logging.getLogger("Perfcntrs")
    if debug_mode:
        logger.setLevel(logging.DEBUG)
    else:
        logger.setLevel(logging.INFO)

    # Add handlers to logger
    logger.addHandler(handler)
    logger.addHandler(ch)

    if json_data["execution"]["perfcntrs"]:
        logger.info("BarrierPoint performance analysis set to [ON]")
    else:
        logger.info("BarrierPoint performance analysis set to [OFF]")
        sys.exit()

    app_dict = {}
    for app in json_data["Application"]:
        app_dict[app] = json_data["Application"][app]

    # Settings print
    print("*** [BarrierPoint Methodology] ***\n")
    print("Performance Counters execution with the following settings:")
    print("> BP directory: {}\n> Benchmarks directory: {}\n> Libraries directory: {}\
    \n> Output directory: {}".format(rootdir, benchpath, libspath, outpath))
    print("> nThreads: {}\n> Suffix: {}\n> Debug: {}\n> Execution repetitions: {}\
    \n> Dry-run: {}".format(nthreads, suffix, debug_mode, perf_iters, dry_run))
    print("> Applications and Inputs: {}".format(app_dict))
    print("*************************\n")

    theApps = []
    for (ap, nt, suf, b) in product(app_dict, nthreads, suffix, barrier_mode):
        appParams = app_dict[ap]
        outpath_suf = outpath + "/bperf." + suf
        logger.info("Running {}.{}t (mode {}, input: {})".format(
            ap, nt, b, appParams))
        theApps.append(Experiment(ap, nt, b, appParams, perf_iters, rootdir,
                                  outpath_suf, suf, libspath, debug_mode, dry_run, benchpath))
        if not theApps[-1].CreateExperimentBatch():
            logger.error('Create Experiments returned with an error')
            sys.exit()
        else:
            if not theApps[-1].RunExperimentBatch():
                logger.error('Run Experiments returned with an error')
                sys.exit()
            else:
                logger.info('== Finished Performance Counter executions ==')


if __name__ == '__main__':
    main()
