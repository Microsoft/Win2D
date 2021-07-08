// Copyright (c) Microsoft Corporation. All rights reserved.
//
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.

#include "pch.h"
#include <propkey.h>

namespace ABI { namespace Microsoft { namespace Graphics { namespace Canvas
{
    using namespace ABI::Windows::Storage::Streams;
    using namespace ABI::Windows::Storage;
    using namespace ::Microsoft::WRL::Wrappers;

#if WINVER > _WIN32_WINNT_WINBLUE
    using ABI::Windows::Graphics::Imaging::BitmapPixelFormat;
#endif

    static void VerifyWellFormedSubrectangle(D2D1_RECT_U subRectangle, D2D1_SIZE_U targetSize)
    {
        if (subRectangle.right <= subRectangle.left ||
            subRectangle.bottom <= subRectangle.top)
        {
            ThrowHR(E_INVALIDARG);
        }

        if (subRectangle.right > targetSize.width || subRectangle.bottom > targetSize.height)
        {
            ThrowHR(E_INVALIDARG);
        }
    }

    static bool IsBlockAligned(unsigned blockSize, D2D1_RECT_U const& r)
    {
        if (blockSize == 1)
            return true;

        auto IsAligned = [=](uint32_t coord) { return (coord % blockSize) == 0; };

        return IsAligned(r.left) &&
               IsAligned(r.top) &&
               IsAligned(r.right) &&
               IsAligned(r.bottom);
    }


    class BitmapSubRectangle
    {
        unsigned m_totalBytes;
        unsigned m_bytesPerRow;
        unsigned m_blocksHigh;

        DXGI_FORMAT m_format;
        
    public:
        BitmapSubRectangle(ComPtr<ID2D1Bitmap> const& d2dBitmap, D2D1_RECT_U const& rect)
        {
            VerifyWellFormedSubrectangle(rect, d2dBitmap->GetPixelSize());

            m_format = d2dBitmap->GetPixelFormat().format;
            auto blockSize = GetBlockSize(m_format);

            if (!IsBlockAligned(blockSize, rect))
                ThrowHR(E_INVALIDARG, Strings::BlockCompressedSubRectangleMustBeAligned);

            auto bytesPerBlock = GetBytesPerBlock(m_format);

            auto pixelWidth = (rect.right - rect.left);
            auto blocksWide = pixelWidth / blockSize;

            auto pixelHeight = (rect.bottom - rect.top);
            m_blocksHigh = pixelHeight / blockSize;

            auto totalBlocks = blocksWide * m_blocksHigh;

            m_totalBytes = totalBlocks * bytesPerBlock;
            m_bytesPerRow = blocksWide * bytesPerBlock;
        }

        unsigned GetTotalBytes() const { return m_totalBytes; }
        unsigned GetBytesPerRow() const { return m_bytesPerRow; }
        unsigned GetBlocksHigh() const { return m_blocksHigh; }

        DXGI_FORMAT GetFormat() const { return m_format; }
    };


    bool FileFormatSupportsHdr(GUID const& containerFormat)
    {
        return containerFormat == GUID_ContainerFormatWmp;
    }

    GUID GetGUIDForFileFormat(CanvasBitmapFileFormat fileFormat)
    {
        switch (fileFormat)
        {
            case CanvasBitmapFileFormat::Bmp: return GUID_ContainerFormatBmp;
            case CanvasBitmapFileFormat::Png: return GUID_ContainerFormatPng;
            case CanvasBitmapFileFormat::Jpeg: return GUID_ContainerFormatJpeg;
            case CanvasBitmapFileFormat::Tiff: return GUID_ContainerFormatTiff;
            case CanvasBitmapFileFormat::Gif: return GUID_ContainerFormatGif;
            case CanvasBitmapFileFormat::JpegXR: return GUID_ContainerFormatWmp;
            default:
            {
                ThrowHR(E_INVALIDARG);
            }
        }
    }

    static GUID GetEncoderFromFileExtension(
        WinString fileName)
    {
        static const struct ExtensionTable
        {
            wchar_t* Extension;
            GUID Guid;
        } extensionTable[] =
        {
            { L"bmp", GUID_ContainerFormatBmp },
            { L"dib", GUID_ContainerFormatBmp },
            { L"rle", GUID_ContainerFormatBmp },
            { L"png", GUID_ContainerFormatPng },
            { L"jpg", GUID_ContainerFormatJpeg },
            { L"jpe", GUID_ContainerFormatJpeg },
            { L"jpeg", GUID_ContainerFormatJpeg },
            { L"jfif", GUID_ContainerFormatJpeg },
            { L"exif", GUID_ContainerFormatJpeg },
            { L"tif", GUID_ContainerFormatTiff },
            { L"tiff", GUID_ContainerFormatTiff },
            { L"gif", GUID_ContainerFormatGif },
            { L"wdp", GUID_ContainerFormatWmp },
            { L"jxr", GUID_ContainerFormatWmp },
        };

        auto rend = std::reverse_iterator<wchar_t const*>(begin(fileName));
        auto rbegin = std::reverse_iterator<wchar_t const*>(end(fileName));
        auto rseparatorIt = std::find(rbegin, rend, L'.');
        if (rseparatorIt != rend)
        {
            auto extension = rseparatorIt.base();

            for (auto const& tableEntry : extensionTable)
            {
                if (_wcsicmp(extension, tableEntry.Extension) == 0)
                    return tableEntry.Guid;
            }
        }

        ThrowHR(E_INVALIDARG, Strings::UnrecognizedImageFileExtension);
    }


    static bool TryEnableIndexing(ComPtr<IWICBitmapFrameDecode> const& frameDecode)
    {
#if WINVER > _WIN32_WINNT_WINBLUE
        auto jpegDecode = MaybeAs<IWICJpegFrameDecode>(frameDecode);

        if (!jpegDecode)
            return false;

        BOOL supportsIndexing;
        ThrowIfFailed(jpegDecode->DoesSupportIndexing(&supportsIndexing));

        if (!supportsIndexing)
            return false;

        auto const indexingGranularity = 64U;
        ThrowIfFailed(jpegDecode->SetIndexing(WICJpegIndexingOptionsGenerateOnLoad, indexingGranularity));

        return true;
#else
        UNREFERENCED_PARAMETER(frameDecode);
        return false;
#endif
    }

    static UINT GetOrientationFromFrameDecode(ComPtr<IWICBitmapFrameDecode> const& frameDecode)
    {
        ComPtr<IWICMetadataQueryReader> queryReader;

        HRESULT getMetadataHr = frameDecode->GetMetadataQueryReader(&queryReader);
        if (getMetadataHr == WINCODEC_ERR_UNSUPPORTEDOPERATION)
        {
            // This is expected for some formats, such as BMP.
            return PHOTO_ORIENTATION_NORMAL;
        }
        else
            ThrowIfFailed(getMetadataHr);

        PROPVARIANT orientation;
        PropVariantInit(&orientation);
            
        HRESULT lookupHr = queryReader->GetMetadataByName(L"System.Photo.Orientation", &orientation);

        const bool propertyNotFound = 
            lookupHr == WINCODEC_ERR_PROPERTYNOTFOUND ||
            lookupHr == WINCODEC_ERR_PROPERTYNOTSUPPORTED;

        // Anything other than property-not-found is unexpected
        if (!propertyNotFound)
            ThrowIfFailed(lookupHr);

        // The property wasn't specified, or specifies an out-of-range value
        if (propertyNotFound ||
            orientation.vt != VT_UI2 ||
            orientation.uiVal < 1 ||
            orientation.uiVal > 8)
        {                
            return PHOTO_ORIENTATION_NORMAL;
        }
        else
        {
            return orientation.uiVal;
        }
    }

