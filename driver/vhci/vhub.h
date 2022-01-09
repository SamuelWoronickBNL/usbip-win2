#pragma once

#include "pageable.h"
#include "dev.h"
#include "usbip_vhci_api.h"

PAGEABLE vpdo_dev_t *vhub_find_vpdo(vhub_dev_t *vhub, int port);

PAGEABLE void vhub_attach_vpdo(vhub_dev_t *vhub, vpdo_dev_t *vpdo);
PAGEABLE void vhub_detach_vpdo_and_release_port(vhub_dev_t *vhub, vpdo_dev_t *vpdo);
PAGEABLE void vhub_get_hub_descriptor(vhub_dev_t *vhub, USB_HUB_DESCRIPTOR *pdesc);

PAGEABLE NTSTATUS vhub_get_information_ex(vhub_dev_t *vhub, USB_HUB_INFORMATION_EX *pinfo);
PAGEABLE NTSTATUS vhub_get_capabilities_ex(vhub_dev_t *vhub, USB_HUB_CAPABILITIES_EX *pinfo);
PAGEABLE NTSTATUS vhub_get_port_connector_properties(vhub_dev_t *vhub, USB_PORT_CONNECTOR_PROPERTIES *pinfo, ULONG *poutlen);

PAGEABLE void vhub_mark_unplugged_vpdo(vhub_dev_t *vhub, vpdo_dev_t *vpdo);
PAGEABLE void vhub_mark_unplugged_all_vpdos(vhub_dev_t *vhub);

PAGEABLE NTSTATUS vhub_get_ports_status(vhub_dev_t *vhub, ioctl_usbip_vhci_get_ports_status *st);
PAGEABLE NTSTATUS vhub_get_imported_devs(vhub_dev_t *vhub, ioctl_usbip_vhci_imported_dev *idevs, size_t cnt);

PAGEABLE int acquire_port(vhub_dev_t &vhub);
PAGEABLE NTSTATUS release_port(vhub_dev_t &vhub, int port);

PAGEABLE int get_vpdo_count(const vhub_dev_t &vhub);
PAGEABLE int get_plugged_vpdo_count(vhub_dev_t &vhub, bool lock = true);

PAGEABLE constexpr auto is_valid_port(int port)
{
	return port > 0 && port <= vhub_dev_t::NUM_PORTS;
}
