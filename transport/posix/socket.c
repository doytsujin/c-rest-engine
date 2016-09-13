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

static
DWORD
VmSockPosixCreateSignalSockets(
    PVM_SOCKET*                      ppReaderSocket,
    PVM_SOCKET*                      ppWriterSocket
    );

static
DWORD
VmSockPosixEventQueueAdd_inlock(
    PVM_SOCK_EVENT_QUEUE             pQueue,
    PVM_SOCKET                       pSocket
    );

static
DWORD
VmSockPosixAcceptConnection(
    PVM_SOCKET                       pListener,
    PVM_SOCKET*                      ppSocket
    );

static
DWORD
VmSockPosixSetDescriptorNonBlocking(
    int                              fd
    );

static
DWORD
VmSockPosixSetReuseAddress(
    int                              fd
    );

static
VOID
VmSockPosixFreeEventQueue(
    PVM_SOCK_EVENT_QUEUE             pQueue
    );

static
VOID
VmSockPosixFreeSocket(
    PVM_SOCKET                       pSocket
    );

DWORD
VmSockPosixOpenClient(
    PCSTR                            pszHost,
    USHORT                           usPort,
    VM_SOCK_CREATE_FLAGS             dwFlags,
    PVM_SOCKET*                      ppSocket
    )
{
    DWORD                            dwError = REST_ENGINE_SUCCESS;
    struct addrinfo                  hints   = {0};
    struct addrinfo*                 pAddrInfo = NULL;
    struct addrinfo*                 pInfo = NULL;
    int                              fd = -1;
    PVM_SOCKET                       pSocket = NULL;
    CHAR                             szPort[32];

    if (!pszHost || !usPort || !ppSocket)
    {
        dwError = ERROR_INVALID_PARAMETER;
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
    }

    sprintf(szPort, "%d", usPort);

    if (dwFlags & VM_SOCK_CREATE_FLAGS_IPV6)
    {
        hints.ai_family = AF_INET6;
    }
    else if (dwFlags & VM_SOCK_CREATE_FLAGS_IPV4)
    {
        hints.ai_family = AF_INET;
    }
    else
    {
        hints.ai_family = AF_UNSPEC;
    }

    if (dwFlags & VM_SOCK_CREATE_FLAGS_UDP)
    {
        hints.ai_socktype = SOCK_DGRAM;
    }
    else
    {
        hints.ai_socktype = SOCK_STREAM;
    }

    hints.ai_flags    = AI_CANONNAME | AI_NUMERICSERV;

    if (getaddrinfo(pszHost, szPort, &hints, &pAddrInfo) != 0)
    {
        VMREST_LOG_ERROR("getaddrinfo() Failed with errno %d", errno);
        dwError = VM_SOCK_POSIX_ERROR_SYS_CALL_FAILED;
    }
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    for (pInfo = pAddrInfo; (fd < 0) && (pInfo != NULL); pInfo = pInfo->ai_next)
    {
        fd = socket(pInfo->ai_family, pInfo->ai_socktype, pInfo->ai_protocol);

        if (fd < 0)
        {
            continue;
        }

        if (connect(fd, pInfo->ai_addr, pInfo->ai_addrlen) < 0)
        {
            close(fd);
            fd = -1;

            continue;
        }

        break;
    }

    if (fd < 0)
    {
        dwError = ERROR_CONNECTION_UNAVAIL;
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
    }

    if (dwFlags & VM_SOCK_CREATE_FLAGS_NON_BLOCK)
    {
        dwError = VmSockPosixSetDescriptorNonBlocking(fd);
    }
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    dwError = VmRESTAllocateMemory(
                  sizeof(*pSocket),
                  (PVOID*)&pSocket
                  );
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    pSocket->refCount = 1;

    pSocket->type = VM_SOCK_TYPE_CLIENT;

    if (dwFlags & VM_SOCK_CREATE_FLAGS_UDP)
    {
        pSocket->protocol = VM_SOCK_PROTOCOL_UDP;
    }
    else
    {
        pSocket->protocol = VM_SOCK_PROTOCOL_TCP;
    }

    dwError = VmRESTAllocateMutex(&pSocket->pMutex);
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    memcpy(&pSocket->addr, pInfo->ai_addr, pInfo->ai_addrlen);
    pSocket->addrLen = pInfo->ai_addrlen;
    pSocket->fd = fd;

    *ppSocket = pSocket;

cleanup:

    if (pAddrInfo)
    {
        freeaddrinfo(pAddrInfo);
    }

    return dwError;

error:

    if (ppSocket)
    {
        *ppSocket = NULL;
    }
    if (pSocket)
    {
        VmSockPosixFreeSocket(pSocket);
    }
    if (fd >= 0)
    {
        close(fd);
    }

    goto cleanup;
}

