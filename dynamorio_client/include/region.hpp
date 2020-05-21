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

#ifndef INTER_BARRIER_REGION_H
#define INTER_BARRIER_REGION_H

#include<string>
#include<unordered_map>
#include<vector>


/* This class represents the region between different synchronization points.
 * We use this to store and manage the performance data we gather during the
 * execution, such as BBV and LRU Stack Distance.
 */
class Region{
public:
  Region(std::string synch_point_name, uint64_t total_instr,
         std::unordered_map<uint64_t,uint32_t> current_bbv,
         std::vector<uint64_t> lru_stack_dist);

  std::string synch_name;
  uint64_t instr_count;
  std::unordered_map<uint64_t, uint32_t> bbv;
  std::vector<uint64_t> lru_hist; /* LRU Stack Distance histogram */
};

#endif