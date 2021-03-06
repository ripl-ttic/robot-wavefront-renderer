cmake_minimum_required(VERSION 2.6.0)

# The BASE_PATH is where the models, data, config, etc. directories are located
if (NOT BASE_PATH)
    set(BASE_PATH ${CMAKE_INSTALL_PREFIX})
else (NOT EXISTS ${BASE_PATH})
    message(WARNING "${BASE_PATH} is not a valid directory. Setting BASE_PATH: ${CMAKE_INSTALL_PREFIX}")
    set(BASE_PATH ${CMAKE_INSTALL_PREFIX})
endif()

# pull in the pods macros. See cmake/pods.cmake for documentation
set(POD_NAME robot-status-renderer)
include(cmake/pods.cmake)

# Necessary to build on OSX
find_package(OpenGL REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB2 REQUIRED glib-2.0)
pkg_check_modules(LCM REQUIRED lcm)
pkg_check_modules(BOT2_VIS REQUIRED bot2-vis)

if(NOT BOT2_VIS_FOUND)
    message("\n\n")
    message("bot2-vis not found.  Not building robot-status renderer")
    message("\n")
    return()
endif()


add_subdirectory(src)
