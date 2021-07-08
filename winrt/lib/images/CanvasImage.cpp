// Copyright (c) Microsoft Corporation. All rights reserved.
//
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.

#include "pch.h"

namespace ABI { namespace Microsoft { namespace Graphics { namespace Canvas
{
    using namespace ABI::Windows::Foundation;

    DeviceContextLease GetDeviceContextForGetBounds(ICanvasDevice* device, ICanvasResourceCreator* resourceCreator)
    {
        if (auto drawingSession = MaybeAs<ICanvasDrawingSession>(resourceCreator))
        {
            // A CanvasDrawingSession is already wrapping a device context that
            // may have interesting state set on it (ie unit mode and dpi) so we
            // return that.
            ComPtr<ID2D1DeviceContext1> deviceContext;
            ThrowIfFailed(As<ICanvasResourceWrapperNative>(drawingSession)->GetNativeResource(nullptr, 0, IID_PPV_ARGS(&deviceContext)));
            return DeviceContextLease(std::move(deviceContext));
        }

        return As<ICanvasDeviceInternal>(device)->GetResourceCreationDeviceContext();
    }
    
    static Rect GetImageBoundsImpl(
        ICanvasImageInternal* imageInternal,
        ICanvasResourceCreator* resourceCreator,
        Numerics::Matrix3x2 const* transform)
    {
        ComPtr<ICanvasDevice> device;
        ThrowIfFailed(resourceCreator->get_Device(&device));

        auto d2dDeviceContext = GetDeviceContextForGetBounds(device.Get(), resourceCreator);

        auto d2dImage = imageInternal->GetD2DImage(device.Get(), d2dDeviceContext.Get());

        D2D1_MATRIX_3X2_F previousTransform;
        d2dDeviceContext->GetTransform(&previousTransform);

        auto restoreTransformWarden = MakeScopeWarden([&] { d2dDeviceContext->SetTransform(previousTransform); });

        d2dDeviceContext->SetTransform(ReinterpretAs<D2D1_MATRIX_3X2_F const*>(transform));
        
        D2D1_RECT_F d2dBounds;
        ThrowIfFailed(d2dDeviceContext->GetImageWorldBounds(d2dImage.Get(), &d2dBounds));

        return FromD2DRect(d2dBounds);
    }

    HRESULT GetImageBoundsImpl(
        ICanvasImageInternal* imageInternal,
        ICanvasResourceCreator* resourceCreator,
        Numerics::Matrix3x2 const* transform,
        Rect* bounds)
    {
        if (!transform)
            transform = &Identity3x2();

        return ExceptionBoundary(
            [&]
            {
                CheckInPointer(resourceCreator);
                CheckInPointer(bounds);

                *bounds = GetImageBoundsImpl(imageInternal, resourceCreator, transform);
            });
    }


    //
    // CanvasImageFactory
    //
    

    ActivatableStaticOnlyFactory(CanvasImageFactory);

    
    CanvasImageFactory::CanvasImageFactory()
        : m_adapter(CanvasImageAdapter::GetInstance())
    {
    }

    
    IFACEMETHODIMP CanvasImageFactory::SaveAsync(
        ICanvasImage* image,
        Rect sourceRectangle,
        float dpi,
        ICanvasResourceCreator* resourceCreator,
        IRandomAccessStream* stream,
        CanvasBitmapFileFormat fileFormat,
        IAsyncAction** action)
    {
        return SaveWithQualityAsync(
            image,
            sourceRectangle,
            dpi,
            resourceCreator,
            stream,
            fileFormat,
            DEFAULT_CANVASBITMAP_QUALITY,
            action);
    }
    

    IFACEMETHODIMP CanvasImageFactory::SaveWithQualityAsync(
        ICanvasImage* image,
        Rect sourceRectangle,
        float dpi,
        ICanvasResourceCreator* resourceCreator,
        IRandomAccessStream* stream,
        CanvasBitmapFileFormat fileFormat,
        float quality,
        IAsyncAction** action)
    {
        return SaveWithQualityAndBufferPrecisionAsync(
            image,
            sourceRectangle,
            dpi,
            resourceCreator,
            stream,
            fileFormat,
            quality,
            CanvasBufferPrecision::Precision8UIntNormalized,
            action);
    }


    static DXGI_FORMAT GetFormat(CanvasBufferPrecision precision)
    {
        switch (precision)
        {
        case CanvasBufferPrecision::Precision8UIntNormalized: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case CanvasBufferPrecision::Precision8UIntNormalizedSrgb: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case CanvasBufferPrecision::Precision16UIntNormalized: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case CanvasBufferPrecision::Precision16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case CanvasBufferPrecision::Precision32Float: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default:
            ThrowHR(E_INVALIDARG);
        }
    }
    

