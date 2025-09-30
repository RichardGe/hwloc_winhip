#pragma once

#include <hwloc.h>

#ifdef __cplusplus
extern "C" {
#endif

// get a NUMA index for a given PCI BDF device
// This implementation is for Windows only.
// return 0 if success
int GetNumaNodeForPciBdf(unsigned short seg_in, unsigned char  bus_in, unsigned char  dev_in, unsigned char  func_in, int *out_numa);


#ifdef __cplusplus
}
#endif
