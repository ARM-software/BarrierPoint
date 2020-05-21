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

#include<string>
#include<iostream>
#include<unordered_map>
#include<inttypes.h>
#include"thread_data.hpp"
#include"dr_api.h"

extern uint64_t cache_line_mask;

/* Per-thread data functions: Saves and gathers the instrumented information to/from
 * each thread. This includes basic block information and LRU stack distances.
 */
ThreadData::ThreadData(int thread_id, bool is_master_thread, uint32_t region_id){
  tid = (unsigned int)thread_id;
  cur_bb_tag = 0;
  in_single = false;
  is_master = is_master_thread;
  cur_synch_id = region_id;
  cur_instr_count = 0;

#ifdef VALIDATE
  std::string region_file_name = std::to_string(thread_id) + "_region";
  region_file = dr_open_file(region_file_name.c_str(), DR_FILE_WRITE_OVERWRITE);
  /* This file stores all the basic blocks that are seen by the tool before
   * being inserted in the code cache.
   */
  std::string disassemble_file_name = std::to_string(thread_id);
  disassemble_file = dr_open_file(disassemble_file_name.c_str(), DR_FILE_WRITE_OVERWRITE);
  /* This file stores all the instructions that are performing any kind of
   * memory access: their operands are being taken into account by the tool for
   * computing the LRU Stack distance.
   */
  std::string memory_file_name = std::to_string(thread_id) + "_mem";
  memory_access_file = dr_open_file(memory_file_name.c_str(), DR_FILE_WRITE_OVERWRITE);
  /* This file stores all the basic blocks that are seen by the tool when
   * actually executed from the target application: these basic blocks are the
   * ones taken into account for computing the basic block vectors (BBVs) within
   * synchronization points.
   */
  std::string runtime_bb_file_name = std::to_string(thread_id) + "_run_bb";

  runtime_bb_file = dr_open_file(runtime_bb_file_name.c_str(), DR_FILE_WRITE_OVERWRITE);
#endif

  /* LRU Stack Distance Initialization */
  reuse_dist = new ReuseDistance(500, false, 100);
  seg_base = reinterpret_cast<byte*>(dr_get_dr_segment_base(tls_seg));
  buf_base = reinterpret_cast<mem_ref_t*>(dr_raw_mem_alloc(MEM_BUF_SIZE,
                              DR_MEMPROT_READ | DR_MEMPROT_WRITE, nullptr));
  DR_ASSERT(seg_base != nullptr && buf_base != nullptr);
  BUF_PTR(seg_base) = buf_base;
}


/* Add the bb into the hash map. We store its address and instructions number
 * in the cur_bbv hash map. We update the actual total number of executed instructions
 * using cur_instr_count counter.
 */
void ThreadData::add_bb(uint64_t key, uint64_t inst_n){

#ifdef VALIDATE
    dr_fprintf(runtime_bb_file, "[DR.thread_data] TID: %d Executed at runtime "
               PFX " with %d instructions\n", tid, key, inst_n);
    dr_flush_file(runtime_bb_file);
#endif

  auto search = cur_bbv.find(key);
  if(search != cur_bbv.end())
    cur_bbv[key] = cur_bbv[key] + inst_n;
  else
    cur_bbv[key] = inst_n;
    
  /* Increment instruction global counter */
  cur_instr_count = cur_instr_count + inst_n;
}


/* Saving all the inter-barrier information */
void ThreadData::save_barrier(std::string synch_point_name){
  regions.insert(std::make_pair(cur_synch_id, Region{synch_point_name, cur_instr_count,
  cur_bbv, cur_lru_hist}));
  DR_ASSERT_MSG(!regions.empty(), "[DR.thread_data] ERROR: Inter-barrier region is empty");

#ifdef VALIDATE
  /* Save a different dump file for each inter-barrier region detected */
  dr_fprintf(region_file, "[DR.thread_data] Saving Barrier for synchronization name %s\n",
             synch_point_name.c_str());
  dr_flush_file(region_file);
  dr_fprintf(region_file, "[DR.thread_data] Total Number of instruction is %" PRIu64 "\n",
             cur_instr_count);
  dr_flush_file(region_file);
  dr_fprintf(region_file, "[DR.thread_data] Thread id is %d\n", tid);
  dr_flush_file(region_file);
  dr_fprintf(region_file, "[DR.thread_data] Basic Block Saved is the following:\n");
  for(auto& bb: cur_bbv){
    dr_fprintf(region_file,"[DR.thread_data] BB @ %" PRIx64 " Instructions %" PRIu32 "\n",
               bb.first, bb.second);
  }
  dr_flush_file(region_file);

  dr_fprintf(region_file, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");

#endif

  /* Update the next synchronization point */
  cur_synch_id++;

  cur_instr_count=0;
  cur_bbv.clear();
  cur_lru_hist.clear();
}


/* Returns the instruction count for the given synchronization id */
const uint64_t ThreadData::get_instr_count(uint32_t synch_id){
  auto search = regions.find(synch_id);
  if(search != regions.end())
    return search->second.instr_count;
  else
    return 0;
}


/* Adds the memory addresses to the treap to extract the LRU stack distance
 * and stores the result in the histogram.
 */
void ThreadData::add_address(void){
  mem_ref_t *mem_ref, *buf_ptr;
  buf_ptr = BUF_PTR(seg_base);

  for(mem_ref = (mem_ref_t *)(buf_base); mem_ref < buf_ptr; mem_ref++){
    /* Address of cache read, depends on the cache block size! */
#ifdef MASK_ADDRESSES
    uint64_t address = reinterpret_cast<uint64_t>(mem_ref->addr) & cache_line_mask;
#else
    uint64_t address = reinterpret_cast<uint64_t>(mem_ref->addr);
#endif

#ifdef VALIDATE
    /* The address we're dumping into the file changes whether you have
     * chosen to mask the memory address or not and according to the value
     * of the cache_line_mask specified
     */
    dr_fprintf(memory_access_file ,"[DR.thread_data] Memory address @ " PFX " \n", address);
#endif

    int_least64_t mem_access_diff  = reuse_dist->lookup_distance(reinterpret_cast<addr_t>(address));
    /* Check if this is the first time the address is added in the treap */
    if (mem_access_diff != -1){
      uint64_t log2_rd = mem_access_diff != 0 ? ((sizeof(unsigned long long)*8-1)
                                                  - __builtin_clzll(mem_access_diff)) : 0;
      if(log2_rd >= cur_lru_hist.size())
        cur_lru_hist.resize(log2_rd+1);
      cur_lru_hist[log2_rd]++;
    }
  }
  /* Reset back the buffer pointer */
  BUF_PTR(seg_base) = buf_base;
  return;
}


/* Clean the memory buffer discarding its data */
void ThreadData::clean_buffer(void){
  BUF_PTR(seg_base) = buf_base;
  return;
}