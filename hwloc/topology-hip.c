
#include "private/autogen/config.h"
#include "hwloc.h"
#include "hwloc/plugins.h"
#include "private/misc.h"
#include "private/debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>


/* Minimal HIP ABI surface. Do not include HIP headers. */
#ifndef HIP_SUCCESS
#define HIP_SUCCESS 0
#endif

#ifdef __cplusplus
extern "C" {
#endif
int hipGetDeviceCount(int* count);
int hipSetDevice(int device);
int hipDeviceGetName(char* name, int len, int deviceId);
int hipDeviceGetPCIBusId(char* pBusId, int len, int deviceId);
int hipMemGetInfo(size_t* freeBytes, size_t* totalBytes);
#ifdef __cplusplus
}
#endif

static const char*
hwloc_hip_guess_vendor_from_name(const char* name)
{
	if (!name || !*name) return NULL;
	if (strstr(name, "NVIDIA")) return "NVIDIA Corporation";
	if (strstr(name, "AMD") || strstr(name, "Radeon")) return "Advanced Micro Devices, Inc.";
	if (strstr(name, "Intel")) return "Intel Corporation";
	return "Unknown";
}

static int
hwloc_hip_parse_pci_busid(const char* s, unsigned* domain, unsigned* bus, unsigned* dev, unsigned* func)
{
	/* "dddd:bb:dd.f" hex, e.g. "0000:2b:00.0" */
	unsigned d = 0, b = 0, de = 0, f = 0;
	if (!s) return -1;
	if (sscanf(s, "%x:%x:%x.%x", &d, &b, &de, &f) != 4) return -1;
	if (domain) *domain = d;
	if (bus) *bus = b;
	if (dev) *dev = de;
	if (func) *func = f;
	return 0;
}

static int
hwloc_hip_discover(struct hwloc_backend* backend, struct hwloc_disc_status* dstatus)
{
	struct hwloc_topology* topology = backend->topology;
	enum hwloc_type_filter_e filter;
	int devcount = 0;
	int hret;
	int i;

	assert(dstatus->phase == HWLOC_DISC_PHASE_IO);

	hwloc_topology_get_type_filter(topology, HWLOC_OBJ_OS_DEVICE, &filter);
	if (filter == HWLOC_TYPE_FILTER_KEEP_NONE)
		return 0;

	hret = hipGetDeviceCount(&devcount);
	if (hret != HIP_SUCCESS) {
		if (HWLOC_SHOW_ALL_ERRORS())
			fprintf(stderr, "hwloc/hip: hipGetDeviceCount failed: %d\n", hret);
		return -1;
	}
	if (devcount <= 0)
		return 0;

	hwloc_debug("%d HIP devices\n", devcount);

	for (i = 0; i < devcount; i++) {
		hwloc_obj_t osdev, parent;
		char namebuf[256] = {0};
		const char* vendor;
		size_t free_k = 0, total_k = 0;
		char busid[64] = {0};
		unsigned pcidomain = 0, pcibus = 0, pcidev = 0, pcifunc = 0;
		char infobuf[64];
		char osname[64];
		char intstr[64];

		/* Get model name */
		if (hipDeviceGetName(namebuf, (int)sizeof(namebuf), i) != HIP_SUCCESS)
			namebuf[0] = '\0';

		/* Create OS device */
		osdev = hwloc_alloc_setup_object(topology, HWLOC_OBJ_OS_DEVICE, HWLOC_UNKNOWN_INDEX);
		if (!osdev)
			continue;
		snprintf(osname, sizeof(osname), "hipd%u", (unsigned)i);
		osdev->name = strdup(osname);
		osdev->depth = HWLOC_TYPE_DEPTH_UNKNOWN;
		osdev->attr->osdev.type = HWLOC_OBJ_OSDEV_COPROC;

		osdev->subtype = strdup("HIP");
		hwloc_obj_add_info(osdev, "Backend", "HIP");

		if (namebuf[0] != '\0')
			hwloc_obj_add_info(osdev, "GPUModel", namebuf);

		snprintf(intstr, sizeof(intstr), "%u", i);
		hwloc_obj_add_info(osdev, "HIPDeviceIndex", intstr);

		vendor = hwloc_hip_guess_vendor_from_name(namebuf);
		printf("VENDOR %s\n" , vendor);
		if (vendor && vendor[0] != '\0')
			hwloc_obj_add_info(osdev, "GPUVendor", vendor);

		/* Query VRAM on that device: set device then hipMemGetInfo */
		if (hipSetDevice(i) == HIP_SUCCESS) {
			size_t free_b = 0, total_b = 0;
			if (hipMemGetInfo(&free_b, &total_b) == HIP_SUCCESS) {
				total_k = total_b / 1024;
			}
		}
		snprintf(infobuf, sizeof(infobuf), "%llu", (unsigned long long)total_k);
		hwloc_obj_add_info(osdev, "HIPVRAMSize", infobuf);

		/* Attach under PCI parent if available */
		parent = NULL;
		if (hipDeviceGetPCIBusId(busid, (int)sizeof(busid), i) == HIP_SUCCESS) {
			if (hwloc_hip_parse_pci_busid(busid, &pcidomain, &pcibus, &pcidev, &pcifunc) == 0) 
			{
				int out_numa = -1;
				int ret = GetNumaNodeForPciBdf(pcidomain , pcibus, pcidev, pcifunc, &out_numa);
				if ( ret == 0 )
				{
					snprintf(intstr, sizeof(intstr), "%u", out_numa);
					hwloc_obj_add_info(osdev, "NUMAnode", intstr);
				}

				parent = hwloc_pci_find_parent_by_busid(topology, pcidomain, pcibus, pcidev, pcifunc);
			} else {
				hwloc_debug("HIP: failed to parse PCI bus id '%s'\n", busid);
			}
		} else {
			hwloc_debug("HIP: hipDeviceGetPCIBusId failed for device %d\n", i);
		}
		if (!parent)
			parent = hwloc_get_root_obj(topology);

		hwloc_insert_object_by_parent(topology, parent, osdev);
	}

	return 0;
}

/* Component boilerplate unchanged... */
static struct hwloc_backend*
hwloc_hip_component_instantiate(struct hwloc_topology* topology,
				struct hwloc_disc_component* component,
				unsigned excluded_phases __hwloc_attribute_unused,
				const void* _data1 __hwloc_attribute_unused,
				const void* _data2 __hwloc_attribute_unused,
				const void* _data3 __hwloc_attribute_unused)
{
	struct hwloc_backend* backend = hwloc_backend_alloc(topology, component);
	if (!backend) return NULL;
	backend->discover = hwloc_hip_discover;
	return backend;
}

static struct hwloc_disc_component hwloc_hip_disc_component = {
	"hip",
	HWLOC_DISC_PHASE_IO,
	HWLOC_DISC_PHASE_GLOBAL,
	hwloc_hip_component_instantiate,
	10,
	1,
	NULL
};

static int
hwloc_hip_component_init(unsigned long flags)
{
	if (flags) return -1;
	if (hwloc_plugin_check_namespace("hip", "hwloc_backend_alloc") < 0)
		return -1;
	return 0;
}

#ifdef HWLOC_INSIDE_PLUGIN
HWLOC_DECLSPEC extern const struct hwloc_component hwloc_hip_component;
#endif

const struct hwloc_component hwloc_hip_component = {
	HWLOC_COMPONENT_ABI,
	hwloc_hip_component_init, NULL,
	HWLOC_COMPONENT_TYPE_DISC,
	0,
	&hwloc_hip_disc_component
};
