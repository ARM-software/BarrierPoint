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

/* BarrierPoint DR client: Gathers basic block information and computes
 * LRU stack distance with corresponding LDVs. Creates the necessary input files
 * for simpoint execution.
 */

#include"barrierpoint.hpp"
extern std::unordered_map<app_pc, std::string> parallel_omp_f;


BarrierPoint::BarrierPoint(){
  synch_count = 0;
}


/* Increment the synchronization number */
void BarrierPoint::incr_synch_count(void){
  synch_count++;
}


/* Add the single thread data, to be able to post process it */
void BarrierPoint::add_thread_data(ThreadData data){
  threads.push_back(data);
}


/* Save all the gathered data into files */
void BarrierPoint::save(std::string output_path){
  std::cout << "Saving data into " << output_path << std::endl;
  generate_fake_tid();
  align_synch_bb();
  save_bbv_inst_count(output_path);
  save_bp_id(output_path);
  save_bbv_count(output_path);
  save_ldv_hist(output_path);
  save_ldv_bb(output_path);

}


/* DynamoRIO traces the basic blocks corresponding to the parallel
 * synchronization functions as belonging to the region being ended when,
 * instead, they have to be taken into account in the next region.
 */
void BarrierPoint::align_synch_bb(void){
  uint64_t bb_count_balance=0;
  for(auto &t: threads){
    for(uint32_t id=0; id < synch_count; id++){
      auto region = t.regions.find(id);
      /* Update the instruction count balance from the previous region */
      region->second.instr_count = region->second.instr_count + bb_count_balance;
      bb_count_balance=0;

      /* For all the omp parallel equivalent functions,
       * look for its corresponding basic block in this region: if found, print it.
       */
      for(auto &f_omp : parallel_omp_f){
        uint64_t bb_addr = reinterpret_cast<uint64_t>(f_omp.first);
        auto bb_found = region->second.bbv.find(bb_addr);
        if(bb_found != region->second.bbv.end()){
          region->second.bbv.erase(bb_addr);
          bb_count_balance = bb_found->second;
          if(region->second.instr_count >= bb_count_balance)
            region->second.instr_count = region->second.instr_count - bb_count_balance;
          else
            region->second.instr_count = 0;
        }
      }
    }
  }
}


/* Generate fake thread ids: we need this for saving the thread information inside the output files */
void BarrierPoint::generate_fake_tid(void){
  /* The master thread will be the first element in the vector. */
  std::sort(threads.begin(), threads.end());
  for(std::size_t i=0; i < threads.size(); i++){
    threads[i].fake_tid = i;
  }
}


/* Dump the ldv histogram */
void BarrierPoint::save_ldv_hist(std::string out_path){
  std::ofstream ldv_fp(out_path + ".ldv_hist", std::ofstream::out);
  for(auto &t: threads){
    for(uint32_t id=0; id < synch_count; id++){
      auto region = t.regions.find(id);
      ldv_fp << "Th:" << std::setw(3) << t.fake_tid << " b:" << std::setw(4) << id;
      if(region != t.regions.end())
        /* Actually dump the histogram content (ent = entry) */
        for(auto &ent : region->second.lru_hist)
          ldv_fp << " " << std::setw(10) << ent;
      /* If the thread has not seen that synchronization point, its histogram is empty */
      ldv_fp << std::endl;
    }
  }
  ldv_fp.close();
}


void BarrierPoint::save_ldv_bb(std::string out_path){
  /* Compute the maximum size of the saved LRU Stack Distance */
  size_t max_size = 0;
  for(auto &t: threads){
    for(uint32_t id=0; id < synch_count; id++){
      auto region = t.regions.find(id);
      if(region != t.regions.end())
        max_size = std::max(max_size,region->second.lru_hist.size());
    }
  }


  /* Actually dump the file */
  std::ofstream ldv_fp(out_path + ".ldv_bb", std::ofstream::out);
  auto delim = "W";
  auto actual_delim = "T";
  for(uint32_t synch_id=0; synch_id < synch_count; synch_id++){
    ldv_fp << delim;
    delim=actual_delim;
    for(auto &t: threads){
      int entry_count=0;
      auto region = t.regions.find(synch_id);
      if(region != t.regions.end())
        for(auto &ent : region->second.lru_hist){
          if(ent !=0)
            ldv_fp << ":" << 1+entry_count+(t.fake_tid*max_size) << ":" << ent << " ";
          entry_count++;
        }
    }
    ldv_fp << std::endl;
  }
  ldv_fp.close();
  system((std::string("gzip -f " + out_path  +".ldv_bb").c_str()));
}


