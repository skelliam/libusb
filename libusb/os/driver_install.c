#include <config.h>
#include <windows.h>
#include <setupapi.h>
#include <io.h>
#include <stdio.h>
#include <inttypes.h>
#include <objbase.h>  // for GUID ops. requires libole32.a
#include <api/difxapi.h>

#include "libusbi.h"
#include "windows_usb.h"
#include "driver_install.h"

const char inf[] = "Date = \"03/08/2010\"\n\n" \
	"ProviderName = \"libusb 1.0\"\n" \
	"WinUSB_SvcDesc = \"WinUSB Driver Service\"\n" \
	"DiskName = \"libusb (WinUSB) Device Install Disk\"\n" \
	"ClassName = \"libusb (WinUSB) devices\"\n\n" \
	"[Version]\n" \
	"DriverVer = %Date%,1\n" \
	"Signature = \"$Windows NT$\"\n" \
	"Class = %ClassName%\n" \
	"ClassGuid = {78a1c341-4539-11d3-b88d-00c04fad5171}\n" \
	"Provider = %ProviderName%\n" \
	"CatalogFile = libusb_device.cat\n\n" \
	"[ClassInstall32]\n" \
	"Addreg = WinUSBDeviceClassReg\n\n" \
	"[WinUSBDeviceClassReg]\n" \
	"HKR,,,0,%ClassName%\n" \
	"HKR,,Icon,,-20\n\n" \
	"[Manufacturer]\n" \
	"%ProviderName% = libusbDevice_WinUSB,NTx86,NTamd64\n\n" \
	"[libusbDevice_WinUSB.NTx86]\n" \
	"%DeviceName% = USB_Install, USB\\%DeviceID%\n\n" \
	"[libusbDevice_WinUSB.NTamd64]\n" \
	"%DeviceName% = USB_Install, USB\\%DeviceID%\n\n" \
	"[USB_Install]\n" \
	"Include=winusb.inf\n" \
	"Needs=WINUSB.NT\n\n" \
	"[USB_Install.Services]\n" \
	"Include=winusb.inf\n" \
	"AddService=WinUSB,0x00000002,WinUSB_ServiceInstall\n\n" \
	"[WinUSB_ServiceInstall]\n" \
	"DisplayName     = %WinUSB_SvcDesc%\n" \
	"ServiceType     = 1\n" \
	"StartType       = 3\n" \
	"ErrorControl    = 1\n" \
	"ServiceBinary   = %12%\\WinUSB.sys\n\n" \
	"[USB_Install.Wdf]\n" \
	"KmdfService=WINUSB, WinUsb_Install\n\n" \
	"[WinUSB_Install]\n" \
	"KmdfLibraryVersion=1.9\n\n" \
	"[USB_Install.HW]\n" \
	"AddReg=Dev_AddReg\n\n" \
	"[Dev_AddReg]\n" \
	"HKR,,DeviceInterfaceGUIDs,0x10000,%DeviceGUID%\n\n" \
	"[USB_Install.CoInstallers]\n" \
	"AddReg=CoInstallers_AddReg\n" \
	"CopyFiles=CoInstallers_CopyFiles\n\n" \
	"[CoInstallers_AddReg]\n" \
	"HKR,,CoInstallers32,0x00010000,\"WdfCoInstaller01009.dll,WdfCoInstaller\",\"WinUSBCoInstaller2.dll\"\n\n" \
	"[CoInstallers_CopyFiles]\n" \
	"WinUSBCoInstaller2.dll\n" \
	"WdfCoInstaller01009.dll\n\n" \
	"[DestinationDirs]\n" \
	"CoInstallers_CopyFiles=11\n\n" \
	"[SourceDisksNames]\n" \
	"1 = %DiskName%,,,\\x86\n" \
	"2 = %DiskName%,,,\\amd64\n" \
	"3 = %DiskName%,,,\\ia64\n\n" \
	"[SourceDisksFiles.x86]\n" \
	"WinUSBCoInstaller2.dll=1\n" \
	"WdfCoInstaller01009.dll=1\n\n" \
	"[SourceDisksFiles.amd64]\n" \
	"WinUSBCoInstaller2.dll=2\n" \
	"WdfCoInstaller01009.dll=2\n\n" \
	"[SourceDisksFiles.ia64]\n";


