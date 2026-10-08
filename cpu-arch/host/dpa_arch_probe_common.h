#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dpa_aff_cfg {
    uint64_t dpa_handle; 
    uint64_t M;         
    uint64_t seed;       
    uint64_t data_addr;
    uint64_t data_mask;
    uint64_t data_stride;
    uint64_t start_index;
    uint64_t se_cyc;     
    uint64_t se_time;    
    uint64_t se_inst;    
    uint64_t se_sink;    
};

#ifdef __cplusplus
}
#endif
