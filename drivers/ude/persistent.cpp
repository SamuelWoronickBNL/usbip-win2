/*
 * Copyright (C) 2023 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "persistent.h"
#include "trace.h"
#include "persistent.tmh"

#include "context.h"

#include <libdrv\wdf_cpp.h>
#include <libdrv\strconv.h>
#include <libdrv\handle.h>

#include <resources/messages.h>

namespace 
{

using namespace usbip;

/*
 * @param path L"\\SystemRoot\\usbip2_ude.log" -> C:\Windows\...
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
inline PAGED auto create_log(_In_ const wchar_t *path)
{
        PAGED_CODE();

        UNICODE_STRING name;
        RtlInitUnicodeString(&name, path);

        OBJECT_ATTRIBUTES attr;
        InitializeObjectAttributes(&attr, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);

        const auto opts = FILE_WRITE_THROUGH | FILE_SYNCHRONOUS_IO_NONALERT | FILE_SEQUENTIAL_ONLY;
        IO_STATUS_BLOCK ios;
        HANDLE h{};

        if (auto err = ZwCreateFile(&h, GENERIC_WRITE, &attr, &ios, nullptr, // AllocationSize
                                    FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_SUPERSEDE, opts, nullptr, 0)) {
                Trace(TRACE_LEVEL_ERROR, "ZwCreateFile('%!USTR!') %!STATUS!", &name, err);
        }

        return libdrv::handle(h);
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
inline PAGED void write(_In_ HANDLE h, _In_ const wchar_t *str, _In_ ULONG maxlen = 1024)
{
        PAGED_CODE();
        if (!h) {
                return;
        }

        auto bytes = static_cast<ULONG>(wcsnlen_s(str, maxlen)*sizeof(*str));
        IO_STATUS_BLOCK ios; 

        if (auto err = ZwWriteFile(h, nullptr, nullptr, nullptr, &ios, (void*)str, bytes, nullptr, nullptr)) {
                Trace(TRACE_LEVEL_ERROR, "ZwWriteFile %!STATUS!", err);
        }
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto make_collection(_In_ WDFOBJECT parent)
{
        PAGED_CODE();

        WDF_OBJECT_ATTRIBUTES attr;
        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject = parent;

        WDFCOLLECTION col{};
        if (auto err = WdfCollectionCreate(&attr, &col)) {
                Trace(TRACE_LEVEL_ERROR, "WdfCollectionCreate %!STATUS!", err);
        }
        
        return col;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto get_persistent_devices(_In_ WDFKEY key)
{
        PAGED_CODE();

        auto col = make_collection(key);
        if (!col) {
                return col;
        }

        WDF_OBJECT_ATTRIBUTES str_attr;
        WDF_OBJECT_ATTRIBUTES_INIT(&str_attr);
        str_attr.ParentObject = col;

        UNICODE_STRING value_name;
        RtlUnicodeStringInit(&value_name, persistent_devices_value_name);

        if (auto err = WdfRegistryQueryMultiString(key, &value_name, &str_attr, col)) {
                Trace(TRACE_LEVEL_ERROR, "WdfRegistryQueryMultiString('%!USTR!') %!STATUS!", &value_name, err);
                col = WDF_NO_HANDLE; // parent will destory it
        }

        return col;
}

constexpr auto empty(_In_ const UNICODE_STRING &s)
{
        return libdrv::empty(s) || !*s.Buffer;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto parse_string(_Out_ vhci::ioctl::plugin_hardware &r, _In_ const UNICODE_STRING &str)
{
        PAGED_CODE();

        UNICODE_STRING host;
        UNICODE_STRING service;
        UNICODE_STRING busid;

        const auto sep = L',';

        libdrv::split(host, busid, str, sep);
        if (empty(host)) {
                return STATUS_INVALID_PARAMETER;
        }

        libdrv::split(service, busid, busid, sep);
        if (empty(service) || empty(busid)) {
                return STATUS_INVALID_PARAMETER;
        }

        return copy(r.host, sizeof(r.host), host, 
                    r.service, sizeof(r.service), service, 
                    r.busid, sizeof(r.busid), busid);
}

/*
 * Target is self. 
 */
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto make_target(_In_ WDFDEVICE vhci)
{
        PAGED_CODE();
        wdf::ObjectDelete target;

        if (WDFIOTARGET t; auto err = WdfIoTargetCreate(vhci, WDF_NO_OBJECT_ATTRIBUTES, &t)) {
                Trace(TRACE_LEVEL_ERROR, "WdfIoTargetCreate %!STATUS!", err);
                return target;
        } else {
                target.reset(t);
        }

        auto fdo = WdfDeviceWdmGetDeviceObject(vhci);

        WDF_IO_TARGET_OPEN_PARAMS params;
        WDF_IO_TARGET_OPEN_PARAMS_INIT_EXISTING_DEVICE(&params, fdo);

        if (auto err = WdfIoTargetOpen(target.get<WDFIOTARGET>(), &params)) {
                Trace(TRACE_LEVEL_ERROR, "WdfIoTargetOpen %!STATUS!", err);
                target.reset();
        }

        return target;
}