DWORD
VmSockPosixOpenServer(
    USHORT                           usPort,
    int                              iListenQueueSize,
    VM_SOCK_CREATE_FLAGS             dwFlags,
    PVM_SOCKET*                      ppSocket
    )
{
    DWORD                            dwError = REST_ENGINE_SUCCESS;
    union
    {
#ifdef AF_INET6
        struct sockaddr_in6          servaddr_ipv6;
#endif
        struct sockaddr_in           servaddr_ipv4;
    } servaddr;
    struct
    {
        int                          domain;
        int                          type;
        int                          protocol;
    } socketParams;
    struct sockaddr*                 pSockAddr = NULL;
    socklen_t                        addrLen = 0;
    int                              fd = -1;
    PVM_SOCKET                       pSocket = NULL;

    if (dwFlags & VM_SOCK_CREATE_FLAGS_IPV6)
    {
#ifdef AF_INET6
        socketParams.domain = AF_INET6;
#else
        dwError = ERROR_NOT_SUPPORTED;
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
#endif
    }
    else
    {
        socketParams.domain = AF_INET;
    }

    if (dwFlags & VM_SOCK_CREATE_FLAGS_UDP)
    {
        socketParams.type = SOCK_DGRAM;
    }
    else
    {
        socketParams.type = SOCK_STREAM;
    }

    socketParams.protocol = 0;

    fd = socket(socketParams.domain, socketParams.type, socketParams.protocol);
    if (fd < 0)
    {
        VMREST_LOG_ERROR("Socket call failed with Error code %d", errno);
        dwError = VM_SOCK_POSIX_ERROR_SYS_CALL_FAILED;
    }
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    if (dwFlags & VM_SOCK_CREATE_FLAGS_REUSE_ADDR)
    {
        dwError = VmSockPosixSetReuseAddress(fd);
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
    }

    memset(&servaddr, 0, sizeof(servaddr));

    if (dwFlags & VM_SOCK_CREATE_FLAGS_IPV6)
    {
#ifdef AF_INET6
        servaddr.servaddr_ipv6.sin6_family = AF_INET6;
        servaddr.servaddr_ipv6.sin6_addr = in6addr_any;
        servaddr.servaddr_ipv6.sin6_port = htons(usPort);

        pSockAddr = (struct sockaddr*) &servaddr.servaddr_ipv6;
        addrLen = sizeof(servaddr.servaddr_ipv6);

#if defined(SOL_IPV6) && defined(IPV6_V6ONLY)
        int one = 1;
        setsockopt(fd, SOL_IPV6, IPV6_V6ONLY, (void *) &one, sizeof(one));
#endif

#else
        dwError = ERROR_NOT_SUPPORTED;
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
#endif
    }
    else
    {
        servaddr.servaddr_ipv4.sin_family = AF_INET;
        servaddr.servaddr_ipv4.sin_addr.s_addr = htonl(INADDR_ANY);
        servaddr.servaddr_ipv4.sin_port = htons(usPort);

        pSockAddr = (struct sockaddr*) &servaddr.servaddr_ipv4;
        addrLen = sizeof(servaddr.servaddr_ipv4);
    }

    if (bind(fd, pSockAddr, addrLen) < 0)
    {
        VMREST_LOG_ERROR("bind() call failed with Error code %d", errno);
        dwError = VM_SOCK_POSIX_ERROR_SYS_CALL_FAILED;
    }
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    if (dwFlags & VM_SOCK_CREATE_FLAGS_NON_BLOCK)
    {
        dwError = VmSockPosixSetDescriptorNonBlocking(fd);
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
    }

    if (!(dwFlags & VM_SOCK_CREATE_FLAGS_UDP))
    {
        if (iListenQueueSize <= 0)
        {
            iListenQueueSize = VM_SOCK_POSIX_DEFAULT_LISTEN_QUEUE_SIZE;
        }

        if (listen(fd, iListenQueueSize) < 0)
        {
            VMREST_LOG_ERROR("Listen() on server socket with fd %d failed with Error code %d", fd, errno);
            dwError = VM_SOCK_POSIX_ERROR_SYS_CALL_FAILED;
        }
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
    }
    VMREST_LOG_DEBUG("Server Listening on socket with fd %d ...", fd);

    dwError = VmRESTAllocateMemory(
                  sizeof(*pSocket),
                  (PVOID*)&pSocket
                  );
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    pSocket->refCount = 1;

    dwError = VmRESTAllocateMutex(&pSocket->pMutex);
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    pSocket->type = VM_SOCK_TYPE_LISTENER;

    if (dwFlags & VM_SOCK_CREATE_FLAGS_UDP)
    {
        pSocket->protocol = VM_SOCK_PROTOCOL_UDP;
    }
    else
    {
        pSocket->protocol = VM_SOCK_PROTOCOL_TCP;
    }

    pSocket->fd = fd;
    pSocket->pStreamBuffer = NULL;

    *ppSocket = pSocket;

cleanup:

    return dwError;

error:

    if (ppSocket)
    {
        *ppSocket = NULL;
    }

    if (pSocket)
    {
        VmSockPosixFreeSocket(pSocket);
    }
    if (fd >= 0)
    {
        close(fd);
    }

    goto cleanup;
}

