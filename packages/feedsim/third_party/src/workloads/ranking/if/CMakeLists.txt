thrift_library(
    "ranking"
    ""      # services
    "cpp2"  # Language generator
    ""      # Options
    "${CMAKE_CURRENT_LIST_DIR}" # Directory where thrift file lives
    "${CMAKE_CURRENT_LIST_DIR}" # Directory where thrift objects will be built
    "ranking/if"
)
