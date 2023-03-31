#include "nvs_partition.hpp"
#include "nvs_flash.h"

#ifndef NVS_PARTITION_LOOKUP_HPP_
#define NVS_PARTITION_LOOKUP_HPP_

namespace nvs {

namespace partition_lookup {

nvs_err_t lookup_nvs_partition(const char* label, NVSPartition **p);

#ifdef CONFIG_NVS_ENCRYPTION
nvs_err_t lookup_nvs_encrypted_partition(const char* label, nvs_sec_cfg_t* cfg, NVSPartition **p);
#endif // CONFIG_NVS_ENCRYPTION

} // partition_lookup

} // nvs

#endif // NVS_PARTITION_LOOKUP_HPP_
