set(SOURCE_FILES
    main.cpp
)

set(EXT_SOURCE_FILES
    ${PROTO_SRCS}
)

set(LIBRARIES
    pthread
    atomic
    spdlog::spdlog
    CONAN_PKG::cxxopts
    unvme
    mcmq    
)

add_executable(mcmqhost ${SOURCE_FILES} ${EXT_SOURCE_FILES})
target_link_libraries(mcmqhost ${LIBRARIES})
