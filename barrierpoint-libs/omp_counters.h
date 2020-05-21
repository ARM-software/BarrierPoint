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

/* Auxiliary functions for PAPI performance counters in Barrierpoint */

#ifndef __BP_PERFCNTRS_H__
#define __BP_PERFCNTRS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <omp.h>
#include <papi.h>

int papi_max_num_threads = 0;
int papi_event_set;
#define MAX_STR_LEN 128
#pragma omp threadprivate(papi_event_set)

#define MAX_CNTRS 6

int  BP_AVAILABLE_CNTRS = 0;

int _BP_PERFCNTRS = 0;
int _BP_PERFCNTRS_VERBOSE = 0;
int _BP_PERFCNTRS_OMPPARALLEL = 0;
int _BP_PERFCNTRS_SAMPLING = 0;
int  BP_PERFCNTRS_INITIALIZED = 0;
int  PERF_CNTRS_STARTED = 0;

int  t_events = 0;
char event_output_filename[1024];
int  event_codes[MAX_CNTRS];
long long event_values[MAX_CNTRS + 1]; /* +1 is the cycle counter */

int  perfc_thread_started = 1;

/* Only when instrumenting OMP parallel regions */
#define MAX_PARALLEL_PHASES 25800
unsigned long long event_values_pbarrier[MAX_PARALLEL_PHASES][MAX_CNTRS + 1]; /* +1 is the cycle counter */
int perfc_omp_parallel_region_count = 0;

/* Only when instrumenting samplings */
unsigned long long event_values_sample[MAX_CNTRS + 1]; /* +1 is the cycle counter */

#pragma omp threadprivate(perfc_thread_started, perfc_omp_parallel_region_count, \
event_values, event_values_pbarrier, event_values_sample)

/****************************** AUX FUNCTIONS ******************************/
inline __attribute__((always_inline)) int env2int(const char *name){
  const char *val = getenv(name);
  if(val)
    return strtol(val, NULL, 0);
  return 0;
}

inline __attribute__((always_inline)) void _papi_error(char *error_str){
  PAPI_perror(error_str);
  exit(2);
}
/**************************** END AUX FUNCTIONS ****************************/

