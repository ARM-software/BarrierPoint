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
 * Copyright (c) 2011-2018 Google, Inc.  All rights reserved.
 * Copyright (c) 2010 Massachusetts Institute of Technology  All rights reserved.
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
 * * Neither the name of Google, Inc., nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL GOOGLE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* DynamoRIO client to detect OMP synchronization points in the code and gather
 * basic block (BB) information as well as the Least Recently Used (LRU) stack distance
 * for each identified barrier.
 * This client works by:
 * - Parsing the application libraries at load time and adding callbacks to all relevant
 * OMP synchronization functions. These callbacks are private to each threat.
 * - During execution, each thread will gather their own performance statistics,
 * basic blocks (BB) and Least Recently Used (LRU) stack distance on their TLS.
 * - For each BB, gather number of instructions (event_app_instruction)
 * and memory references (instrument_mem).
 *  - The memory references are stored into a treap structure (thread_data.cpp)
 * to calculate the LRU.
 * - All the collected data is computed and outputed to simpoint-ready files (barrierpoint.cpp)
 */

/* DynamoRIO dependencies */ 
#include "dr_api.h"
#include "drmgr.h"
#include "drwrap.h"
#include "drreg.h"
#include "drutil.h"
#include "drsyms.h"
#include "droption.h"
#include "drx.h"
/* BarrierPoint dependencies */
#include "region.hpp"
#include "thread_data.hpp"
#include "barrierpoint.hpp"

#include <string.h>
#include <stdint.h>
#include <string>
#include <stddef.h>

void register_trace_events(void);
void unregister_trace_events(void);
static void event_exit(void);
static void event_omp_parallel(void *wrapcxt, OUT void **user_data);
static void event_roi_init(void *wrapcxt, OUT void **user_data);
static void event_roi_end(void *wrapcxt, OUT void **user_data);
static bool enumerate_symbols_event(const char *name, size_t modoffs, void *data);
static void event_barrier(void *wrapcxt, OUT void **user_data);
static void event_single(void *wrapcxt, OUT void **user_data);

/* Thread specific callback functions */
static void event_thread_init(void* drcontext);
static void event_thread_exit(void* drcontext);

void save_data(void *drcontext, std::string omp_name);

/* Index used for keeping track of thread local storage */
static void register_roi_f(const module_data_t *mod);
static int tls_idx;

/* Main module initial address. We use this to filter out whether a basic block 
 * belongs to the application itself or to a dynamically loaded module.
 */
static app_pc exe_start;
reg_id_t tls_seg;
uint tls_offs;

#define MINSERT instrlist_meta_preinsert

typedef struct{
  char const *f_name;
  void (*f_pre)(void *wrapcxt, OUT void **user_data);
  void (*f_post)(void *wrapcxt, void *user_data);
} wrap_callback_t;

static bool in_roi=false;
static bool roi_has_ended=false;
static bool roi_start_detected = false;
static bool roi_end_detected = false;

/* Mutex global variable for synchronizing access to all_thread_data */
BarrierPoint barrierpoint;
static void *synch_mutex;

/* Optional client option for tracing basic block vectors belonging to dynamic libraries */
static droption_t<bool> trace_libraries(
  DROPTION_SCOPE_CLIENT, "trace_libraries", false,
  "Count, in addition, lib instructions",
  "Count, along with the instructions in the application itself, the instructions in "
  "shared libraries."
);

/* Output folder and file option */
static droption_t<std::string> out_path(
  DROPTION_SCOPE_CLIENT, "out_path", "./barrierpoint",
  "Specify output folder and file name",
  "Specify output folder and file name"
);

uint64_t cache_line_mask;

droption_t<uint64_t> cache_line_size_bytes(
  DROPTION_SCOPE_CLIENT, "cache_line_size_bytes", 64,
  "Trace each single specific memory address instead of masking them with the cache line block",
  "Trace each single specific memory address instead of masking them with the cache line block"
);

/* Option to ignore identified barriers generated by OMP_SINGLE */
static droption_t<bool> ignore_single(
  DROPTION_SCOPE_CLIENT, "ignore_single", false,
  "Ignore omp single and thus its corresponding implicit parallel",
  "Ignore omp single and thus its correspondign implicit parallel"
);

#define GOMP_F_SIZE 2

