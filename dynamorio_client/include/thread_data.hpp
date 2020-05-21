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

#ifndef THREAD_DATA_H
#define THREAD_DATA_H

#include<cstdint>
#include<unordered_map>
#include<vector>
#include"region.hpp"
#include"dr_defines.h"
#include"reuse_distance.hpp"
#include"dr_api.h"

enum {
  MEMTRACE_TLS_OFFS_BUF_PTR, /* Allocated TLS slot offsets */
  MEMTRACE_TLS_COUNT, /* total number of TLS slots allocated */
};

typedef struct _mem_ref_t{
  app_pc addr;
} mem_ref_t;

extern reg_id_t tls_seg;
extern uint tls_offs;

/* Max number of mem_ref a buffer can have. It should be big enough to hold
 * all entries between clean calls.
 */
#define MAX_NUM_MEM_REFS 4096
#define MEM_BUF_SIZE (sizeof(mem_ref_t) * MAX_NUM_MEM_REFS)

#define TLS_SLOT(tls_base, enum_val) (void **)((byte *)(tls_base) + tls_offs + (enum_val))
#define BUF_PTR(tls_base) *(mem_ref_t **)TLS_SLOT(tls_base, MEMTRACE_TLS_OFFS_BUF_PTR)

/* This class represents all thread data, including inter-barrier region information
 * and reuse distance.
 */
class ThreadData{
public:
  unsigned int tid;
  unsigned int fake_tid; /* Fake thread id for building output files. */
  bool is_master; /* checks for master thread */
  bool in_single; /* omp single calls tracker */
  ReuseDistance *reuse_dist;
  uint64_t cur_bb_tag;
  /* Region data associated with a global identifier */
  std::unordered_map<uint32_t, Region> regions;

  /* All public functions are accessible for the rest of the program */
  ThreadData(int thread_id, bool is_master_thread, uint32_t region_id);

  void add_bb(uint64_t key, uint64_t inst_n);
  void add_address(void);
  void clean_buffer(void);
  void save_barrier(std::string synch_point_name);
  const uint64_t get_instr_count(uint32_t synch_id);

  /* Define sorting according to the thread_id */
  bool operator < (const ThreadData& data) const{
    return (tid < data.tid);
  }

#ifdef VALIDATE
  file_t disassemble_file;
  file_t memory_access_file;
  file_t runtime_bb_file;
  file_t region_file;
#endif

private:
  /* Status for the current region */
  uint64_t cur_instr_count;
  uint32_t cur_synch_id; /* Current synchronization point id. */
  std::unordered_map<uint64_t,uint32_t> cur_bbv;
  std::vector<uint64_t> cur_lru_hist;
  /* Memory buffer containing instructions which have not yet been fed to the treap */
  byte *seg_base;
  mem_ref_t *buf_base;
};

#endif