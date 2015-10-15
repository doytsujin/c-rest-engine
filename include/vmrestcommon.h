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

#ifndef __VMRESTCOMMON_H__
#define __VMRESTCOMMON_H__

#ifdef __cplusplus
extern "C" {
#endif

/*
 * @brief Allocation of heap memory for rest engine.
 *
 * @param[in]           size of memory to be allocated
 * @param[out]          pointer to allocated memory
 * @return Returns 0 for success
 */
uint32_t
VmRESTAllocateMemory(
    size_t   dwSize,
    void**   ppMemory
    );

/*
 * @brief Free of head memory for rest engine.
 *
 * @param[in]           pointer to allocated memory
 * @return Returns 0 for success
 */

void
VmRESTFreeMemory(
    void*  pMemory
    );

#ifdef __cplusplus
}
#endif

#endif /*  __VMRESTCOMMON_H__ */