    static GUID const& DxgiFormatToWic(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R16G16B16A16_UNORM:
            return GUID_WICPixelFormat64bppRGBA;
        
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return GUID_WICPixelFormat64bppRGBAHalf;
        
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return GUID_WICPixelFormat128bppRGBAFloat;

        default:
            return GUID_WICPixelFormat32bppBGRA;
        }
    }


    IFACEMETHODIMP CanvasImageFactory::SaveWithQualityAndBufferPrecisionAsync(
        ICanvasImage* image,
        Rect sourceRectangle,
        float dpi,
        ICanvasResourceCreator* resourceCreator,
        IRandomAccessStream* stream,
        CanvasBitmapFileFormat fileFormat,
        float quality,
        CanvasBufferPrecision bufferPrecision,
        IAsyncAction** action)
    {
        return ExceptionBoundary(
            [&]
            {
                CheckInPointer(image);
                CheckInPointer(resourceCreator);
                CheckInPointer(stream);
                CheckAndClearOutPointer(action);

                if (fileFormat == CanvasBitmapFileFormat::Auto)
                    ThrowHR(E_INVALIDARG, Strings::AutoFileFormatNotAllowed);

                if (quality < 0.0f || quality > 1.0f)
                    ThrowHR(E_INVALIDARG);

                WICImageParameters wicImageParameters{};
                wicImageParameters.PixelFormat.format = GetFormat(bufferPrecision);
                wicImageParameters.PixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
                wicImageParameters.DpiX = dpi;
                wicImageParameters.DpiY = dpi;
                wicImageParameters.Top = sourceRectangle.Y * dpi / DEFAULT_DPI;
                wicImageParameters.Left = sourceRectangle.X * dpi / DEFAULT_DPI;
                wicImageParameters.PixelWidth = static_cast<uint32_t>(SizeDipsToPixels(sourceRectangle.Width, dpi));
                wicImageParameters.PixelHeight = static_cast<uint32_t>(SizeDipsToPixels(sourceRectangle.Height, dpi));

                auto canvasDevice = GetCanvasDevice(resourceCreator);
                auto d2dDevice = GetWrappedResource<ID2D1Device>(canvasDevice);

                auto d2dImage = As<ICanvasImageInternal>(image)->GetD2DImage(canvasDevice.Get(), nullptr, GetImageFlags::None, dpi);

                auto adapter = m_adapter;
                auto istream = adapter->CreateStreamOverRandomAccessStream(stream);

                auto newAction = adapter->RunAsync(
                    [=]
                    {
                        adapter->SaveImage(
                            d2dImage.Get(),
                            wicImageParameters,
                            d2dDevice.Get(),
                            istream.Get(),
                            GetGUIDForFileFormat(fileFormat),
                            quality);
                    });
                ThrowIfFailed(newAction.CopyTo(action));
            });
    }


    IFACEMETHODIMP CanvasImageFactory::ComputeHistogram(
        ICanvasImage* image,
        Rect sourceRectangle,
        ICanvasResourceCreator* resourceCreator,
        Effects::EffectChannelSelect channelSelect,
        int32_t numberOfBins,
        uint32_t* valueCount,
        float** valueElements)
    {
        return ExceptionBoundary(
            [&]
            {
                CheckInPointer(image);
                CheckInPointer(resourceCreator);
                CheckInPointer(valueCount);
                CheckAndClearOutPointer(valueElements);

                if (numberOfBins < 2 || numberOfBins > 1024)
                    ThrowHR(E_INVALIDARG);

                // Look up a device context.
                ComPtr<ICanvasDevice> device;
                ThrowIfFailed(resourceCreator->get_Device(&device));
                
                auto deviceInternal = As<ICanvasDeviceInternal>(device);

                auto deviceContext = deviceInternal->GetResourceCreationDeviceContext();
                
                // Look up the histogram and atlas effects.
                auto effects = deviceInternal->LeaseHistogramEffect(deviceContext.Get());

                auto releaseEffects = MakeScopeWarden(
                    [&]
                    {
                        effects.AtlasEffect->SetInput(0, nullptr);
                        deviceInternal->ReleaseHistogramEffect(std::move(effects));
                    });

                // Configure the atlas effect to select what region of the source image we want to feed into the histogram.
                float realizedDpi;

                auto d2dImage = As<ICanvasImageInternal>(image)->GetD2DImage(device.Get(), deviceContext.Get(), GetImageFlags::None, DEFAULT_DPI, &realizedDpi);

                if (realizedDpi != 0 && realizedDpi != DEFAULT_DPI)
                {
                    ThrowIfFailed(D2D1::SetDpiCompensatedEffectInput(deviceContext.Get(), effects.AtlasEffect.Get(), 0, As<ID2D1Bitmap>(d2dImage).Get()));
                }
                else
                {
                    effects.AtlasEffect->SetInput(0, d2dImage.Get());
                }

                effects.AtlasEffect->SetValue(D2D1_ATLAS_PROP_INPUT_RECT, ToD2DRect(sourceRectangle));

                // Configure the histogram effect.
                effects.HistogramEffect->SetInputEffect(0, effects.AtlasEffect.Get());

                effects.HistogramEffect->SetValue(D2D1_HISTOGRAM_PROP_CHANNEL_SELECT, channelSelect);
                effects.HistogramEffect->SetValue(D2D1_HISTOGRAM_PROP_NUM_BINS, numberOfBins);

                // Evaluate the histogram by drawing the effect.
                deviceContext->BeginDraw();

                deviceContext->DrawImage(As<ID2D1Image>(effects.HistogramEffect).Get());

                ThrowIfFailed(deviceContext->EndDraw());

                // Read back the results.
                ComArray<float> array(numberOfBins);

                ThrowIfFailed(effects.HistogramEffect->GetValue(D2D1_HISTOGRAM_PROP_HISTOGRAM_OUTPUT,
                                                                reinterpret_cast<BYTE*>(array.GetData()),
                                                                array.GetSize() * sizeof(float)));

                array.Detach(valueCount, valueElements);
            });
    }


