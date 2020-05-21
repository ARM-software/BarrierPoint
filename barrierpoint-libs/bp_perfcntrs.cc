/*
 * Copyright (c) 2020, Arm Limited and Contributors.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Library of Region of interest and GOMP directives to trigger PAPI
 * Performance counters
 */

#include <dlfcn.h>
#include <stdlib.h>
#include <iostream>
#include <omp.h>
#include "omp_counters.h"
#include <papi.h>

namespace BarrierPointsNS {

  int _BP_number=-1;
  bool _inside_single = false;
  bool _ignore_single = false;
  bool _measuring = false;
  bool _inside_roi = false;
  bool _inside_barrier = false;
  int total_threads = 0;

  void BarrierPoints(){
    if(_inside_roi && omp_get_thread_num() == 0) {
      _BP_number++;
#ifdef DEBUG
      std::cout << "[BarrierPoint] Executing BP: " << _BP_number << std::endl;
#endif
    }
  }

  extern "C" void GOMP_parallel (void (*fn) (void *), void *data, unsigned num_threads, unsigned int flags){
    typedef void (*GOMP_parallel_t) (void (*fn) (void *), void *data, unsigned num_threads, unsigned int flags);
    GOMP_parallel_t GOMP_parallel = (GOMP_parallel_t) dlsym(RTLD_NEXT, "GOMP_parallel");

#ifdef DEBUG
    std::cout << "[BarrierPoint] Intercepted GOMP_parallel ALL | Thread: " \
    << omp_get_thread_num() << std::endl;
#endif
    if(!_measuring){
#ifdef DEBUG
      std::cout << "[BarrierPoint] Intercepted GOMP_parallel" << std::endl;
#endif

      if(_inside_roi) {
        BarrierPoints();
        _measuring = true;
        parallelRegionPerformanceCounters();
        _measuring = false;
      }
    }
    return (GOMP_parallel)(fn, data, num_threads, flags);
  }

  extern "C" void GOMP_barrier (void) {
    typedef void (*GOMP_barrier_t) (void);
    GOMP_barrier_t GOMP_barrier = (GOMP_barrier_t) dlsym(RTLD_NEXT, "GOMP_barrier");

    char * s = secure_getenv("ROI_BP");
    if(s) {
#ifdef DEBUG
      std::cout << "ROI_BP: " << s << std::endl;
#endif
      _inside_roi = strtol(s, NULL, 10) == 1 ? true : false;
    }
    
    if(_inside_roi){
      BarrierPoints();

      if(_BP_number == 0){
        _measuring = true;
        initPerformanceCounters();
        startPerformanceCounters();
        _measuring = false;
      }else
        barrierRegionPerformanceCounters();
    }else if(_BP_number > -1) {
#ifdef DEBUG
      std::cout << "[BarrierPoint] Intercepted end of ROI barrier | Total number of threads: " \
                << omp_get_num_threads() << std::endl;
#endif
      stopPerformanceCounters();
    }
    return (GOMP_barrier)();
  }

}