/* Dump the instruction count for all the inter-barrier regions of all threads */
void BarrierPoint::save_bbv_inst_count(std::string out_path){
  std::ofstream fp_inst(out_path + ".bbv_inscount", std::ofstream::out);
  auto delim = "";
  auto actual_delim = ",";
  for(uint32_t synch_id=0; synch_id < synch_count; synch_id++){
    delim = "";
    for(auto& t : threads){
      fp_inst << delim << t.get_instr_count(synch_id);
      delim = actual_delim;
    }
    fp_inst << "\n";
  }
  fp_inst.close();
  system((std::string("gzip -f " + out_path  + ".bbv_inscount").c_str()));
}


/* Save all the synchronization points seen by the master thread. */
void BarrierPoint::save_bp_id(std::string out_path){
  /* Gather the master thread */
  std::ofstream bp_id(out_path + ".bp_id", std::ofstream::out);
  /* Temporary hash data structure for taking into account how many times we
   * have seen the same synchronization function
   */
  std::unordered_map<std::string, uint32_t> synch_times;

  bp_id << "BarrierPoint,Iteration,Routine" << std::endl;
  for(auto t : threads){
    /* Just pick the routines for the master thread */
    if(t.is_master){
      /* The first synchronization point is implicitly the ROI_start: */
      bp_id << "0,0,ROI_Start" << std::endl;
      for(uint32_t id=0; id < synch_count; id++){
        auto region = t.regions.find(id);
        DR_ASSERT_MSG(region != t.regions.end(),
                      "[DR.barrierpoint] Master Thread is missing some piece of info");

        /* Keep track of how many times we've seen this function so far */
        std::string f_name = region->second.synch_name;

        if(synch_times.count(f_name) == 0)
          synch_times[f_name] = 0;
        else
          synch_times[f_name]++;
        /* We output id+1 because of the implicit starting point for ROI_start+1
         * Inter-barrier regions are identified by the name of the initial sync point
         */
        if(f_name != "thread_exit")
          bp_id << id+1 << "," << synch_times[region->second.synch_name]
                << "," << f_name << std::endl;
      }
    }
  }
  bp_id.close();
}

/* Iterate over the basic blocks and associate all of them to a unique identifier,
 * saving to output at the end. We do this with an unordered map.
 */
void BarrierPoint::save_bbv_count(std::string out_path){
  int64_t id = 1;
  /* bb_ids in the format: <bb tag> --> id */
  std::unordered_map<uint64_t, uint32_t> bb_ids;
  /* Initialize bb_ids, which associates BBs to IDs */
  for(auto &t : threads){
    /* For each region, forcing the time ordering */
    for(uint32_t synch_id=0; synch_id < synch_count; synch_id++){
      auto search = t.regions.find(synch_id);
      if(search != t.regions.end())
        for(auto &bb : search->second.bbv){
          /* If it's the first time we see a new address, associate it with a new ID */
          if(bb_ids.count(bb.first) == 0){
            bb_ids[bb.first] = id;
            id++;
          }
        }
    }
  }
  int bb_total_number = id - 1;

  /* Save the actual file */
  std::ofstream fp_count(out_path + ".bbv_count", std::ofstream::out);
  
  /* Synch_id is initialized as 1 since all synchronization points
   * are counted starting from 1
   */
  auto delim = "W";
  auto actual_delim = "T";
  for(uint32_t synch_id=0; synch_id < synch_count; synch_id++){
    fp_count << delim;
    delim = actual_delim;
    for(auto &t : threads){
      /* If the thread has seen that synchronization point, you dump all the bb
       * seen with their id and their instruction count (within the bb itself,
       * the actual number of executed instructions is instead saved in the inst_count file).
       */
      uint64_t bb_complex_identifier = 0;
      auto search = t.regions.find(synch_id);
      if(search != t.regions.end())
        for(auto &bb: search->second.bbv){
          /* The output has the format: bb_identifier:instruction_count */
          bb_complex_identifier = bb_ids[bb.first] + (t.fake_tid * bb_total_number);
          fp_count << ":" << bb_complex_identifier <<  ":" << bb.second << " ";
        }
    }
    fp_count << "\n";
  }
  fp_count.close();
  system((std::string("gzip -f " + out_path  + ".bbv_count").c_str()));
}


void BarrierPoint::free(void){
  for(auto &t : threads){
    for(std::pair<addr_t, line_ref_t*> elem : t.reuse_dist->cache_map){
      delete elem.second;
    }
  }
  threads.clear();
}