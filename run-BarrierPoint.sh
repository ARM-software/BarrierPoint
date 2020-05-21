#!/bin/bash

declare config_json='configs.json'

# DR analysis
python DrAnalysis.py -c $config_json
# Perfcntr analysis
python RunBenchmarks.py -c $config_json