constexpr auto get_delay(_In_ ULONG attempt, _In_ ULONG cnt)
{
        NT_ASSERT(cnt);
        enum { UNIT = 10, MAX_DELAY = 30*60 }; // seconds
        return attempt > 1 ? min(UNIT*attempt/cnt, MAX_DELAY) : 0; // first two attempts without a delay
}

_IRQL_requires_same_
_IRQL_requires_max_(APC_LEVEL)
PAGED void sleep(_In_ int seconds, _Inout_ volatile bool &stopped)
{
        PAGED_CODE();
        
        enum { 
                _100_NSEC = 1,  // in units of 100 nanoseconds
                USEC = 10*_100_NSEC, // microsecond
                MSEC = 1000*USEC, // millisecond
                SEC = 1000*MSEC, // second
        };

        enum { RESOLUTION = 5 };
        auto n = seconds/RESOLUTION + bool(seconds % RESOLUTION);

        for (int i = 0; i < n && !stopped; ++i) { // to be able to interrupt
                if (LARGE_INTEGER intv{ .QuadPart = -RESOLUTION*SEC }; // relative
                    auto err = KeDelayExecutionThread(KernelMode, false, &intv)) {
                        TraceDbg("KeDelayExecutionThread %!STATUS!", err);
                }
        }
}

/*
 * WskGetAddressInfo() can return STATUS_INTERNAL_ERROR(0xC00000E5), but after some delay it will succeed.
 * This can happen after reboot if dnscache(?) service is not ready yet.
 */
constexpr auto can_retry(_In_ DWORD error)
{
        switch (error) {
        case ERROR_USBIP_ADDRINFO:
        case ERROR_USBIP_CONNECT:
        case ERROR_USBIP_NETWORK:
                return true;
        }

        return false;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED auto plugin_hardware(
        _In_ WDFSTRING line, 
        _In_ WDFIOTARGET target,
        _Inout_ vhci::ioctl::plugin_hardware &req,
        _Inout_ WDF_MEMORY_DESCRIPTOR &input,
        _Inout_ WDF_MEMORY_DESCRIPTOR &output,
        _In_ const ULONG outlen)
{
        PAGED_CODE();

        UNICODE_STRING str{};
        WdfStringGetUnicodeString(line, &str);

        if (auto err = parse_string(req, str)) {
                Trace(TRACE_LEVEL_ERROR, "'%!USTR!' parse %!STATUS!", &str, err);
                return true; // remove malformed string
        }

        Trace(TRACE_LEVEL_INFORMATION, "%s:%s/%s", req.host, req.service, req.busid);
        req.port = 0;

        if (ULONG_PTR BytesReturned; // send IOCTL to itself
            auto err = WdfIoTargetSendIoctlSynchronously(target, WDF_NO_HANDLE, vhci::ioctl::PLUGIN_HARDWARE, 
                                                         &input, &output, nullptr, &BytesReturned)) {
                Trace(TRACE_LEVEL_ERROR, "WdfIoTargetSendIoctlSynchronously %!STATUS!", err);
                return !can_retry(err);
        } else {
                NT_ASSERT(BytesReturned == outlen);
                return true;
        }
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void plugin_persistent_devices(_In_ vhci_ctx &ctx)
{
        PAGED_CODE();

        wdf::Registry key;
        if (auto err = WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_QUERY_VALUE, 
                                                          WDF_NO_OBJECT_ATTRIBUTES, &key)) {
                Trace(TRACE_LEVEL_ERROR, "WdfDriverOpenParametersRegistryKey %!STATUS!", err);
                return;
        }

        auto col = get_persistent_devices(key.get());
        if (!(col && WdfCollectionGetCount(col))) {
                return;
        }

        auto vhci = get_device(&ctx);

        auto target = make_target(vhci);
        if (!target) {
                return;
        }

        vhci::ioctl::plugin_hardware req{{ .size = sizeof(req) }};

        WDF_MEMORY_DESCRIPTOR input;
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&input, &req, sizeof(req));

        constexpr auto outlen = offsetof(vhci::ioctl::plugin_hardware, port) + sizeof(req.port);

        WDF_MEMORY_DESCRIPTOR output;
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&output, &req, outlen);

        for (ULONG attempt = 0; !ctx.stop_thread; ++attempt) {

                ULONG cnt = min(WdfCollectionGetCount(col), ARRAYSIZE(ctx.devices));
                if (!cnt) {
                        break;
                }

                if (auto secs = get_delay(attempt, cnt)) {
                        TraceDbg("attempt #%lu, devices %lu -> sleep %d sec.", attempt, cnt, secs);
                        sleep(secs, ctx.stop_thread);
                }

                for (ULONG i = 0; i < cnt && !ctx.stop_thread; ) {

                        if (auto str = (WDFSTRING)WdfCollectionGetItem(col, i);
                            plugin_hardware(str, target.get<WDFIOTARGET>(), req, input, output, outlen)) {
                                WdfCollectionRemove(col, str);
                                --cnt;
                        } else {
                                ++i;
                        }
                }
        }
}

