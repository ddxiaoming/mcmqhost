set(SOURCE_FILES
    pcie_link.cpp
    pcie_link_mcmq.cpp
    pcie_link_vfio.cpp
    memory_space.cpp
    nvme_driver.cpp
)

set(LIBRARIES
    pthread
    atomic
    spdlog::spdlog
)

ADD_LIBRARY(unvme STATIC ${SOURCE_FILES})
TARGET_LINK_LIBRARIES(unvme ${LIBRARIES})
