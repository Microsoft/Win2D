// Copyright (c) Microsoft Corporation. All rights reserved.
//
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.

#include "pch.h"
#include "brushes/CanvasSolidColorBrush.h"
#include "brushes/CanvasLinearGradientBrush.h"
#include "brushes/CanvasRadialGradientBrush.h"
#include "drawing/CanvasGradientMesh.h"
#include "effects/ColorManagementProfile.h"
#include "effects/EffectTransferTable3D.h"
#include "geometry/CanvasCachedGeometry.h"
#include "images/CanvasCommandList.h"
#include "images/CanvasVirtualBitmap.h"
#include "text/CanvasFontFace.h"
#include "text/CanvasFontSet.h"
#include "text/CanvasTextLayout.h"
#include "text/CanvasTextRenderingParameters.h"
#include "text/CanvasTypography.h"
#include "text/CanvasTextAnalyzer.h"
#include "text/CanvasNumberSubstitution.h"
#include "svg/CanvasSvgDocument.h"
#include "svg/CanvasSvgElement.h"
#include "svg/CanvasSvgPaintAttribute.h"
#include "svg/CanvasSvgPathAttribute.h"
#include "svg/CanvasSvgPointsAttribute.h"
#include "svg/CanvasSvgStrokeDashArrayAttribute.h"


std::unordered_map<IUnknown*, WeakRef> ResourceManager::m_resources;
std::recursive_mutex ResourceManager::m_mutex;

std::unordered_multiset<IUnknown*> ResourceManager::m_wrappingResources;
std::unordered_set<IUnknown*> ResourceManager::m_creatingWrappers;

// When adding new types here, please also update the "Types that support interop" table in winrt\docsrc\Interop.aml.
std::vector<ResourceManager::TryCreateFunction> ResourceManager::tryCreateFunctions =
{
    TryCreate<ID2D1Device1,                CanvasDevice,                      MakeWrapper>,
    TryCreate<ID2D1DeviceContext1,         CanvasDrawingSession,              MakeWrapper>,
    TryCreate<ID2D1Bitmap1,                CanvasRenderTarget,                MakeWrapperWithDevice,  IsRenderTargetBitmap>,
    TryCreate<ID2D1Bitmap1,                CanvasBitmap,                      MakeWrapperWithDevice>,
    TryCreate<ID2D1CommandList,            CanvasCommandList,                 MakeWrapperWithDevice>,
    TryCreate<IDXGISwapChain1,             CanvasSwapChain,                   MakeWrapperWithDeviceAndDpi>,
    TryCreate<ID2D1Geometry,               CanvasGeometry,                    MakeWrapperWithDevice>,
    TryCreate<ID2D1GeometryRealization,    CanvasCachedGeometry,              MakeWrapperWithDevice>,
    TryCreate<DWriteTextLayoutType,        CanvasTextLayout,                  MakeWrapperWithDevice>,
    TryCreate<IDWriteTextFormat1,          CanvasTextFormat,                  MakeWrapper>,
    TryCreate<ID2D1StrokeStyle1,           CanvasStrokeStyle,                 MakeWrapper>,
    TryCreate<ID2D1SolidColorBrush,        CanvasSolidColorBrush,             MakeWrapperWithDevice>,
    TryCreate<ID2D1LinearGradientBrush,    CanvasLinearGradientBrush,         MakeWrapperWithDevice>,
    TryCreate<ID2D1RadialGradientBrush,    CanvasRadialGradientBrush,         MakeWrapperWithDevice>,
    TryCreate<ID2D1ImageBrush,             CanvasImageBrush,                  MakeWrapperWithDevice>,
    TryCreate<ID2D1BitmapBrush1,           CanvasImageBrush,                  MakeWrapperWithDevice>,
#if WINVER > _WIN32_WINNT_WINBLUE
    TryCreate<ID2D1GradientMesh,           CanvasGradientMesh,                MakeWrapperWithDevice>,
    TryCreate<ID2D1ImageSource,            CanvasVirtualBitmap,               MakeWrapperWithDevice>,
    TryCreate<ID2D1TransformedImageSource, CanvasVirtualBitmap,               MakeWrapperWithDevice>,
    TryCreate<ID2D1LookupTable3D,          EffectTransferTable3D,             MakeWrapperWithDevice>,
    TryCreate<IDWriteRenderingParams3,     CanvasTextRenderingParameters,     MakeWrapper>,
    TryCreate<IDWriteFontSet,              CanvasFontSet,                     MakeWrapper>,
    TryCreate<IDWriteFontFaceReference,    CanvasFontFace,                    MakeWrapper>,
    TryCreate<ID2D1SvgDocument,            CanvasSvgDocument,                 MakeWrapperWithDevice>,
    TryCreate<ID2D1SvgElement,             CanvasSvgTextElement,              MakeWrapperWithDevice,  IsSvgTextElement>,
    TryCreate<ID2D1SvgElement,             CanvasSvgNamedElement,             MakeWrapperWithDevice>,

    TryCreate<ID2D1SvgPaint,               CanvasSvgPaintAttribute,           MakeWrapperWithDevice>,
    TryCreate<ID2D1SvgPathData,            CanvasSvgPathAttribute,            MakeWrapperWithDevice>,
    TryCreate<ID2D1SvgPointCollection,     CanvasSvgPointsAttribute,          MakeWrapperWithDevice>,
    TryCreate<ID2D1SvgStrokeDashArray,     CanvasSvgStrokeDashArrayAttribute, MakeWrapperWithDevice>,
#else
    TryCreate<IDWriteRenderingParams2,     CanvasTextRenderingParameters, MakeWrapper>,
    TryCreate<IDWriteFontCollection,       CanvasFontSet,                 MakeWrapper>,
    TryCreate<IDWriteFont2,                CanvasFontFace,                MakeWrapper>,
#endif
    TryCreate<IDWriteTypography,           CanvasTypography,              MakeWrapper>,
    TryCreate<IDWriteNumberSubstitution,   CanvasNumberSubstitution,      MakeWrapper>,
    TryCreate<ID2D1ColorContext,           ColorManagementProfile,        MakeWrapperWithDevice>,

    // Effects get their very own try-create function. These are special because ID2D1Effect
    // can map to many different Win2D wrapper types depending on its D2D1_PROPERTY_CLSID.
    CanvasEffect::TryCreateEffect
};


