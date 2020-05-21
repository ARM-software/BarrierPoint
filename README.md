# BarrierPoint

A cross-architecture tool to identify, select and analyse representative regions of parallel OpenMP applications. It allows the representation of whole applications through only their most representative regions, with high accuracy rates.
Based on the work of Trevor Carlson et al. [P1 & R1](#references).

For a quick run check the [build](#build) and [run](#run) sections. A sample OpenMP matrix multiplication code is provided.

The BarrierPoint tool is composed of two parts: the identification and selection of representative regions; and the performance analysis of the selected regions.
The generated output files can be optionally plotted to estimate the error that the representative regions have over executing the whole application.

## Identification and Selection of BPs

BarrierPoint dynamically identifies barrier points (BPs) in an application through a custom DynamoRIO [R2](#references) instrumentation client. BPs are synchronisation points in the code where all OpenMP threads are guaranteed to wait before resuming. Examples of synchronisation points include the start of a parallel section or OMP barriers.

Each BP is represented by architecture-agnostic metrics, namely basic block information (includes number of instructions) and Least Recently Used (LRU) Stack Distance. Check the README file inside `dynamorio_client`for more information on how the DynamoRIO extracts these metrics.

After BPs are identified, BarrierPoint invokes Simpoint [R3](#references), to cluster and weight the BPs and select the most representative ones. Check the used simpoint parameters inside `DrAnalysis.py`.


## BP Performance Analysis

BarrierPoint can also provide a performance analysis of each identified BP as well of the whole application, through performance counters obtained with PAPI [R4](#references).

Currently, BarrierPoint gathers the following performance statistics: cycles, instructions, L1D and L2D misses. We have found that these four statistics can be used to estimate the error of the representative regions versus the whole application.
To facilitate this, we provide plot capabilities to BarrierPoint, that estimate the reconstruction error of an application for any given number of runs.
The number of runs is relevant, since multithread execution is bound to have high variability between threads. The tool was designed with this in mind, easily allowing multiple executions of an application, for both the BP identification and selection, as well as the performance analysis.


## Dependencies

To run BarrierPoint you will need:

- [DynamoRIO](https://github.com/DynamoRIO)
- [Simpoint 3.2](http://cseweb.ucsd.edu/~calder/simpoint/releases/SimPoint.3.2.tar.gz) (fetched through the included Makefile)
- [PAPI]([http://icl.utk.edu/papi/software/](http://icl.utk.edu/papi/software/)) (Tested with version 5.7.0)
- Cmake
- Python 2 or 3
- Matplotlib (for plotting)

## Build

BarrierPoint has been built and tested on different Aarch64 and x86-64 machines with GCC 7.

### 1. Install dependencies

You can get DynamoRIO from [R2](https://www.dynamorio.org/) and PAPI from [R4](https://icl.utk.edu/papi/).
Simpoint is automatically downloaded and installed during the install process.

### 2. Build BP libraries and DR Client

```
# Build BP libraries and simpoint with top-level Makefile
$ make

# Build the DR client
$ cd dynamorio_client && mkdir build && cd build
$ export DYNAMORIO_BUILD_DIR=path/to/dynamorio_build
$ cmake ..
$ make
```

Note: If you are using a custom PAPI build, please set `$PAPI_DIR` inside the main BarrierPoint Makefile.

## Prepare Application Binaries

To be compatible with BarrierPoint, applications need to define a Region-of-Interest (RoI) in their source code. BarrierPoint will only identify representative regions inside the RoI.
To set a compatible RoI, just add the following code snippets to your code, delimiting the sections you want to analyse (please note that only a single RoI is supported at the moment):

**Start RoI:**
```
setenv("ROI_BP", "1", 1);
#pragma omp barrier
```

**Stop RoI:**
```
setenv("ROI_BP", "0", 1);
#pragma omp barrier
```

A sample matrix multiplication application is provided inside the `sample`directory, with the RoI set. By default, the configuration files are already setup for this application. You just need to compile the app and move it to the `benchmarks`folder before running the tool.

## Run BarrierPoint

Modify the configuration file `configs.json` with the parameters you want to run. Check the [Configuration section](#configuration-parameters) below for additional information on the configuration parameters.

Before running BarrierPoint make sure that the application to execute is available in the correct path (set in the configuration file) and its name matches the one in the configuration file.

**Also make sure that the `$DYNAMORIO_BUILD_DIR` flag is set**, so BarrierPoint knows DR location.

```
$ ./run-BarrierPoint.sh
```

## Plotting the Error

Modify the configuration file `configs_plot.json` with the parameters you want to plot, then start the plot generation. Check the [Configuration section](#configuration-parameters) below for additional information on the configuration parameters.

```
$ ./plot-BarrierPoint.sh
```
The plotting step of BarrierPoint generates the error estimation of running just the identified BPs versus the whole application. It contains the errors for all four performance statistics: cycles, instructions, L1D and L2D misses.
Due to variability across runs (greatly caused by load distribution across threads) each figure contains multiple plots, one for each run of the application. All these parameters can be modified in the configuration files.

## Configuration Parameters

BarrierPoint uses JSON configuration files to set the parameters required to run the tool and plot the error estimates.
This sections goes through the configuration file format used in BarrierPoint.

### configs.json
This configuration file refers to the BP identification, selection and performance analysis. All parameters are featured in the provided file and must not be removed (or else they will be invalid).
The default parameters are tailored for the included sample matrix multiplication app. Please change the configuration accordingly to your needs.

- **paths**: List of paths used by BarrierPoint. All default paths are set.
	- **rootdir**: BarrierPoint path.
	- **benchpath**: Application binaries path.
	- **libspath**: BarrierPoint libraries path.
	- **outpath**: Output path.

- **execution**: Defines which BarrierPoint steps will be executed. Set to true or false (Default: both true).
	- **bp_identification** Run the BP identification and selection (uses DynamoRIO).
	- **perfcntrs** Run the performance analysis (uses PAPI).

- **threads**: Set the number of threads used by the applications. This parameter is a list of multiple comma-separated values. The tool will do multiple executions of each application for each number of threads set. This option sets the `OMP_NUM_THREADS` to the number of threads specified. It also substitutes any input in the application parameters in the form of ``{}``, for applications that set the number of threads through an explicit parameter.
	- E.g.: `[2, 4 ,8] `

- **Application**: Set of applications and their respective input parameters, comma-separated. If one of the input parameters is the number of threads, instead of hardcoding the number, you can instead pass ``{}`` . BarrierPoint will then substitute the brackets for the number of threads set in the previous configuration parameter.
	- E.g.: ` { "matrixmul" : " ", "matrixmul2":"-threads {}" } `
	- In the above example we are running two apps: `matrixmul` and `matrixmul2`, which takes the number of threads as an input.

- **Suffix**: Strings to append to the generated files and folders, comma separated. This is useful to identify different runs of the same application under different circumstances (e.g. running the same app with modifications or at a different date). The adopted naming convention for the output files and folders already uses the name of the application and the thread count. If multiple suffixes are provided, multiple executions of the application/threads are done.
	- E.g. `["sample", "dd-mm-yyyy"]
	- The above example will run the provided applications one for every suffix and for every thread count.
	- Using the `matrixmul` as an example, the output files will look like: `matrixmul-sample.4t`and `matrixmul-dd-mm-yyyy.2t`

- **Debug**: Prints additional debug information to a logfile. Default is false.

- **DR_iterations**: Number of iterations for the BP identification and selection step of BarrierPoint. More iterations will result in better error estimates when plotting. This step uses DynamoRIO, so expect it to be slow the more iterations you add. The generated output files will contain the correspondent number of the run.
	- Default is set to 10 iterations. Reduce if it takes too long to generate the barrier points.

- **Perfcntrs_iterations**: Number of iterations for the performance analysis step of BarrierPoint. More iterations will result in better error estimates when plotting. The generated output files will contain the correspondent number of the run.
	- Default is set to 20 iterations. Reduce if it takes too long to gather performance statistics for the app.

- **dry_run**: . Dry BarrierPoint run, without DynamoRIO instrumentation or PAPI performance analysis. This flag requires previously generated files. When on, it will still run the simpoint clustering step and the performance statistics parsing. Useful when sharing BP files and do not want to repeat the whole generation process again. Default is set to false.


### configs_plot.json
This configuration file refers to the plotting of the error estimate between the selected BPs and the whole application. All parameters are featured in the provided file and must not be removed (or else they will be invalid).
The default parameters are tailored for the included sample matrix multiplication app (if you run BarrierPoint before to identify, select and analyse the BPs). Please change the configuration accordingly to your needs.

Note that each plot figure will contain the a plot for each of the iterations set in the application execution before. As long as the application name and suffixes match, the tool will do this automatically. This is due to the variability across runs (greatly caused by load distribution across threads).

- **paths**: List of paths used by BarrierPoint plotting. All default paths are set.
	- **rootdir**: BarrierPoint path.
	- **outpath**: BP Output path (generated by the tool when running an application).
	- **plotpath**: Plot output path. Location where the plots are saved.

- **Application**: Set of applications and their suffixes, comma-separated.  It will plot, for each app, the combination of all *barrier* and *perfcntrs* suffixes. All the files must exist in the `outpath`folder. For each application, you can set:
	- **Barriers_suffix** refers to the suffixes generated during the BP identification and selection step (using DynamoRIO). Comma-separated.
	- **Perfcntrs_suffix** refers to the suffixes generated during the BP performance analysis (using PAPI). Comma-separated.
	- E.g.: ` { "matrixmul" : { "Barriers_suffix": ["sample", "dd-mm-yyyy"] , "Perfcntrs_suffix": ["sample"] } }`
	- In the above example we are plotting 2 graphs for `matrixmul`: the first estimating the error of the selected BPs with suffix `sample`and the second estimating the error of the selected BPs with suffix `dd-mm-yyyy`. In both case, the error estimation uses the same performance statistics, generated in the run with the suffix `sample`.

- **threads**: List of number threads to plot, comma-separated. These have to match the ones previously generated and available in the `outpath`folder. Each thread number will generate a different graph.
	- E.g. `[2, 4, 8]`

- **plot_format**: Output plot file format. Accepts `png` or `pdf`. Default is `pdf`.

- **Debug**: Prints additional debug information to a logfile. Default is false.


## Publications

- [P1](http://dx.doi.org/10.1109/ISPASS.2014.6844456) Carlson, T. E., Heirman, W., Van Craeynest, K., & Eeckhout, L. (2014, March). Barrierpoint: Sampled simulation of multi-threaded applications. In 2014 IEEE International Symposium on Performance Analysis of Systems and Software (ISPASS) (pp. 2-12). IEEE.
- [P2](https://ieeexplore.ieee.org/abstract/document/7581284) Ferrerón, A., Jagtap, R., & Rusitoru, R. (2016, September). Identifying representative regions of parallel HPC applications: a cross-architectural evaluation. In 2016 IEEE International Symposium on Workload Characterization (IISWC) (pp. 1-2). IEEE.
- [P3](https://ieeexplore.ieee.org/abstract/document/7975275) Ferrerón, A., Jagtap, R., Bischoff, S., & Ruşitoru, R. (2017, April). Crossing the architectural barrier: Evaluating representative regions of parallel HPC applications. In 2017 IEEE International Symposium on Performance Analysis of Systems and Software (ISPASS) (pp. 109-120). IEEE
- [P4](https://ieeexplore.ieee.org/abstract/document/8366944) Tairum Cruz, M., Bischoff, S., & Rusitoru, R. (2018, April). Shifting the barrier: Extending the boundaries of the BarrierPoint methodology. In 2018 IEEE International Symposium on Performance Analysis of Systems and Software (ISPASS) (pp. 120-122). IEEE.

## References
- [R1] [Trevor Carlson's BarrierPoint](https://github.com/trevorcarlson/barrierpoint)
- [R2] [DynamoRIO](https://www.dynamorio.org/)
- [R3] [Simpoint ](https://cseweb.ucsd.edu/~calder/simpoint/simpoint_overview.htm)
- [R4] [PAPI](https://icl.utk.edu/papi/)


## License

This project is licensed under [Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0). For more information, see LICENSE.txt.

This project also contains code derived from other projects as listed below:

- Some code in `DrAnalysis.py` is derived from the [original BarrierPoint tool](https://github.com/trevorcarlson/barrierpoint/tree/91d1f54f10ed5a442dfd1b5b50276c249964481b) by Trevor Carlson, which uses the MIT license. The original license text can be found in `LICENSE-MIT.txt`.

- Some code featured into the BarrierPoint `dynamorio_client` is derived from existing DynamoRIO clients, under BSD and LGPL licenses. The original license text can be found in `dynamorio_client/LICENSE-BSD-LGPL.txt`.


## Contributions / Pull Requests

Contributions are accepted under Apache-2.0. Only submit contributions where you have authored all of the code. If you do this on work time, make sure you have your employer's approval.