/* OMP function names to track */
static wrap_callback_t gomp_f[] = {
  {.f_name="GOMP_barrier", .f_pre=event_barrier, .f_post=NULL},
  {.f_name="GOMP_single_start", .f_pre=event_single, .f_post=NULL}
};

/* Regions of interest */
static wrap_callback_t roi_f[] = {
  {.f_name="SimRoiStart", .f_pre=event_roi_init, .f_post=nullptr},
  {.f_name="SimRoiEnd", .f_pre=event_roi_end, .f_post=nullptr}
};

/* Dictionary of functions called right afterwards the parallel omp function call.
 * We use this to understand, when called back, their exact function name.
 */
std::unordered_map<app_pc, std::string> parallel_omp_f;

/* Function list to be registered for a callback */
std::vector<std::tuple<std::string, std::size_t>> omp_functions;


/* Callback called upon module loading for matching all of the
 * non-synchronization but omp related function calls.
 */
static bool enumerate_symbols_event(const char *name, size_t modoffs, void *data){
  std::string f_name(name);

  std::size_t omp_f_found = f_name.find("omp_fn.");
  /* Add the found openMP function call to this global variable.
   * We are going to register it in the register_omp function
   */
  if(omp_f_found != std::string::npos)
    omp_functions.push_back(std::tuple<std::string, std::size_t>(f_name, modoffs));

  return true;
}


static void event_roi_init(void *wrapcxt, OUT void **user_data){
  dr_printf("[DR] RoI has been initialized\n");
  roi_start_detected = true;
  
  /* Register all the useful callbacks, but only once.
   * This may be executed by multiple threads.
   */
  static bool only_once = true;
  if(only_once){
    register_trace_events();
    in_roi=true;
    only_once = false;
  }else
    DR_ASSERT_MSG(false,"[DR] ERROR: You have defined multiple regions of interest");
}


static void event_roi_end(void *wrapcxt, OUT void **user_data){
  dr_printf("[DR] RoI has ended\n");
  roi_end_detected = true;
  
  /* Unregister all the useful callbacks, but only once.
   * This may be executed by multiple threads.
   */
  static bool only_once = true;
  if(only_once){
    unregister_trace_events();
    in_roi=false;
    roi_has_ended=true;
    only_once = false;
  }else
    DR_ASSERT_MSG(false, "[DR] ERROR: You have defined multiple regions of interest");

  /* roi_end is equivalent to 'thread_exit' for the master thread: we save here
   * its last synchronization point
   */
  ThreadData *data = reinterpret_cast<ThreadData*>(
      drmgr_get_tls_field(drwrap_get_drcontext(wrapcxt), tls_idx));
  if(data->is_master)
    save_data(drwrap_get_drcontext(wrapcxt), "thread_exit");
  else
    DR_ASSERT_MSG(false, "[DR] ERROR: Thread executing ROI end function is NOT master");
}


static void register_roi_f(const module_data_t *mod){
  /* Initialize drsyms */
  for(int i=0; i < GOMP_F_SIZE ; i++){
    size_t modoffs = 0;
    drsym_error_t symres = DRSYM_ERROR;
    symres = drsym_lookup_symbol(mod->full_path, roi_f[i].f_name, &modoffs, DRSYM_DEMANGLE);
    if(symres == DRSYM_SUCCESS){
      app_pc startup_wrap = modoffs + mod->start;
      dr_printf("[DR] wrapping %s @" PFX "\n", roi_f[i].f_name, startup_wrap);
      drwrap_wrap(startup_wrap,roi_f[i].f_pre, roi_f[i].f_post);
    }
  }
}


/* After OMP parallel, the compiler adds in the code an invocation for a symbol containing
 * "FUNCTION_WHERE_OMP_PARALLEL_HAS_BEEN_DEFINED.omp_fn.INCREMENTAL_NUMBER".
 * We want to trace that back in such a way that in the bp_id file we know
 * exactly where in the code the omp parallel invocations are defined in the code.
 */
static void register_omp_f(const module_data_t * mod){
  drsym_enumerate_symbols(mod->full_path, enumerate_symbols_event, NULL, DRSYM_LEAVE_MANGLED);
  /* If a new omp function has been found in a global variable, register that function */
  size_t modoffs;
  std::string f_name;
  if(!omp_functions.empty()){
    for(auto& f : omp_functions){
      std::tie(f_name, modoffs) = f;
      app_pc startup_wrap = modoffs + mod->start ;
      dr_fprintf(STDERR, "[DR] wrapping %s @" PFX "\n", f_name.c_str(), startup_wrap);
      drwrap_wrap(startup_wrap, event_omp_parallel, nullptr);
      /* Register the wrapped functions in a dictionary to be used in the callback event */
      auto search = parallel_omp_f.find(startup_wrap);
      if(search == parallel_omp_f.end())
        parallel_omp_f[startup_wrap] = f_name;
    }
    omp_functions.clear();
  }
}