DWORD
VmSockPosixCreateEventQueue(
    int                              iEventQueueSize,
    PVM_SOCK_EVENT_QUEUE*            ppQueue
    )
{
    DWORD                            dwError = REST_ENGINE_SUCCESS;
    PVM_SOCK_EVENT_QUEUE             pQueue = NULL;

    if (!ppQueue)
    {
        VMREST_LOG_ERROR("Invalid params");
        dwError = ERROR_INVALID_PARAMETER;
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
    }

    if (iEventQueueSize <= 0)
    {
        iEventQueueSize = VM_SOCK_POSIX_DEFAULT_QUEUE_SIZE;
    }

    dwError = VmRESTAllocateMemory(
                  sizeof(*pQueue),
                  (PVOID*)&pQueue
                  );
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    dwError = VmSockPosixCreateSignalSockets(
                  &pQueue->pSignalReader,
                  &pQueue->pSignalWriter
                  );
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    dwError = VmRESTAllocateMutex(&pQueue->pMutex);
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    dwError = VmRESTAllocateMemory(
                  iEventQueueSize * sizeof(*pQueue->pEventArray),
                  (PVOID*)&pQueue->pEventArray
                  );
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    pQueue->dwSize = iEventQueueSize;

    pQueue->epollFd = epoll_create(pQueue->dwSize);
    if (pQueue->epollFd < 0)
    {
        VMREST_LOG_ERROR("epoll create failed with Error code %d", errno);
        dwError = VM_SOCK_POSIX_ERROR_SYS_CALL_FAILED;
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
    }

    pQueue->state  = VM_SOCK_POSIX_EVENT_STATE_WAIT;
    pQueue->nReady = -1;
    pQueue->iReady = 0;

    dwError = VmSockPosixEventQueueAdd_inlock(
                  pQueue,
                  pQueue->pSignalReader
                  );
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    *ppQueue = pQueue;
    VMREST_LOG_DEBUG("Event queue creation successful");

cleanup:

    return dwError;

error:

    if (ppQueue)
    {
        *ppQueue = NULL;
    }

    if (pQueue)
    {
        VmSockPosixFreeEventQueue(pQueue);
    }

    goto cleanup;
}

DWORD
VmSockPosixEventQueueAdd(
    PVM_SOCK_EVENT_QUEUE             pQueue,
    PVM_SOCKET                       pSocket
    )
{
    DWORD                            dwError = REST_ENGINE_SUCCESS;
    BOOLEAN                          bLocked = TRUE;

    if (!pQueue || !pSocket)
    {
        VMREST_LOG_ERROR("Invalid params");
        dwError = ERROR_INVALID_PARAMETER;
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
    }

    dwError = VmRESTLockMutex(pQueue->pMutex);
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    bLocked = TRUE;

    dwError = VmSockPosixEventQueueAdd_inlock(
                  pQueue,
                  pSocket
                  );
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

cleanup:

    if (bLocked)
    {
        VmRESTUnlockMutex(pQueue->pMutex);
    }

    return dwError;

error:

    goto cleanup;
}

