#include "libunvme/memory_space.h"
#include "libunvme/nvme_driver.h"
#include "libunvme/pcie_link_mcmq.h"
#include "libunvme/pcie_link_vfio.h"

#include "cxxopts.hpp"
#include "spdlog/cfg/env.h"
#include "spdlog/spdlog.h"

#include <fstream>
#include <thread>
#include <vector>

#define WRITE_SSD_ENTRY 0x0000
#define READ_SSD_ENTRY 0x0000

using cxxopts::OptionException;

static constexpr size_t PARTITION_SIZE = 256UL << 20; // 256MB
static const size_t CHUNK_SIZE = 256UL << 20; // 256 MB
static const size_t PAGE_SIZE = 0x4000; // 16 KB

cxxopts::ParseResult parse_arguments(int argc, char* argv[])
{
  try {
    cxxopts::Options options(argv[0], " - Host frontend for MCMQ");

    // clang-format off
    options.add_options()
        ("b,backend", "Backend type", cxxopts::value<std::string>()->default_value("mcmq"))
        ("m,memory", "Path to the shared memory file",
         cxxopts::value<std::string>()->default_value("/dev/shm/ivshmem"))
        ("c,config", "Path to the SSD config file",
         cxxopts::value<std::string>()->default_value("ssdconfig.yaml"))
        ("r,result", "Path to the result file",
         cxxopts::value<std::string>()->default_value("result.json"))
        ("f,file", "Path to the file to be loaded",
         cxxopts::value<std::string>())
        ("g,group", "VFIO group",
         cxxopts::value<std::string>())
        ("d,device", "PCI device ID",
         cxxopts::value<std::string>())
        ("h,help", "Print help");
    // clang-format on

    auto result = options.parse(argc, argv);

    if (result.count("help")) {
      std::cerr << options.help({""}) << std::endl;
      exit(EXIT_SUCCESS);
    }

    return result;
  } catch (const OptionException& e) {
    exit(EXIT_FAILURE);
  }
}


inline uint32_t mach_read_from_4(const unsigned char* b) {
  return (static_cast<uint32_t>(b[0]) << 24)
      | (static_cast<uint32_t>(b[1]) << 16)
      | (static_cast<uint32_t>(b[2]) << 8)
      | static_cast<uint32_t>(b[3]);
}

struct ExchangeArg {
  unsigned long host_addr;
  unsigned long flash_page_id;
  unsigned long n_pages;
};

void load_a_file(NVMeDriver *driver,
                 unsigned int ctx,
                 MemorySpace *memory_space,
                 const std::string &filename,
                 int partition,
                 bool is_data_file) {

  auto *write_to_ssd_buffer = new unsigned char[PAGE_SIZE];
  auto *read_from_ssd_buffer = new unsigned char[PAGE_SIZE];


  auto dma_buffer = memory_space->allocate_pages(1);
  auto* scratchpad = driver->get_scratchpad();
  auto argbuf = scratchpad->allocate(sizeof(ExchangeArg));

  ExchangeArg exchange_arg {};

  std::ifstream ifs(filename, std::ios::binary);
  if (ifs.is_open()) {
    spdlog::error("open file {} failed.\n", filename);
  }
  ifs.seekg(0, std::ios::end);
  auto file_size = ifs.tellg();

  if (is_data_file) {
    // 查看ibd文件对应的space id
    ifs.seekg(0, std::ios::beg);

    ifs.read((char *) write_to_ssd_buffer, PAGE_SIZE);

    unsigned int space_id = mach_read_from_4(write_to_ssd_buffer + 34);

    spdlog::info("{} belongs to space {}\n", filename, space_id);
  }

  // 写入文件到SSD
  unsigned int n_pages = file_size / PAGE_SIZE;
  for (unsigned long j = 0; j < n_pages; ++j) {
    ifs.seekg((long)(j * PAGE_SIZE), std::ios::beg);
    ifs.read((char *) write_to_ssd_buffer, PAGE_SIZE);
    memory_space->write(dma_buffer, write_to_ssd_buffer, PAGE_SIZE);
    exchange_arg.n_pages = 1;
    exchange_arg.host_addr = dma_buffer;
    exchange_arg.flash_page_id = PARTITION_SIZE * partition + j;
    scratchpad->write(argbuf, &exchange_arg, sizeof(ExchangeArg));
    driver->invoke_function(ctx, WRITE_SSD_ENTRY, argbuf); // 写一页到SSD

    driver->invoke_function(ctx, READ_SSD_ENTRY, argbuf); // 从SSD中读一页上来
    memory_space->read(dma_buffer, read_from_ssd_buffer, PAGE_SIZE);
    if (memcmp(write_to_ssd_buffer, read_from_ssd_buffer, PAGE_SIZE) != 0) {
      spdlog::error("content check error in file: {}, page id: {}\n", filename, j);
    }
  }
  spdlog::info("successfully write file {}\n", filename);

  memory_space->free_pages(dma_buffer, PAGE_SIZE);
  scratchpad->free_pages(argbuf, PAGE_SIZE);
  delete[] write_to_ssd_buffer;
  delete[] read_from_ssd_buffer;
}