static void event_omp_parallel(void *wrapcxt, OUT void **user_data){
  /* Get current virtual address for this function */
  app_pc f_addr = drwrap_get_func(wrapcxt);
  /* Gather the current function name matching the current address to the
   * parallel_omp_f dictionary.
   */
  auto search = parallel_omp_f.find(f_addr);
  if(search != parallel_omp_f.end()){
    if(in_roi){
      void* drcontext = drwrap_get_drcontext(wrapcxt);
      save_data(drcontext, search->second);
    }
  }else{
    dr_printf("[DR] ERROR: Failed looking up for function at address: " PFX "\n", f_addr);
    DR_ASSERT(false);
  }
}

#ifdef VALIDATE
static file_t modules_f;
#endif


/* Register omp callback functions when loading libGOMP, roi_init and roi_end */
static void module_load_event(void *drcontext, const module_data_t *mod, bool loaded){
#ifdef VALIDATE
  static bool first_time = true;
  /* Keep an external file with a list of all loaded modules (for validation only) */
  if(first_time){
    modules_f = dr_open_file("loaded.modules", DR_FILE_WRITE_OVERWRITE);
    dr_fprintf(modules_f, "[DR] loading %s @" PFX "\n", mod->full_path, mod->start);
    first_time = false;
  }else
    dr_fprintf(modules_f, "[DR] loading %s @" PFX "\n", mod->full_path, mod->start);
#endif

  /* Register ROI detection functions */
  register_roi_f(mod);
  /* Register omp generic functions */
  register_omp_f(mod);

  /* Register function call */
  const char *ret = strstr(dr_module_preferred_name(mod), "libgomp.so");
  if (ret != NULL){
    for(int i=0; i < GOMP_F_SIZE; i++){
      app_pc towrap = (app_pc)dr_get_proc_address(mod->handle, gomp_f[i].f_name);
      if (towrap != NULL){
        bool ok =  drwrap_wrap(towrap, gomp_f[i].f_pre, gomp_f[i].f_post);
        if (ok)
          dr_fprintf(STDERR, "[DR] wrapped %s @" PFX "\n", gomp_f[i].f_name, towrap);
        else
          dr_fprintf(STDERR, "[DR] FAILED to wrap %s  @" PFX
                             ": already wrapped?\n", gomp_f[i].f_name ,towrap);
      }
    }
  }
}


/* Dumps the memory reference info to the log file */
static void clean_call(uint64_t inst_count, uint64_t address){
  void *drcontext = dr_get_current_drcontext();
  ThreadData *data = reinterpret_cast<ThreadData*>(drmgr_get_tls_field(drcontext, tls_idx));
#ifdef TRACE_MEM_BEFORE_ROI
  /* Save basic blocks */
  if(in_roi)
    data->add_bb(address, inst_count);
  if(!roi_has_ended) /* Save memory addresses being accessed in this bb */
    data->add_address();
  else
    data->clean_buffer();
#else
  if(in_roi){
    data->add_bb(address, inst_count);
    data->add_address();
  }else
    data->clean_buffer();
#endif
}


static void
insert_load_buf_ptr(void *drcontext, instrlist_t *ilist, instr_t *where, reg_id_t reg_ptr){
  dr_insert_read_raw_tls(drcontext, ilist, where, tls_seg,
                         tls_offs + MEMTRACE_TLS_OFFS_BUF_PTR, reg_ptr);
}


static void
insert_update_buf_ptr(void *drcontext, instrlist_t *ilist, instr_t *where,
                      reg_id_t reg_ptr, int adjust){
  MINSERT(
    ilist, where,
    XINST_CREATE_add(drcontext, opnd_create_reg(reg_ptr), OPND_CREATE_INT16(adjust))
  );
  dr_insert_write_raw_tls(drcontext, ilist, where, tls_seg,
                          tls_offs + MEMTRACE_TLS_OFFS_BUF_PTR, reg_ptr);
}


