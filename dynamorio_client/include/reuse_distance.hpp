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

#ifndef REUSE_DISTANCE_H
#define REUSE_DISTANCE_H

#include "reuse_distance_impl.h"

typedef uintptr_t addr_t;

/* This class keeps track of the addresses reuse distances */
class ReuseDistance{
public:
  ReuseDistance();
  ReuseDistance(unsigned int skip_distance, bool verify, unsigned int reuse_threshold);
  int_least64_t lookup_distance(addr_t tag);
  std::unique_ptr<line_ref_list_t> ref_list;
  std::unordered_map<addr_t, line_ref_t*> cache_map;

private:
  int total_refs;
  bool verify_skip;
  unsigned int reuse_threshold;
  unsigned int skip_dist;
};

#endif