    static WICBitmapTransformOptions GetTransformOptionsFromPhotoOrientation(UINT photoOrientation)
    {
        switch (photoOrientation)
        {
        case PHOTO_ORIENTATION_NORMAL: return WICBitmapTransformRotate0;
        case PHOTO_ORIENTATION_ROTATE90: return WICBitmapTransformRotate270;
        case PHOTO_ORIENTATION_ROTATE180: return WICBitmapTransformRotate180;
        case PHOTO_ORIENTATION_ROTATE270: return WICBitmapTransformRotate90;
        case PHOTO_ORIENTATION_FLIPHORIZONTAL: return WICBitmapTransformFlipHorizontal;
        case PHOTO_ORIENTATION_FLIPVERTICAL: return WICBitmapTransformFlipVertical;
        case PHOTO_ORIENTATION_TRANSPOSE: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
        case PHOTO_ORIENTATION_TRANSVERSE: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
        default:
            ThrowHR(E_INVALIDARG);
        }
    }

    template<typename T>
    static ComPtr<IWICBitmapSource> CreateWicBitmapSourceWithExifTransform(ICanvasDevice* device, T fileNameOrStream)
    {
        auto adapter = CanvasBitmapAdapter::GetInstance();

        auto source = adapter->CreateWicBitmapSource(device, fileNameOrStream);

        if (source.Transform == WICBitmapTransformRotate0)
            return source.Source;

        return adapter->CreateFlipRotator(source.Source, source.Transform);        
    }

    
    //
    // DefaultBitmapAdapter
    //

    DefaultBitmapAdapter::DefaultBitmapAdapter()
        : m_wicAdapter(WicAdapter::GetInstance())
    {
    }

    WicBitmapSource DefaultBitmapAdapter::CreateWicBitmapSource(ICanvasDevice* device, HSTRING fileName, bool tryEnableIndexing)
    {
        ComPtr<IWICStream> stream;
        ThrowIfFailed(m_wicAdapter->GetFactory()->CreateStream(&stream));

        WinString fileNameString(fileName);
        ThrowIfFailed(stream->InitializeFromFilename(static_cast<const wchar_t*>(fileNameString), GENERIC_READ));

        return CreateWicBitmapSource(device, stream.Get(), tryEnableIndexing);
    }

    static bool IsSupportedPixelFormat(ICanvasDevice* device, GUID const& frameFormat, GUID const& wicFormat, DXGI_FORMAT dxgiFormat)
    {
        return (frameFormat == wicFormat) &&
               As<ICanvasDeviceInternal>(device)->GetResourceCreationDeviceContext()->IsDxgiFormatSupported(dxgiFormat);
    }

    WicBitmapSource DefaultBitmapAdapter::CreateWicBitmapSource(ICanvasDevice* device, IStream* fileStream, bool tryEnableIndexing)
    {
        auto factory = m_wicAdapter->GetFactory();
        ComPtr<IWICBitmapDecoder> wicBitmapDecoder;
        ThrowIfFailed(factory->CreateDecoderFromStream(
            fileStream,
            nullptr,
            WICDecodeMetadataCacheOnDemand,
            &wicBitmapDecoder));

        auto ddsDecoder = MaybeAs<IWICDdsDecoder>(wicBitmapDecoder);
        if (ddsDecoder)
        {
            // If it's a block compressed DDS file then we want to pass the
            // frame back unmodified.  This allows us to directly copy the
            // compressed data from the file into the bitmap.
            
            WICDdsParameters ddsParameters;
            ThrowIfFailed(ddsDecoder->GetParameters(&ddsParameters));

            if (ddsParameters.DxgiFormat == DXGI_FORMAT_BC1_UNORM ||
                ddsParameters.DxgiFormat == DXGI_FORMAT_BC2_UNORM ||
                ddsParameters.DxgiFormat == DXGI_FORMAT_BC3_UNORM)
            {
                ComPtr<IWICBitmapFrameDecode> ddsFrame;
                ThrowIfFailed(ddsDecoder->GetFrame(0, 0, 0, &ddsFrame));

                // DDS files never need a transform and are not indexed
                return WicBitmapSource{ ddsFrame, WICBitmapTransformRotate0, false };
            }
        }

        ComPtr<IWICBitmapFrameDecode> wicBitmapFrameDecode;
        ThrowIfFailed(wicBitmapDecoder->GetFrame(0, &wicBitmapFrameDecode));

        bool isIndexed = tryEnableIndexing ? TryEnableIndexing(wicBitmapFrameDecode) : false;

        auto orientation = GetOrientationFromFrameDecode(wicBitmapFrameDecode);
        auto transformOptions = GetTransformOptionsFromPhotoOrientation(orientation);

        ComPtr<IWICFormatConverter> wicFormatConverter;
        ThrowIfFailed(factory->CreateFormatConverter(&wicFormatConverter));

        auto targetPixelFormat = GUID_WICPixelFormat32bppPBGRA;

        GUID containerFormat;
        ThrowIfFailed(wicBitmapDecoder->GetContainerFormat(&containerFormat));

        // Load the data using an extended range format if:
        //  - The file format supports extended range (JpegXR)
        //  - And the pixel format is int16, float16, or float32
        //  - And the graphics device supports that pixel format
        if (FileFormatSupportsHdr(containerFormat))
        {
            GUID frameFormat;
            ThrowIfFailed(wicBitmapFrameDecode->GetPixelFormat(&frameFormat));

            if (IsSupportedPixelFormat(device, frameFormat, GUID_WICPixelFormat64bppRGBA,       DXGI_FORMAT_R16G16B16A16_UNORM) ||
                IsSupportedPixelFormat(device, frameFormat, GUID_WICPixelFormat64bppRGBAHalf,   DXGI_FORMAT_R16G16B16A16_FLOAT) ||
                IsSupportedPixelFormat(device, frameFormat, GUID_WICPixelFormat128bppRGBAFloat, DXGI_FORMAT_R32G32B32A32_FLOAT))
            {
                targetPixelFormat = frameFormat;
            }
        }

        ThrowIfFailed(wicFormatConverter->Initialize(
            wicBitmapFrameDecode.Get(),
            targetPixelFormat,
            WICBitmapDitherTypeNone,
            NULL,
            0,
            WICBitmapPaletteTypeMedianCut));

        return WicBitmapSource{ wicFormatConverter, transformOptions, isIndexed };
    }

