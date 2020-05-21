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

/*
  Double precision parallel matrix multiplication.
*/

#include <stdio.h>
#include <stdlib.h>
#include <omp.h>

#define size 10
#define iterations 10

double A[size][size];
double B[size][size];
double C[size][size];

int main( int argc, char* argv[] )
{
    int i, j, k, itr;
    srand(1);

    // Begin Region of Interest
    setenv("ROI_BP", "1", 1);
    #pragma omp barrier

    // Initialize buffers.
    for (i = 0; i < size; ++i) {
        for (j = 0; j < size; ++j) {
            A[i][j] = (double)rand()/(double)RAND_MAX;
            B[i][j] = (double)rand()/(double)RAND_MAX;
            C[i][j] = 0.0;
        }
    }

    for (itr = 0 ; itr < iterations ; itr++) {
        printf("Iteration: %d\n", itr);
        #pragma omp parallel for private(j,k) shared(A,B,C)
        // C <- C + A x B
        for (i = 0; i < size; ++i)
            for (j = 0; j < size; ++j)
                for (k = 0; k < size; ++k)
                    C[i][j] += A[i][k] * B[k][j];
    }

    // End Region of Interest
    setenv("ROI_BP", "0", 1);
    #pragma omp barrier

    return 0;
}