DWORD
VmSockPosixWaitForEvent(
    PVM_SOCK_EVENT_QUEUE             pQueue,
    int                              iTimeoutMS,
    PVM_SOCKET*                      ppSocket,
    PVM_SOCK_EVENT_TYPE              pEventType,
    PVM_SOCK_IO_BUFFER*              ppIoBuffer
    )
{
    DWORD                            dwError = REST_ENGINE_SUCCESS;
    BOOLEAN                          bLocked = FALSE;
    VM_SOCK_EVENT_TYPE               eventType = VM_SOCK_EVENT_TYPE_UNKNOWN;
    PVM_SOCKET                       pSocket = NULL;

    if (!pQueue || !ppSocket || !pEventType)
    {
        VMREST_LOG_ERROR("Invalid params");
        dwError = ERROR_INVALID_PARAMETER;
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
    }

    dwError = VmRESTLockMutex(pQueue->pMutex);
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    bLocked = TRUE;

    if ((pQueue->state == VM_SOCK_POSIX_EVENT_STATE_PROCESS) &&
        (pQueue->iReady >= pQueue->nReady))
    {
        pQueue->state = VM_SOCK_POSIX_EVENT_STATE_WAIT;
    }

    if (pQueue->state == VM_SOCK_POSIX_EVENT_STATE_WAIT)
    {
        pQueue->iReady = 0;
        pQueue->nReady = -1;

        while (pQueue->nReady < 0)
        {
            pQueue->nReady = epoll_wait(
                                 pQueue->epollFd,
                                 pQueue->pEventArray,
                                 pQueue->dwSize,
                                 iTimeoutMS
                                 );
            if ((pQueue->nReady < 0) && (errno != EINTR))
            {
                VMREST_LOG_ERROR("epoll_wait() failed with Error code %d", errno);
                dwError = VM_SOCK_POSIX_ERROR_SYS_CALL_FAILED;
                BAIL_ON_POSIX_SOCK_ERROR(dwError);
            }
        }
        pQueue->state = VM_SOCK_POSIX_EVENT_STATE_PROCESS;
    }

    if (pQueue->state == VM_SOCK_POSIX_EVENT_STATE_PROCESS)
    {
        if (pQueue->iReady < pQueue->nReady)
        {
            struct epoll_event* pEvent = &pQueue->pEventArray[pQueue->iReady];
            PVM_SOCKET pEventSocket = (PVM_SOCKET)pEvent->data.ptr;

            if (!pEventSocket)
            {
                VMREST_LOG_ERROR("Bad socket information");
                dwError = ERROR_INVALID_STATE;
                BAIL_ON_POSIX_SOCK_ERROR(dwError);
            }

            if (pEvent->events & (EPOLLERR | EPOLLHUP))
            {
                eventType = VM_SOCK_EVENT_TYPE_CONNECTION_CLOSED;

                pSocket = pEventSocket;
            }
            else if (pEventSocket->type == VM_SOCK_TYPE_LISTENER)
            {
                switch (pEventSocket->protocol)
                {
                    case VM_SOCK_PROTOCOL_TCP:

                        dwError = VmSockPosixAcceptConnection(
                                      pEventSocket,
                                      &pSocket);
                        BAIL_ON_POSIX_SOCK_ERROR(dwError);

                        dwError = VmSockPosixSetNonBlocking(pSocket);
                        BAIL_ON_POSIX_SOCK_ERROR(dwError);

                        dwError = VmSockPosixEventQueueAdd_inlock(
                                      pQueue,
                                      pSocket);
                        BAIL_ON_POSIX_SOCK_ERROR(dwError);

                        eventType = VM_SOCK_EVENT_TYPE_NEW_CONNECTION;

                        break;

                    case VM_SOCK_PROTOCOL_UDP:

                        pSocket = VmSockPosixAcquireSocket(pEventSocket);
                        eventType = VM_SOCK_EVENT_TYPE_DATA_AVAILABLE;

                        break;

                    default:

                        dwError = ERROR_INVALID_STATE;
                        BAIL_ON_POSIX_SOCK_ERROR(dwError);

                        break;
                }
            }
            else if (pEventSocket->type == VM_SOCK_TYPE_SIGNAL)
            {
                pSocket = VmSockPosixAcquireSocket(pEventSocket);
                eventType = VM_SOCK_EVENT_TYPE_DATA_AVAILABLE;
            }
            else
            {
                pSocket = VmSockPosixAcquireSocket(pEventSocket);
                eventType = VM_SOCK_EVENT_TYPE_DATA_AVAILABLE;
            }
        }
        pQueue->iReady++;
    }

    *ppSocket = pSocket;
    *pEventType = eventType;
    *ppIoBuffer = (PVM_SOCK_IO_BUFFER)pSocket->pData;

cleanup:

    if (bLocked)
    {
        VmRESTUnlockMutex(pQueue->pMutex);
    }

    if (ppIoBuffer)
    {
        *ppIoBuffer = NULL;
    }

    return dwError;

error:

    if (ppSocket)
    {
        *ppSocket = NULL;
    }
    if (pEventType)
    {
        *pEventType = VM_SOCK_EVENT_TYPE_UNKNOWN;
    }
    if (pSocket)
    {
        VmSockPosixReleaseSocket(pSocket);
    }

    goto cleanup;
}

VOID
VmSockPosixCloseEventQueue(
    PVM_SOCK_EVENT_QUEUE             pQueue
    )
{
    if (pQueue)
    {
        if (pQueue->pSignalWriter)
        {
            char szBuf[] = {0};
            write(pQueue->pSignalWriter->fd, szBuf, sizeof(szBuf));
        }
        VmSockPosixFreeEventQueue(pQueue);
    }
}

DWORD
VmSockPosixSetNonBlocking(
    PVM_SOCKET                       pSocket
    )
{
    DWORD                            dwError = REST_ENGINE_SUCCESS;
    BOOLEAN                          bLocked = FALSE;

    dwError = VmRESTLockMutex(pSocket->pMutex);
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    bLocked = TRUE;

    dwError = VmSockPosixSetDescriptorNonBlocking(pSocket->fd);
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

cleanup:

    if (bLocked)
    {
        VmRESTUnlockMutex(pSocket->pMutex);
    }

    return dwError;

error:

    goto cleanup;
}

DWORD
VmSockPosixGetProtocol(
    PVM_SOCKET                       pSocket,
    PDWORD                           pdwProtocol
    )
{
    DWORD                            dwError = REST_ENGINE_SUCCESS;
    BOOLEAN                          bLocked = FALSE;
    DWORD                            dwProtocol = 0;

    if (!pSocket || !pdwProtocol)
    {
        VMREST_LOG_ERROR("Invalid params");
        dwError = ERROR_INVALID_PARAMETER;
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
    }

    dwError = VmRESTLockMutex(pSocket->pMutex);
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    bLocked = TRUE;

    switch (pSocket->protocol)
    {
        case VM_SOCK_PROTOCOL_UDP:

            dwProtocol = SOCK_DGRAM;

            break;

        case VM_SOCK_PROTOCOL_TCP:

            dwProtocol = SOCK_STREAM;

            break;

        default:

            dwError = ERROR_INTERNAL_ERROR;
            BAIL_ON_POSIX_SOCK_ERROR(dwError);

            break;
    }

    *pdwProtocol = dwProtocol;

cleanup:

    if (bLocked)
    {
        VmRESTUnlockMutex(pSocket->pMutex);
    }

    return dwError;

error:

    if (pdwProtocol)
    {
        *pdwProtocol = 0;
    }

    goto cleanup;
}

