﻿/*
 * Copyright (C) 2022 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#pragma once

#include <ntdef.h>

struct vpdo_dev_t;
struct _WSK_DATA_INDICATION;

size_t wsk_data_size(_In_ const vpdo_dev_t &vpdo);

void wsk_data_push(_Inout_ vpdo_dev_t &vpdo, _In_ _WSK_DATA_INDICATION *DataIndication, _In_ size_t BytesIndicated);
size_t wsk_data_release(_Inout_ vpdo_dev_t &vpdo, _In_ size_t len);

struct WskDataCopyState
{
        _WSK_DATA_INDICATION *cur{};
        size_t offset{};
        bool next{};
};

NTSTATUS wsk_data_copy(_In_ const vpdo_dev_t &vpdo, _Out_ void *dest, _In_ size_t offset, _In_ size_t len, 
                       _Inout_opt_ WskDataCopyState *consume = nullptr, _Out_opt_ size_t *actual = nullptr);


