if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()
daisy_executable(NAME diagnostics SOURCES diagnostics.cpp
                 LIBS alchemy::surface alchemy::host_link)
