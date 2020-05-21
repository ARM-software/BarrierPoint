CXX = g++
PAPI_DIR ?=

ifdef PAPI_DIR
	PAPI_INCLUDE=$(PAPI_DIR)/include
	PAPI_LIBRARY=$(PAPI_DIR)/lib
endif

ifeq ($(DEBUG),ON)
	DBG = -DDEBUG
endif

all: bp_perfcntrs bp_dr

bp_dr: bp_dr.cc
	$(CXX) -Wall -fPIC $(DBG) -shared bp_dr.cc -o libbp_dr.so -ldl

bp_perfcntrs: bp_perfcntrs.cc
	$(CXX) -I$(PAPI_INCLUDE) -L$(PAPI_LIBRARY) -Wall -fPIC $(DBG) -shared bp_perfcntrs.cc -o libbp_perfcntrs.so -ldl -lpapi -fopenmp

clean:
	rm -rf libbp_dr.so libbp_perfcntrs.so
