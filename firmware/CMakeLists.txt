cmake_minimum_required(VERSION 3.16)

# Default to the verified board when the caller has not selected a target.
if(NOT DEFINED IDF_TARGET AND NOT DEFINED ENV{IDF_TARGET})
    set(IDF_TARGET esp32s3)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(newsmsforward)
