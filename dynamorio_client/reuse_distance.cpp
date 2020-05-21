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

/* **********************************************************
 * Copyright (c) 2016-2019 Google, Inc.  All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Google, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* Reuse distance functions */
#include "reuse_distance.hpp"

ReuseDistance::ReuseDistance(){
  skip_dist=500;
  verify_skip=false;
  reuse_threshold=100;
  ref_list = std::unique_ptr<line_ref_list_t>(
  new line_ref_list_t(reuse_threshold, skip_dist, verify_skip));
}


ReuseDistance::ReuseDistance(unsigned int skip_distance, bool verify, unsigned int threshold){
  verify_skip=verify;
  reuse_threshold=threshold;
  skip_dist=skip_distance;
  ref_list = std::unique_ptr<line_ref_list_t>(
  new line_ref_list_t(reuse_threshold, skip_dist, verify_skip));
}


/* Given a memory address, it returns the LRU stack distance.
 * It returns -1 if it's the first time the address has been used.
 */
int_least64_t ReuseDistance::lookup_distance(addr_t tag){
  int_least64_t dist;
  total_refs++;
  std::unordered_map<addr_t, line_ref_t *>::iterator it = cache_map.find(tag);

  if (it == cache_map.end()){
    line_ref_t *ref = new line_ref_t(tag);
    /* insert into the map */
    cache_map.insert(std::pair<addr_t, line_ref_t *>(tag, ref));
    /* insert into the list */
    ref_list->add_to_front(ref);
    dist = -1;
  }else
    dist = ref_list->move_to_front(it->second);
  return dist;
}