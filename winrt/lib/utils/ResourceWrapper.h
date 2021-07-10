// Copyright (c) Microsoft Corporation. All rights reserved.
//
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.

#pragma once

namespace ABI { namespace Microsoft { namespace Graphics { namespace Canvas
{
    //
    // ResourceWrapper is a helper for implementing WinRT types that are
    // conceptually wrappers around a native resource.
    //

#define RESOURCE_WRAPPER_RUNTIME_CLASS(TResource, TWrapper, TWrapperInterface, ...)         \
    public RuntimeClass<                                                                    \
        RuntimeClassFlags<WinRtClassicComMix>,                                              \
        TWrapperInterface,                                                                  \
        ChainInterfaces<                                                                    \
            MixIn<TWrapper, ResourceWrapper<TResource, TWrapper, TWrapperInterface>>,       \
            ABI::Windows::Foundation::IClosable,                                            \
            CloakedIid<ABI::Microsoft::Graphics::Canvas::ICanvasResourceWrapperNative>>,    \
        __VA_ARGS__>,                                                                       \
    public ResourceWrapper<TResource, TWrapper, TWrapperInterface>


    template<typename TResource, typename TWrapper, typename TWrapperInterface>
    class ResourceWrapper : public ABI::Windows::Foundation::IClosable, 
                            public ABI::Microsoft::Graphics::Canvas::ICanvasResourceWrapperNative,
                            private LifespanTracker<TWrapper>
    {
        ClosablePtr<TResource> m_resource;
        //Store the identity of the wrapper so we can safely use it in the destructor (ReleaseResource).
        IUnknown* m_wrapperIdentity;

    protected:
        ResourceWrapper(TResource* resource)
            : ResourceWrapper(resource, GetOuterInspectable())
        { }

        ResourceWrapper(TResource* resource, IInspectable* outerInspectable)
            : m_resource(resource)
        {
            if (resource)
            {
                m_wrapperIdentity = AsUnknown(outerInspectable).Get();
                ResourceManager::Add(resource, outerInspectable, m_wrapperIdentity);
            }
        }

        virtual ~ResourceWrapper()
        {
            Close();
        }

        void ReleaseResource()
        {
            if (m_resource)
            {
                auto resource = m_resource.Close();

                ResourceManager::Remove(resource.Get(), m_wrapperIdentity);
            }
        }

        void SetResource(TResource* resource)
        {
            ReleaseResource();

            if (resource)
            {
                m_resource = resource;

                ResourceManager::Add(resource, GetOuterInspectable(), m_wrapperIdentity);
            }
        }

    public:
        ComPtr<TResource> const& GetResource()
        {
            return m_resource.EnsureNotClosed();
        }

        ComPtr<TResource> const& MaybeGetResource()
        {
            return m_resource.UncheckedGet();
        }

        bool HasResource()
        {
            return (bool)m_resource;
        }

        //
        // IClosable
        //

        IFACEMETHODIMP Close() override
        {
            return ExceptionBoundary(
                [&]
                {
                    ReleaseResource();
                });
        }

        //
        // ICanvasResourceWrapperNative
        //

        IFACEMETHODIMP GetNativeResource(ICanvasDevice* device, float dpi, REFIID iid, void** outResource) override
        {
            return ExceptionBoundary(
                [&]
                {
                    CheckAndClearOutPointer(outResource);

                    auto& resource = GetResource();

                    // Validation methods are overloaded to provide compile time dispatch, so they only actually
                    // bother to check anything if TWrapper implements ICanvasResourceWrapperWith(Device|Dpi).
                    auto wrapper = static_cast<TWrapper*>(this);

                    ValidateDevice(wrapper, device);
                    ValidateDpi(wrapper, dpi);

                    ThrowIfFailed(resource.CopyTo(iid, outResource));
                });
        }

    private:
        // Forward validation requests for types that need them to the ResourceManager.
        static void ValidateDevice(ICanvasResourceWrapperWithDevice* wrapper, ICanvasDevice* device)
        {
            ResourceManager::ValidateDevice(wrapper, device);
        }

        static void ValidateDpi(ICanvasResourceWrapperWithDpi* wrapper, float dpi)
        {
            ResourceManager::ValidateDpi(wrapper, dpi);
        }

        // No-op validation requests for other types.
        static void ValidateDevice(void*, ICanvasDevice*) { }
        static void ValidateDpi(void*, float) { }


        // Looks up the outermost IInspectable of the runtime class that has mixed in this ResourceWrapper
        // instance. It is important to use the right IInspectable (of which there are several copies
        // due to multiple inheritance) because when the ResourceWrapper constructor executes, things
        // are not yet initialized far enough for QI or AsWeak to work from any of the other versions.
        //
        // outer_inspectable_t is a typedef so it can be customized by types with special needs (eg. CanvasBitmap).

        typedef TWrapperInterface outer_inspectable_t;

        IInspectable* GetOuterInspectable()
        {
            TWrapper* wrapper = static_cast<TWrapper*>(this);
            TWrapper::outer_inspectable_t* outer = wrapper;
            return outer;
        }
    };
}}}}