    IFACEMETHODIMP CanvasImageFactory::IsHistogramSupported(
        ICanvasDevice* device,
        boolean* result)
    {
        return ExceptionBoundary(
            [&]
            {
                CheckInPointer(device);
                CheckInPointer(result);

                ComPtr<ID3D11Device> d3dDevice;
                ThrowIfFailed(As<IDirect3DDxgiInterfaceAccess>(device)->GetInterface(IID_PPV_ARGS(&d3dDevice)));

                bool isSupported = false;

                if (d3dDevice->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0)
                {
                    // Feature level 11 and above always supports compute shaders.
                    isSupported = true;
                }
                else
                {
                    // On feature level 10, compute shaders are optional.
                    D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS features = { 0 };
                    
                    if (SUCCEEDED(d3dDevice->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &features, sizeof(features))))
                    {
                        isSupported = !!features.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x;
                    }
                }

                *result = isSupported;
            });
    }


    ComPtr<IAsyncAction> DefaultCanvasImageAdapter::RunAsync(
        std::function<void()>&& fn)
    {
        auto action = Make<AsyncAction>(std::move(fn));
        CheckMakeResult(action);
        return action;
    }


    ComPtr<IStream> DefaultCanvasImageAdapter::CreateStreamOverRandomAccessStream(
        IRandomAccessStream* stream)
    {
        ComPtr<IStream> istream;
        ThrowIfFailed(::CreateStreamOverRandomAccessStream(stream, IID_PPV_ARGS(&istream)));
        return istream;
    }

    
    void DefaultCanvasImageAdapter::SaveImage(
        ID2D1Image* image,
        WICImageParameters const& parameters,
        ID2D1Device* device,
        IStream* stream,
        GUID const& containerFormat,
        float quality)
    {        
        auto factory = GetFactory();

        ComPtr<IWICBitmapEncoder> encoder;
        ThrowIfFailed(factory->CreateEncoder(containerFormat, nullptr, &encoder));
        ThrowIfFailed(encoder->Initialize(stream, WICBitmapEncoderNoCache));

        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> frameProperties;
        ThrowIfFailed(encoder->CreateNewFrame(&frame, &frameProperties));

        bool supportsQuality =
            containerFormat == GUID_ContainerFormatJpeg ||
            containerFormat == GUID_ContainerFormatWmp;

        if (supportsQuality)
        {
            PROPBAG2 option{};
            option.pstrName = L"ImageQuality";
            VARIANT value{};
            value.vt = VT_R4;
            value.fltVal = quality;
            ThrowIfFailed(frameProperties->Write(1, &option, &value));
        }

        ThrowIfFailed(frame->Initialize(frameProperties.Get()));

        // If the file format supports extended range (JpegXR) then tell WIC to encode
        // using the same pixel format that we are rasterizing the D2D image with.
        if (FileFormatSupportsHdr(containerFormat))
        {
            auto wicPixelFormat = DxgiFormatToWic(parameters.PixelFormat.format);
            ThrowIfFailed(frame->SetPixelFormat(&wicPixelFormat));
        }

        ComPtr<IWICImageEncoder> imageEncoder;
        ThrowIfFailed(factory->CreateImageEncoder(device, &imageEncoder));

        ThrowIfFailed(imageEncoder->WriteFrame(image, frame.Get(), &parameters));

        ThrowIfFailed(frame->Commit());
        ThrowIfFailed(encoder->Commit());
    }


    ComPtr<IWICImagingFactory2> const& DefaultCanvasImageAdapter::GetFactory()
    {
        std::call_once(m_initWicAdapterFlag, [this] {
            // Hold on to the wic adapter so it doesn't keep getting destroyed
            // and recreated.
            m_wicAdapter = WicAdapter::GetInstance();
        });

        return m_wicAdapter->GetFactory();
    }
       
        
}}}}
