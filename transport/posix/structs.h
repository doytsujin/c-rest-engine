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

typedef struct _VM_SOCKET
{
    LONG                             refCount;
    VM_SOCK_TYPE                     type;
    VM_SOCK_PROTOCOL                 protocol;
    struct sockaddr                  addr;
    socklen_t                        addrLen;
    PVM_STREAM_BUFFER                pStreamBuffer;
    struct sockaddr*                 pAddr;
    PVMREST_MUTEX                    pMutex;
    int                              fd;
    SSL*                             ssl;
    PVOID                            pData;
    uint32_t                         inUse;
} VM_SOCKET;

typedef struct _VM_SOCK_EVENT_QUEUE
{
    PVMREST_MUTEX                    pMutex;
    PVM_SOCKET                       pSignalReader;
    PVM_SOCKET                       pSignalWriter;
    VM_SOCK_POSIX_EVENT_STATE        state;
    int                              epollFd;
    struct epoll_event *             pEventArray;
    DWORD                            dwSize;
    int                              nReady;
    int                              iReady;
} VM_SOCK_EVENT_QUEUE;

typedef struct _VM_SOCK_IO_CONTEXT
{
    VM_SOCK_EVENT_TYPE               eventType;
    VM_SOCK_IO_BUFFER                IoBuffer;
    CHAR                             DataBuffer[1];
} VM_SOCK_IO_CONTEXT, *PVM_SOCK_IO_CONTEXT;

typedef struct _SOCK_SSL_INFO
{
    SSL_CTX*                         sslContext;
    uint32_t                         isSecure;
} SOCK_SSL_INFO;