DWORD
VmSockPosixSetData(
    PVM_SOCKET                       pSocket,
    PVOID                            pData,
    PVOID*                           ppOldData
    )
{
    DWORD                            dwError = REST_ENGINE_SUCCESS;
    BOOLEAN                          bLocked = FALSE;
    PVOID                            pOldData = NULL;

    if (!pSocket)
    {
        VMREST_LOG_ERROR("Invalid params");
        dwError = ERROR_INVALID_PARAMETER;
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
    }

    dwError = VmRESTLockMutex(pSocket->pMutex);
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    bLocked = TRUE;

    pOldData = pSocket->pData;

    pSocket->pData = pData;

    if (ppOldData)
    {
        *ppOldData = pOldData;
    }

cleanup:

    if (bLocked)
    {
        VmRESTUnlockMutex(pSocket->pMutex);
    }

    return dwError;

error:

    if (ppOldData)
    {
        *ppOldData = NULL;
    }

    goto cleanup;
}

DWORD
VmSockPosixGetData(
    PVM_SOCKET                       pSocket,
    PVOID*                           ppData
    )
{
    DWORD                            dwError = REST_ENGINE_SUCCESS;
    BOOLEAN                          bLocked = FALSE;

    if (!pSocket || !ppData)
    {
        VMREST_LOG_ERROR("Invalid params");
        dwError = ERROR_INVALID_PARAMETER;
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
    }

    dwError = VmRESTLockMutex(pSocket->pMutex);
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    bLocked = TRUE;

    *ppData = pSocket->pData;

cleanup:

    if (bLocked)
    {
        VmRESTUnlockMutex(pSocket->pMutex);
    }

    return dwError;

error:

    if (ppData)
    {
        *ppData = NULL;
    }

    goto cleanup;
}

DWORD
VmSockPosixRead(
    PVM_SOCKET                       pSocket,
    PVM_SOCK_IO_BUFFER               pIoBuffer
    )
{
    DWORD                            dwError = REST_ENGINE_SUCCESS;
    BOOLEAN                          bLocked = FALSE;
    int                              flags   = 0;
    ssize_t                          nRead   = 0;
    DWORD                            dwBufSize = 0;

    if (!pSocket || !pIoBuffer || !pIoBuffer->pData)
    {
        VMREST_LOG_ERROR("Invalid params");
        dwError = ERROR_INVALID_PARAMETER;
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
    }

    if (pIoBuffer->dwExpectedSize < pIoBuffer->dwCurrentSize)
    {
        dwError = ERROR_INVALID_PARAMETER;
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
    }

    dwBufSize = pIoBuffer->dwExpectedSize - pIoBuffer->dwCurrentSize;
    pIoBuffer->addrLen = sizeof pIoBuffer->clientAddr;

    dwError = VmRESTLockMutex(pSocket->pMutex);
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    bLocked = TRUE;

    nRead = recvfrom(
                pSocket->fd,
                pIoBuffer->pData + pIoBuffer->dwCurrentSize,
                dwBufSize,
                flags,
                (struct sockaddr*)&pIoBuffer->clientAddr,
                &pIoBuffer->addrLen);
    VMREST_LOG_DEBUG("\nRead Status on Socket with fd = %d\nRequested: %d bytes\nRead %d bytes\n", pSocket->fd, dwBufSize, nRead);

    if (nRead < 0)
    {
        VMREST_LOG_ERROR("recvfrom() returned Error code %d", errno);
        dwError = VM_SOCK_POSIX_ERROR_SYS_CALL_FAILED;
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
    }

    pIoBuffer->dwCurrentSize += nRead;
    pIoBuffer->dwBytesTransferred = nRead;

cleanup:

    if (bLocked)
    {
        VmRESTUnlockMutex(pSocket->pMutex);
    }

    return dwError;

error:

    goto cleanup;
}

