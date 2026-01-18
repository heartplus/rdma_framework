# FindIBVerbs.cmake
# Find the InfiniBand Verbs library
#
# This module defines:
#   IBVerbs_FOUND - True if IBVerbs was found
#   IBVerbs_INCLUDE_DIRS - Include directories for IBVerbs
#   IBVerbs_LIBRARIES - Libraries to link against
#   IBVerbs::IBVerbs - Imported target

find_path(IBVerbs_INCLUDE_DIR
    NAMES infiniband/verbs.h
    PATHS
        /usr/include
        /usr/local/include
        /opt/mellanox/include
)

find_library(IBVerbs_LIBRARY
    NAMES ibverbs
    PATHS
        /usr/lib
        /usr/lib64
        /usr/local/lib
        /usr/local/lib64
        /opt/mellanox/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(IBVerbs
    REQUIRED_VARS IBVerbs_LIBRARY IBVerbs_INCLUDE_DIR
)

if(IBVerbs_FOUND)
    set(IBVerbs_INCLUDE_DIRS ${IBVerbs_INCLUDE_DIR})
    set(IBVerbs_LIBRARIES ${IBVerbs_LIBRARY})

    if(NOT TARGET IBVerbs::IBVerbs)
        add_library(IBVerbs::IBVerbs UNKNOWN IMPORTED)
        set_target_properties(IBVerbs::IBVerbs PROPERTIES
            IMPORTED_LOCATION "${IBVerbs_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${IBVerbs_INCLUDE_DIR}"
        )
    endif()
endif()

mark_as_advanced(IBVerbs_INCLUDE_DIR IBVerbs_LIBRARY)
