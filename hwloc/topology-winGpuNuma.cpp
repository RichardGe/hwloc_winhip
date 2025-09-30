#include "topology-winGpuNuma.h"

#include "private/autogen/config.h"

// if on Windows
#ifdef HWLOC_WIN_SYS 

#include <initguid.h>
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <vector>
#include <string>
#include <devpropdef.h>
#include <devguid.h>
#include <stdio.h>
#include <algorithm>


#pragma comment(lib, "setupapi.lib")


bool di_get_str(HDEVINFO set, SP_DEVINFO_DATA &dev, const DEVPROPKEY &key, std::wstring &out)
{
	DEVPROPTYPE type = DEVPROP_TYPE_EMPTY; DWORD sz = 0;
	(void)SetupDiGetDevicePropertyW(set, &dev, &key, &type, nullptr, 0, &sz, 0);
	if (!sz) 
		return false;
	out.assign(sz/sizeof(wchar_t), L'\0');
	if (!SetupDiGetDevicePropertyW(set, &dev, &key, &type, (PBYTE)out.data(), sz, &sz, 0)) 
		return false;
	if (type != DEVPROP_TYPE_STRING) 
		return false;
	if (!out.empty() && out.back()==L'\0') out.pop_back();
	return true;
}

bool open_by_instance_id(const std::wstring &instId, HDEVINFO *outSet, SP_DEVINFO_DATA *outDev)
{
	HDEVINFO set = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES);
	if (set == INVALID_HANDLE_VALUE) 
		return false;

	SP_DEVINFO_DATA dev = {}; dev.cbSize = sizeof(dev);
	if (!SetupDiOpenDeviceInfoW(set, instId.c_str(), NULL, 0, &dev)) {
		SetupDiDestroyDeviceInfoList(set); 
		return false;
	}
	*outSet = set; *outDev = dev; 
	return true;
}

bool di_get_u32(HDEVINFO set, SP_DEVINFO_DATA &dev, const DEVPROPKEY &key, uint32_t *out)
{
	DEVPROPTYPE type = DEVPROP_TYPE_EMPTY; DWORD val = 0; DWORD sz = sizeof(val);
	if (!SetupDiGetDevicePropertyW(set, &dev, &key, &type, (PBYTE)&val, sz, &sz, 0)) 
		return false;
	if (type != DEVPROP_TYPE_UINT32 && type != DEVPROP_TYPE_INT32) 
		return false;
	*out = val; 
	return true;
}


bool resolve_numa_from_instance_chain(const std::wstring &startInst, USHORT *outNode)
{
	std::wstring cur = startInst;
	for (int hop = 0; hop < 64; ++hop) {
		HDEVINFO set = INVALID_HANDLE_VALUE; SP_DEVINFO_DATA dev = {};
		if (!open_by_instance_id(cur, &set, &dev)) 
			return false;

		uint32_t node_u32 = 0xFFFFFFFFu;
		if (di_get_u32(set, dev, DEVPKEY_Device_Numa_Node, &node_u32) && node_u32 != 0xFFFFFFFFu) {
			*outNode = (USHORT)node_u32;
			SetupDiDestroyDeviceInfoList(set);
			return true;
		}

		uint32_t prox = 0;
		if (di_get_u32(set, dev, DEVPKEY_Numa_Proximity_Domain, &prox)) {
			USHORT n = 0xFFFF;
			if (GetNumaProximityNodeEx((ULONG)prox, &n)) {
				*outNode = n;
				SetupDiDestroyDeviceInfoList(set);
				return true;
			}
		}

		std::wstring parentId;
		bool hasParent = di_get_str(set, dev, DEVPKEY_Device_Parent, parentId);
		SetupDiDestroyDeviceInfoList(set);
		if (!hasParent) 
			break;
		cur = std::move(parentId);
	}
	return false;
}

static bool get_u32_prop(HDEVINFO set, SP_DEVINFO_DATA &dev, const DEVPROPKEY &key, uint32_t *out)
{
	DEVPROPTYPE type = DEVPROP_TYPE_EMPTY;
	DWORD val = 0, sz = sizeof(val);
	if (!SetupDiGetDevicePropertyW(set, &dev, &key, &type, (PBYTE)&val, sz, &sz, 0)) 
		return false;
	if (type != DEVPROP_TYPE_UINT32 && type != DEVPROP_TYPE_INT32) 
		return false;
	*out = val; 
	return true;
}