    ComPtr<IWICBitmapSource> DefaultBitmapAdapter::CreateFlipRotator(
        ComPtr<IWICBitmapSource> const& source,
        WICBitmapTransformOptions transformOptions)
    {
        ComPtr<IWICBitmapFlipRotator> bitmapFlipRotator;

        ThrowIfFailed(m_wicAdapter->GetFactory()->CreateBitmapFlipRotator(&bitmapFlipRotator));
        ThrowIfFailed(bitmapFlipRotator->Initialize(source.Get(), transformOptions));

        return bitmapFlipRotator;
    }


    ComPtr<CanvasBitmap> CanvasBitmap::CreateNew(
        ICanvasDevice* canvasDevice,
        HSTRING fileName,
        float dpi,
        CanvasAlphaMode alpha)
    {
        ComPtr<ICanvasDeviceInternal> canvasDeviceInternal;
        ThrowIfFailed(canvasDevice->QueryInterface(canvasDeviceInternal.GetAddressOf()));

        auto wicBitmapSource = CreateWicBitmapSourceWithExifTransform(canvasDevice, fileName);

        auto d2dBitmap = canvasDeviceInternal->CreateBitmapFromWicResource(wicBitmapSource.Get(), dpi, alpha);

        auto bitmap = Make<CanvasBitmap>(
            canvasDevice,
            d2dBitmap.Get());
        CheckMakeResult(bitmap);
        
        return bitmap;
    }


    ComPtr<CanvasBitmap> CanvasBitmap::CreateNew(
        ICanvasDevice* canvasDevice,
        IStream* fileStream,
        float dpi,
        CanvasAlphaMode alpha)
    {
        ComPtr<ICanvasDeviceInternal> canvasDeviceInternal;
        ThrowIfFailed(canvasDevice->QueryInterface(canvasDeviceInternal.GetAddressOf()));

        auto wicBitmapSource = CreateWicBitmapSourceWithExifTransform(canvasDevice, fileStream);

        auto d2dBitmap = canvasDeviceInternal->CreateBitmapFromWicResource(wicBitmapSource.Get(), dpi, alpha);

        auto bitmap = Make<CanvasBitmap>(
            canvasDevice,
            d2dBitmap.Get());
        CheckMakeResult(bitmap);
        
        return bitmap;
    }


    ComPtr<CanvasBitmap> CanvasBitmap::CreateNew(
        ICanvasDevice* device,
        uint32_t byteCount,
        BYTE* bytes,
        int32_t widthInPixels,
        int32_t heightInPixels,
        float dpi,
        DirectXPixelFormat format,
        CanvasAlphaMode alpha)
    {
        auto dxgiFormat = static_cast<DXGI_FORMAT>(format);
        auto blockSize = GetBlockSize(dxgiFormat);

        if ((widthInPixels % blockSize) != 0 || (heightInPixels % blockSize) != 0)
            ThrowHR(E_INVALIDARG, Strings::BlockCompressedDimensionsMustBeMultipleOf4);
        
        auto blocksWide = widthInPixels / blockSize;
        auto pitch = GetBytesPerBlock(dxgiFormat) * blocksWide;

        // Validate that the buffer is large enough
        auto blocksHigh = heightInPixels / blockSize;
        auto bytesNeeded = blocksHigh * pitch;

        if (byteCount < bytesNeeded)
            ThrowHR(E_INVALIDARG);

        auto d2dBitmap = As<ICanvasDeviceInternal>(device)->CreateBitmapFromBytes(
            (bytesNeeded > 0) ? bytes : nullptr,
            pitch,
            widthInPixels,
            heightInPixels,
            dpi,
            format,
            alpha);

        auto bitmap = Make<CanvasBitmap>(
            device,
            d2dBitmap.Get());
        CheckMakeResult(bitmap);

        return bitmap;
    }

    
    ComPtr<CanvasBitmap> CanvasBitmap::CreateNew(
        ICanvasDevice* device,
        uint32_t colorCount,
        Color* colors,
        int32_t widthInPixels,
        int32_t heightInPixels,
        float dpi,
        CanvasAlphaMode alpha)
    {
        auto convertedBytes = ConvertColorsToBgra(colorCount, colors);

        return CreateNew(
            device,
            static_cast<uint32_t>(convertedBytes.size()),
            convertedBytes.empty() ? nullptr : &convertedBytes[0],
            widthInPixels,
            heightInPixels,
            dpi,
            PIXEL_FORMAT(B8G8R8A8UIntNormalized),
            alpha);
    }


#if WINVER > _WIN32_WINNT_WINBLUE

    //
    // BitmapPixelFormat's values are intended to be reinterpreted as
    // DXGI_FORMAT values.  Unfortunately, some of the chosen color values map
    // to UINT rather than UNORM formats.  The intended use is UNORM, and so we
    // fudge the BitmapPixelFormat values in these cases.
    //
    static DirectXPixelFormat GetFudgedFormat(BitmapPixelFormat format)
    {
        using namespace ABI::Windows::Graphics::Imaging;
        
        switch (format)
        {
        case BitmapPixelFormat_Rgba16: return PIXEL_FORMAT(R16G16B16A16UIntNormalized);
        case BitmapPixelFormat_Rgba8:  return PIXEL_FORMAT(R8G8B8A8UIntNormalized);
        case BitmapPixelFormat_Gray8:  return PIXEL_FORMAT(A8UIntNormalized);

        case BitmapPixelFormat_Bgra8: // BitmapPixelFormat already uses the UNORM value here
        default:
            return static_cast<DirectXPixelFormat>(format);
        }        
    }


    ComPtr<CanvasBitmap> CanvasBitmap::CreateNew(
        ICanvasDevice* device,
        ISoftwareBitmap* sourceBitmap)
    {
        using namespace ABI::Windows::Graphics::Imaging;
        using ::Windows::Foundation::IMemoryBufferByteAccess;

        //
        // Extract information from the SoftwareBitmap
        //

        BitmapPixelFormat bitmapPixelFormat;
        ThrowIfFailed(sourceBitmap->get_BitmapPixelFormat(&bitmapPixelFormat));

        BitmapAlphaMode bitmapAlphaMode;
        ThrowIfFailed(sourceBitmap->get_BitmapAlphaMode(&bitmapAlphaMode));

        double dpiX;
        ThrowIfFailed(sourceBitmap->get_DpiX(&dpiX));

        ComPtr<IBitmapBuffer> bitmapBuffer;
        ThrowIfFailed(sourceBitmap->LockBuffer(BitmapBufferAccessMode_Read, &bitmapBuffer));

        ComPtr<IMemoryBufferReference> memoryBuffer;
        ThrowIfFailed(As<IMemoryBuffer>(bitmapBuffer)->CreateReference(&memoryBuffer));

        uint32_t bufferSize;
        uint8_t* buffer;
        ThrowIfFailed(As<IMemoryBufferByteAccess>(memoryBuffer)->GetBuffer(&buffer, &bufferSize));

        BitmapPlaneDescription bitmapPlaneDescription;
        ThrowIfFailed(bitmapBuffer->GetPlaneDescription(0, &bitmapPlaneDescription));

        auto pixelWidth = static_cast<uint32_t>(bitmapPlaneDescription.Width);
        auto pixelHeight = static_cast<uint32_t>(bitmapPlaneDescription.Height);

        //
        // Create an ID2D1Bitmap1 from the source bitmap's data
        //

        auto d2dBitmap = As<ICanvasDeviceInternal>(device)->CreateBitmapFromBytes(
            buffer + bitmapPlaneDescription.StartIndex,
            bitmapPlaneDescription.Stride,
            pixelWidth,
            pixelHeight,
            static_cast<float>(dpiX),
            GetFudgedFormat(bitmapPixelFormat),
            ToCanvasAlphaMode(bitmapAlphaMode));

        //
        // Wrap a CanvasBitmap around it
        //

        auto bitmap = Make<CanvasBitmap>(
            device,
            d2dBitmap.Get());
        CheckMakeResult(bitmap);

        return bitmap;
    }

#endif

    
    //
    // ICanvasBitmapStatics
    //