static void
insert_save_addr(void *drcontext, instrlist_t *ilist, instr_t *where, opnd_t ref,
                 reg_id_t reg_ptr, reg_id_t reg_addr){
  bool ok;
  /* we use reg_ptr as scratch to get addr */
  ok = drutil_insert_get_mem_addr(drcontext, ilist, where, ref, reg_addr, reg_ptr);
  DR_ASSERT(ok);
  insert_load_buf_ptr(drcontext, ilist, where, reg_ptr);
  MINSERT(ilist, where,
          XINST_CREATE_store(drcontext,
                             OPND_CREATE_MEMPTR(reg_ptr, offsetof(mem_ref_t, addr)),
                             opnd_create_reg(reg_addr)));
}


/* Insert inline code to add a memory reference info entry into the buffer */
static void
instrument_mem(void *drcontext, instrlist_t *ilist, instr_t *where, opnd_t ref,
               bool write){
#ifdef VALIDATE
  ThreadData *data = reinterpret_cast<ThreadData*>(drmgr_get_tls_field(drcontext, tls_idx));
  dr_fprintf(data->memory_access_file ,"[DR] TID %d mem instrumenting PC " PFX "\n", 
             data->tid, instr_get_app_pc(where));
  dr_flush_file(data->disassemble_file);
#endif

  /* We need two scratch registers */
  reg_id_t reg_ptr, reg_tmp;
  if (drreg_reserve_register(drcontext, ilist, where, NULL, &reg_ptr) != DRREG_SUCCESS ||
      drreg_reserve_register(drcontext, ilist, where, NULL, &reg_tmp) !=DRREG_SUCCESS){
    DR_ASSERT(false); /* cannot recover */
    return;
  }
  /* Inject code that saves the address into the buffer */
  insert_save_addr(drcontext, ilist, where, ref, reg_ptr, reg_tmp);
  insert_update_buf_ptr(drcontext, ilist, where, reg_ptr, sizeof(mem_ref_t));
  /* Restore scratch registers */
  if (drreg_unreserve_register(drcontext, ilist, where, reg_ptr) != DRREG_SUCCESS ||
      drreg_unreserve_register(drcontext, ilist, where, reg_tmp) != DRREG_SUCCESS)
    DR_ASSERT(false);
}


/* For each memory reference app instr, we insert inline code to fill the buffer
 * with an instruction entry and memory reference entries.
 */
static dr_emit_flags_t
event_app_instruction(void *drcontext, void *tag, instrlist_t *bb, instr_t *instr,
                      bool for_trace, bool translating, void *user_data){
  /* drmgr enables auto-predication by default, which predicates
   * all instructions with the predicate of the current instruction on Arm.
   * We disable it because we want to unconditionally execute the
   * following lines of instrumentation
   */
  drmgr_disable_auto_predication(drcontext,bb);

  /* By default, take into account only BBs belonging to the application itself.
   * Setting the 'trace_libraries' parameter enables DR to trace all dynamic libraries
   */
  module_data_t *mod = dr_lookup_module(dr_fragment_app_pc(tag));
  if (!trace_libraries.get_value())
    if (mod != NULL){
      bool belongs_to_app = (mod->start == exe_start);
      dr_free_module_data(mod);
      if (!belongs_to_app)
        return DR_EMIT_DEFAULT;
    }

  if (instr_is_app(instr)){
    if (instr_reads_memory(instr) || instr_writes_memory(instr)){
      int i;
      /* insert code to add an entry for each memory reference opnd */
      for (i = 0; i < instr_num_srcs(instr); i++){
        if (opnd_is_memory_reference(instr_get_src(instr, i)))
          instrument_mem(drcontext, bb, instr, instr_get_src(instr, i), false);
      }

      for (i = 0; i < instr_num_dsts(instr); i++){
        if (opnd_is_memory_reference(instr_get_dst(instr, i)))
          instrument_mem(drcontext, bb, instr, instr_get_dst(instr, i), true);
      }
    }

    if(drmgr_is_first_instr(drcontext, instr)){
      /* Extract the number of instructions in the basic block */
      uint64_t instr_count = 0;
      instr_t *instr_it;
      for(instr_it = instrlist_first_app(bb); instr_it != NULL;
          instr_it = instr_get_next_app(instr_it)){
        if(!instr_is_meta(instr_it))
          instr_count++;
      }
      uint64_t address = reinterpret_cast<uint64_t>(tag);

    if (/* XXX i#1698: there are constraints for code between ldrex/strex pairs,
         * so we minimize the instrumentation in between by skipping the clean call.
         * As we're only inserting instrumentation on a memory reference, and the
         * app should be avoiding memory accesses in between the ldrex...strex,
         * the only problematic point should be before the strex.
         * However, there is still a chance that the instrumentation code may clear the
         * exclusive monitor state.
         * Using a fault to handle a full buffer should be more robust, and the
         * forthcoming buffer filling API (i#513) will provide that.
         */
      IF_AARCHXX_ELSE(!instr_is_exclusive_store(instr), true)){
        dr_insert_clean_call(drcontext, bb, instr, (void *)clean_call, false, 2,
                             OPND_CREATE_INT64(instr_count), OPND_CREATE_INT64(address));
    }
    /* Since we want to keep track for the execution of a whole basic block, we save its
     * statistics the first time we see it and skip the other ones. */

#ifdef VALIDATE
    ThreadData *data = reinterpret_cast<ThreadData*>(drmgr_get_tls_field(drcontext, tls_idx));
    instrlist_disassemble(drcontext, (app_pc)tag, bb, data->disassemble_file);
    dr_flush_file(data->disassemble_file);
#endif

    }
  }
  return DR_EMIT_DEFAULT;
}


