/* **********************************************************
 * Copyright (c) 2020, Arm Limited and Contributors.
 * **********************************************************/

/*
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

#ifndef BARRIERPOINT_H
#define BARRIERPOINT_H

#include <vector>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <algorithm>
#include <queue>
#include "thread_data.hpp"
#include "dr_api.h"

/* This class gathers together all the piece of data needed for running the
 * BarrierPoint methodology.
 */
class BarrierPoint{
public:
  BarrierPoint();
  void incr_synch_count(void);
  void add_thread_data(ThreadData data);
  uint32_t synch_count; /* Current synchronization point id. */
  void save(std::string output_path);
  void free(void);

private:
  /* Status for the current region */
  std::vector<ThreadData> threads;
  /* In this vector we save the parallel function names called right after omp parallel */
  void save_bbv_inst_count(std::string out_path);
  void save_bp_id(std::string out_path);
  void save_bbv_count(std::string out_path);
  void save_ldv_hist(std::string out_path);
  void save_ldv_bb(std::string out_path);
  void generate_fake_tid(void);
  void align_synch_bb(void);
};

#endif