/* Set performance counters. This function must be called before starting any instrumentation */
inline __attribute__((always_inline)) void initPerformanceCounters(){
  #pragma omp master 
  {
    if (BP_PERFCNTRS_INITIALIZED == 0) {
      BP_PERFCNTRS_INITIALIZED = 1;
      _BP_PERFCNTRS = env2int("BP_PERFCNTRS");
      _BP_PERFCNTRS_VERBOSE = env2int("BP_PERFCNTRS_VERBOSE");
      _BP_PERFCNTRS_OMPPARALLEL = env2int("BP_PERFCNTRS_OMPPARALLEL");
      _BP_PERFCNTRS_SAMPLING = env2int("BP_PERFCNTRS_SAMPLING");

      /* Ensure the maximum number of counters is respected */
      if (_BP_PERFCNTRS) {
        BP_AVAILABLE_CNTRS = PAPI_num_counters();

        if (_BP_PERFCNTRS_VERBOSE)
          std::cout << "[OMP PerfCntrs] Initializing\n \
                    [OMP PerfCntrs] Number of available counters: "
                    << BP_AVAILABLE_CNTRS << std::endl;

        if (BP_AVAILABLE_CNTRS > MAX_CNTRS) {
          BP_AVAILABLE_CNTRS = MAX_CNTRS;
          std::cout << "[OMP PerfCntrs] This module only supports up to "
                    << MAX_CNTRS << " counters\n";
        }

        if (_BP_PERFCNTRS_SAMPLING && _BP_PERFCNTRS_OMPPARALLEL) {
          std::cout << "[OMP PerfCntrs] Only Sampling-based or Parallel Region-based \
                    can be enabled, switching to Parallel Region-based\n";
          _BP_PERFCNTRS_SAMPLING = 0;
        }

        /* Output file to store the results */
        char *_output_filename = getenv("BP_PERFCNTRS_OUTPUT_FILE");
        if (_output_filename)
          strcpy(event_output_filename, _output_filename);
        else
          strcpy(event_output_filename, "/tmp/perfcntrs_events.out");

        if (_BP_PERFCNTRS_VERBOSE)
          std::cout << "[OMP PerfCntrs] Dumping results in: "
                    << event_output_filename << std::endl;

        if (PAPI_library_init(PAPI_VER_CURRENT) != PAPI_VER_CURRENT)
          _papi_error((char *)"[OMP PerfCntrs] PAPI_library_init error");
        papi_max_num_threads = omp_get_max_threads();
        if (PAPI_thread_init((long unsigned int (*)()) omp_get_thread_num) != PAPI_OK)
          _papi_error((char *)"[OMP PerfCntrs] PAPI_thread_init error");

        /* Get the events; specify events separated by commas */
        char *events_str_ptr = getenv("BP_PERFCNTRS_EVENTS");
        char events_str[1024];

        if (events_str_ptr)
          strcpy(events_str, events_str_ptr);
        else{
          strcpy(events_str, "PAPI_TOT_INS,PAPI_TOT_CYC"); /* Default counters */
          std::cout << "[OMP PerfCntrs] No PAPI counters set. Using the default counters: \
                    PAPI_TOT_INS and PAPI_TOT_CYC\n";
        }

        char *event = strtok(events_str, ",");
        while (event) {
          if (t_events == BP_AVAILABLE_CNTRS) {
            std::cerr << "[OMP PerfCntrs] Too many events!\n";
            exit(1);
          }
          if (PAPI_event_name_to_code(event, &event_codes[t_events]) != PAPI_OK){
            std::cerr << "[OMP PerfCntrs] Invalid PAPI event: " << event << std::endl;
            exit(1);
          }
          t_events++;
          event = strtok(NULL, ",");
        }

        if (_BP_PERFCNTRS_VERBOSE) {
          std::cout << "[OMP PerfCntrs] Registering " << t_events << " events\n";
          char event_names[MAX_CNTRS + 1][MAX_STR_LEN];
          int i;
          for (i = 0; i < t_events; i++) {
            PAPI_event_code_to_name(event_codes[i], event_names[i]);
            std::cout << " " << event_names[i] << ",";
          }
          std::cout << "\n";
        }
      }
    } /* end if _BP_PERFCNTRS_INITIALIZED */
  } /* end of pragma omp master */

  #pragma omp barrier

  if (_BP_PERFCNTRS) {
    /* Init all threads */
    #pragma omp parallel 
    {
      int i;

      if (_BP_PERFCNTRS_VERBOSE)
        std::cout << "[OMP PerfCntrs] Thread " << omp_get_thread_num()
                  << " in its init phase\n";

      papi_event_set = PAPI_NULL;
      if (PAPI_create_eventset(&papi_event_set) != PAPI_OK)
        _papi_error((char *)"[OMP PerfCntrs] PAPI_create_eventset");

      for (i = 0; i < t_events; i++)
        if (PAPI_add_event(papi_event_set, event_codes[i]) != PAPI_OK)
          _papi_error((char *)"[OMP PerfCntrs] PAPI_add_event");
    }
  }
}

/* Initalize and reset performance counters */
inline __attribute__((always_inline)) void startPerformanceCounters(){
  if (_BP_PERFCNTRS) {
    #pragma omp parallel
    {
      if (_BP_PERFCNTRS_VERBOSE)
        std::cout << "[OMP PerfCntrs] Thread " << omp_get_thread_num() 
                  << " in its start phase\n";

      perfc_thread_started = 1;
      perfc_omp_parallel_region_count = 0;

      int rc = PAPI_start(papi_event_set);
      if (rc == PAPI_EISRUN) {
        if (PAPI_stop(papi_event_set, event_values) != PAPI_OK)
          _papi_error((char *)"[OMP PerfCntrs] PAPI_stop in startPerformanceCounters");
        if (PAPI_reset(papi_event_set) != PAPI_OK)
          _papi_error((char *)"[OMP PerfCntrs] PAPI_reset in startPerformanceCounters");
        rc = PAPI_start(papi_event_set);
      }
      if (rc != PAPI_OK)
        _papi_error((char *)"[OMP PerfCntrs] PAPI_start in startPerformanceCounters");
    }
    PERF_CNTRS_STARTED = 1;
  }
}