/* Memtrace Callback.
 * We transform string loops into regular loops so that we can more easily
 * monitor every memory reference.
 */
static dr_emit_flags_t
event_bb_app2app(void *drcontext, void *tag, instrlist_t *bb, bool for_trace,
                 bool translating){
  DR_ASSERT_MSG(drutil_expand_rep_string(drcontext, bb), "[DR] Error: Failed expanding rep string");
  return DR_EMIT_DEFAULT;
}


void register_trace_events(void){
  /* Ignore thread information that started and finished execution before the ROI */
  drmgr_register_thread_exit_event(event_thread_exit);
}


void unregister_trace_events(void){
  drmgr_unregister_thread_init_event(event_thread_init);
  drmgr_unregister_bb_app2app_event(event_bb_app2app);
  drmgr_unregister_bb_insertion_event(event_app_instruction);
}


/* Upon execution ending, save all the data we've gathered into files */
static void event_exit(void){
  dr_printf("%s\n", "[DR] BarrierPoint client execution is ending");
  drmgr_unregister_tls_field(tls_idx);
  drmgr_unregister_module_load_event(module_load_event);
  drmgr_unregister_thread_exit_event(event_thread_exit);
  /* Assert if ROI was set */
  if(!roi_start_detected)
    DR_ASSERT_MSG(false, "[DR] SimRoiStart has not been detected. \
                  Have you defined it in the source code?");
  if(!roi_end_detected)
    DR_ASSERT_MSG(false, "[DR] SimRoiEnd has not been detected. \
                  Have you defined it in the source code?");

  /* Dump Traces into a file */
  barrierpoint.save(out_path.get_value());

  barrierpoint.free();
  dr_mutex_destroy(synch_mutex);
  drwrap_exit();
  drutil_exit();
  drreg_exit();
  drmgr_exit();
  drsym_exit();
}


static void event_barrier(void *wrapcxt, OUT void **user_data){
  if(in_roi){
    void* drcontext = drwrap_get_drcontext(wrapcxt);
    ThreadData *data = reinterpret_cast<ThreadData*>(drmgr_get_tls_field(drcontext, tls_idx));
    if(!data->in_single)
      save_data(drcontext, "GOMP_barrier");
    /* If set, ignore implicit barriers generated by OMP Single */
    else{
      dr_printf("[DR] Barrier Ignored due to a single\n");
      data->in_single=false;
    }
  }
}


/* Called on synchronization points, this function saves all the current intra-barrier
 * region information gathered so far.
 */
void save_data(void *drcontext, std::string omp_name){
  ThreadData *data = reinterpret_cast<ThreadData*>(drmgr_get_tls_field(drcontext, tls_idx));
  data->save_barrier(omp_name);
  /* Update the synchronization point number */
  if(data->is_master)
    barrierpoint.incr_synch_count();
}


/* Updates the thread internal status for tracking that the next barrier
 * will be implicitly generated by omp single
 */
static void event_single(void *wrapcxt, OUT void **user_data){
  if(in_roi && ignore_single.get_value()){
    ThreadData *data = reinterpret_cast<ThreadData*>(drmgr_get_tls_field(drwrap_get_drcontext(wrapcxt), tls_idx));
    data->in_single = true;
  }
}


