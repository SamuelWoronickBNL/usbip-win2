#include <libusbip\common.h>
#include <libusbip\dbgcode.h>
#include <libusbip\setupdi.h>

#include <cassert>
#include <string>

#include <initguid.h>
#include "vhci.h"

namespace
{

bool walker_devpath(std::wstring &path, HDEVINFO dev_info, SP_DEVINFO_DATA *data)
{
        if (auto inf = usbip::get_intf_detail(dev_info, data, usbip::vhci::GUID_DEVINTERFACE_USB_HOST_CONTROLLER)) {
                assert(inf->cbSize == sizeof(*inf)); // this is not a size/length of DevicePath
                path = inf->DevicePath;
                return true;
        }

        return false;
}

auto get_vhci_devpath()
{
        std::wstring path;
        auto f = [&path] (auto&&...args) { return walker_devpath(path, std::forward<decltype(args)>(args)...); };

        usbip::traverse_intfdevs(usbip::vhci::GUID_DEVINTERFACE_USB_HOST_CONTROLLER, f);
        return path;
}

} // namespace


auto usbip::vhci_driver_open() -> Handle
{
        Handle h;

        auto devpath = get_vhci_devpath();
        if (devpath.empty()) {
                return h;
        }
        
        dbg("device path: %S", devpath.c_str());
        
        auto fh = CreateFile(devpath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        h.reset(fh);

        return h;
}

auto usbip::vhci_get_imported_devs(HANDLE hdev) -> std::vector<vhci::ioctl_imported_dev>
{
        std::vector<vhci::ioctl_imported_dev> v(vhci::TOTAL_PORTS + 1);
        auto idevs_bytes = DWORD(v.size()*sizeof(v[0]));

        if (!DeviceIoControl(hdev, vhci::IOCTL_GET_IMPORTED_DEVICES, nullptr, 0, v.data(), idevs_bytes, nullptr, nullptr)) {
                dbg("failed to get imported devices: 0x%lx", GetLastError());
                v.clear();
        }

        return v;
}

bool usbip::vhci_attach_device(HANDLE hdev, vhci::ioctl_plugin &r)
{
        auto ok = DeviceIoControl(hdev, vhci::IOCTL_PLUGIN_HARDWARE, &r, sizeof(r), &r, sizeof(r.port), nullptr, nullptr);
        if (!ok) {
                dbg("%s: DeviceIoControl error %#x", __func__, GetLastError());
        }
        return ok;
}

int usbip::vhci_detach_device(HANDLE hdev, int port)
{
        vhci::ioctl_unplug r{ port };

        if (DeviceIoControl(hdev, vhci::IOCTL_UNPLUG_HARDWARE, &r, sizeof(r), nullptr, 0, nullptr, nullptr)) {
                return 0;
        }

        auto err = GetLastError();
        dbg("%s: DeviceIoControl error %#x", __func__, err);

        switch (err) {
        case ERROR_FILE_NOT_FOUND:
                return ERR_NOTEXIST;
        case ERROR_INVALID_PARAMETER:
                return ERR_INVARG;
        default:
                return ERR_GENERAL;
        }
}