/* Stop instrumentation. Store the values in the output file */
inline __attribute__((always_inline)) void stopPerformanceCounters(){
  if (_BP_PERFCNTRS) {
    if (!PERF_CNTRS_STARTED)
      std::cout << "[OMP PerfCntrs] Trying to stop non-started counters\n";
    
    #pragma omp parallel
    {
      if (!perfc_thread_started) {
        std::cout << "[OMP PerfCntrs] Trying to stop non-started counters (thread "
                  << omp_get_thread_num() << ")\n";
      }

      /* Get the counters values */
      int i, j;

      if (PAPI_stop(papi_event_set, event_values) != PAPI_OK)
        _papi_error((char *)"[OMP PerfCntrs] PAPI_stop in stopPerformanceCounters");

      int tid = omp_get_thread_num();
      int pid = getpid();

      if (_BP_PERFCNTRS_VERBOSE)
        std::cout << "[OMP PerfCntrs] Thread " << tid << " in its stop phase\n";

      FILE *outfile = fopen(event_output_filename, "a");
      if (!outfile){
        std::cerr << "[OMP PerfCntrs] Could not create output file\n";
        exit(1);
      }

      char event_names[MAX_CNTRS + 1][MAX_STR_LEN];
      for (i = 0; i < t_events; i++)
        if (PAPI_event_code_to_name(event_codes[i], event_names[i]) != PAPI_OK)
          _papi_error((char *)"[OMP PerfCntrs] PAPI_event_code_to_name in stopPerformanceCounters");

      #pragma omp critical
      {
        if (_BP_PERFCNTRS_OMPPARALLEL) {
          for (j = 0; j < perfc_omp_parallel_region_count; j++)
            for (i = 0; i < t_events; i++) 
              fprintf(outfile, "%s[%d][%d][%d]=%lld\n", event_names[i], tid, \
                      pid, j, event_values_pbarrier[j][i]);
        }
        /* When BP_PERFCNTRS_OMPPARALLEL is set, the last phase goes from
         * the last "omp parallel" to the end of the region of interest.
         * Otherwise perfc_omp_parallel_region_count will be 0.
         */
        for (i = 0; i < t_events; i++)
          fprintf(outfile, "%s[%d][%d][%d]=%lld\n", event_names[i], tid, pid,
                  perfc_omp_parallel_region_count, event_values[i]);
      }

      fclose(outfile);
      perfc_thread_started = 0;
    }

    PERF_CNTRS_STARTED = 0;
  }
}