/* Initialize, per each thread created, its corresponding data structure representation.
 * If a thread is spawned outside of the ROI, it will not take into account
 * synchronization points or bb/ldv collecting. */
static void event_thread_init(void* drcontext){
  static bool first_time_called = true;
  bool is_master_thread = false;

  if(first_time_called){
    is_master_thread = true;
    first_time_called = false;
  }

  /* Allocate per thread specific data structure */
  ThreadData* data = reinterpret_cast<ThreadData*>(dr_thread_alloc(drcontext, sizeof(data)));
  DR_ASSERT_MSG(data != NULL,"Failed Allocating per_thread data");
  data = new ThreadData{dr_get_thread_id(drcontext),is_master_thread, barrierpoint.synch_count};
  drmgr_set_tls_field(drcontext,tls_idx,data);
}


/* Upon thread exiting, we save all the important collected data.
 * event_thread_exit can be seen as a "Synchronization point".
 */
static void event_thread_exit(void* drcontext){
  ThreadData *data = reinterpret_cast<ThreadData*>(drmgr_get_tls_field(drcontext, tls_idx));
  /* We treat thread_exit as a last synchronization point.
   * The master thread has its correspondent in roi_end.
   */
  if(!data->is_master)
    save_data(drcontext, "thread_exit");

  /* Add the new per_thread_t with global scope to all_thread_data */
  dr_mutex_lock(synch_mutex);

  barrierpoint.add_thread_data(*(data));

  dr_mutex_unlock(synch_mutex);

#ifdef VALIDATE
  dr_close_file(data->disassemble_file);
  dr_close_file(data->memory_access_file);
  dr_close_file(data->runtime_bb_file);
#endif

  dr_thread_free(drcontext, data, sizeof(data));

  return;
}


DR_EXPORT void dr_client_main(client_id_t id, int argc, const char *argv[]){
  dr_set_client_name("BarrierPoint client",
                     "https://github.com/ARM-software/BarrierPoint");
  dr_log(NULL, DR_LOG_ALL, 1, "[DR] Barrierpoint initializing\n");
  if (dr_is_notify_on())
    dr_fprintf(STDERR, "[DR] BarrierPoint is running\n");
  if (!droption_parser_t::parse_argv(DROPTION_SCOPE_CLIENT, argc, argv, NULL, NULL))
    DR_ASSERT_MSG(false, "[DR] ERROR: Could't parse correctly the flag options");
  
  /* Initialize Cache Line mask */
  cache_line_mask = ~(cache_line_size_bytes.get_value()-1);

  /* Get main module address */
  module_data_t *exe = dr_get_main_module();
  if (exe != NULL)
    exe_start = exe->start;
  else
    DR_ASSERT_MSG(false, "[DR] ERROR: Couldn't find where the main module starts");
  dr_free_module_data(exe);

  /* We need 2 reg slots beyond drreg's eflag slots => 3 slots */
  drreg_options_t ops = { sizeof(ops), 3, false};
  /* Initialize the client */
  if(!drmgr_init() || !drwrap_init() || drreg_init(&ops) != DRREG_SUCCESS || !drutil_init())
    DR_ASSERT(false);

  drsym_init(0);

  /* Reserves a thread-local storage for every thread */
  tls_idx = drmgr_register_tls_field();
  DR_ASSERT_MSG(tls_idx > 0," drmgr_register_tls_field() Failed\n");
  drmgr_register_module_load_event(module_load_event);
  /* We want to trace threads initialization even if we are outside of the ROI */
  drmgr_register_thread_init_event(event_thread_init);
  dr_register_exit_event(event_exit);

  /* We instrument basic blocks no matter if they are inside the ROI or not */
  drmgr_register_bb_instrumentation_event(nullptr, event_app_instruction, nullptr);
  drmgr_register_bb_app2app_event(event_bb_app2app, nullptr);

  /*Initialize the mutex */
  synch_mutex = dr_mutex_create();
  /* The TLS field provided by DR cannot be directly accessed from the code
   * cache. For better performance, we allocate a raw TLS so that we can
   * directly access and update it with a single instrucion.
   */
  DR_ASSERT(dr_raw_tls_calloc(&tls_seg, &tls_offs, MEMTRACE_TLS_COUNT, 0));
}