struct res {
	char* id;
	char* subdir;
	char* name;
};

const struct res resource[] = { {"AMD64_DLL1" , "amd64", "WdfCoInstaller01009.dll"},
								{"AMD64_DLL2" , "amd64", "winusbcoinstaller2.dll"},
								{"X86_DLL1", "x86", "WdfCoInstaller01009.dll"},
								{"X86_DLL2", "x86", "winusbcoinstaller2.dll"} };
const int nb_resources = sizeof(resource)/sizeof(resource[0]);

extern char* sanitize_path(const char* path);
char* guid_to_string(const GUID guid)
{
	static char guid_string[MAX_GUID_STRING_LENGTH];
	
	sprintf(guid_string, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
		(unsigned int)guid.Data1, guid.Data2, guid.Data3, 
		guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
		guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
	return guid_string;
}

void free_di(struct driver_info *start)
{
	struct driver_info *tmp;
	while(start != NULL) {
		tmp = start;
		start = start->next;
		free(tmp);
	}
}

bool cfgmgr32_available = false;

static int init_cfgmgr32(void)
{
	DLL_LOAD(Cfgmgr32.dll, CM_Get_Parent, TRUE);
	DLL_LOAD(Cfgmgr32.dll, CM_Get_Child, TRUE);
	DLL_LOAD(Cfgmgr32.dll, CM_Get_Sibling, TRUE);
	DLL_LOAD(Cfgmgr32.dll, CM_Get_Device_IDA, TRUE); 
	DLL_LOAD(Cfgmgr32.dll, CM_Get_Device_IDW, TRUE);

	return LIBUSB_SUCCESS;
}

struct driver_info* list_driverless(void)
{
	unsigned i, j;
	DWORD size, reg_type, install_state;
	CONFIGRET r;
	HDEVINFO dev_info;
	SP_DEVINFO_DATA dev_info_data;
	SP_DEVICE_INTERFACE_DETAIL_DATA *dev_interface_details = NULL;
	char *sanitized_short = NULL;
	char *prefix[3] = {"VID_", "PID_", "MI_"};
	char *token;
	char path[MAX_PATH_LENGTH];
	char desc[MAX_DESC_LENGTH];
	char driver[MAX_DESC_LENGTH];
	struct driver_info *ret = NULL, *cur = NULL, *drv_info;
	bool driverless;

	if (!cfgmgr32_available) {
		init_cfgmgr32();
	}

	// List all connected USB devices
	dev_info = SetupDiGetClassDevs(NULL, "USB", NULL, DIGCF_PRESENT|DIGCF_ALLCLASSES);
	if (dev_info == INVALID_HANDLE_VALUE) {
		return NULL;
	}

	// Find the ones that are driverless
	for (i = 0; ; i++)
	{
		driverless = false;
		safe_free(sanitized_short);

		dev_info_data.cbSize = sizeof(dev_info_data);
		if (!SetupDiEnumDeviceInfo(dev_info, i, &dev_info_data)) {
			break;
		}

		// SPDRP_DRIVER seems to do a better job at detecting driverless devices than
		// SPDRP_INSTALL_STATE
		if ( SetupDiGetDeviceRegistryProperty(dev_info, &dev_info_data, SPDRP_DRIVER, 
			&reg_type, (BYTE*)driver, MAX_KEY_LENGTH, &size)) {
			// Driverless devices should return an error
			continue;
		}
//		usbi_dbg("driver: %s", driver);
/*
		if ( (!SetupDiGetDeviceRegistryProperty(dev_info, &dev_info_data, SPDRP_INSTALL_STATE, 
			&reg_type, (BYTE*)&install_state, 4, &size)) 
		  && (size != 4) ) {
			usbi_warn(NULL, "could not detect installation state of driver for %d: %s", 
				i, windows_error_str(0));
			continue;
		} 
		usbi_dbg("install state: %d", install_state);
		if ( (install_state != InstallStateFailedInstall)
		  && (SetupDiGetDeviceRegistryProperty(dev_info, &dev_info_data, SPDRP_DRIVER, 
			&reg_type, (BYTE*)driver, MAX_KEY_LENGTH, &size) != ERROR_INVALID_DATA) ) {
			continue;
		}
*/

		// TODO: can't always get a device desc => provide one
		if ( (!SetupDiGetDeviceRegistryProperty(dev_info, &dev_info_data, SPDRP_DEVICEDESC, 
			&reg_type, (BYTE*)desc, MAX_KEY_LENGTH, &size)) ) {
			usbi_warn(NULL, "could not read device description for %d: %s", 
				i, windows_error_str(0));
//			continue;
		}

		r = CM_Get_Device_ID(dev_info_data.DevInst, path, MAX_PATH_LENGTH, 0);
		if (r != CR_SUCCESS) {
			usbi_err(NULL, "could not retrieve simple path for device %d: CR error %d", 
				i, r);
			continue;
		}

		sanitized_short = sanitize_path(path);

		if (sanitized_short == NULL) {
			usbi_err(NULL, "could not sanitize path for device %d", i);
			continue;
		}
		usbi_dbg("Driverless USB device (%d): %s", i, sanitized_short);

		drv_info = calloc(1, sizeof(struct driver_info));
		if (drv_info == NULL) {
			free_di(ret);
			return NULL;
		}
		if (cur == NULL) {
			ret = drv_info;
		} else {
			cur->next = drv_info;
		}
		cur = drv_info;

		safe_strcpy(drv_info->desc, sizeof(drv_info->desc), desc);

		token = strtok (sanitized_short, "#&");
		while(token != NULL) {
			for (j = 0; j < 3; j++) {
				if (safe_strncmp(token, prefix[j], strlen(prefix[j])) == 0) {
					switch(j) {
					case 0:
						safe_strcpy(drv_info->vid, sizeof(drv_info->vid), token);
						break;
					case 1:
						safe_strcpy(drv_info->pid, sizeof(drv_info->pid), token);
						break;
					case 2:
						safe_strcpy(drv_info->mi, sizeof(drv_info->mi), token);
						break;
					default:
						usbi_err(NULL, "unexpected case");
						break;
					}
				}
			}
			token = strtok (NULL, "#&");
		}
	}

	return ret;
}

// TODO: dynamic res addon
int extract_dlls(char* path)
{
	HANDLE h;
	HGLOBAL h_load;
	void *data;
	DWORD size;
	char filename[MAX_PATH_LENGTH];
	FILE* fd;
	int i;

	for (i=0; i< nb_resources; i++) {
		h = FindResource(NULL, resource[i].id, "DLL");
		if (h == NULL) {
			usbi_dbg("could not find resource %s", resource[i].id);
			return -1;
		}
		h_load = LoadResource(NULL, h);
		if (h_load == NULL) {
			usbi_dbg("could not load resource %s", resource[i].id);
			return -1;
		}
		data = LockResource(h_load);
		if (data == NULL) {
			usbi_dbg("could not access data for %s", resource[i].id);
			return -1;
		}
		size = SizeofResource(NULL, h);
		if (size == 0) {
			usbi_dbg("could not access size of %s", resource[i].id);
			return -1;
		}

		safe_strcpy(filename, MAX_PATH_LENGTH, path);
		safe_strcat(filename, MAX_PATH_LENGTH, "\\");
		safe_strcat(filename, MAX_PATH_LENGTH, resource[i].subdir);

		if ( (_access(filename, 02) != 0) && (CreateDirectory(filename, 0) == 0) ) {
			usbi_err(NULL, "could not access directory: %s", filename);
			return -1;
		}
		safe_strcat(filename, MAX_PATH_LENGTH, "\\");
		safe_strcat(filename, MAX_PATH_LENGTH, resource[i].name);
		
	
		fd = fopen(filename, "wb");
		if (fd == NULL) {
			usbi_err(NULL, "failed to create file: %s", filename);
			return -1;
		}

		fwrite(data, size, 1, fd);
		fclose(fd);
	}

	usbi_dbg("so far, so good");
	return 0;

}

// Create an inf and extract coinstallers in the directory pointed by path
int create_inf(struct driver_info* drv_info, char* path)
{
	char filename[MAX_PATH_LENGTH];
	FILE* fd;
	GUID guid;

	// TODO? create a reusable temp dir if path is NULL?
	if ((path == NULL) || (drv_info == NULL))
		return -1;

	// Try to create directory if it doesn't exist
	if ( (_access(path, 02) != 0) && (CreateDirectory(path, 0) == 0) ) {
		usbi_err(NULL, "could not access directory: %s", path);
		return -1;
	}

	extract_dlls(path);

	safe_strcpy(filename, MAX_PATH_LENGTH, path);
	safe_strcat(filename, MAX_PATH_LENGTH, "\\libusb_device.inf");

	fd = fopen(filename, "w");
	if (fd == NULL) {
		usbi_err(NULL, "failed to create file: %s", filename);
		return -1;
	}

	fprintf(fd, "; libusb_device.inf\n");
	fprintf(fd, "; Copyright (c) 2010 libusb (GNU LGPL)\n");
	fprintf(fd, "[Strings]\n");
	fprintf(fd, "DeviceName = \"%s\"\n", drv_info->desc);
	fprintf(fd, "DeviceID = \"%s&%s", drv_info->vid, drv_info->pid);
	if (drv_info->mi[0] != 0) {
		fprintf(fd, "&%s\"\n", drv_info->mi);
	} else {
		fprintf(fd, "\"\n");
	}
	CoCreateGuid(&guid);
	fprintf(fd, "DeviceGUID = \"%s\"\n", guid_to_string(guid));
	fwrite(inf, sizeof(inf)-1, 1, fd);
	return 0;

	// TODO: extract coinstaller files from resource
	// TODO: create cat file for XP?
}

int install_device(char* path)
{
	DWORD r;
	BOOL reboot_needed;
//	INSTALLERINFO installer_info;

	r = DriverPackagePreinstall(path, DRIVER_PACKAGE_LEGACY_MODE|DRIVER_PACKAGE_REPAIR);
	// Will fail if inf not signed, unless DRIVER_PACKAGE_LEGACY_MODE is specified.
	// r = 87 ERROR_INVALID_PARAMETER on path == NULL
	// r = 2 ERROR_FILE_NOT_FOUND if no inf in path
	// r = 5 ERROR_ACCESS_DENIED if needs admin elevation
	// r = 0xE0000003 ERROR_GENERAL_SYNTAX the syntax of the inf is invalid
	// r = 0xE0000304 ERROR_INVALID_CATALOG_DATA => no cat
	// r = 0xE0000247 if user decided not to install on warnings
	// r = 0x800B0100 ERROR_WRONG_INF_STYLE => missing cat entry in inf
	// r = 0xB7 => missing DRIVER_PACKAGE_REPAIR flag
	switch(r) {
	case ERROR_INVALID_PARAMETER:
		usbi_err(NULL, "invalid path");
		return -1;
	case ERROR_FILE_NOT_FOUND:
		usbi_err(NULL, "unable to find inf file on %s", path);
		return -1;
	case ERROR_ACCESS_DENIED:
		usbi_err(NULL, "this process needs to be run with administrative privileges");
		return -1;
	case ERROR_WRONG_INF_STYLE:
	case ERROR_GENERAL_SYNTAX:
		usbi_err(NULL, "the syntax of the inf is invalid");
		return -1;
	case ERROR_INVALID_CATALOG_DATA:
		usbi_err(NULL, "unable to locate cat file");
		return -1;
	case ERROR_DRIVER_STORE_ADD_FAILED:
		usbi_err(NULL, "cancelled by user");
		return -1;
	// TODO: make DRIVER_PACKAGE_REPAIR optional
	case ERROR_ALREADY_EXISTS:
		usbi_err(NULL, "driver already exists");
		return -1;
	default:
		usbi_err(NULL, "unhandled error %X", r);
		return -1;
	}

	// TODO: use 
	r = DriverPackageInstall(path, DRIVER_PACKAGE_LEGACY_MODE|DRIVER_PACKAGE_REPAIR,
		NULL, &reboot_needed);
	usbi_dbg("ret = %X", r);

	return 0;
}