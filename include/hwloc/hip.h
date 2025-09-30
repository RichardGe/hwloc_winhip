#ifndef HWLOC_HIP_H
#define HWLOC_HIP_H

#include "hwloc.h"

#ifdef __cplusplus
extern "C" {
#endif


static __hwloc_inline int
hwloc_hip_get_device_cpuset(hwloc_topology_t topology, unsigned int dv_ind, hwloc_cpuset_t set)
{
	hwloc_obj_t osdev = 0;
	
	// search the GPU with index 'dv_ind'
	for (;;)
	{
		osdev = hwloc_get_next_osdev(topology, osdev);
		if ( osdev == 0 )
			break;

		if (!osdev->attr) 
			continue;

		hwloc_obj_osdev_type_t t = osdev->attr->osdev.type;
		if (t != HWLOC_OBJ_OSDEV_GPU && t != HWLOC_OBJ_OSDEV_COPROC) 
			continue;

		const char* hipdeviceIndex_str = hwloc_obj_get_info_by_name(osdev, "HIPDeviceIndex");
		if ( hipdeviceIndex_str )
		{
			int hipdeviceIndex_int = atoi(hipdeviceIndex_str);

			if ( dv_ind == hipdeviceIndex_int )
			{
				const char* NUMAnode_str = hwloc_obj_get_info_by_name(osdev, "NUMAnode");
				if ( NUMAnode_str )
				{
					int NUMAnode_int = atoi(NUMAnode_str);

					// convert a NUMA index into a 'cpuset'
					hwloc_obj_t nn = hwloc_get_numanode_obj_by_os_index(topology, NUMAnode_int);
					if (!nn)
						return -1;
					return hwloc_cpuset_from_nodeset(topology, set, nn->nodeset);
				}
				else
					return -1; 
			}
		}
		else
			return -1; 
	}
	return -1;
}


#ifdef __cplusplus
} /* extern "C" */
#endif


#endif /* HWLOC_HIP_H */