DWORD
VmSockPosixWrite(
    PVM_SOCKET                       pSocket,
    const struct sockaddr*           pClientAddress,
    socklen_t                        addrLength,
    PVM_SOCK_IO_BUFFER               pIoBuffer
    )
{
    DWORD                            dwError = REST_ENGINE_SUCCESS;
    BOOLEAN                          bLocked  = FALSE;
    int                              flags    = 0;
    ssize_t                          nWritten = 0;
    DWORD                            dwBytesToWrite = 0;
    const struct sockaddr*           pClientAddressLocal = NULL;
    socklen_t                        addrLengthLocal = 0;
    uint32_t                         bytes = 0;
    uint32_t                         bytesLeft = 0;
    uint32_t                         bytesWritten = 0;

    if (!pSocket || !pIoBuffer || !pIoBuffer->pData)
    {
        VMREST_LOG_ERROR("Invalid params");
        dwError = ERROR_INVALID_PARAMETER;
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
    }

    /**** TODO: Fix this. Call this api in loop rather loop inside this function
     dwBytesToWrite = pIoBuffer->dwExpectedSize - pIoBuffer->dwCurrentSize; 
    ****/

    dwBytesToWrite = pIoBuffer->dwExpectedSize;

    bytes = dwBytesToWrite;
    bytesLeft = bytes;

    switch (pSocket->protocol)
    {
        case VM_SOCK_PROTOCOL_TCP:

            pClientAddressLocal = &pSocket->addr;
            addrLengthLocal     = pSocket->addrLen;

            break;

        case VM_SOCK_PROTOCOL_UDP:

            if (!pClientAddress || addrLength <= 0)
            {
                dwError = ERROR_INVALID_PARAMETER;
                BAIL_ON_VMREST_ERROR(dwError);
            }

            memcpy(
                &pIoBuffer->clientAddr,
                pClientAddress,
                addrLength);

            pClientAddressLocal = pClientAddress;
            addrLengthLocal = addrLength;

            break;

        default:

            dwError = ERROR_NOT_SUPPORTED;
            BAIL_ON_POSIX_SOCK_ERROR(dwError);

            break;
    }

    dwError = VmRESTLockMutex(pSocket->pMutex);
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    bLocked = TRUE;

    while(bytesWritten < bytes )
    {
         nWritten = sendto(
                        pSocket->fd,
                        (pIoBuffer->pData + bytesWritten),
                        bytesLeft,
                        flags,
                        pClientAddressLocal,
                        addrLengthLocal);

         if (nWritten >= 0)
         {
             bytesWritten += nWritten;
             bytesLeft -= nWritten;
             VMREST_LOG_DEBUG("\nBytes written this write %d, Total bytes written %u", nWritten, bytesWritten);
             nWritten = 0;
         }
         else
         {
             if (errno == 11)
             {
                 VMREST_LOG_DEBUG("retry write");
                 usleep(1000);
                 nWritten = 0;
                 continue;
             }
             VMREST_LOG_ERROR("Write failed with errorno %d", errno);
             dwError = VM_SOCK_POSIX_ERROR_SYS_CALL_FAILED;
             BAIL_ON_VMREST_ERROR(dwError);
        }

    }
    VMREST_LOG_DEBUG("\nWrite Status on Socket with fd = %d\nRequested: %d bytes\nWritten %d bytes\n", pSocket->fd, bytes, bytesWritten);

    pIoBuffer->dwCurrentSize += nWritten;
    pIoBuffer->dwBytesTransferred = nWritten;

cleanup:

    if (bLocked)
    {
        VmRESTUnlockMutex(pSocket->pMutex);
    }

    return dwError;

error:

    goto cleanup;
}

PVM_SOCKET
VmSockPosixAcquireSocket(
    PVM_SOCKET                       pSocket
    )
{
    if (pSocket)
    {
        /**** TODO: Fix this. Atomic increment.
        InterlockedIncrement(&pSocket->refCount);
        ****/
        pSocket->refCount++;
    }

    return pSocket;
}

VOID
VmSockPosixReleaseSocket(
    PVM_SOCKET                       pSocket
    )
{
    if (pSocket)
    {
        /**** TODO: Fix this. Atomic decrement.
         if (InterlockedDecrement(&pSocket->refCount) == 0)
        ****/
        if (((pSocket->refCount)--) == 0)
        {
            VmSockPosixFreeSocket(pSocket);
        }
    }
}

DWORD
VmSockPosixCloseSocket(
    PVM_SOCKET                       pSocket
    )
{
    DWORD                            dwError = REST_ENGINE_SUCCESS;
    BOOLEAN                          bLocked = FALSE;

    dwError = VmRESTLockMutex(pSocket->pMutex);
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    bLocked = TRUE;

    if (pSocket->fd >= 0)
    {
        close(pSocket->fd);
        pSocket->fd = -1;
    }

cleanup:

    if (bLocked)
    {
        VmRESTUnlockMutex(pSocket->pMutex);
    }

    return dwError;

error:

    goto cleanup;
}

