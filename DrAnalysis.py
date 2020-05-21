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

import logging
import os
import subprocess
import argparse
import gzip
import functools
import json
from itertools import product
import sys


class SimPoints(object):

    def __init__(self, BBfile='bbvbarrier.bb.gz', fixedLength='off',
                 deleteSPOut=False, maxK=20, dim=15, outputfiles='./'):

        self.logger = logging.getLogger('BarrierPoint.simpoint')

        self.BBfile = BBfile    # unprojected sparse-format frequency vector file
        # the file is compressed with gzip (-inputVectorsGzipped)
        self.extraOpts = []     # extra-options if needed

        self.execF = './simpoint'

        self.fixedLength = fixedLength
        if fixedLength == 4:
            self.fixedLength = 'on'
            # initialize k-means by sampling
            self.extraOpts.append('-initkm samp')
            self.execF = './simpoint.normalize2'
        elif fixedLength == 3:
            self.fixedLength = 'on'
            self.extraOpts.append('-initkm samp')
            self.execF = './simpoint.normalize'
        elif fixedLength == 2 or fixedLength == 'normalize':
            self.fixedLength = 'on'
            # initialize k-means by furtherst-first
            self.extraOpts.append('-initkm ff')
            self.execF = './simpoint.normalize'
        elif fixedLength not in ('off', 'on'):
            self.fixedLength = {False: 'off', True: 'on'}[bool(fixedLength)]
            # specifies if the vectors provided are fixed-length
            # (for barrier points they are not)

        self.deleteSPOut = deleteSPOut  # keep temporal files ?
        self.tSimPoints = '{}t.simpoints'.format(outputfiles)
        self.tWeights = '{}t.weights'.format(outputfiles)
        self.tLabels = '{}t.labels'.format(outputfiles)

        # using 'search' (binary search for values of k), maximum number of clusters
        self.maxK = maxK  # original SimPoint value: 15, original BP value: 20

        self.dim = 'noProject' if (dim == 0) else dim
        # number of dimensions down to randomly project the frequency vectors
        # default value is 15
        self.execCmd = """{} -loadFVFile "{}" -inputVectorsGzipped -fixedLength {} -maxK {} -dim {} \
                       -coveragePct 1.0 -saveSimpoints {} -saveSimpointWeights {} -saveLabels {} \
                       -verbose 0 {}""".format(self.execF, self.BBfile, self.fixedLength, self.maxK,
                                               self.dim, self.tSimPoints, self.tWeights, self.tLabels,
                                               ' '.join(self.extraOpts))

        self.SPData = dict()  # result of the clustering

        # Call the SimPoint tool
        self.logger.info('Starting SimPoint')
        if not self.RunSimPoints():
            self.logger.error('SimPoint clustering failed')
        self.logger.info('Finished SimPoint')

    def getSimPointData(self):
        # Returns the results of the SimPoint clustering
        # SPData is a dictionary of lists; keys: id, weigths, simpoints, labels, match

        return self.SPData

    def RunSimPoints(self):
        # Runs the SimPoint clustering

        self.logger.debug("Executing command:\n{}".format(self.execCmd))
        proc = subprocess.Popen(['bash', '-c', self.execCmd])
        proc.communicate()
        if proc.returncode != 0:
            self.logger.warn("Process {} returned {} code".format(
                self.execCmd, proc.returncode))
            return False

        self.logger.debug('Data analysis')
        if not self.AnalysisSimPoints():
            self.logger.warn('SimPoints analysis returned with errors')
            return False

        return True

    def AnalysisSimPoints(self):
        # Parses the output from SimPoint
        # To compute SimPoints we are skipping the first BP, the one from the start of the ROI
        # to the first parallel region; however, to ease the performance evaluation
        # with performance counters we assume that BP to be #0.
        # Hence, we have to shift the BP identifiers by 1 afterwards.

        self.SPData['id'] = []
        self.SPData['weights'] = []
        # tWeights file has the following format: weight id
        try:
            _wfile = open(self.tWeights, 'r')
            for line in _wfile:
                weight, num = line.split()
                self.SPData['id'].append(int(num))
                self.SPData['weights'].append(float(weight))
            _wfile.close()

        except OSError as err:
            self.logger.error('{}  tWeights: {}'.format(err, self.tWeights))
            return False

        self.SPData['simpoints'] = []
        # tSimPoints file has the following format:
        #   simpoint id
        try:
            _sfile = open(self.tSimPoints, 'r')
            for i, line in enumerate(_sfile):
                sp, num = line.split()
                if int(num) != self.SPData['id'][i]:
                    self.logger.error('Invalid SimPoint index')
                    return False
                self.SPData['simpoints'].append(int(sp))
            _sfile.close()

        except OSError as err:
            self.logger.error(
                '{}  tSimPoints: {}'.format(err, self.tSimPoints))
            return False

        self.SPData['labels'] = []
        self.SPData['match'] = []
        # tLabels file has the following format: label match
        # where label is the id of the assigned cluster for each barrier point
        # and match is the distance from center of each input vector (i.e., barrier point)
        try:
            _lfile = open(self.tLabels, 'r')
            for line in _lfile:
                label, match = line.split()
                self.SPData['labels'].append(int(label))
                self.SPData['match'].append(float(match))
            _lfile.close()

        except OSError as err:
            self.logger.error('{}  tLabels: {}'.format(err, self.tLabels))
            return False

        # Cleaning the files if needed:
        if self.deleteSPOut:
            for f in (self.tWeights, self.tSimPoints, self.tLabels):
                try:
                    os.remove(f)
                except:
                    self.logger.warn("File {} could not be removed".format(f))
        return True