    IFACEMETHODIMP CanvasBitmapFactory::CreateFromDirect3D11Surface(
        ICanvasResourceCreator* resourceCreator,
        IDirect3DSurface* surface,
        ICanvasBitmap** canvasBitmap)
    {
        return CreateFromDirect3D11SurfaceImpl(
            resourceCreator,
            surface,
            DEFAULT_DPI,
            nullptr,
            canvasBitmap);
    }

    IFACEMETHODIMP CanvasBitmapFactory::CreateFromDirect3D11SurfaceWithDpi(
        ICanvasResourceCreator* resourceCreator,
        IDirect3DSurface* surface,
        float dpi,
        ICanvasBitmap** canvasBitmap)
    {
        return CreateFromDirect3D11SurfaceImpl(
            resourceCreator,
            surface,
            dpi,
            nullptr,
            canvasBitmap);
    }

    IFACEMETHODIMP CanvasBitmapFactory::CreateFromDirect3D11SurfaceWithDpiAndAlpha(
        ICanvasResourceCreator* resourceCreator,
        IDirect3DSurface* surface,
        float dpi,
        CanvasAlphaMode alpha,
        ICanvasBitmap** canvasBitmap)
    {
        return CreateFromDirect3D11SurfaceImpl(
            resourceCreator,
            surface,
            dpi,
            &alpha,
            canvasBitmap);
    }

    static CanvasAlphaMode AlphaModeFromFormat(DirectXPixelFormat format)
    {
        switch (format)
        {
            case PIXEL_FORMAT(B8G8R8X8UIntNormalized):
            case PIXEL_FORMAT(R8G8UIntNormalized):
            case PIXEL_FORMAT(R8UIntNormalized):
                return CanvasAlphaMode::Ignore;

            default:
                return CanvasAlphaMode::Premultiplied;
        }
    }

    HRESULT CanvasBitmapFactory::CreateFromDirect3D11SurfaceImpl(
        ICanvasResourceCreator* resourceCreator,
        IDirect3DSurface* surface,
        float dpi,
        CanvasAlphaMode const* requestedAlpha,
        ICanvasBitmap** canvasBitmap)
    {
        return ExceptionBoundary(
            [&]
            {
                CheckInPointer(resourceCreator);
                CheckInPointer(surface);
                CheckAndClearOutPointer(canvasBitmap);

                ComPtr<ICanvasDevice> canvasDevice;
                ThrowIfFailed(resourceCreator->get_Device(&canvasDevice));

                CanvasAlphaMode alpha;
                if (requestedAlpha)
                {
                    alpha = *requestedAlpha;
                }
                else
                {
                    Direct3DSurfaceDescription surfaceDescription;
                    ThrowIfFailed(surface->get_Description(&surfaceDescription));

                    alpha = AlphaModeFromFormat(surfaceDescription.Format);
                }

                auto d2dBitmap = As<ICanvasDeviceInternal>(canvasDevice)->CreateBitmapFromSurface(surface, dpi, alpha);

                auto newBitmap = ResourceManager::GetOrCreate<ICanvasBitmap>(canvasDevice.Get(), d2dBitmap.Get());

                ThrowIfFailed(newBitmap.CopyTo(canvasBitmap));
            });
    }

    IFACEMETHODIMP CanvasBitmapFactory::CreateFromBytes(
        ICanvasResourceCreator* resourceCreator,
        uint32_t byteCount,
        BYTE* bytes,
        int32_t widthInPixels,
        int32_t heightInPixels,
        DirectXPixelFormat format,
        ICanvasBitmap** canvasBitmap)
    {
        return CreateFromBytesWithDpiAndAlpha(
            resourceCreator,
            byteCount,
            bytes,
            widthInPixels,
            heightInPixels,
            format,
            DEFAULT_DPI,
            AlphaModeFromFormat(format),
            canvasBitmap);
    }

    IFACEMETHODIMP CanvasBitmapFactory::CreateFromBytesWithDpi(
        ICanvasResourceCreator* resourceCreator,
        uint32_t byteCount,
        BYTE* bytes,
        int32_t widthInPixels,
        int32_t heightInPixels,
        DirectXPixelFormat format,
        float dpi,
        ICanvasBitmap** canvasBitmap)
    {
        return CreateFromBytesWithDpiAndAlpha(
            resourceCreator,
            byteCount,
            bytes,
            widthInPixels,
            heightInPixels,
            format,
            dpi,
            AlphaModeFromFormat(format),
            canvasBitmap);
    }

    IFACEMETHODIMP CanvasBitmapFactory::CreateFromBytesWithDpiAndAlpha(
        ICanvasResourceCreator* resourceCreator,
        uint32_t byteCount,
        BYTE* bytes,
        int32_t widthInPixels,
        int32_t heightInPixels,
        DirectXPixelFormat format,
        float dpi,
        CanvasAlphaMode alpha,
        ICanvasBitmap** canvasBitmap)
    {
        return ExceptionBoundary(
            [&]
            {
                CheckInPointer(resourceCreator);
                if (byteCount)
                    CheckInPointer(bytes);
                CheckAndClearOutPointer(canvasBitmap);

                ComPtr<ICanvasDevice> canvasDevice;
                ThrowIfFailed(resourceCreator->get_Device(&canvasDevice));

                auto newBitmap = CanvasBitmap::CreateNew(
                    canvasDevice.Get(), 
                    byteCount, 
                    bytes, 
                    widthInPixels,
                    heightInPixels,
                    dpi,
                    format, 
                    alpha);

                ThrowIfFailed(newBitmap.CopyTo(canvasBitmap));
            });
    }

    IFACEMETHODIMP CanvasBitmapFactory::CreateFromBytesWithBuffer(
        ICanvasResourceCreator* resourceCreator,
        IBuffer* buffer,
        int32_t widthInPixels,
        int32_t heightInPixels,
        DirectXPixelFormat format,
        ICanvasBitmap** canvasBitmap)
    {
        return CreateFromBytesWithBufferAndDpiAndAlpha(
            resourceCreator,
            buffer,
            widthInPixels,
            heightInPixels,
            format,
            DEFAULT_DPI,
            AlphaModeFromFormat(format),
            canvasBitmap);
    }

