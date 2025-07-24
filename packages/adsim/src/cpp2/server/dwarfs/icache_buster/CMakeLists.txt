set(NUM_METHODS 1800000)
set(NUM_SPLITS 192)
set(ICACHEBUSTER_SRCS
    ${CMAKE_CURRENT_BINARY_DIR}/ICacheBuster.cc
    ${CMAKE_CURRENT_BINARY_DIR}/ICacheBuster.h
)
math(EXPR PART_STOP "${NUM_SPLITS} - 1")
foreach(PART RANGE ${PART_STOP})
    list(APPEND ICACHEBUSTER_SRCS
        "${CMAKE_CURRENT_BINARY_DIR}/ICacheBuster.part${PART}.cc")
endforeach()

if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/ICacheBuster.h)
    execute_process(COMMAND python3
        ${CMAKE_CURRENT_LIST_DIR}/gen_icache_buster.py
        --num_methods=${NUM_METHODS}
        --num_splits=${NUM_SPLITS}
        --output_dir ${CMAKE_CURRENT_BINARY_DIR})
endif()

add_library(icachebuster ${ICACHEBUSTER_SRCS})