class BarrierPoints(object):
    # Run analysis to obtain the barrier points
    def __init__(self, miniApp, nthreads, appParams, outpath='./tmp', out_suffix='',
                 iteration=1, benchpath='./benchmarks', drpath=''):

        # Define the logger
        self.logger = logging.getLogger('BarrierPoint')

        # Attributes & properties
        self.miniApp = miniApp      # miniApp name
        self.nthreads = nthreads    # number of threads
        self.iteration = iteration  # iteration run number, append to folder
        # minimum length for barrier points (if 0, no minimum length)
        self.benchpath = benchpath  # Benchmark path
        self.drpath = drpath  # DynamoRIO path
        self.out_suffix = out_suffix  # Suffix for output folders

        self.appParams = self.GetBenchParams(appParams, nthreads)

        # path to store the output files
        if out_suffix:
            self._toolOutPath = "{}/{}-{}.{}t/{:0>3d}/".format(
                outpath, miniApp, out_suffix, nthreads, iteration)
        else:
            self._toolOutPath = "{}/{}.{}t/{:0>3d}/".format(
                outpath, miniApp, nthreads, iteration)

        self.BBV_count = None       # output file produced by BBV tool (1)
        self.BBV_inscount = None    # output file produced by BBV tool (2)
        self.LDV_reuse = None       # output file produced by LDV tool (1)
        self.BP = None              # output file produced by combining the above files
        self.selectedBP = None      # output file produced with selected barrier points

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
        # Substitutes {} in the config.json file for number of threads for execution
        if "{}" in appParams:
            return appParams.format(nthreads)
        else:
            return appParams

    def __CreateVectors(self, dryRun=False):
        # Runs the DR client to obtain the BBV/LDV for the application/nthreads
        # Output files are stored in "toolOutPath"
        # Use dryRun to just set up the proper vars, needed for SimPoints
        if not os.path.isdir(self.toolOutPath):
            try:
                os.makedirs(self.toolOutPath)
            except OSError:
                self.logger.warn(
                    "CreateVectors could not create output path {}, \
                    storing results in local folder".format(self.toolOutPath))
                self.toolOutPath = '.'

        if self.out_suffix:
            _opath = "{}/{}-{}.{}t".format(self.toolOutPath,
                                           self.miniApp, self.out_suffix, self.nthreads)
        else:
            _opath = "{}/{}.{}t".format(self.toolOutPath,
                                        self.miniApp, self.nthreads)

        self.BBV_count = "{}.bbv_count.gz".format(_opath)
        self.BBV_inscount = "{}.bbv_inscount.gz".format(_opath)
        self.LDV_reuse = "{}.ldv_bb.gz".format(_opath)

        # Set the DynamoRIO run command. Requires preload of libbp_dr.so
        if not os.path.isfile("./dynamorio_client/build/libbarrierpoint.so"):
            self.logger.warn(
                "libbarrierpoint.so not found in {}".format("dynamorio_client/build/"))
            return False

        cmd = 'export LD_PRELOAD={} OMP_NUM_THREADS={} GOMP_CPU_AFFINITY=\"0-{}\"; '.format(
              "./barrierpoint-libs/libbp_dr.so", self.nthreads, self.nthreads - 1)
        cmd += self.drpath + \
               """/bin64/drrun -stderr_mask 15 -c ./dynamorio_client/build/libbarrierpoint.so \
               -out_path {} -- {} {}""".format(
               _opath, self.GetBenchPath(self.benchpath, self.miniApp), self.appParams)

        self.logger.debug("Executing command: {}".format(cmd))

        if not dryRun:  # Runs the command
            proc = subprocess.Popen(['bash', '-c', cmd])
            proc.communicate()
            if proc.returncode != 0:
                self.logger.warn(
                    "Process {} return {} code".format(cmd, proc.returncode))
                return False

        return True

    def __CombineBBVsandLDVs(self):
        # Combines the BBVs and LDVs

        for fn in [self.BBV_count, self.BBV_inscount, self.LDV_reuse]:
            if not os.path.isfile(fn):
                self.logger.error(
                    "Combining BBVs and LDVs: file {} not found".format(fn))
                return False
        if self.out_suffix:
            # output file with BPVs
            self.BP = "{}/{}-{}.{}t.bpv.gz".format(
                self.toolOutPath, self.miniApp, self.out_suffix, self.nthreads)
            # output file the selected BP and weights
            self.selectedBP = "{}/{}-{}.{}t.barrierpoints".format(
                self.toolOutPath, self.miniApp, self.out_suffix, self.nthreads)
        else:
            # output file with BPVs
            self.BP = "{}/{}.{}t.bpv.gz".format(
                self.toolOutPath, self.miniApp, self.nthreads)
            # output file the selected BP and weights
            self.selectedBP = "{}/{}.{}t.barrierpoints".format(
                self.toolOutPath, self.miniApp, self.nthreads)
        self.temporalSPfiles = self.selectedBP + '_'

        # LDV_reuse file

        _bbmax = 0
        _total_reuse_per_phase = []
        _reuse_data_per_phase = []

        try:
            _rfile = gzip.open(self.LDV_reuse, 'rt')
            self.logger.debug('Reading LDV_reuse: {}'.format(self.LDV_reuse))
            _rfile.readline()  # skip first line 'W'
            for _line in _rfile:
                # Line Format: # T:1:43824 :2:4219 :3:4556 :4:527 :5:183 ...
                _data = list(map(int, _line.split(':')[1:]))
                # odd numbers represent the tread/reuse distance identifier (id)
                # and even numbers represent the reuse histogram frequencies (freq)
                _bbmax = max(_data[::2] + [_bbmax])  # max of the freq

                # skip 'T', get the sum of all the freq
                _total_reuse_per_phase.append(sum(_data[1::2]))
                _reuse_data_per_phase.append(
                    zip(_data[::2], _data[1::2]))  # tuples (id, freq)
            _rfile.close()

        except OSError as err:
            self.logger.error('{}  LDV_reuse: {}'.format(err, self.LDV_reuse))
            return False

        # BBV_inscount file
        self.ins_per_phase = []

        try:
            _ifile = gzip.open(filename=self.BBV_inscount, mode='rt')
            self.logger.debug(
                'Reading BBV_inscount: {}'.format(self.BBV_inscount))
            _line = _ifile.readline()  # it corresponds to the ROI_start = first parallel region
            for _line in _ifile:
                # list of the sum of instructions
                # EXAMPLE: 355328,739280,568597,776441,...
                self.ins_per_phase.append(sum(map(int, _line.split(','))))
                # executed by all threads in each phase
            _ifile.close()

            # sanity check
            assert len(self.ins_per_phase) == len(_reuse_data_per_phase)

        except OSError as err:
            self.logger.error(
                '{}  BBV_inscount: {}'.format(err, self.BBV_inscount))
            return False

        # BBV_count file and output (BP)

        try:
            _cfile = gzip.open(self.BBV_count, 'rt')
            _bfile = gzip.open(self.BP, 'wt')
            self.logger.debug(
                'Reading BBV_inscount: {}'.format(self.BBV_count))

            _cfile.readline()  # skip first line 'W'
            i = 0   # line number
            for _line in _cfile:
                # data is a list where odd numbers are the BB identifier,
                # which was computed as bbvid + threadid * numbbvids
                # (i.e., it differentiates per thread)
                # and even numbers are the instruction count for that BB/thread
                # to the BB identifier,
                # EXAMPLE: # T:499:2 :82:5 :500:8 :501:1 :502:2 :503:2 :504:3
                _data = list(map(int, _line.split(':')[1:]))
                # we add the maximum frequency of the histogram of reuse distances
                _new_bbv_nums = map(lambda x: x + _bbmax, _data[::2])

                _bfile.write('T')  # new line of output file

                # i.e., (id, freq)
                for _rd_bbid, _rd_count in _reuse_data_per_phase[i]:
                    _new_rd_count = (
                        _rd_count * self.ins_per_phase[i]) / _total_reuse_per_phase[i]
                    # freq in percentage (w/ respect to the sum of frequencies stored in
                    # _total_reuse_per_phase) times the total instructions of this phase
                    # adjusted reuse values
                    _bfile.write(':{}:{} '.format(_rd_bbid, _new_rd_count))

                for _bbid, _bb_data in zip(_new_bbv_nums, _data[1::2]):
                    # the adjusted BB identifers w/ the histogram frequencies
                    # and the BB instructions from the original file
                    _bfile.write(':{}:{} '.format(_bbid, _bb_data))

                # 2 different things are done at this step:
                #  a) Ponderate the reuse distances with the number of instructions executed
                #  b) For each barrier create two set of pairs: the LD-related,
                #     adjusted (a) and the original BB:count
                # Keep in mind that the objective is to create a fingerprint
                # for each of the barriers so we can cluster them in the next step

                _bfile.write('\n')
                i += 1

            _bfile.close()
            _cfile.close()

        except OSError as err:
            self.logger.error('{}  BBV_inscount: {}, BP: {}'.format(
                err, self.BBV_inscount, self.BP))
            return False

        return True

    def __ClusterBarrierPoints(self):
        # Runs the cluster analysis for BarrierPoint
        self.SimPoints = SimPoints(
            BBfile=self.BP, outputfiles=self.temporalSPfiles)
        self.SPData = self.SimPoints.getSimPointData()

        id2index = [None for i in range(max(self.SPData['id']) + 1)]
        for i, d in enumerate(self.SPData['id']):
            id2index[d] = i

        # This BB map assigns, to each BarrierPoint, the index (i.e., barrier number) of its
        #  cluster (based on the SimPoint analysis)
        self.SimPointBBMap = [self.SPData['simpoints']
                              [id2index[i]] for i in self.SPData['labels']]

        # At this point we have selected the representative clusters
        # The BPs indexes are shifted (see the comments on the SimPoint analysis function)
        self.logger.info('Barrier points selected: {}'.format(
            ', '.join(map(lambda x: str(x + 1), sorted(self.SPData['simpoints'])))))

        # Debug print
        _debug_pline = 16
        _debug_pprint = ''
        for i in range(len(self.SimPointBBMap)):
            if i % _debug_pline == 0:
                _debug_pprint += '\n'
            _debug_pprint += '%5d' % (self.SimPointBBMap[i] + 1)
        self.logger.debug("SimPointBBMap: {}".format(_debug_pprint))

        # Now determine the scaling factors for the BPs,
        #  as there are non-fixed length intervals (__SelectSignificantBarrierPoints)

        # Alternative BP selection for earlier iterations:
        #   Earlier iterations allow us to reduce the fast-forwarding phases of simulation
        #   We are going to select the three closest barrier points to the selected one of each group
        #   but only taking into account the ones that are before

        self.SPData['simpointsEC'] = []
        self.SPData['simpointsEE'] = []
        self.logger.debug('Computing earlier BPs')
        # i is the id, sp the index in the t.labels
        for i, sp in enumerate(self.SPData['simpoints']):
            # BPs that belong to the simpoint cluster
            cluster = [j for j, jj in enumerate(
                self.SimPointBBMap) if jj == sp]
            # distances to the center of that BP
            distanc = [self.SPData['match'][x] for x in cluster]
            # maxD_05 is the distance to the center
            maxD_05 = self.SPData['match'][sp] + max(distanc) * 0.05
            # maxD_10 = self.SPData['match'][sp] + max(distanc)*0.10
            # maxD_20 = self.SPData['match'][sp] + max(distanc)*0.20

            closer = []
            # closer = [jj for j,jj in enumerate(cluster) if (distanc[j] <= maxD_05 and jj <= sp)]
            for j, jj in enumerate(cluster):
                # if (distanc[j] <= maxD_20 and jj <= sp):
                # if (distanc[j] <= maxD_10 and jj <= sp):
                if (distanc[j] <= maxD_05 and jj <= sp):
                    closer.append((jj, distanc[j]))

            # The closest BP based on distances
            # NOTE: 'reduce' function can be ported to a 'for loop' for better python3 compatibility
            self.SPData['simpointsEC'].append(functools.reduce(
                lambda x, y: x if x[1] < y[1] else y, closer)[0])
            # The earliest one
            self.SPData['simpointsEE'].append(closer[0][0])

            self.logger.debug('Cluster {}: {}\nDistances: {}\nMaxD: {}, MaxD_05 {}'.
                              format(sp + 1, ' '.join(map(lambda x: str(x + 1), cluster)),
                                     ' '.join(map(str, distanc)), max(distanc), maxD_05))
            self.logger.debug('Selected closer: {}, earliest: {}'.
                              format(self.SPData['simpointsEC'][-1] +
                              1, self.SPData['simpointsEE'][-1] + 1))

        # For 'simpointsEC' and 'simpointsEE' we want to compute the proper weights as well,
        #  so we are going to create equivalent maps to SimPointBBMap

        self.SimPointBBMap_EC = list(self.SimPointBBMap)
        for s1, s2 in zip(self.SPData['simpoints'], self.SPData['simpointsEC']):
            if s1 == s2:
                continue
            for i in range(len(self.SimPointBBMap)):
                if self.SimPointBBMap_EC[i] == s1:
                    self.SimPointBBMap_EC[i] = s2

        # Debug print
        _debug_pline = 16
        _debug_pprint = ''
        for i in range(len(self.SimPointBBMap_EC)):
            if i % _debug_pline == 0:
                _debug_pprint += '\n'
            _debug_pprint += '%5d' % (self.SimPointBBMap_EC[i] + 1)
        self.logger.debug('SimPointBBMap_EC: {}'.format(_debug_pprint))

        self.SimPointBBMap_EE = list(self.SimPointBBMap)
        for s1, s2 in zip(self.SPData['simpoints'], self.SPData['simpointsEE']):
            if s1 == s2:
                continue
            for i in range(len(self.SimPointBBMap)):
                if self.SimPointBBMap_EE[i] == s1:
                    self.SimPointBBMap_EE[i] = s2

        # Debug print
        _debug_pline = 16
        _debug_pprint = ''
        for i in range(len(self.SimPointBBMap_EE)):
            if i % _debug_pline == 0:
                _debug_pprint += '\n'
            _debug_pprint += '%5d' % (self.SimPointBBMap_EE[i] + 1)
        self.logger.debug('SimPointBBMap_EE: {}'.format(_debug_pprint))

        return True

    def __SelectSignificantBarrierPoints(self):
        # Last step, where we check how significant the selected SimPoints are,
        #  with respect to the instructions executed in each phase

        scaling_factors = [(1.0 + ((float(self.ins_per_phase[i]) - self.ins_per_phase[sp]) / self.ins_per_phase[sp]))
                           for i, sp in enumerate(self.SimPointBBMap)]

        scaling_factors_EC = [(1.0 + ((float(self.ins_per_phase[i]) - self.ins_per_phase[sp]) / self.ins_per_phase[sp]))
                              for i, sp in enumerate(self.SimPointBBMap_EC)]

        scaling_factors_EE = [(1.0 + ((float(self.ins_per_phase[i]) - self.ins_per_phase[sp]) / self.ins_per_phase[sp]))
                              for i, sp in enumerate(self.SimPointBBMap_EE)]

        # Debug print
        _debug_pline = 16
        _debug_pprint = ''
        for i in range(len(scaling_factors)):
            if i % _debug_pline == 0:
                _debug_pprint += '\n'
            _debug_pprint += '%10.4f' % (scaling_factors[i])
        self.logger.debug('Scaling factors: {}'.format(_debug_pprint))

        bp_to_scaling = [0.0 for i in range(len(self.SimPointBBMap))]
        for i, sp in enumerate(self.SimPointBBMap):
            bp_to_scaling[sp] += scaling_factors[i]
        # bp_to_scaling is a list where the non-zero elements are the selected barrier points,
        #  storing the sum of scaling factors of the regions they represent
        # self.logger.debug('BP to Scaling factors, non-zero elements: %s', ' '.join(map("{0:.4f}".format, bp_to_scaling)))

        bp_to_scaling_EC = [0.0 for i in range(len(self.SimPointBBMap_EC))]
        for i, sp in enumerate(self.SimPointBBMap_EC):
            bp_to_scaling_EC[sp] += scaling_factors_EC[i]

        bp_to_scaling_EE = [0.0 for i in range(len(self.SimPointBBMap_EE))]
        for i, sp in enumerate(self.SimPointBBMap_EE):
            bp_to_scaling_EE[sp] += scaling_factors_EE[i]

        # Debug print
        _debug_pline = 16
        _debug_pprint = ''
        _nonZeroBP = [(i + 1, e) for i, e in enumerate(bp_to_scaling)
                      if e != 0.0]  # list of tuples
        for i in range(len(_nonZeroBP)):
            if i % _debug_pline == 0:
                _debug_pprint += '\n'
            _debug_pprint += '%5d: %.4f' % (_nonZeroBP[i])
        self.logger.debug(
            'BP to Scaling factors, non-zero elements: {}'.format(_debug_pprint))
        # self.logger.debug('BP to Scaling factors, non-zero elements: %s',\
        #    ', '.join("%s: %.4f" % t for t in [(i,e) for i, e in enumerate(bp_to_scaling) if e != 0.0]))

        # total_ins = sum(self.ins_per_phase)
        self.SelectedBarrierPoints = []
        self.SelectedMultipliers = []
        for sp in sorted(self.SPData['simpoints']):
            # percent_sp = (float(self.ins_per_phase[sp]) * bp_to_scaling[sp]) / float(total_ins)
            # if percent_sp > 0.005:
            self.SelectedBarrierPoints.append(sp)
            self.SelectedMultipliers.append(bp_to_scaling[sp])

        self.logger.info('Number of significant BPs: {} (out of {})'.format(
            len(self.SelectedBarrierPoints), len(self.SimPointBBMap)))
        self.logger.debug('Significant BPs: {}\nMultipliers: {}'.format(
            ', '.join(map(lambda x: str(x + 1), self.SelectedBarrierPoints)),
            ', '.join(map(str, self.SelectedMultipliers))))

        try:
            _f = open(self.selectedBP, 'w')
            for i, sp in enumerate(self.SelectedBarrierPoints):
                _f.write('{} {:.4f}\n'.format(
                    sp + 1, self.SelectedMultipliers[i]))
            _f.close()
            self.logger.info(
                'Barrier points and multiplier stored in: {}'.format(self.selectedBP))

        except OSError as err:
            self.logger.error(
                '{}  selectedBPs: {}'.format(err, self.selectedBP))
            return False

        # Alternative BarrierPoint: EC (earlier but closest)
        self.SelectedBarrierPoints_EC = []
        self.SelectedMultipliers_EC = []
        for sp in sorted(self.SPData['simpointsEC']):
            self.SelectedBarrierPoints_EC.append(sp)
            self.SelectedMultipliers_EC.append(bp_to_scaling_EC[sp])

        self.logger.debug('Alternative barrier points selected (closest):\n'
                          'Significant BP: {}\nMultipliers: {}'.format(
                              ', '.join(map(lambda x: str(x + 1), self.SelectedBarrierPoints_EC)), ',\
        '.join(map(str, self.SelectedMultipliers_EC))))

        try:
            _f = open(self.selectedBP + '_EC05', 'w')
            # f = open(self.selectedBP + '_EC10', 'w')
            # f = open(self.selectedBP + '_EC20', 'w')
            for i, sp in enumerate(self.SelectedBarrierPoints_EC):
                _f.write('{} {:.4f}\n'.format(
                    sp + 1, self.SelectedMultipliers_EC[i]))
            _f.close()
            self.logger.info('Barrier points EC and multiplier stored in: {}'.format(
                self.selectedBP + '_EC'))

        except OSError as err:
            self.logger.error('{}  selected_BP: {}'.format(
                err, self.selectedBP + '_EC'))
            return False

        # Alternative BarrierPoint: EE (earliest)
        self.SelectedBarrierPoints_EE = []
        self.SelectedMultipliers_EE = []
        for sp in sorted(self.SPData['simpointsEE']):
            self.SelectedBarrierPoints_EE.append(sp)
            self.SelectedMultipliers_EE.append(bp_to_scaling_EE[sp])

        self.logger.debug('Alternative Barrier points selected (earliest):\n'
                          'Significant BP: {}\nMultipliers: {}'.format(
                              ', '.join(map(lambda x: str(x + 1),
                                            self.SelectedBarrierPoints_EE)),
                              ', '.join(map(str, self.SelectedMultipliers_EE))))

        try:
            _f = open(self.selectedBP + '_EE05', 'w')
            # f = open(self.selectedBP + '_EE10', 'w')
            # f = open(self.selectedBP + '_EE20', 'w')
            for i, sp in enumerate(self.SelectedBarrierPoints_EE):
                _f.write('{} {:.4f}\n'.format(
                    sp + 1, self.SelectedMultipliers_EE[i]))
            _f.close()
            self.logger.info('Barrier points EE and multiplier stored in: {}'.format(
                self.selectedBP + '_EE'))

        except OSError as err:
            self.logger.error('{}  selectedBP: {}'.format(
                err, self.selectedBP + '_EE'))
            return False

        return True

    def CreateBarrierPoints(self, dryRun=False):
        # Runs the BarrierPoint analysis for miniApp with a given number of nthreads
        # If dryRun s set to True it does not execute the DR instrumentation,
        # and assumes the files already exist
        # Returns False if error

        self.logger.info('Creating Barrier Points')
        self.logger.debug('Application: {}, Threads: {}'.format(
            self.miniApp, self.nthreads))

        # First we create Basic Block Vectors and LRU stack Distance Vectors
        self.logger.debug('Calling Create VectorsBBVs')
        if not self.__CreateVectors(dryRun):
            self.logger.warn('Create Vectors returned with an error')
            return False
        else:
            self.logger.debug('Create Vectors returned without errors\n---')

        # Second we combine all the vectors
        self.logger.debug('Calling Combining Function')
        if not self.__CombineBBVsandLDVs():
            self.logger.warn('Combining BBV and LDV returned with an error')
            return False
        else:
            self.logger.debug(
                'Combining BBV and LDV returned without errors\n---')

        # Then we need to cluster vectors using SimPoint 3.2
        self.logger.debug('Calling Clustering Function (SimPoint)')
        if not self.__ClusterBarrierPoints():
            self.logger.warn('ClusterBarrierPoints() returned with an error')
            return False
        else:
            self.logger.debug(
                'ClusterBarrierPoints() returned without errors\n---')

        # Finally we determine significant barrier points (depending on the instructions per phase)
        self.logger.debug('Calling SelectSignificantBarrierPoints()')
        if not self.__SelectSignificantBarrierPoints():
            self.logger.warn(
                'SelectSignificantBarrierPoints() returned with an error')
            return False
        else:
            self.logger.debug(
                'SelectSignificantBarrierPoints() returned without errors\n---')

        self.logger.info('Barrier points selection: {}\n'
                         'Alternative (closest) BP selection: {}\n'
                         'Alternative (earliest) BP selection: {}'.format(
                             ', '.join(map(lambda x: str(x + 1),
                                           self.SelectedBarrierPoints)),
                             ', '.join(map(lambda x: str(x + 1),
                                           self.SelectedBarrierPoints_EC)),
                             ', '.join(map(lambda x: str(x + 1), self.SelectedBarrierPoints_EE))))

        return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-c', '--config_file',
                        required=True, help='JSON config file')
    args = parser.parse_args()

    # Decode JSON configuration file
    # Check the README and the provided config file for more information
    with open(args.config_file, 'r') as f:
        json_data = json.load(f)

        rootdir = json_data["paths"]["rootdir"]
        benchpath = json_data["paths"]["benchpath"]
        libspath = json_data["paths"]["libspath"]
        dry_run = json_data["dry_run"]
        debug_mode = json_data["Debug"]
        outpath = json_data["paths"]["outpath"] + '/barriers'
        suffix = json_data["Suffix"]
        nthreads = json_data["threads"]
        dr_iters = json_data["DR_iterations"]

    # Define the logger
    formatter = logging.Formatter('[%(asctime)s] [%(levelname)s] - (%(name)s) - %(message)s')

    # File handler for debug info
    handler = logging.FileHandler("debug-barrierpoint.log", "w")
    handler.setFormatter(formatter)

    # Console handler (only INFO level)
    ch = logging.StreamHandler()
    ch.setLevel(logging.INFO)
    ch.setFormatter(formatter)

    logger = logging.getLogger("BarrierPoint")
    if debug_mode:
        logger.setLevel(logging.DEBUG)
    else:
        logger.setLevel(logging.INFO)

    # Add handlers to logger
    logger.addHandler(handler)
    logger.addHandler(ch)

    # Requires path to built DynamoRIO folder
    # drpath = os.getenv('DYNAMORIO_HOME')
    drpath = os.getenv('DYNAMORIO_BUILD_DIR')
    if not drpath or not os.path.isfile(drpath + "/bin64/drrun"):
        logger.error(
            'Please set $DYNAMORIO_BUILD_DIR to the DynamoRIO build directory')
        sys.exit()

    if json_data["execution"]["bp_identification"]:
        logger.info("BarrierPoint Identification set to [ON]")
    else:
        logger.info("BarrierPoint Identification set to [OFF]")
        sys.exit()

    app_dict = {}
    for app in json_data["Application"]:
        app_dict[app] = json_data["Application"][app]

    # Settings print
    print("*** [BarrierPoint Methodology] ***\n")
    print("Barrier point identification with the following settings:")
    print("> BP directory: {}\n> Benchmarks directory: {}\n> Libraries directory: {}\
    \n> Output directory: {}".format(rootdir, benchpath, libspath, outpath))
    print("> nThreads: {}\n> Suffix: {}\n> Execution repetitions: {}\n> Debug: {}\
    \n> Dry-Run: {}".format(nthreads, suffix, dr_iters, debug_mode, dry_run))
    print("> Applications and Inputs: {}".format(app_dict))
    print("*************************\n")

    theApps = []
    # Cartesian Product, equivalent to a nested loop.
    # For each application, for the number of threads to explore
    for (ap, nt, suf) in product(app_dict, nthreads, suffix):
        appParams = app_dict[ap]
        for i in range(dr_iters):
            logger.info("Running {}.{}t (iter: {}, input: {})".format(
                ap, nt, i, appParams))
            theApps.append(BarrierPoints(ap, nt, appParams, outpath,
                                         suf, i, benchpath, drpath))
            if not theApps[-1].CreateBarrierPoints(dry_run):
                logger.error('CreateBarrierPoints() returned with an error')
            else:
                logger.info(
                    '== Finished BP identification and selection ==')


if __name__ == '__main__':
    main()