void load_data_file(NVMeDriver *driver, unsigned int ctx, MemorySpace *memory_space) {
  std::vector<std::string> filenames {
      "/home/lemon/mysql/data/sbtest/sbtest1.ibd",
      "/home/lemon/mysql/data/sbtest/sbtest2.ibd",
      "/home/lemon/mysql/data/sbtest/sbtest3.ibd",
      "/home/lemon/mysql/data/sbtest/sbtest4.ibd",
      "/home/lemon/mysql/data/sbtest/sbtest5.ibd",
      "/home/lemon/mysql/data/sbtest/sbtest6.ibd",
      "/home/lemon/mysql/data/sbtest/sbtest7.ibd",
      "/home/lemon/mysql/data/sbtest/sbtest8.ibd",
      "/home/lemon/mysql/data/sbtest/sbtest9.ibd",
      "/home/lemon/mysql/data/sbtest/sbtest10.ibd",
      "/home/lemon/mysql/data/sbtest/sbtest11.ibd",
      "/home/lemon/mysql/data/sbtest/sbtest12.ibd",
      "/home/lemon/mysql/data/sbtest/sbtest13.ibd",
      "/home/lemon/mysql/data/sbtest/sbtest14.ibd",
      "/home/lemon/mysql/data/sbtest/sbtest15.ibd",
      "/home/lemon/mysql/data/sbtest/sbtest16.ibd",
      "/home/lemon/mysql/data/sbtest/sbtest17.ibd",
      "/home/lemon/mysql/data/sbtest/sbtest18.ibd",
      "/home/lemon/mysql/data/sbtest/sbtest19.ibd",
      "/home/lemon/mysql/data/sbtest/sbtest20.ibd",
  };

  std::vector<int> partitions {
    0,1,2,3,4,5,6,7,8,9,
    10,11,12,13,14,15,16,17,18,19
  };

  for (int i = 0; i < filenames.size(); ++i) {
    load_a_file(driver, ctx, memory_space, filenames[i], partitions[i], true);
  }
}

void load_log_file(NVMeDriver *driver, unsigned int ctx, MemorySpace *memory_space) {
  std::string filename = "/home/lemon/mysql/data/ib_logfile0";
  int partition = 20;
  load_a_file(driver, ctx, memory_space, filename, partition, false);
}

int main(int argc, char* argv[])
{
  spdlog::cfg::load_env_levels();

  auto args = parse_arguments(argc, argv);

  std::string backend;
  std::string config_file, result_file;
  std::string load_file;
  try {
    backend = args["backend"].as<std::string>();
    config_file = args["config"].as<std::string>();
    result_file = args["result"].as<std::string>();
    load_file = args["file"].as<std::string>();
  } catch (const OptionException& e) {
    spdlog::error("Failed to parse options: {}", e.what());
    exit(EXIT_FAILURE);
  }

  std::unique_ptr<MemorySpace> memory_space;
  std::unique_ptr<PCIeLink> link;

  if (backend == "mcmq") {
    std::string shared_memory;

    try {
      shared_memory = args["memory"].as<std::string>();
    } catch (const OptionException& e) {
      spdlog::error("Failed to parse options: {}", e.what());
      exit(EXIT_FAILURE);
    }

    memory_space = std::make_unique<SharedMemorySpace>(shared_memory);
    link = std::make_unique<PCIeLinkMcmq>();
  } else if (backend == "vfio") {
    std::string group, device_id;

    try {
      group = args["group"].as<std::string>();
      device_id = args["device"].as<std::string>();
    } catch (const OptionException& e) {
      spdlog::error("Failed to parse options: {}", e.what());
      exit(EXIT_FAILURE);
    }

    memory_space = std::make_unique<VfioMemorySpace>(
        0x1000, 2 * 1024 * 1024 + CHUNK_SIZE);
    link = std::make_unique<PCIeLinkVfio>(group, device_id);
  } else {
    spdlog::error("Unknown backend type: {}", backend);
    return EXIT_FAILURE;
  }

  if (!link->init()) {
    spdlog::error("Failed to initialize PCIe link");
    return EXIT_FAILURE;
  }

  link->map_dma(*memory_space);
  link->start();

  NVMeDriver driver(1, 1024, link.get(), memory_space.get(), false);
  driver.start();

  unsigned int ctx = driver.create_context(
      "/home/jimx/projects/storpu/libstorpu/libtest.so");
  spdlog::info("Created context {}", ctx);
  driver.set_thread_id(1);

  load_data_file(&driver, ctx, memory_space.get());
  load_log_file(&driver, ctx, memory_space.get());


  driver.shutdown();
  link->stop();

  return 0;
}
