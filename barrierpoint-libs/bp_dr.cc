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

/* Region of interest library for DynamoRIO BarrierPoint client */

#include <dlfcn.h>
#include <iostream>
#include "sim_api.h"

namespace BarrierPointsNS {

  bool _inside_roi = false;
  bool _dr_flag = false;

  extern "C" void GOMP_barrier (void) {
    typedef void (*GOMP_barrier_t) (void);
    GOMP_barrier_t GOMP_barrier = (GOMP_barrier_t) dlsym(RTLD_NEXT, "GOMP_barrier");

    char * s = secure_getenv("ROI_BP");
    if(s)
      _inside_roi = strtol(s, NULL, 10) == 1 ? true : false;
    
    if(_inside_roi){
      if (!_dr_flag){
        _dr_flag = true;
#ifdef DEBUG
        std::cout << "[BarrierPoint] Start of RoI" << std::endl;
#endif
        SimRoiStart();
        return;
      }
    }else
      if (_dr_flag){
        _dr_flag = false;
#ifdef DEBUG
        std::cout << "[BarrierPoint] End of RoI" << std::endl;
#endif
        SimRoiEnd();
        return;
      }

    return (GOMP_barrier)();
  }
}
