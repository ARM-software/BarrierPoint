cmake_minimum_required(VERSION 2.8)

# DynamoRIO compile options. Their value is by default the one used in the methodology
option(WITH_VALIDATION "Basic Block Vector Detection Validation" OFF)
if(WITH_VALIDATION)
	add_definitions(-DVALIDATE)
endif()

option(MASK_ADDRESS "Mask the memory addresses using the cache line value" ON)
if(MASK_ADDRESS)
	add_definitions(-DMASK_ADDRESSES)
endif()

option(TRACE_BEFORE_ROI "Trace memory accesses before start of the ROI" ON)
if(TRACE_BEFORE_ROI)
	add_definitions(-DTRACE_MEM_BEFORE_ROI)
endif()

set (CMAKE_CXX_FLAGS "-Wall -Werror")
set (CMAKE_CXX_STANDARD 11)
file(GLOB SOURCES RELATIVE ${CMAKE_SOURCE_DIR} "*.cpp")
add_library(barrierpoint SHARED main.cpp ${SOURCES})

if(NOT DEFINED ENV{DYNAMORIO_BUILD_DIR})
	message(FATAL_ERROR "Please define the following environment variable: export DYNAMORIO_BUILD_DIR=<path/to/dr/build/folder> ")
endif ()

string(CONCAT DR_PACKAGE $ENV{DYNAMORIO_BUILD_DIR} "/cmake/")
set(DynamoRIO_DIR ${DR_PACKAGE} CACHE PATH
	"DynamoRIO installation's cmake directory")

find_package(DynamoRIO)
if (NOT DynamoRIO_FOUND)
 message(FATAL_ERROR "DynamoRIO package required to build")
endif(NOT DynamoRIO_FOUND)

string(CONCAT DR_INCLUDE $ENV{DYNAMORIO_BUILD_DIR} "/include/")
string(CONCAT DR_EXT_INCLUDE $ENV{DYNAMORIO_BUILD_DIR} "/ext/include/")
include_directories(${DR_INCLUDE})
include_directories("./include/")
include_directories(${DR_EXT_INCLUDE})

configure_DynamoRIO_client(barrierpoint)
use_DynamoRIO_extension(barrierpoint drmgr)
use_DynamoRIO_extension(barrierpoint drwrap)
use_DynamoRIO_extension(barrierpoint drutil)
use_DynamoRIO_extension(barrierpoint drreg)
use_DynamoRIO_extension(barrierpoint drx)
use_DynamoRIO_extension(barrierpoint drsyms)
use_DynamoRIO_extension(barrierpoint droption)