// Called by the ResourceWrapper constructor, to add itself to the interop mapping table.
void ResourceManager::Add(IUnknown* resource, IInspectable* wrapper, IUnknown* wrapperIdentity)
{
    ComPtr<IUnknown> resourceIdentity = AsUnknown(resource);

    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    //If this resource is being wrapped by GetOrCreate, then add wrapperIdentity to m_creatingWrappers instead of adding the resource to m_resources.
    if (m_wrappingResources.find(resourceIdentity.Get()) != m_wrappingResources.end()) {
        m_creatingWrappers.insert(wrapperIdentity);
        return;
    }

    auto result = m_resources.insert(std::make_pair(resourceIdentity.Get(), AsWeak(wrapper)));

    if (!result.second)
        ThrowHR(E_UNEXPECTED);
}


// Called by ResourceWrapper::Close, to remove itself from the interop mapping table.
void ResourceManager::Remove(IUnknown* resource, IUnknown* wrapperIdentity)
{
    ComPtr<IUnknown> resourceIdentity = AsUnknown(resource);

    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    //If this wrapper is being created by GetOrCreate, remove from m_creatingWrappers intead of removing the resource from m_resources.
    if (m_creatingWrappers.erase(wrapperIdentity) > 0) {
        return;
    }

    auto result = m_resources.erase(resourceIdentity.Get());

    if (result != 1)
        ThrowHR(E_UNEXPECTED);
}