/*
 * If load_thread is set to NULL here, it is possible that this thread will be suspended 
 * while vhci_cleanup will be executed, WDFDEVICE will be destroyed, the driver will be unloaded.
 * IoCreateSystemThread is used to prevent this.
 */
_IRQL_requires_same_
_Function_class_(KSTART_ROUTINE)
PAGED void run(_In_ void *ctx)
{
        PAGED_CODE();
        KeSetPriorityThread(KeGetCurrentThread(), LOW_PRIORITY + 1);

        auto &vhci = *static_cast<vhci_ctx*>(ctx);
        plugin_persistent_devices(vhci);

        if (auto thread = (_KTHREAD*)InterlockedExchangePointer(reinterpret_cast<PVOID*>(&vhci.load_thread), nullptr)) {
                ObDereferenceObject(thread);
                TraceDbg("thread %04x closed", ptr04x(thread));
        } else {
                TraceDbg("thread %04x exited", ptr04x(thread));
        }
}

} // namespace 


_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED void usbip::plugin_persistent_devices(_In_ vhci_ctx *vhci)
{
        PAGED_CODE();

        const auto access = THREAD_ALL_ACCESS;
        auto fdo = WdfDeviceWdmGetDeviceObject(get_device(vhci));

        if (HANDLE handle; 
            auto err = IoCreateSystemThread(fdo, &handle, access, nullptr, nullptr, nullptr, run, vhci)) {
                Trace(TRACE_LEVEL_ERROR, "PsCreateSystemThread %!STATUS!", err);
        } else {
                PVOID thread;
                NT_VERIFY(NT_SUCCESS(ObReferenceObjectByHandle(handle, access, *PsThreadType, KernelMode, 
                                                               &thread, nullptr)));

                NT_VERIFY(NT_SUCCESS(ZwClose(handle)));
                NT_VERIFY(!InterlockedExchangePointer(reinterpret_cast<PVOID*>(&vhci->load_thread), thread));
                TraceDbg("thread %04x launched", ptr04x(thread));
        }
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGED NTSTATUS usbip::copy(
        _Out_ char *host, _In_ USHORT host_sz, _In_ const UNICODE_STRING &uhost,
        _Out_ char *service, _In_ USHORT service_sz, _In_ const UNICODE_STRING &uservice,
        _Out_ char *busid, _In_ USHORT busid_sz, _In_ const UNICODE_STRING &ubusid)
{
        PAGED_CODE();

        struct {
                char *dst;
                USHORT dst_sz;
                const UNICODE_STRING &src;
        } const v[] = {
                {host, host_sz, uhost},
                {service, service_sz, uservice},
                {busid, busid_sz, ubusid},
        };

        for (auto &[dst, dst_sz, src]: v) {
                if (auto err = libdrv::unicode_to_utf8(dst, dst_sz, src)) {
                        Trace(TRACE_LEVEL_ERROR, "unicode_to_utf8('%!USTR!') %!STATUS!", &src, err);
                        return err;
                }
        }

        return STATUS_SUCCESS;
}
