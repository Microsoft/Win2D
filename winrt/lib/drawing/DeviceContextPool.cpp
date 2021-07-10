// Copyright (c) Microsoft Corporation. All rights reserved.
//
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.

#include "pch.h"

#include "DeviceContextPool.h"


//
// DeviceContextPool implementation
//


DeviceContextPool::DeviceContextPool(ID2D1Device1* d2dDevice)
    : m_d2dDevice(d2dDevice)
{
}


DeviceContextLease DeviceContextPool::TakeLease()
{
    Lock lock(m_mutex);

    if (!m_d2dDevice)
        ThrowHR(RO_E_CLOSED);
    
    if (m_deviceContexts.empty())
    {
        //Store a local copy of the non-null device pointer before unlocking.
        auto localDevice = m_d2dDevice;
        //Release lock before creating the device context to avoid potential deadlock scenarios.
        lock.unlock();
        ComPtr<ID2D1DeviceContext1> deviceContext;
        ThrowIfFailed(localDevice->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            &deviceContext));
        return DeviceContextLease(this, std::move(deviceContext));
    }
    else
    {
        DeviceContextLease newLease(this, std::move(m_deviceContexts.back()));
        m_deviceContexts.pop_back();
        return newLease;
    }
}


void DeviceContextPool::ReturnLease(ComPtr<ID2D1DeviceContext1>&& deviceContext)
{
    if (!deviceContext)
        return;
        
    Lock lock(m_mutex);

    //
    // If the pool has been closed we just discard the context
    //
    if (!m_d2dDevice)
        return;

    //
    // When a leased device context is returned it is added back to the pool,
    // unless the pool has reached its maximum size, in which the context is
    // destroyed.  This is to give the pool a chance to shrink back down to a
    // reasonable size if there is ever any large scale concurrency going on.
    //
    // Max pool size is picked from number of CPUs - reasoning being that you
    // should expect to be able to have that many threads running and reusing
    // contexts without recreating them.
    //
    static auto maxPoolSize = std::max(std::thread::hardware_concurrency(), 1U);

    if (m_deviceContexts.size() < maxPoolSize)
        m_deviceContexts.emplace_back(std::move(deviceContext));
}


void DeviceContextPool::Close()
{
    Lock lock(m_mutex);

    m_deviceContexts.clear();
    m_d2dDevice = nullptr;
}