ComPtr<IInspectable> ResourceManager::GetOrCreate(ICanvasDevice* device, IUnknown* resource, float dpi)
{
    ComPtr<IUnknown> resourceIdentity = AsUnknown(resource);
    ComPtr<IInspectable> wrapper;

    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    // Do we already have a wrapper around this resource?
    auto it = m_resources.find(resourceIdentity.Get());
    if (it != m_resources.end())
    {
        wrapper = LockWeakRef<IInspectable>(it->second);
    }

    // Create a new wrapper instance?
    if (!wrapper)
    {
        //Add the resource to the list of resources being wrapped, then unlock to avoid deadlock scenarios
        m_wrappingResources.insert(resourceIdentity.Get());
        lock.unlock();

        //Ensure the resource is removed from m_wrappingResources on leaving scope.
        auto endWrapWarden = MakeScopeWarden([&] { 
            std::lock_guard<std::recursive_mutex> endWrapLock(m_mutex);
            auto endWrapIt = m_wrappingResources.find(resourceIdentity.Get());
            if (endWrapIt != m_wrappingResources.end()) {
                m_wrappingResources.erase(endWrapIt);
            }
        });

        for (auto& tryCreateFunction : tryCreateFunctions)
        {
            if (tryCreateFunction(device, resource, dpi, &wrapper))
            {
                break;
            }
        }

        // Fail if we did not find a way to wrap this type.
        if (!wrapper)
        {
            ThrowHR(E_NOINTERFACE, Strings::ResourceManagerUnknownType);
        }

        lock.lock();
        //Check to see if another wrapper was created simultaneously for this resource while we were creating a wrapper.
        ComPtr<IInspectable> existingWrapper;
        it = m_resources.find(resourceIdentity.Get());
        if (it != m_resources.end())
        {
            existingWrapper = LockWeakRef<IInspectable>(it->second);
        }
        if (existingWrapper) {
            //If so, unlock and use the other wrapper.
            lock.unlock();
            wrapper = existingWrapper;
        } else {
            //Else, remove the wrapper from the m_creatingWrappers set and add the resource to m_resources.
            m_creatingWrappers.erase(AsUnknown(wrapper.Get()).Get());
            auto result = m_resources.insert(std::make_pair(resourceIdentity.Get(), AsWeak(wrapper.Get())));
            if (!result.second) {
                ThrowHR(E_UNEXPECTED);
            }
            lock.unlock();
        }
    } else {
        lock.unlock();
    }

    // Validate that the object we got back reports the expected device and DPI.
    ValidateDevice(wrapper.Get(), device);
    ValidateDpi(wrapper.Get(), dpi);

    return wrapper;
}


// Validation rules:
//  - If the caller specified a device or dpi, and the wrapper has device/dpi, these must match.
//  - If the caller specified device or dpi but the wrapper has no device/dpi, we'll allow that, ignoring the parameter.
//  - If the caller did not specify device or dpi, we'll accept any existing wrapper instance regardless of its device/dpi.


void ResourceManager::ValidateDevice(IInspectable* wrapper, ICanvasDevice* device)
{
    auto wrapperWithDevice = MaybeAs<ICanvasResourceWrapperWithDevice>(wrapper);

    if (wrapperWithDevice)
    {
        ValidateDevice(wrapperWithDevice.Get(), device);
    }
}


void ResourceManager::ValidateDevice(ICanvasResourceWrapperWithDevice* wrapper, ICanvasDevice* device)
{
    if (device)
    {
        ComPtr<ICanvasDevice> wrapperDevice;
        ThrowIfFailed(wrapper->get_Device(&wrapperDevice));

        if (device != wrapperDevice.Get())
        {
            ThrowHR(E_INVALIDARG, Strings::ResourceManagerWrongDevice);
        }
    }
}


void ResourceManager::ValidateDpi(IInspectable* wrapper, float dpi)
{
    auto wrapperWithDpi = MaybeAs<ICanvasResourceWrapperWithDpi>(wrapper);

    if (wrapperWithDpi)
    {
        ValidateDpi(wrapperWithDpi.Get(), dpi);
    }
}


void ResourceManager::ValidateDpi(ICanvasResourceWrapperWithDpi* wrapper, float dpi)
{
    if (dpi != 0)
    {
        float wrapperDpi;
        ThrowIfFailed(wrapper->get_Dpi(&wrapperDpi));

        if (dpi != wrapperDpi)
        {
            ThrowHR(E_INVALIDARG, Strings::ResourceManagerWrongDpi);
        }
    }
}


void ResourceManager::RegisterType(TryCreateFunction tryCreate)
{
    assert(std::find(tryCreateFunctions.begin(), tryCreateFunctions.end(), tryCreate) == tryCreateFunctions.end());

    tryCreateFunctions.push_back(tryCreate);
}


void ResourceManager::UnregisterType(TryCreateFunction tryCreate)
{
    auto it = std::find(tryCreateFunctions.begin(), tryCreateFunctions.end(), tryCreate);

    assert(it != tryCreateFunctions.end());

    tryCreateFunctions.erase(it);
}