static
DWORD
VmSockPosixCreateSignalSockets(
    PVM_SOCKET*                      ppReaderSocket,
    PVM_SOCKET*                      ppWriterSocket
    )
{
    DWORD                            dwError = REST_ENGINE_SUCCESS;
    PVM_SOCKET                       pReaderSocket = NULL;
    PVM_SOCKET                       pWriterSocket = NULL;
    PVM_SOCKET*                      sockets[] = { &pReaderSocket, &pWriterSocket };
    int                              fdPair[] = { -1, -1 };
    DWORD                            iSock = 0;

    if (socketpair(AF_LOCAL, SOCK_STREAM, 0, fdPair) < 0)
    {
        VMREST_LOG_ERROR("socketpair() failed with errbi %d", errno);
        dwError = VM_SOCK_POSIX_ERROR_SYS_CALL_FAILED;
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
    }

    for (; iSock < sizeof(sockets)/sizeof(sockets[0]); iSock++)
    {
        PVM_SOCKET pSocket = NULL;

        dwError = VmRESTAllocateMemory(
                      sizeof(VM_SOCKET),
                      (PVOID*)sockets[iSock]
                      );
        BAIL_ON_POSIX_SOCK_ERROR(dwError);

        pSocket = *sockets[iSock];

        pSocket->refCount = 1;

        dwError = VmRESTAllocateMutex(&pSocket->pMutex);
        BAIL_ON_POSIX_SOCK_ERROR(dwError);

        pSocket->protocol = VM_SOCK_PROTOCOL_TCP;
        pSocket->type = VM_SOCK_TYPE_SIGNAL;
        pSocket->fd = fdPair[iSock];

        fdPair[iSock] = -1;
    }

    *ppReaderSocket = pReaderSocket;
    *ppWriterSocket = pWriterSocket;

cleanup:

    return dwError;

error:

    *ppReaderSocket = NULL;
    *ppWriterSocket = NULL;

    if (pReaderSocket)
    {
        VmSockPosixFreeSocket(pReaderSocket);
    }
    if (pWriterSocket)
    {
        VmSockPosixFreeSocket(pWriterSocket);
    }
    for (iSock = 0; iSock < sizeof(fdPair)/sizeof(fdPair[0]); iSock++)
    {
        if (fdPair[iSock] >= 0)
        {
            close(fdPair[iSock]);
        }
    }

    goto cleanup;
}

static
DWORD
VmSockPosixEventQueueAdd_inlock(
    PVM_SOCK_EVENT_QUEUE             pQueue,
    PVM_SOCKET                       pSocket
    )
{
    DWORD                            dwError = REST_ENGINE_SUCCESS;
    struct                           epoll_event event = {0};

    event.data.ptr = pSocket;
    event.events = EPOLLIN;

    if (epoll_ctl(pQueue->epollFd, EPOLL_CTL_ADD, pSocket->fd, &event) < 0)
    {
        VMREST_LOG_ERROR("epoll_ctl() failed with Error code %d", errno);
        dwError = VM_SOCK_POSIX_ERROR_SYS_CALL_FAILED;
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
    }

    VmSockPosixAcquireSocket(pSocket);

error:

    return dwError;
}

static
DWORD
VmSockPosixAcceptConnection(
    PVM_SOCKET                       pListener,
    PVM_SOCKET*                      ppSocket
    )
{
    DWORD                            dwError = REST_ENGINE_SUCCESS;
    PVM_SOCKET                       pSocket = NULL;
    int                              fd = -1;
    PVM_STREAM_BUFFER                pStrmBuf = NULL;

    dwError = VmRESTAllocateMemory(
                  sizeof(*pSocket),
                  (PVOID*)&pSocket
                  );
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    dwError = VmRESTAllocateMemory(
                  sizeof(VM_STREAM_BUFFER),
                  (void**)&pStrmBuf
                  );
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    pStrmBuf->dataProcessed = 0;
    pStrmBuf->dataRead = 0;
    memset(pStrmBuf->pData, '\0', 4096);

    pSocket->pStreamBuffer = pStrmBuf;

    pSocket->refCount = 1;

    dwError = VmRESTAllocateMutex(&pSocket->pMutex);
    BAIL_ON_POSIX_SOCK_ERROR(dwError);

    pSocket->protocol = pListener->protocol;
    pSocket->type = VM_SOCK_TYPE_SERVER;

    fd = accept(pListener->fd, &pSocket->addr, &pSocket->addrLen);
    if (fd < 0)
    {
        VMREST_LOG_ERROR("Accept() failed with Error Code %d", errno);
        dwError = VM_SOCK_POSIX_ERROR_SYS_CALL_FAILED;
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
    }

    VMREST_LOG_DEBUG("Accepted connection with fd %d", fd);
    pSocket->fd = fd;

    pSocket->pAddr = &pSocket->addr;

    *ppSocket = pSocket;

cleanup:

    return dwError;

error:

    *ppSocket = NULL;

    if (pSocket)
    {
        VmSockPosixFreeSocket(pSocket);
    }
    if (fd >= 0)
    {
        close(fd);
    }

    goto cleanup;
}

static
DWORD
VmSockPosixSetDescriptorNonBlocking(
    int                              fd
    )
{
    DWORD                            dwError = REST_ENGINE_SUCCESS;
    int                              flags = 0;

    if ((flags = fcntl(fd, F_GETFL, 0)) < 0)
    {
        VMREST_LOG_ERROR("fcntl() GET failed with Error Code %d", errno);
        dwError = VM_SOCK_POSIX_ERROR_SYS_CALL_FAILED;
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
    }

    flags |= O_NONBLOCK;

    if ((flags = fcntl(fd, F_SETFL, flags)) < 0)
    {
        VMREST_LOG_ERROR("fcntl() SET failed with Error Code %d", errno);
        dwError = VM_SOCK_POSIX_ERROR_SYS_CALL_FAILED;
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
    }

error:

    return dwError;
}