    IFACEMETHODIMP CanvasBitmapFactory::CreateFromBytesWithBufferAndDpi(
        ICanvasResourceCreator* resourceCreator,
        IBuffer* buffer,
        int32_t widthInPixels,
        int32_t heightInPixels,
        DirectXPixelFormat format,
        float dpi,
        ICanvasBitmap** canvasBitmap)
    {
        return CreateFromBytesWithBufferAndDpiAndAlpha(
            resourceCreator,
            buffer,
            widthInPixels,
            heightInPixels,
            format,
            dpi,
            AlphaModeFromFormat(format),
            canvasBitmap);
    }

    IFACEMETHODIMP CanvasBitmapFactory::CreateFromBytesWithBufferAndDpiAndAlpha(
        ICanvasResourceCreator* resourceCreator,
        IBuffer* buffer,
        int32_t widthInPixels,
        int32_t heightInPixels,
        DirectXPixelFormat format,
        float dpi,
        CanvasAlphaMode alpha,
        ICanvasBitmap** canvasBitmap)
    {
        using ::Windows::Storage::Streams::IBufferByteAccess;

        return ExceptionBoundary(
            [&]
            {
                CheckInPointer(buffer);

                auto byteAccess = As<IBufferByteAccess>(buffer);

                uint32_t byteCount;
                uint8_t* bytes;

                ThrowIfFailed(buffer->get_Length(&byteCount));
                ThrowIfFailed(byteAccess->Buffer(&bytes));

                ThrowIfFailed(CreateFromBytesWithDpiAndAlpha(
                    resourceCreator, 
                    byteCount, 
                    bytes,
                    widthInPixels,
                    heightInPixels,
                    format,
                    dpi,
                    alpha,
                    canvasBitmap));
            });
    }

    IFACEMETHODIMP CanvasBitmapFactory::CreateFromColors(
        ICanvasResourceCreator* resourceCreator,
        uint32_t colorCount,
        ABI::Windows::UI::Color* colors,
        int32_t widthInPixels,
        int32_t heightInPixels,
        ICanvasBitmap** canvasBitmap)
    {
        return CreateFromColorsWithDpiAndAlpha(
            resourceCreator,
            colorCount,
            colors,
            widthInPixels,
            heightInPixels,
            DEFAULT_DPI,
            CanvasAlphaMode::Premultiplied,
            canvasBitmap);
    }

    IFACEMETHODIMP CanvasBitmapFactory::CreateFromColorsWithDpi(
        ICanvasResourceCreator* resourceCreator,
        uint32_t colorCount,
        ABI::Windows::UI::Color* colors,
        int32_t widthInPixels,
        int32_t heightInPixels,
        float dpi,
        ICanvasBitmap** canvasBitmap)
    {
        return CreateFromColorsWithDpiAndAlpha(
            resourceCreator,
            colorCount,
            colors,
            widthInPixels,
            heightInPixels,
            dpi,
            CanvasAlphaMode::Premultiplied,
            canvasBitmap);
    }

    IFACEMETHODIMP CanvasBitmapFactory::CreateFromColorsWithDpiAndAlpha(
        ICanvasResourceCreator* resourceCreator,
        uint32_t colorCount,
        ABI::Windows::UI::Color* colors,
        int32_t widthInPixels,
        int32_t heightInPixels,
        float dpi,
        CanvasAlphaMode alpha,
        ICanvasBitmap** canvasBitmap)
    {
        return ExceptionBoundary(
            [&]
            {
                CheckInPointer(resourceCreator);
                if (colorCount)
                    CheckInPointer(colors);
                CheckAndClearOutPointer(canvasBitmap);

                ComPtr<ICanvasDevice> canvasDevice;
                ThrowIfFailed(resourceCreator->get_Device(&canvasDevice));

                auto newBitmap = CanvasBitmap::CreateNew(
                    canvasDevice.Get(), 
                    colorCount,
                    colors,
                    widthInPixels,
                    heightInPixels,
                    dpi,
                    alpha);

                ThrowIfFailed(newBitmap.CopyTo(canvasBitmap));
            });
    }

#if WINVER > _WIN32_WINNT_WINBLUE

    IFACEMETHODIMP CanvasBitmapFactory::CreateFromSoftwareBitmap(
        ICanvasResourceCreator* resourceCreator,
        ISoftwareBitmap* sourceBitmap,
        ICanvasBitmap** canvasBitmap)
    {
        return ExceptionBoundary(
            [&]
            {
                CheckInPointer(resourceCreator);
                CheckInPointer(sourceBitmap);
                CheckAndClearOutPointer(canvasBitmap);

                ComPtr<ICanvasDevice> canvasDevice;
                ThrowIfFailed(resourceCreator->get_Device(&canvasDevice));

                auto newBitmap = CanvasBitmap::CreateNew(
                    canvasDevice.Get(),
                    sourceBitmap);

                ThrowIfFailed(newBitmap.CopyTo(canvasBitmap));
            });
    }

#endif

    IFACEMETHODIMP CanvasBitmapFactory::LoadAsyncFromHstring(
        ICanvasResourceCreator* resourceCreator,
        HSTRING fileName,
        ABI::Windows::Foundation::IAsyncOperation<CanvasBitmap*>** canvasBitmapAsyncOperation)
    {
        return LoadAsyncFromHstringWithDpiAndAlpha(
            resourceCreator,
            fileName,
            DEFAULT_DPI,
            CanvasAlphaMode::Premultiplied,
            canvasBitmapAsyncOperation);
    }

    IFACEMETHODIMP CanvasBitmapFactory::LoadAsyncFromHstringWithDpi(
        ICanvasResourceCreator* resourceCreator,
        HSTRING fileName,
        float dpi,
        ABI::Windows::Foundation::IAsyncOperation<CanvasBitmap*>** canvasBitmapAsyncOperation)
    {
        return LoadAsyncFromHstringWithDpiAndAlpha(
            resourceCreator,
            fileName,
            dpi,
            CanvasAlphaMode::Premultiplied,
            canvasBitmapAsyncOperation);
    }

    IFACEMETHODIMP CanvasBitmapFactory::LoadAsyncFromHstringWithDpiAndAlpha(
        ICanvasResourceCreator* resourceCreator,
        HSTRING rawFileName,
        float dpi,
        CanvasAlphaMode alpha,
        ABI::Windows::Foundation::IAsyncOperation<CanvasBitmap*>** canvasBitmapAsyncOperation)
    {
        return ExceptionBoundary(
            [&]
            {
                CheckInPointer(resourceCreator);
                CheckInPointer(rawFileName);
                CheckAndClearOutPointer(canvasBitmapAsyncOperation);

                ComPtr<ICanvasDevice> canvasDevice;
                ThrowIfFailed(resourceCreator->get_Device(&canvasDevice));

                WinString fileName(rawFileName);

                auto asyncOperation = Make<AsyncOperation<CanvasBitmap>>(
                    [=]
                    {
                        return CanvasBitmap::CreateNew(canvasDevice.Get(), fileName, dpi, alpha);
                    });

                CheckMakeResult(asyncOperation);
                ThrowIfFailed(asyncOperation.CopyTo(canvasBitmapAsyncOperation));
            });
    }

