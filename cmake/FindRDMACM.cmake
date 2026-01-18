# FindRDMACM.cmake
# Find the RDMA Connection Manager library
#
# This module defines:
#   RDMACM_FOUND - True if RDMACM was found
#   RDMACM_INCLUDE_DIRS - Include directories for RDMACM
#   RDMACM_LIBRARIES - Libraries to link against
#   RDMACM::RDMACM - Imported target

find_path(RDMACM_INCLUDE_DIR
    NAMES rdma/rdma_cma.h
    PATHS
        /usr/include
        /usr/local/include
        /opt/mellanox/include
)

find_library(RDMACM_LIBRARY
    NAMES rdmacm
    PATHS
        /usr/lib
        /usr/lib64
        /usr/local/lib
        /usr/local/lib64
        /opt/mellanox/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(RDMACM
    REQUIRED_VARS RDMACM_LIBRARY RDMACM_INCLUDE_DIR
)

if(RDMACM_FOUND)
    set(RDMACM_INCLUDE_DIRS ${RDMACM_INCLUDE_DIR})
    set(RDMACM_LIBRARIES ${RDMACM_LIBRARY})

    if(NOT TARGET RDMACM::RDMACM)
        add_library(RDMACM::RDMACM UNKNOWN IMPORTED)
        set_target_properties(RDMACM::RDMACM PROPERTIES
            IMPORTED_LOCATION "${RDMACM_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${RDMACM_INCLUDE_DIR}"
        )
    endif()
endif()

mark_as_advanced(RDMACM_INCLUDE_DIR RDMACM_LIBRARY)
