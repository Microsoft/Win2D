// Copyright (c) Microsoft Corporation. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use these files except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations
// under the License.

#pragma once

#include <RegisteredEvent.h>
#include "RecreatableDeviceManager.h"

namespace ABI { namespace Microsoft { namespace Graphics { namespace Canvas
{
    using namespace ::Microsoft::WRL::Wrappers;
    using namespace ABI::Windows::ApplicationModel;
    using namespace ABI::Windows::Graphics::Display;
    using namespace ABI::Windows::UI::Core;
    using namespace ABI::Windows::UI::Xaml;
    using namespace ABI::Windows::UI::Xaml::Controls;

    //
    // See CanvasControl.h / CanvasAnimatedControl.h for the shape of TRAITS.
    //

    typedef ITypedEventHandler<DisplayInformation*, IInspectable*> DpiChangedEventHandler;

    template<typename TRAITS>
    class IBaseControlAdapter
    {
    public:
        virtual std::pair<ComPtr<IInspectable>, ComPtr<IUserControl>> CreateUserControl(IInspectable* canvasControl) = 0;
        virtual std::unique_ptr<IRecreatableDeviceManager<TRAITS>> CreateRecreatableDeviceManager() = 0;
        virtual RegisteredEvent AddApplicationSuspendingCallback(IEventHandler<SuspendingEventArgs*>*) = 0;
        virtual float GetLogicalDpi() = 0;

        virtual RegisteredEvent AddDpiChangedCallback(DpiChangedEventHandler* handler) = 0;

        virtual ComPtr<IWindow> GetWindowOfCurrentThread() = 0;

        //
        // Helpers for adding member-function callbacks
        //

#define CB_HELPER(NAME)                                                 \
        template<typename T, typename METHOD, typename... EXTRA_ARGS>   \
        RegisteredEvent NAME(T* obj, METHOD method, EXTRA_ARGS... extraArgs) \
        {                                                               \
            return AddCallback(&IBaseControlAdapter::NAME, obj, method, extraArgs...); \
        }

        CB_HELPER(AddApplicationSuspendingCallback);
        CB_HELPER(AddDpiChangedCallback);

#undef CB_HELPER

        template<typename CLASS, typename DELEGATE, typename T, typename METHOD, typename... EXTRA_ARGS>
        RegisteredEvent AddCallback(RegisteredEvent(CLASS::* addMethod)(DELEGATE*, EXTRA_ARGS...), T* obj, METHOD method, EXTRA_ARGS... extraArgs)
        {
            auto callback = Callback<DELEGATE>(obj, method);
            CheckMakeResult(callback);
            return (this->*addMethod)(callback.Get(), extraArgs...);
        }
    };