static bool get_strlist_prop(HDEVINFO set, SP_DEVINFO_DATA &dev, const DEVPROPKEY &key, std::vector<std::wstring> &out)
{
	DEVPROPTYPE type = DEVPROP_TYPE_EMPTY;
	wchar_t buf[8192]; DWORD sz = sizeof(buf);
	if (!SetupDiGetDevicePropertyW(set, &dev, &key, &type, (PBYTE)buf, sz, &sz, 0)) 
		return false;
	if (type != DEVPROP_TYPE_STRING_LIST) 
		return false;
	out.clear();
	for (const wchar_t *p = buf; *p; p += wcslen(p) + 1) out.emplace_back(p);
	return !out.empty();
}

static bool parse_locationpaths_bdf(const std::vector<std::wstring> &paths, uint32_t *seg, uint32_t *dev, uint32_t *func)
{
	if (paths.empty()) 
		return false;

	// Use longest path for maximum detail.
	const std::wstring *best = &paths[0];
	for (auto &p : paths) if (p.size() > best->size()) best = &p;

	// Segment from PCIROOT(n)
	*seg = 0u;
	size_t r = best->find(L"PCIROOT(");
	if (r != std::wstring::npos) {
		size_t l = best->find(L'(', r);
		size_t e = best->find(L')', l == std::wstring::npos ? r : l + 1);
		if (l != std::wstring::npos && e != std::wstring::npos && e > l + 1) {
			std::wstring num = best->substr(l + 1, e - (l + 1));
			*seg = (uint32_t)wcstoul(num.c_str(), nullptr, 0); // accepts decimal or hex (0x..)
		}
	}

	// Last PCI(DDFF) gives device/function in hex.
	size_t pos = best->rfind(L"PCI(");
	if (pos == std::wstring::npos) 
		return false;
	pos += 4;
	size_t e = best->find(L')', pos);
	if (e == std::wstring::npos) 
		return false;
	std::wstring hex = best->substr(pos, e - pos);
	if (hex.size() != 4) 
		return false;
	unsigned ddff = (unsigned)wcstoul(hex.c_str(), nullptr, 16);
	*dev  = (ddff >> 8) & 0xFFu;
	*func = ddff & 0xFFu;
	return true;
}



int GetNumaNodeForPciBdf(unsigned short seg_in, unsigned char  bus_in, unsigned char  dev_in, unsigned char  func_in, int *out_numa)
{
	if (!out_numa) 
		return -1;

	HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, NULL, NULL, DIGCF_PRESENT);
	if (set == INVALID_HANDLE_VALUE) 
		return -2;

	for (DWORD i = 0;; ++i) {
		SP_DEVINFO_DATA dev = {}; dev.cbSize = sizeof(dev);
		if (!SetupDiEnumDeviceInfo(set, i, &dev)) 
			break;

		uint32_t bus=0;
		if (!get_u32_prop(set, dev, DEVPKEY_Device_BusNumber, &bus)) 
			continue;

		std::vector<std::wstring> paths;
		if (!get_strlist_prop(set, dev, DEVPKEY_Device_LocationPaths, paths)) 
			continue;

		uint32_t seg=0, dv=0, fn=0;
		if (!parse_locationpaths_bdf(paths, &seg, &dv, &fn)) 
			continue;

		// Segment can be 0 on Windows; accept exact match or both zero.
		bool seg_match = (seg == seg_in) || (seg == 0 && seg_in == 0);
		if (!seg_match) 
			continue;
		if (bus != bus_in || dv != dev_in || fn != func_in) 
			continue;

		// Resolve NUMA from instance chain (stable, not localized).
		ULONG id_sz = 0;
		if (CM_Get_Device_ID_Size(&id_sz, dev.DevInst, 0) != CR_SUCCESS) 
		{ 
			SetupDiDestroyDeviceInfoList(set); 
			return -3; 
		}
		std::vector<wchar_t> id(id_sz + 1);
		if (CM_Get_Device_IDW(dev.DevInst, id.data(), (ULONG)id.size(), 0) != CR_SUCCESS) 
		{ 
			SetupDiDestroyDeviceInfoList(set); 
			return -3; 
		}

		USHORT node = 0xFFFF;
		if (!resolve_numa_from_instance_chain(id.data(), &node)) 
		{ 
			SetupDiDestroyDeviceInfoList(set); 
			return -4; 
		}

		*out_numa = (int)node;
		SetupDiDestroyDeviceInfoList(set);
		return 0;
	}

	SetupDiDestroyDeviceInfoList(set);
	return -5; // not found
}

#else // if on other OS than Windows


int GetNumaNodeForPciBdf(unsigned short seg_in, unsigned char  bus_in, unsigned char  dev_in, unsigned char  func_in, int *out_numa)
{
	return -1; 
}


#endif // #ifdef HWLOC_WIN_SYS

