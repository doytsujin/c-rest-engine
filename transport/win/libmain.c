/*
 * Copyright © 2012-2015 VMware, Inc.  All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the “License”); you may not
 * use this file except in compliance with the License.  You may obtain a copy
 * of the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an “AS IS” BASIS, without
 * warranties or conditions of any kind, EITHER EXPRESS OR IMPLIED.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */


#include "includes.h"

DWORD
VmWinSockInitialize(
    PVM_SOCK_PACKAGE* ppPackage
    )
{
    DWORD dwError = 0;
    WSADATA wsaData = { 0 };

    dwError = WSAStartup(MAKEWORD(2, 2), &wsaData);
    BAIL_ON_VMREST_ERROR(dwError);

    *ppPackage = gpVmWinSockPackage;

cleanup:

    return dwError;

error:

    *ppPackage = NULL;

    goto cleanup;
}

VOID
VmWinSockShutdown(
    PVM_SOCK_PACKAGE pPackage
    )
{
    WSACleanup();
}