    template<typename TRAITS>
    class BaseControl
        : public Implements<
            typename TRAITS::controlInterface_t, 
            ICanvasResourceCreator, 
            ICanvasResourceCreatorWithDpi>
    {
    public:
        typedef typename TRAITS::control_t                     control_t;
        typedef typename TRAITS::adapter_t                     adapter_t;
        typedef typename TRAITS::renderTarget_t                renderTarget_t;
        typedef typename TRAITS::drawEventHandler_t            drawEventHandler_t;
        typedef typename TRAITS::createResourcesEventHandler_t createResourcesEventHandler_t;
        typedef typename TRAITS::drawEventArgs_t               drawEventArgs_t;

    protected:
        struct RenderTarget
        {
            ComPtr<renderTarget_t> Target;
            CanvasBackground BackgroundMode;
            float Dpi;
            Size Size;
        };

    private:
        // The current window is thread local.  We grab this on construction
        // since this will happen on the correct thread.  From then on we use
        // this stored value since we can't always be sure that we'll always be
        // called from that window's thread.
        ComPtr<IWindow> m_window;

        std::shared_ptr<adapter_t> m_adapter;
        std::unique_ptr<IRecreatableDeviceManager<TRAITS>> m_recreatableDeviceManager;
        bool m_isLoaded;
        float m_dpi;

        EventSource<drawEventHandler_t, InvokeModeOptions<StopOnFirstError>> m_drawEventList;

        RegisteredEvent m_applicationSuspendingEventRegistration;
        RegisteredEvent m_dpiChangedEventRegistration;

        std::mutex m_mutex;
        Color m_clearColor;     // protected by m_mutex

        RenderTarget m_currentRenderTarget;

    public:
        BaseControl(std::shared_ptr<adapter_t> adapter)
            : m_adapter(adapter)
            , m_recreatableDeviceManager(adapter->CreateRecreatableDeviceManager())
            , m_isLoaded(false)
            , m_window(adapter->GetWindowOfCurrentThread())
            , m_dpi(adapter->GetLogicalDpi())
            , m_clearColor{}
            , m_currentRenderTarget{}
        {
            CreateBaseClass();
            RegisterEventHandlers();
        }

        IFACEMETHODIMP put_ClearColor(Color value) override
        {
            return ExceptionBoundary(
                [&]
                {
                    SetClearColor(value);
                });
        }

        IFACEMETHODIMP get_ClearColor(Color* value) override
        {
            return ExceptionBoundary(
                [&]
                {
                    CheckInPointer(value);
                    *value = GetClearColor();
                });
        }
        
        IFACEMETHODIMP add_CreateResources(
            createResourcesEventHandler_t* value,
            EventRegistrationToken *token) override
        {
            return ExceptionBoundary(
                [&]
                {
                    CheckInPointer(token);

                    GetControl()->CheckThreadRestrictionIfNecessary();

                    *token = m_recreatableDeviceManager->AddCreateResources(GetControl(), value);
                });
        }        
    
        IFACEMETHODIMP remove_CreateResources(
            EventRegistrationToken token) override
        {
            return ExceptionBoundary(
                [&]
                {
                    GetControl()->CheckThreadRestrictionIfNecessary();
                    m_recreatableDeviceManager->RemoveCreateResources(token);
                });
        }

        IFACEMETHODIMP add_Draw(
            drawEventHandler_t* value,
            EventRegistrationToken* token) override
        {
            return ExceptionBoundary(
                [&]
                {
                    ThrowIfFailed(m_drawEventList.Add(value, token));
                    Changed();
                });
        }

        IFACEMETHODIMP remove_Draw(
            EventRegistrationToken token) override
        {
            return ExceptionBoundary(
                [&]
                {
                    ThrowIfFailed(m_drawEventList.Remove(token));
                });
        }

        IFACEMETHODIMP get_ReadyToDraw(
            boolean* value) override
        {
            return ExceptionBoundary(
                [=]
                {
                    GetControl()->CheckThreadRestrictionIfNecessary();
                    *value = m_recreatableDeviceManager->IsReadyToDraw();
                });
        }

        //
        // ICanvasResourceCreatorWithDpi
        //

        IFACEMETHODIMP get_Device(ICanvasDevice** value) override
        {
            return ExceptionBoundary(
                [&]
                {
                    CheckAndClearOutPointer(value);

                    auto& device = m_recreatableDeviceManager->GetDevice();

                    if (device)
                        ThrowIfFailed(device.CopyTo(value));
                    else
                        ThrowHR(E_INVALIDARG, HStringReference(Strings::CanvasDeviceGetDeviceWhenNotCreated).Get());
                });
        }

        IFACEMETHODIMP get_Dpi(float* dpi) override
        {
            return ExceptionBoundary(
                [&]
                {
                    CheckInPointer(dpi);
                    *dpi = m_dpi;
                });
        }

        IFACEMETHODIMP ConvertPixelsToDips(int pixels, float* dips) override
        {
            return ExceptionBoundary(
                [&]
                {
                    CheckInPointer(dips);
                    *dips = PixelsToDips(pixels, m_dpi);
                });
        }

        IFACEMETHODIMP ConvertDipsToPixels(float dips, int* pixels) override
        {
            return ExceptionBoundary(
                [&]
                {
                    CheckInPointer(pixels);
                    *pixels = DipsToPixels(dips, m_dpi);
                });
        }

    protected:
        virtual void CreateOrUpdateRenderTarget(
            ICanvasDevice* device,
            CanvasBackground newBackgroundMode,
            float newDpi,
            Size newSize,
            RenderTarget* renderTarget) = 0;

        virtual ComPtr<drawEventArgs_t> CreateDrawEventArgs(
            ICanvasDrawingSession* drawingSession) = 0;

        virtual void Changed() = 0;

        //
        // TODO #3276: Combine ChangedClearColor and ChangedSize into one function.
        //
        virtual void ChangedClearColor(bool differentAlphaMode) = 0;

        virtual void ChangedSize() = 0;

        virtual void Unloaded() = 0;

        virtual void CheckThreadRestrictionIfNecessary() = 0;

        IWindow* GetWindow()
        { 
            return m_window.Get(); 
        }

        bool IsWindowVisible() const
        {
            boolean visible;

            HRESULT hr = m_window->get_Visible(&visible);

            if (hr == E_FAIL)
            {
                // In design mode get_Visible will return E_FAIL.  In this case we
                // just treat the window as visible.
                return true;
            }
            else
            {
                ThrowIfFailed(hr);
                return !!visible;
            }
        }

        bool IsLoaded() const
        {
            return m_isLoaded;
        }

        std::shared_ptr<adapter_t> GetAdapter()
        {
            return m_adapter;
        }

        Size GetActualSize()
        {
            auto frameworkElement = As<IFrameworkElement>(GetControl()->GetComposableBase());
            double actualWidth;
            double actualHeight;
            
            ThrowIfFailed(frameworkElement->get_ActualWidth(&actualWidth));
            ThrowIfFailed(frameworkElement->get_ActualHeight(&actualHeight));
            
            return Size{ static_cast<float>(actualWidth), static_cast<float>(actualHeight) };
        }

        void ResetRenderTarget()
        {
            m_currentRenderTarget = RenderTarget{};
            Changed();
        }

        //
        // The entry point for rendering a single frame.  Ultimately, this
        // arranges for RenderWithTarget() to get called.
        //
        template<typename FN>
        void RunWithRenderTarget(Size const& renderTargetSize, FN&& fn)
        {
            m_recreatableDeviceManager->RunWithDevice(GetControl(),
                [&] (ICanvasDevice* device, RunWithDeviceFlags flags)
                {
                    RunWithRenderTarget(renderTargetSize, device, flags, fn);
                });
        }

        //
        // Creates a drawing session, optionally calls the draw handlers and
        // finally closes the drawing session.
        //
        void Draw(renderTarget_t* target, Color const& clearColor, bool callDrawHandlers)
        {
            ComPtr<ICanvasDrawingSession> drawingSession;
            ThrowIfFailed(target->CreateDrawingSession(clearColor, &drawingSession));
            if (callDrawHandlers)
            {
                auto drawEventArgs = GetControl()->CreateDrawEventArgs(drawingSession.Get());
                ThrowIfFailed(m_drawEventList.InvokeAll(GetControl(), drawEventArgs.Get()));
            }

            ThrowIfFailed(As<IClosable>(drawingSession)->Close());
        }        

        Color GetClearColor()
        {
            auto lock = GetLock();
            return m_clearColor;
        }

    private:
        std::unique_lock<std::mutex> GetLock()
        {
            return std::unique_lock<std::mutex>(m_mutex);
        }

        void SetClearColor(Color const& value)
        {
            auto lock = GetLock();

            if (m_clearColor.A == value.A &&
                m_clearColor.R == value.R &&
                m_clearColor.G == value.G &&
                m_clearColor.B == value.B)
            {
                return;
            }

            const bool wasOpaque = m_clearColor.A == 255;
            const bool isOpaque = value.A == 255;
            const bool differentAlphaMode = wasOpaque != isOpaque;

            m_clearColor = value;

            lock.unlock();

            ChangedClearColor(differentAlphaMode);
        }

        template<typename FN>
        void RunWithRenderTarget(Size const& renderTargetSize, ICanvasDevice* device, RunWithDeviceFlags flags, FN&& fn)
        {
            auto clearColor = GetClearColor();
            bool areResourcesCreated = !IsSet(flags, RunWithDeviceFlags::ResourcesNotCreated);

            UpdateCurrentRenderTarget(device, renderTargetSize, flags, clearColor);
            fn(m_currentRenderTarget.Target.Get(), device, clearColor, areResourcesCreated);
        }

        void UpdateCurrentRenderTarget(ICanvasDevice* device, Size const& renderTargetSize, RunWithDeviceFlags flags, Color const& clearColor)
        {
            if (IsSet(flags, RunWithDeviceFlags::NewlyCreatedDevice))
                m_currentRenderTarget = RenderTarget{};

            auto backgroundMode = clearColor.A == 255 ? CanvasBackground::Opaque : CanvasBackground::Transparent;

            // CanvasControl will recreate its CanvasImageSource if any of these
            // properties are different.  CanvasAnimatedControl is able to
            // resize buffers / set SourceSize in some cases to avoid
            // recreating.
            GetControl()->CreateOrUpdateRenderTarget(
                device,
                backgroundMode,
                m_dpi,
                renderTargetSize,
                &m_currentRenderTarget);
        }

        control_t* GetControl()
        {
            return static_cast<TRAITS::control_t*>(this);
        }

        void CreateBaseClass()
        {
            auto base = GetAdapter()->CreateUserControl(As<IInspectable>(GetControl()).Get());

            ThrowIfFailed(GetControl()->SetComposableBasePointers(base.first.Get(), base.second.Get()));
        }

        void RegisterEventHandlers()
        {
            auto frameworkElement = As<IFrameworkElement>(GetControl());

            RegisterEventHandlerOnSelf(
                frameworkElement, 
                &IFrameworkElement::add_Loaded, 
                &BaseControl::OnLoaded);

            RegisterEventHandlerOnSelf(
                frameworkElement,
                &IFrameworkElement::add_Unloaded,
                &BaseControl::OnUnloaded);

            RegisterEventHandlerOnSelf(frameworkElement, &IFrameworkElement::add_SizeChanged, &BaseControl::OnSizeChanged);
            
            m_applicationSuspendingEventRegistration = m_adapter->AddApplicationSuspendingCallback(
                this, 
                &BaseControl::OnApplicationSuspending);

            m_dpiChangedEventRegistration = m_adapter->AddDpiChangedCallback(this, &BaseControl::OnDpiChanged);

            m_recreatableDeviceManager->SetChangedCallback(
                [=]
                {
                    Changed();
                });
        }

        template<typename T, typename DELEGATE, typename HANDLER>
        void RegisterEventHandlerOnSelf(
            ComPtr<T> const& self,
            HRESULT(STDMETHODCALLTYPE T::* addMethod)(DELEGATE*, EventRegistrationToken*),
            HANDLER handler)
        {
            // 'self' here is assumed to be something that was obtained by QI'ing
            // ourselves (or our component base).  In this case we don't need to
            // unregister the event on destruction since once we're destroyed these
            // events won't get fired.
            EventRegistrationToken tokenThatIsThrownAway{};

            auto callback = Callback<DELEGATE>(this, handler);
            CheckMakeResult(callback);

            ThrowIfFailed((self.Get()->*addMethod)(callback.Get(), &tokenThatIsThrownAway));
        }

        HRESULT OnLoaded(IInspectable*, IRoutedEventArgs*)
        {
            return ExceptionBoundary(
                [&]
                {
                    m_isLoaded = true;
                    Changed();
                });
        }

        HRESULT OnUnloaded(IInspectable*, IRoutedEventArgs*)
        {
            return ExceptionBoundary(
                [&]
                {
                    GetControl()->Unloaded();
                    m_isLoaded = false;
                });
        }

        HRESULT OnSizeChanged(IInspectable*, ISizeChangedEventArgs* args)
        {
            return ExceptionBoundary(
                [&]
                {
                    //
                    // OnSizeChanged can get called multiple times.  We only want to
                    // invalidate if it represents a size change from what the
                    // control was last set to.
                    //
                    Size newSize{};
                    ThrowIfFailed(args->get_NewSize(&newSize));

                    if (newSize != m_currentRenderTarget.Size)
                    {
                        ChangedSize();
                    }
                });
        }

        HRESULT OnApplicationSuspending(IInspectable*, ISuspendingEventArgs*)
        {
            return ExceptionBoundary(
                [&]
                {
                    auto& device = m_recreatableDeviceManager->GetDevice();
                    if (!device)
                        return;

                    auto direct3DDevice = As<IDirect3DDevice>(device);
                    ThrowIfFailed(direct3DDevice->Trim());
                });
        }

        HRESULT OnDpiChanged(IDisplayInformation*, IInspectable*)
        {
            return ExceptionBoundary(
                [&]
            {
                float newDpi = m_adapter->GetLogicalDpi();

                if (newDpi != m_dpi)
                {
                    m_dpi = newDpi;
                    m_recreatableDeviceManager->SetDpiChanged();
                }
            });
        }
    };

}}}}
