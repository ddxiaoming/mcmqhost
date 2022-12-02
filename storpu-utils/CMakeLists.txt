set(LIBRARIES
    pthread
    atomic
    spdlog::spdlog
    CONAN_PKG::cxxopts    
    unvme
    mcmq    
)

add_executable(loaddata data_loader.cpp)
target_link_libraries(loaddata ${LIBRARIES})
