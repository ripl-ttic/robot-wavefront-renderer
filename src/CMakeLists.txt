add_definitions(-DBASE_PATH="${BASE_PATH}")
add_definitions(-std=gnu99)

add_library(robot-wavefront-renderer SHARED
            renderer_robot_wavefront_model.c)

target_link_libraries(robot-wavefront-renderer ${OPENGL_LIBRARIES})

set(REQUIRED_LIBS bot2-core bot2-vis)

pods_use_pkg_config_packages(robot-wavefront-renderer ${REQUIRED_LIBS})

pods_install_headers(renderer_robot_wavefront_model.h DESTINATION
                     robot_wavefront_model_renderer)

pods_install_libraries(robot-wavefront-renderer)
pods_install_pkg_config_file(robot-wavefront-renderer
    CFLAGS
    LIBS -lrobot-wavefront-renderer
    REQUIRES ${REQUIRED_LIBS}
    VERSION 0.0.1)
