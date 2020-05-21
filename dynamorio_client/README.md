# DynamoRIO BarrierPoint Client

## How to build

First download and build [DynamoRIO](https://github.com/DynamoRIO/dynamorio).
Then cd into the BarrierPoint client folder (dynamorio_client) and build the client.

BarrierPoint has been built and tested on different Aarch64 and x86-64 machines with GCC 7.

```
$ mkdir build && cd build
$ export DYNAMORIO_BUILD_DIR=path/to/dynamorio_build
$ cmake ..
$ make
```

Make sure your application contains the Region-of-Interest start and stop functions.
Check the main README file for more information.

## BarrierPoint Client Design

The client identifies and selects synchronization points in the code (barrier points or BPs)
and gathers architecture agnostic statistics to classify the different BPs.

### Barrier Points (synchronization points)

The synchronization points are identified by OpenMP directives such as `OMP Parallel`
(which identifies a parallel region) or `OMP Barrier` (explicit synchronization point).

To be able to detect these, the client parses the application libraries at loading time
and adds a callback to all the relevant OpenMP synchronization functions.
These callbacks are private to each thread, since each thread will execute
their own synchronization code and gather their own performance statistics.
We make use of the `ThreadData` class to allocate a 'per-thread local storage' to achieve this.

At the end of the application computation, the tool post-processes all
the gathered data per thread and dumps it into several output files.
These operations are performed in the `BarrierPoint` class.

### Gathering agnostic metrics

The client tracks architecture-agnostic statistics for each of the identified BPs.
The statistics are collect between BPs (in the `Region` class) and are as follows:

- **Basic Block (BB) information**: The application's execution is split
    into basic blocks, which are instruction sequences with a single point of entry and exit.
    For each BB, the client traces the address of its first instruction (identifier),
    its length in instructions and how many times it has been executed by the target application.

- **Least Recently Used (LRU) Stack Distance**: The client also gathers the LRU Stack
    distance, which is the number of distinct memory addresses accessed between
    two consecutive references to the same memory address.

## Output Files

The client outputs many files to the `outputs/barriers` directory, some of them
which will be later used by Simpoint. The list of outputs is as follows:

- **.bp_id** - Contains the BPs that have been identified by the client.
These are associated with a unique identifier and how many times the BP has been
executed by application.

- **.bbv_inscount** - Displays the total number of instructions executed
    by each thread, for each BP. Each column represents a different thread and
    each line a different BP.

- **.bbv_count** - Displays the basic blocks that have been executed by the
    application. An unique incremental identifier is associated to each BB,
    together with the BB length (in terms of instructions).
    It follows the format: `<Basic Block Identifier> : <BB Length>`

- **.ldv_hist** - Displays an histogram for the LRU stack distance for each thread.
Each line represents a different BP.

- **.ldv_bb** - Displays the corresponding LRU stack distance vectors.
Each line represents a different BP.