    IFACEMETHODIMP CanvasBitmapFactory::LoadAsyncFromUri(
        ICanvasResourceCreator* resourceCreator,
        ABI::Windows::Foundation::IUriRuntimeClass* uri,
        ABI::Windows::Foundation::IAsyncOperation<CanvasBitmap*>** canvasBitmapAsyncOperation)
    {
        return LoadAsyncFromUriWithDpiAndAlpha(
            resourceCreator, 
            uri, 
            DEFAULT_DPI,
            CanvasAlphaMode::Premultiplied,
            canvasBitmapAsyncOperation);
    }

    IFACEMETHODIMP CanvasBitmapFactory::LoadAsyncFromUriWithDpi(
        ICanvasResourceCreator* resourceCreator,
        ABI::Windows::Foundation::IUriRuntimeClass* uri,
        float dpi,
        ABI::Windows::Foundation::IAsyncOperation<CanvasBitmap*>** canvasBitmapAsyncOperation)
    {
        return LoadAsyncFromUriWithDpiAndAlpha(
            resourceCreator,
            uri,
            dpi,
            CanvasAlphaMode::Premultiplied,
            canvasBitmapAsyncOperation);
    }
     
    IFACEMETHODIMP CanvasBitmapFactory::LoadAsyncFromUriWithDpiAndAlpha(
        ICanvasResourceCreator* resourceCreator,
        ABI::Windows::Foundation::IUriRuntimeClass* uri,
        float dpi,
        CanvasAlphaMode alpha,
        ABI::Windows::Foundation::IAsyncOperation<CanvasBitmap*>** canvasBitmapAsyncOperation)
    {
        return ExceptionBoundary(
            [&]
            {
                CheckInPointer(resourceCreator);
                CheckInPointer(uri);
                CheckAndClearOutPointer(canvasBitmapAsyncOperation);

                ComPtr<ICanvasDevice> canvasDevice;
                ThrowIfFailed(resourceCreator->get_Device(&canvasDevice));

                ComPtr<IRandomAccessStreamReferenceStatics> streamReferenceStatics;
                ThrowIfFailed(GetActivationFactory(HStringReference(RuntimeClass_Windows_Storage_Streams_RandomAccessStreamReference).Get(), &streamReferenceStatics));

                ComPtr<IRandomAccessStreamReference> streamReference;
                ThrowIfFailed(streamReferenceStatics->CreateFromUri(uri, &streamReference));

                // Start opening the file.
                ComPtr<IAsyncOperation<IRandomAccessStreamWithContentType*>> openOperation;
                ThrowIfFailed(streamReference->OpenReadAsync(&openOperation));

                // Our main async operation is constructed as a continuation of the file open,
                // so the lambda will only start executing once the file is ready.
                auto asyncOperation = Make<AsyncOperation<CanvasBitmap>>(openOperation, [=]
                {
                    ComPtr<IRandomAccessStreamWithContentType> randomAccessStream;
                    ThrowIfFailed(openOperation->GetResults(&randomAccessStream));

                    ComPtr<IStream> stream;
                    ThrowIfFailed(CreateStreamOverRandomAccessStream(randomAccessStream.Get(), IID_PPV_ARGS(&stream)));

                    return CanvasBitmap::CreateNew(canvasDevice.Get(), stream.Get(), dpi, alpha);
                });

                CheckMakeResult(asyncOperation);
                ThrowIfFailed(asyncOperation.CopyTo(canvasBitmapAsyncOperation));
            });
    }

    IFACEMETHODIMP CanvasBitmapFactory::LoadAsyncFromStream(
        ICanvasResourceCreator* resourceCreator,
        IRandomAccessStream* stream,
        ABI::Windows::Foundation::IAsyncOperation<CanvasBitmap*>** canvasBitmapAsyncOperation)
    {
        return LoadAsyncFromStreamWithDpiAndAlpha(
            resourceCreator,
            stream,
            DEFAULT_DPI,
            CanvasAlphaMode::Premultiplied,
            canvasBitmapAsyncOperation);
    }

    IFACEMETHODIMP CanvasBitmapFactory::LoadAsyncFromStreamWithDpi(
        ICanvasResourceCreator* resourceCreator,
        IRandomAccessStream* stream,
        float dpi,
        ABI::Windows::Foundation::IAsyncOperation<CanvasBitmap*>** canvasBitmapAsyncOperation)
    {
        return LoadAsyncFromStreamWithDpiAndAlpha(
            resourceCreator,
            stream,
            dpi,
            CanvasAlphaMode::Premultiplied,
            canvasBitmapAsyncOperation);
    }

    IFACEMETHODIMP CanvasBitmapFactory::LoadAsyncFromStreamWithDpiAndAlpha(
        ICanvasResourceCreator* resourceCreator,
        IRandomAccessStream* rawStream,
        float dpi,
        CanvasAlphaMode alpha,
        ABI::Windows::Foundation::IAsyncOperation<CanvasBitmap*>** canvasBitmapAsyncOperation)
    {
        return ExceptionBoundary(
            [&]
            {
                CheckInPointer(resourceCreator);
                CheckInPointer(rawStream);
                CheckAndClearOutPointer(canvasBitmapAsyncOperation);

                ComPtr<ICanvasDevice> canvasDevice;
                ThrowIfFailed(resourceCreator->get_Device(&canvasDevice));

                ComPtr<IRandomAccessStream> stream = rawStream;

                auto asyncOperation = Make<AsyncOperation<CanvasBitmap>>(
                [=]
                {
                    ComPtr<IStream> nativeStream;
                    ThrowIfFailed(CreateStreamOverRandomAccessStream(stream.Get(), IID_PPV_ARGS(&nativeStream)));

                    return CanvasBitmap::CreateNew(canvasDevice.Get(), nativeStream.Get(), dpi, alpha);
                });

                CheckMakeResult(asyncOperation);
                ThrowIfFailed(asyncOperation.CopyTo(canvasBitmapAsyncOperation));
            });
    }


    //
    // CanvasBitmap
    //

    CanvasBitmap::CanvasBitmap(
        ICanvasDevice* device,
        ID2D1Bitmap1* d2dBitmap)
        : CanvasBitmapImpl(device, d2dBitmap)
    {
        assert(!IsRenderTargetBitmap(d2dBitmap) 
            && "CanvasBitmap should never be constructed with a render-target bitmap.  This should have been validated before construction.");
    }

    static void CopyPixelBytes(
        BitmapSubRectangle const& r,
        uint32_t sourceStride,
        uint32_t destinationStride,
        stdext::checked_array_iterator<uint8_t*> const& source,
        stdext::checked_array_iterator<uint8_t*> const& destination)
    {
#ifdef NDEBUG
        // Skip the iterator validation in release builds.
        auto s = source.base();
        auto d = destination.base();
#else
        auto s = source;
        auto d = destination;
#endif

        for (auto i = 0u; i < r.GetBlocksHigh(); ++i)
        {
            std::copy(s, s + r.GetBytesPerRow(), d);

            s += sourceStride;
            d += destinationStride;
        }
    }

