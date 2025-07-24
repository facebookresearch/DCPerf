find_path(Cereal_INCLUDE_DIR
    NAMES cereal/cereal.hpp
    PATHS "${CMAKE_SOURCE_DIR}/third_party/cereal"
    PATH_SUFFIXES
        include)

mark_as_advanced(Cereal_INCLUDE_DIR)


include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Cereal
    REQUIRED_VARS Cereal_INCLUDE_DIR
)

if(Cereal_FOUND AND NOT TARGET Cereal::Cereal)
    add_library(Cereal::Cereal INTERFACE IMPORTED)

    set_target_properties(Cereal::Cereal PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${Cereal_INCLUDE_DIR}"
    )
endif()