/* Store the performance counters values of the parallel region and do a reset */
inline __attribute__((always_inline)) void parallelRegionPerformanceCounters(){
  if (_BP_PERFCNTRS && _BP_PERFCNTRS_OMPPARALLEL && PERF_CNTRS_STARTED) {
    /* We might have parallel regions BEFORE the region of interest, so
     * it is easier to check if the instrumentation has started here
     */

    /* Check for a nested parallelism */
    if(omp_in_parallel() == 1){
      if (!perfc_thread_started)
        std::cout << "[OMP PerfCntrs] Trying to restart non-started counters (thread "
                  << omp_get_thread_num() << ")\n";

      /* Get the counters values */
      int i;
      if (PAPI_read(papi_event_set, event_values) != PAPI_OK)
        _papi_error((char *)"[OMP PerfCntrs] PAPI_read in parallelRegionPerformanceCounters");

      int tid = omp_get_thread_num();

      if (_BP_PERFCNTRS_VERBOSE)
        std::cout << "[OMP PerfCntrs] Thread " << tid << " saw an OMP parallel region\n";

      if (perfc_omp_parallel_region_count < MAX_PARALLEL_PHASES)
        for (i = 0; i < t_events; i++)
          event_values_pbarrier[perfc_omp_parallel_region_count][i] = event_values[i];
      else
        std::cout << "[OMP PerfCntrs] Run out of space for storing intermediate values \
                  (thread " << tid << ", region " << perfc_omp_parallel_region_count << ")\n";

      perfc_omp_parallel_region_count++;

      if (PAPI_reset(papi_event_set) != PAPI_OK)
        _papi_error((char *)"[OMP PerfCntrs] PAPI_reset in parallelRegionPerformanceCounters");
      
    }else{ /* No nested parallelism */
      #pragma omp parallel
      {
        if (!perfc_thread_started) {
          std::cout << "[OMP PerfCntrs] Trying to restart non-started counters \
                    (thread " << omp_get_thread_num() << ")\n";
        }

        /* Get the counters values */
        int i;

        if (PAPI_read(papi_event_set, event_values) != PAPI_OK)
          _papi_error((char *)"[OMP PerfCntrs] PAPI_read in parallelRegionPerformanceCounters");

        int tid = omp_get_thread_num();

        if (_BP_PERFCNTRS_VERBOSE)
          std::cout << "[OMP PerfCntrs] Thread " << tid << " saw an OMP parallel region\n";

        if (perfc_omp_parallel_region_count < MAX_PARALLEL_PHASES)
          for (i = 0; i < t_events; i++)
            event_values_pbarrier[perfc_omp_parallel_region_count][i] = event_values[i];
        else
          std::cout << "[OMP PerfCntrs] Run out of space for storing intermediate values \
                    (thread " << tid << ", region " << perfc_omp_parallel_region_count << ")\n";

        perfc_omp_parallel_region_count++;

        if (PAPI_reset(papi_event_set) != PAPI_OK)
          _papi_error((char *)"[OMP PerfCntrs] PAPI_reset in parallelRegionPerformanceCounters");
      }
    }
  }
}

/* Store the performance counters values of the barrier region and do a reset */
inline __attribute__((always_inline)) void barrierRegionPerformanceCounters(){
  if (_BP_PERFCNTRS && _BP_PERFCNTRS_OMPPARALLEL && PERF_CNTRS_STARTED) {
    /* We might have parallel regions BEFORE the region of interest, so
    * it is easier to check if the instrumentation has started here
    */
    if (!perfc_thread_started) {
      std::cout << "[OMP PerfCntrs] Trying to restart non-started counters (thread "
                << omp_get_thread_num() << ")\n";
    }

    /* Get the counters values */
    int i;

    if (PAPI_read(papi_event_set, event_values) != PAPI_OK)
      _papi_error((char *)"[OMP PerfCntrs] PAPI_read in parallelRegionPerformanceCounters");

    int tid = omp_get_thread_num();

    if (_BP_PERFCNTRS_VERBOSE)
      std::cout << "[OMP PerfCntrs] Thread " << tid << " saw an OMP parallel region\n";

    if (perfc_omp_parallel_region_count < MAX_PARALLEL_PHASES)
      for (i = 0; i < t_events; i++)
        event_values_pbarrier[perfc_omp_parallel_region_count][i] = event_values[i];
    else
      std::cout << "[OMP PerfCntrs] Run out of space for storing intermediate values \
                (thread " << tid << ", region " << perfc_omp_parallel_region_count << ")\n";

    perfc_omp_parallel_region_count++;

    if (PAPI_reset(papi_event_set) != PAPI_OK)
      _papi_error((char *)"[OMP PerfCntrs] PAPI_reset in parallelRegionPerformanceCounters");
  }
}


#ifdef __cplusplus
}
#endif

#endif /* __BP_PERFCNTRS_H__ */