    void GetPixelBytesImpl(
        ComPtr<ICanvasDevice> const& device,
        ComPtr<ID2D1Bitmap1> const& d2dBitmap,
        D2D1_RECT_U const& subRectangle,
        uint32_t* valueCount,
        uint8_t** valueElements)
    {
        CheckInPointer(valueCount);
        CheckAndClearOutPointer(valueElements);

        BitmapSubRectangle r(d2dBitmap, subRectangle);

        ScopedBitmapMappedPixelAccess bitmapPixelAccess(device.Get(), d2dBitmap.Get(), &subRectangle);

        ComArray<BYTE> array(r.GetTotalBytes());

        CopyPixelBytes(
            r, 
            bitmapPixelAccess.GetStride(), 
            r.GetBytesPerRow(),
            begin(bitmapPixelAccess),
            begin(array));

        array.Detach(valueCount, valueElements);
    }

    void GetPixelBytesImpl(
        ComPtr<ICanvasDevice> const& device,
        ComPtr<ID2D1Bitmap1> const& d2dBitmap,
        D2D1_RECT_U const& subRectangle,
        IBuffer* buffer)
    {
        using ::Windows::Storage::Streams::IBufferByteAccess;

        CheckInPointer(buffer);

        auto byteAccess = As<IBufferByteAccess>(buffer);

        BitmapSubRectangle r(d2dBitmap, subRectangle);

        ScopedBitmapMappedPixelAccess bitmapPixelAccess(device.Get(), d2dBitmap.Get(), &subRectangle);

        uint32_t capacity;
        ThrowIfFailed(buffer->get_Capacity(&capacity));

        if (capacity < r.GetTotalBytes())
        {
            WinStringBuilder message;
            message.Format(Strings::WrongArrayLength, r.GetTotalBytes(), capacity);
            ThrowHR(E_INVALIDARG, message.Get());
        }

        ThrowIfFailed(buffer->put_Length(r.GetTotalBytes()));

        uint8_t* destination;
        ThrowIfFailed(byteAccess->Buffer(&destination));

        CopyPixelBytes(
            r,
            bitmapPixelAccess.GetStride(),
            r.GetBytesPerRow(),
            begin(bitmapPixelAccess),
            stdext::make_checked_array_iterator(destination, capacity));
    }

    void GetPixelColorsImpl(
        ComPtr<ICanvasDevice> const& device,
        ComPtr<ID2D1Bitmap1> const& d2dBitmap,
        D2D1_RECT_U const& subRectangle,
        uint32_t* valueCount,
        Color **valueElements)
    {
        CheckInPointer(valueCount);
        CheckAndClearOutPointer(valueElements);

        VerifyWellFormedSubrectangle(subRectangle, d2dBitmap->GetPixelSize());

        if (d2dBitmap->GetPixelFormat().format != DXGI_FORMAT_B8G8R8A8_UNORM)
        {
            ThrowHR(E_INVALIDARG, Strings::PixelColorsFormatRestriction);
        }

        ScopedBitmapMappedPixelAccess bitmapPixelAccess(device.Get(), d2dBitmap.Get(), &subRectangle);

        const unsigned int subRectangleWidth = subRectangle.right - subRectangle.left;
        const unsigned int subRectangleHeight = subRectangle.bottom - subRectangle.top;
        const unsigned int destSizeInPixels = subRectangleWidth * subRectangleHeight;
        ComArray<Color> array(destSizeInPixels);

        byte* sourceRowStart = bitmapPixelAccess.GetLockedData();

        for (unsigned int y = 0; y < subRectangleHeight; y++)
        {
            for (unsigned int x = 0; x < subRectangleWidth; x++)
            {
                uint32_t sourcePixel = *(reinterpret_cast<uint32_t*>(&sourceRowStart[x * 4]));
                Color& destColor = array[y * subRectangleWidth + x];
                destColor.B = (sourcePixel >> 0) & 0xFF;
                destColor.G = (sourcePixel >> 8) & 0xFF;
                destColor.R = (sourcePixel >> 16) & 0xFF;
                destColor.A = (sourcePixel >> 24) & 0xFF;
            }
            sourceRowStart += bitmapPixelAccess.GetStride();
        }

        array.Detach(valueCount, valueElements);
    }

    static void SaveBitmap(
        ID2D1Bitmap1* d2dBitmap,
        ID2D1Device* d2dDevice,
        IStream* stream,
        GUID const& containerFormat,
        float quality)
    {
        if (quality < 0.0f || quality > 1.0f)
            ThrowHR(E_INVALIDARG);
        
        const D2D1_SIZE_U size = d2dBitmap->GetPixelSize();
        float dpiX, dpiY;
        d2dBitmap->GetDpi(&dpiX, &dpiY);

        auto targetPixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

        // Save the data using an extended range format if:
        //  - The file format supports extended range (JpegXR)
        //  - And the pixel format is int16, float16, or float32
        if (FileFormatSupportsHdr(containerFormat))
        {
            auto bitmapFormat = d2dBitmap->GetPixelFormat().format;

            switch (bitmapFormat)
            {
                case DXGI_FORMAT_R16G16B16A16_UNORM:
                case DXGI_FORMAT_R16G16B16A16_FLOAT:
                case DXGI_FORMAT_R32G32B32A32_FLOAT:
                    targetPixelFormat = bitmapFormat;
                    break;
            }
        }

        WICImageParameters parameters{};
        parameters.PixelFormat.format = targetPixelFormat;
        parameters.PixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        parameters.DpiX = dpiX;
        parameters.DpiY = dpiY;
        parameters.Top = 0;
        parameters.Left = 0;
        parameters.PixelWidth = size.width;
        parameters.PixelHeight = size.height;

        CanvasImageAdapter::GetInstance()->SaveImage(
            d2dBitmap,
            parameters,
            d2dDevice,
            stream,
            containerFormat,
            quality);
    }

    void SaveBitmapToFileImpl(
        ComPtr<ID2D1Device> const& d2dDevice,
        ComPtr<ID2D1Bitmap1> const& d2dBitmap,
        HSTRING rawfileName,
        CanvasBitmapFileFormat fileFormat,
        float quality,
        IAsyncAction **resultAsyncAction)
    {
        WinString fileName(rawfileName);

        auto asyncAction = Make<AsyncAction>(
            [=]
            {
                GUID encoderGuid;
                if (fileFormat == CanvasBitmapFileFormat::Auto)
                    encoderGuid = GetEncoderFromFileExtension(fileName);
                else
                    encoderGuid = GetGUIDForFileFormat(fileFormat);

                ComPtr<IWICStream> wicStream;
                ThrowIfFailed(WicAdapter::GetInstance()->GetFactory()->CreateStream(&wicStream));
                
                ThrowIfFailed(wicStream->InitializeFromFilename(static_cast<wchar_t const*>(fileName), GENERIC_WRITE));

                SaveBitmap(d2dBitmap.Get(), d2dDevice.Get(), wicStream.Get(), encoderGuid, quality);
            });

        CheckMakeResult(asyncAction);
        ThrowIfFailed(asyncAction.CopyTo(resultAsyncAction));
    }

