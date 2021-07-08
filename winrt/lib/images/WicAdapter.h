// Copyright (c) Microsoft Corporation. All rights reserved.
//
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.

#pragma once

using namespace Microsoft::WRL;
using ABI::Microsoft::Graphics::Canvas::CanvasBitmapFileFormat;

class DefaultWicAdapter;

class WicAdapter : public Singleton<WicAdapter, DefaultWicAdapter>
{
public:
    virtual ~WicAdapter() = default;

    virtual ComPtr<IWICImagingFactory2> const& GetFactory() = 0;
};


class DefaultWicAdapter : public WicAdapter
{
    ComPtr<IWICImagingFactory2> m_wicFactory;
    std::once_flag m_initFactoryFlag;

public:
    DefaultWicAdapter()
    {
    }

    virtual ComPtr<IWICImagingFactory2> const& GetFactory() override
    {
        std::call_once(m_initFactoryFlag, [this] {
            ThrowIfFailed(
                CoCreateInstance(
                    CLSID_WICImagingFactory,
                    nullptr,
                    CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(&m_wicFactory)));
        });

        return m_wicFactory;
    }
};
