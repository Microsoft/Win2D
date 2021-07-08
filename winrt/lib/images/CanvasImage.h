// Copyright (c) Microsoft Corporation. All rights reserved.
//
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.

#pragma once

#include "WicAdapter.h"

class DeviceContextLease;

namespace ABI { namespace Microsoft { namespace Graphics { namespace Canvas
{
    using namespace ::Microsoft::WRL;
    using namespace ABI::Windows::Foundation;
    using namespace ABI::Windows::Storage::Streams;


    // Options for fine-tuning the behavior of ICanvasImageInternal::GetD2DImage.
    enum class GetImageFlags
    {
        None                        = 0,
        ReadDpiFromDeviceContext    = 1,    // Ignore the targetDpi parameter - read DPI from deviceContext instead
        AlwaysInsertDpiCompensation = 2,    // Ignore the targetDpi parameter - always insert DPI compensation
        NeverInsertDpiCompensation  = 4,    // Ignore the targetDpi parameter - never insert DPI compensation
        MinimalRealization          = 8,    // Do the bare minimum to get back an ID2D1Image - no validation or recursive realization
        AllowNullEffectInputs       = 16,   // Allow partially configured effect graphs where some inputs are null
        UnrealizeOnFailure          = 32,   // If an input is invalid, unrealize the effect and return null rather than throwing
    };

    DEFINE_ENUM_FLAG_OPERATORS(GetImageFlags)


    class __declspec(uuid("2F434224-053C-4978-87C4-CFAAFA2F4FAC"))
    ICanvasImageInternal : public IUnknown
    {
    public:
        // For bitmaps and command lists, GetD2DImage is a trivial getter.
        // For effects it triggers on-demand realization, which requires extra information.
        //
        // The device parameter is required, while deviceContext is optional (but recommended)
        // except when the ReadDpiFromDeviceContext flag is specified. This is because not all
        // callers of GetD2DImage have easy access to a context. It is always possible to get a
        // resource creation context from the device, but the context is only actually necessary
        // if a new effect realization needs to be created, so it is more efficient to have the
        // implementation do this lookup only if/when it turns out to be needed.
        //
        // targetDpi passes in the DPI of the target device context. This is used to
        // determine when a D2D1DpiCompensation effect needs to be inserted. Behavior of
        // this parameter can be overridden by the flag values ReadDpiFromDeviceContext,
        // AlwaysInsertDpiCompensation, or NeverInsertDpiCompensation.
        //
        // realizedDpi returns the DPI of a source bitmap, or zero if the image does not
        // have a fixed DPI. A D2D1DpiCompensation effect will be inserted if targetDpi
        // and realizedDpi are different (flags permitting).

        virtual ComPtr<ID2D1Image> GetD2DImage(
            ICanvasDevice* device,
            ID2D1DeviceContext* deviceContext,
            GetImageFlags flags = GetImageFlags::ReadDpiFromDeviceContext,
            float targetDpi = 0,
            float* realizedDpi = nullptr) = 0;
    };


    HRESULT GetImageBoundsImpl(
        ICanvasImageInternal* imageInternal,
        ICanvasResourceCreator* resourceCreator,
        Numerics::Matrix3x2 const* optionalTransform,
        Rect* bounds);

    DeviceContextLease GetDeviceContextForGetBounds(ICanvasDevice* device, ICanvasResourceCreator* resourceCreator);

    class DefaultCanvasImageAdapter;
    
    class CanvasImageAdapter : public Singleton<CanvasImageAdapter, DefaultCanvasImageAdapter>
    {
    public:
        virtual ~CanvasImageAdapter() = default;

        virtual ComPtr<IAsyncAction> RunAsync(std::function<void()>&& fn) = 0;

        virtual ComPtr<IStream> CreateStreamOverRandomAccessStream(IRandomAccessStream* stream) = 0;

        virtual void SaveImage(
            ID2D1Image* d2dImage,
            WICImageParameters const& wicImageParameters,
            ID2D1Device* device,
            IStream* stream,
            GUID const& containerFormat,
            float quality) = 0;
    };


    class DefaultCanvasImageAdapter : public CanvasImageAdapter
    {
        std::shared_ptr<WicAdapter> m_wicAdapter;
        std::once_flag m_initWicAdapterFlag;
        
    public:
        virtual ComPtr<IAsyncAction> RunAsync(std::function<void()>&& fn) override;
        
        virtual ComPtr<IStream> CreateStreamOverRandomAccessStream(IRandomAccessStream* stream) override;

        virtual void SaveImage(
            ID2D1Image* d2dImage,
            WICImageParameters const& wicImageParameters,
            ID2D1Device* device,
            IStream* stream,
            GUID const& containerFormat,
            float quality) override;

    private:
        ComPtr<IWICImagingFactory2> const& GetFactory();
    };


    class CanvasImageFactory
        : public AgileActivationFactory<ICanvasImageStatics>
        , private LifespanTracker<CanvasImageFactory>
    {
        InspectableClassStatic(RuntimeClass_Microsoft_Graphics_Canvas_CanvasImage, BaseTrust);

        std::shared_ptr<CanvasImageAdapter> m_adapter;
        
    public:
        CanvasImageFactory();
        
        IFACEMETHODIMP SaveAsync(
            ICanvasImage* image,
            Rect sourceRectangle,
            float dpi,
            ICanvasResourceCreator* resourceCreator,
            IRandomAccessStream* stream,
            CanvasBitmapFileFormat fileFormat,
            IAsyncAction** action) override;

        IFACEMETHODIMP SaveWithQualityAsync(
            ICanvasImage* image,
            Rect sourceRectangle,
            float dpi,
            ICanvasResourceCreator* resourceCreator,
            IRandomAccessStream* stream,
            CanvasBitmapFileFormat fileFormat,
            float quality,
            IAsyncAction** action) override;

        IFACEMETHODIMP SaveWithQualityAndBufferPrecisionAsync(
            ICanvasImage* image,
            Rect sourceRectangle,
            float dpi,
            ICanvasResourceCreator* resourceCreator,
            IRandomAccessStream* stream,
            CanvasBitmapFileFormat fileFormat,
            float quality,
            CanvasBufferPrecision bufferPrecision,
            IAsyncAction** action) override;

        IFACEMETHODIMP ComputeHistogram(
            ICanvasImage* image,
            Rect sourceRectangle,
            ICanvasResourceCreator* resourceCreator,
            Effects::EffectChannelSelect channelSelect,
            int32_t numberOfBins,
            uint32_t* valueCount,
            float** valueElements) override;

        IFACEMETHODIMP IsHistogramSupported(
            ICanvasDevice* device,
            boolean* result) override;
    };
}}}}