    void SaveBitmapToStreamImpl(
        ComPtr<ID2D1Device> const& d2dDevice,
        ComPtr<ID2D1Bitmap1> const& d2dBitmap,
        ComPtr<IRandomAccessStream> const& randomAccessStream,
        CanvasBitmapFileFormat fileFormat,
        float quality,
        IAsyncAction **resultAsyncAction)
    {
        if (fileFormat == CanvasBitmapFileFormat::Auto)
        {
            ThrowHR(E_INVALIDARG, Strings::AutoFileFormatNotAllowed);
        }

        auto asyncAction = Make<AsyncAction>(
            [=]
            {
                ComPtr<IStream> stream;
                ThrowIfFailed(CreateStreamOverRandomAccessStream(randomAccessStream.Get(), IID_PPV_ARGS(&stream)));

                SaveBitmap(d2dBitmap.Get(), d2dDevice.Get(), stream.Get(), GetGUIDForFileFormat(fileFormat), quality);
            });

        CheckMakeResult(asyncAction);
        ThrowIfFailed(asyncAction.CopyTo(resultAsyncAction));
    }

    void SetPixelBytesImpl(
        ComPtr<ID2D1Bitmap1> const& d2dBitmap,
        D2D1_RECT_U const& subRectangle,
        uint32_t valueCount,
        uint8_t* valueElements)
    {
        CheckInPointer(valueElements);

        BitmapSubRectangle r(d2dBitmap, subRectangle);

        if (valueCount < r.GetTotalBytes())
        {
            WinStringBuilder message;
            message.Format(Strings::WrongArrayLength, r.GetTotalBytes(), valueCount);
            ThrowHR(E_INVALIDARG, message.Get());
        }

        ThrowIfFailed(d2dBitmap->CopyFromMemory(&subRectangle, valueElements, r.GetBytesPerRow()));
    }

    void SetPixelBytesImpl(
        ComPtr<ID2D1Bitmap1> const& d2dBitmap,
        D2D1_RECT_U const& subRectangle,
        IBuffer* buffer)
    {
        using ::Windows::Storage::Streams::IBufferByteAccess;

        CheckInPointer(buffer);

        auto byteAccess = As<IBufferByteAccess>(buffer);

        uint32_t byteCount;
        uint8_t* bytes;

        ThrowIfFailed(buffer->get_Length(&byteCount));
        ThrowIfFailed(byteAccess->Buffer(&bytes));

        SetPixelBytesImpl(d2dBitmap, subRectangle, byteCount, bytes);
    }

    void SetPixelColorsImpl(
        ComPtr<ID2D1Bitmap1> const& d2dBitmap,
        D2D1_RECT_U const& subRectangle,
        uint32_t valueCount,
        Color *valueElements)
    {
        CheckInPointer(valueElements);

        VerifyWellFormedSubrectangle(subRectangle, d2dBitmap->GetPixelSize());

        const unsigned int subRectangleWidth = subRectangle.right - subRectangle.left;
        const unsigned int subRectangleHeight = subRectangle.bottom - subRectangle.top;

        const uint32_t expectedArraySize = subRectangleWidth * subRectangleHeight;
        if (valueCount < expectedArraySize)
        {
            WinStringBuilder message;
            message.Format(Strings::WrongArrayLength, expectedArraySize, valueCount);
            ThrowHR(E_INVALIDARG, message.Get());
        }

        if (d2dBitmap->GetPixelFormat().format != DXGI_FORMAT_B8G8R8A8_UNORM)
        {
            ThrowHR(E_INVALIDARG, Strings::PixelColorsFormatRestriction);
        }

        auto convertedValues = ConvertColorsToBgra(expectedArraySize, valueElements);

        ThrowIfFailed(d2dBitmap->CopyFromMemory(&subRectangle, convertedValues.data(), subRectangleWidth * 4));
    }


    void CopyPixelsFromBitmapImpl(
        ICanvasBitmap* to,
        ICanvasBitmap* from,
        D2D1_POINT_2U const& destPoint,
        D2D1_RECT_U const* sourceRect)
    {
        assert(to);
        CheckInPointer(from);

        auto toBitmapInternal = As<ICanvasBitmapInternal>(to);
        auto toD2dBitmap = toBitmapInternal->GetD2DBitmap();

        auto fromBitmapInternal = As<ICanvasBitmapInternal>(from);
        auto fromD2dBitmap = fromBitmapInternal->GetD2DBitmap();

        // Compute the source rect.
        D2D1_RECT_U localSourceRect;

        if (!sourceRect)
        {
            auto sourceSize = fromD2dBitmap->GetPixelSize();
            localSourceRect = D2D1_RECT_U{ 0, 0, sourceSize.width, sourceSize.height };
            sourceRect = &localSourceRect;
        }

        // Compute the dest rect.
        auto sourceWidth = sourceRect->right - sourceRect->left;
        auto sourceHeight = sourceRect->bottom - sourceRect->top;

        D2D1_RECT_U destRect{ destPoint.x, destPoint.y, destPoint.x + sourceWidth, destPoint.y + sourceHeight };

        // D2D validates block alignment internally, but preparing our own
        // subrectangles allows us to provide a specific error message, and
        // also computes size info in case we need to do our own CPU copy.
        BitmapSubRectangle toRect(toD2dBitmap, destRect);
        BitmapSubRectangle fromRect(fromD2dBitmap, *sourceRect);

        if (toRect.GetFormat() != fromRect.GetFormat())
        {
            ThrowHR(E_INVALIDARG, Strings::BitmapFormatsDiffer);
        }

        // Are both bitmaps on the same device?
        ComPtr<ICanvasDevice> toDevice;
        ComPtr<ICanvasDevice> fromDevice;

        ThrowIfFailed(As<ICanvasResourceCreator>(to)->get_Device(&toDevice));
        ThrowIfFailed(As<ICanvasResourceCreator>(from)->get_Device(&fromDevice));

        if (IsSameInstance(toDevice.Get(), fromDevice.Get()))
        {
            // Tell D2D to copy for us (typically done via GPU HW).
            ThrowIfFailed(toD2dBitmap->CopyFromBitmap(&destPoint, fromD2dBitmap.Get(), sourceRect));
        }
        else
        {
            // Devices differ, so we must copy via system memory.
            ScopedBitmapMappedPixelAccess fromAccess(fromDevice.Get(), fromD2dBitmap.Get(), sourceRect);

            ThrowIfFailed(toD2dBitmap->CopyFromMemory(&destRect, fromAccess.GetLockedData(), fromAccess.GetStride()));
        }
    }

    ActivatableClassWithFactory(CanvasBitmap, CanvasBitmapFactory);
}}}}