static
DWORD
VmSockPosixSetReuseAddress(
    int                              fd
    )
{
    DWORD                            dwError = REST_ENGINE_SUCCESS;
    int                              on = 1;

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
    {
        VMREST_LOG_ERROR("setsoctopt() failed with Error Code %d", errno);
        dwError = VM_SOCK_POSIX_ERROR_SYS_CALL_FAILED;
        BAIL_ON_POSIX_SOCK_ERROR(dwError);
    }

error:

    return dwError;
}

static
VOID
VmSockPosixFreeEventQueue(
    PVM_SOCK_EVENT_QUEUE             pQueue
    )
{
    if (pQueue->pSignalReader)
    {
        VmSockPosixReleaseSocket(pQueue->pSignalReader);
    }
    if (pQueue->pSignalWriter)
    {
        VmSockPosixReleaseSocket(pQueue->pSignalWriter);
    }
    if (pQueue->pMutex)
    {
        VmRESTFreeMutex(pQueue->pMutex);
    }
    if (pQueue->epollFd >= 0)
    {
        close(pQueue->epollFd);
    }
    if (pQueue->pEventArray)
    {
        VmRESTFreeMemory(pQueue->pEventArray);
    }
    VmRESTFreeMemory(pQueue);
}

static
VOID
VmSockPosixFreeSocket(
    PVM_SOCKET                       pSocket
    )
{
    if (pSocket->fd >= 0)
    {
        close(pSocket->fd);
    }
    if (pSocket->pMutex)
    {
        VmRESTFreeMutex(pSocket->pMutex);
    }
    if (pSocket->pStreamBuffer)
    {
        VmRESTFreeMemory(pSocket->pStreamBuffer);
    }
    VmRESTFreeMemory(pSocket);
}

DWORD
VmSockPosixStartListening(
    PVM_SOCKET                       pSocket,
    int                              iListenQueueSize
    )
{
    return ERROR_NOT_SUPPORTED;
}

DWORD
VmSockPosixAllocateIoBuffer(
    VM_SOCK_EVENT_TYPE               eventType,
    DWORD                            dwSize,
    PVM_SOCK_IO_BUFFER*              ppIoBuffer
    )
{
    DWORD                            dwError = REST_ENGINE_SUCCESS;
    PVM_SOCK_IO_CONTEXT              pIoContext = NULL;

    if (!ppIoBuffer)
    {
        VMREST_LOG_ERROR("Invalid params");
        dwError = ERROR_INVALID_PARAMETER;
        BAIL_ON_VMREST_ERROR(dwError);
    }

    dwError = VmRESTAllocateMemory(
                    sizeof(VM_SOCK_IO_CONTEXT) + dwSize,
                    (PVOID*)&pIoContext);
    BAIL_ON_VMREST_ERROR(dwError);

    pIoContext->eventType = eventType;
    pIoContext->IoBuffer.dwExpectedSize = dwSize;
    pIoContext->IoBuffer.pData = pIoContext->DataBuffer;

    *ppIoBuffer = &pIoContext->IoBuffer;

cleanup:

    return dwError;

error:

    if (pIoContext)
    {
        VmSockPosixFreeIoBuffer(&pIoContext->IoBuffer);
    }

    goto cleanup;
}

VOID
VmSockPosixFreeIoBuffer(
    PVM_SOCK_IO_BUFFER               pIoBuffer
    )
{
    PVM_SOCK_IO_CONTEXT              pIoContext = CONTAINING_RECORD(pIoBuffer, VM_SOCK_IO_CONTEXT, IoBuffer);
    
    if (pIoContext)
    {
        VmRESTFreeMemory(pIoContext);
        pIoContext = NULL;
    }
}

/**
 * @brief  VmwGetClientAddreess
 *
 * @param[in] pSocket 
 * @param[in] pAddress 
 * @param[in] addresLen 
 *
 * @return DWORD - 0 on success
 */
DWORD
VmSockPosixGetAddress(
    PVM_SOCKET                       pSocket,
    struct sockaddr_storage*         pAddress,
    socklen_t*                       pAddresLen
    )
{
    DWORD dwError = ERROR_SUCCESS;

    if (!pSocket ||
        !pAddresLen ||
        !pAddress)
    {
        dwError = ERROR_SUCCESS;
        BAIL_ON_VMREST_ERROR(dwError);
    }

    memcpy(pAddress, &pSocket->addr, pSocket->addrLen);

cleanup:

    return dwError;

error :

    goto cleanup;

}

VOID
VmSockPosixGetStreamBuffer(
    PVM_SOCKET                       pSocket,
    PVM_STREAM_BUFFER*               ppStreamBuffer
    )
{
    if (pSocket->pStreamBuffer)
    {
        *ppStreamBuffer = pSocket->pStreamBuffer;
    }
    else
    {
        *ppStreamBuffer = NULL;
    }
}

VOID
VmSockPosixSetStreamBuffer(
    PVM_SOCKET                       pSocket,
    PVM_STREAM_BUFFER                pStreamBuffer
    )
{
    if (pStreamBuffer)
    {
        pSocket->pStreamBuffer = pStreamBuffer;
    }
    else
    {
        pSocket->pStreamBuffer = NULL;
    }
}
