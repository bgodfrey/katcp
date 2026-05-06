#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "tcpborphserver3.h"

static const struct tbs_platform amd_roach_platform = {
  TBS_VENDOR_AMD,
  "amd-roach",
  "/dev/roach/config",
  "/dev/roach/mem",
  TBS_RAMFILE_PATH_AMD,
  "tcpborphserver.bin",
  FPGA_MANAGER_FLAG,
  FPGA_MANAGER_FW,
  0,
  0,
  0,
  0
};

static const struct tbs_platform amd_zynq_platform = {
  TBS_VENDOR_AMD,
  "amd-zynq",
  "/dev/xdevcfg",
  "/dev/mem",
  TBS_RAMFILE_PATH_AMD,
  "tcpborphserver.bin",
  FPGA_MANAGER_FLAG,
  FPGA_MANAGER_FW,
  0,
  0,
  0,
  1
};

static const struct tbs_platform amd_fpga_manager_platform = {
  TBS_VENDOR_AMD,
  "amd-fpga-manager",
  "/lib/firmware/tcpborphserver.bin",
  "/dev/mem",
  TBS_RAMFILE_PATH_AMD,
  "tcpborphserver.bin",
  FPGA_MANAGER_FLAG,
  FPGA_MANAGER_FW,
  0,
  0,
  1,
  0
};

static const struct tbs_platform intel_socfpga_platform = {
  TBS_VENDOR_INTEL,
  "intel-socfpga",
  "/lib/firmware/tcpborphserver.rbf",
  "/dev/mem",
  TBS_RAMFILE_PATH_AMD,
  "tcpborphserver.rbf",
  FPGA_MANAGER_FLAG,
  FPGA_MANAGER_FW,
  0xff200000,
  0x10000,
  1,
  0
};

static int compatible_contains(const char *needle)
{
  FILE *f;
  char buffer[512];
  size_t got;

  f = fopen("/proc/device-tree/compatible", "rb");
  if(f == NULL){
    return 0;
  }

  got = fread(buffer, 1, sizeof(buffer) - 1, f);
  fclose(f);

  buffer[got] = '\0';
  return strstr(buffer, needle) != NULL;
}

static const struct tbs_platform *platform_from_name(const char *name)
{
  if(name == NULL){
    return NULL;
  }

  if(!strcmp(name, "intel") || !strcmp(name, "socfpga") || !strcmp(name, "de10")){
    return &intel_socfpga_platform;
  }

  if(!strcmp(name, "amd") || !strcmp(name, "xilinx")){
#ifdef __PPC__
    return &amd_roach_platform;
#elif defined(__ARM_ARCH_7A__)
    return &amd_zynq_platform;
#else
    return &amd_fpga_manager_platform;
#endif
  }

  if(!strcmp(name, "roach") || !strcmp(name, "ppc")){
    return &amd_roach_platform;
  }

  if(!strcmp(name, "zynq")){
    return &amd_zynq_platform;
  }

  if(!strcmp(name, "zynqmp") || !strcmp(name, "fpga-manager")){
    return &amd_fpga_manager_platform;
  }

  return NULL;
}

const struct tbs_platform *current_platform_tbs(void)
{
  const char *override;
  const struct tbs_platform *platform;

  override = getenv("KATCP_FPGA_PLATFORM");
  if(override == NULL){
    override = getenv("KATCP_FPGA_VENDOR");
  }

  platform = platform_from_name(override);
  if(platform != NULL){
    return platform;
  }

  if(compatible_contains("altr,socfpga") || compatible_contains("intel,socfpga")){
    return &intel_socfpga_platform;
  }

#ifdef __PPC__
  return &amd_roach_platform;
#elif defined(__ARM_ARCH_7A__)
  return &amd_zynq_platform;
#elif defined(__ARM_ARCH_8A__)
  return &amd_fpga_manager_platform;
#else
  return &amd_fpga_manager_platform;
#endif
}

int platform_is_intel_tbs(void)
{
  return current_platform_tbs()->p_vendor == TBS_VENDOR_INTEL;
}

int platform_use_fpga_manager_tbs(void)
{
  return current_platform_tbs()->p_use_fpga_manager;
}

int platform_do_flip_tbs(void)
{
  return current_platform_tbs()->p_do_flip;
}
