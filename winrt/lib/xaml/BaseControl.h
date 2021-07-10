// Copyright (c) Microsoft Corporation. All rights reserved.
//
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.

#pragma once

#include "RemoveFromVisualTree.h"
#include "utils/LockUtilities.h"

namespace ABI { namespace Microsoft { namespace Graphics { namespace Canvas { namespace UI { namespace Xaml
{
    using namespace ::Microsoft::WRL::Wrappers;
    using namespace ABI::Windows::ApplicationModel;
    using namespace ABI::Windows::Graphics::Display;
    using namespace ABI::Windows::UI::Core;
    using namespace ABI::Windows::UI::Xaml::Controls;
    using namespace ABI::Windows::UI::Xaml;
    using namespace ABI::Windows::UI;
    using namespace WinRTDirectX;

    //
    // See CanvasControl.h / CanvasAnimatedControl.h for the shape of TRAITS.
    //

    typedef ITypedEventHandler<DisplayInformation*, IInspectable*> DpiChangedEventHandler;
    typedef ITypedEventHandler<XamlRoot*, XamlRootChangedEventArgs*> XamlRootChangedEventHandler;

    template<typename TRAITS>
    class IBaseControlAdapter
    {
    public:
        virtual ~IBaseControlAdapter() = default;

        virtual bool IsDesignModeEnabled() = 0;

        virtual ComPtr<IInspectable> CreateUserControl(IInspectable* canvasControl) = 0;
        virtual std::unique_ptr<IRecreatableDeviceManager<TRAITS>> CreateRecreatableDeviceManager(IInspectable* parentControl) = 0;
        virtual RegisteredEvent AddApplicationSuspendingCallback(IEventHandler<SuspendingEventArgs*>*) = 0;
        virtual RegisteredEvent AddApplicationResumingCallback(IEventHandler<IInspectable*>*) = 0;
        virtual float GetLogicalDpi() = 0;

        virtual RegisteredEvent AddDpiChangedCallback(DpiChangedEventHandler* handler) = 0;

        virtual RegisteredEvent AddVisibilityChangedCallback(IWindowVisibilityChangedEventHandler* handler, IWindow* window) = 0;

        virtual RegisteredEvent AddXamlRootChangedCallback(XamlRootChangedEventHandler* handler, IXamlRoot* xamlRoot) = 0;

        virtual ComPtr<IWindow> GetWindowOfCurrentThread() = 0;

        //
        // Helpers for adding member-function callbacks
        //

#define CB_HELPER(NAME, DELEGATE)                                       \
        template<typename T, typename METHOD, typename... EXTRA_ARGS>   \
        RegisteredEvent NAME(T* obj, METHOD method, EXTRA_ARGS... extraArgs) \
        {                                                               \
            return AddCallback<DELEGATE>(&IBaseControlAdapter::NAME, obj, method, extraArgs...); \
        }

        CB_HELPER(AddApplicationSuspendingCallback, IEventHandler<SuspendingEventArgs*>);
        CB_HELPER(AddApplicationResumingCallback, IEventHandler<IInspectable*>);
        CB_HELPER(AddDpiChangedCallback, DpiChangedEventHandler);
        CB_HELPER(AddVisibilityChangedCallback, IWindowVisibilityChangedEventHandler);
        CB_HELPER(AddXamlRootChangedCallback, XamlRootChangedEventHandler);

#undef CB_HELPER

        template<typename DELEGATE, typename T, typename METHOD, typename... EXTRA_ARGS>
        RegisteredEvent AddCallback(RegisteredEvent(IBaseControlAdapter::* addMethod)(DELEGATE*, EXTRA_ARGS...), T* obj, METHOD method, EXTRA_ARGS... extraArgs)
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
            ICanvasResourceCreatorWithDpi>,
          private LifespanTracker<typename TRAITS::control_t>
    {
    public:
        typedef typename TRAITS::control_t                     control_t;
        typedef typename TRAITS::adapter_t                     adapter_t;
        typedef typename TRAITS::renderTarget_t                renderTarget_t;
        typedef typename TRAITS::createResourcesEventHandler_t createResourcesEventHandler_t;

    protected:
        struct RenderTarget
        {
            ComPtr<renderTarget_t> Target;
            CanvasAlphaMode AlphaMode;
            float Dpi;
            Size Size;
        };

    private:
        // The current window is thread local.  We grab this on construction
        // since this will happen on the correct thread.  From then on we use
        // this stored value since we can't always be sure that we'll always be
        // called from that window's thread.
        ComPtr<IWindow> m_window;

        // With Xaml islands, we use the XamlRoot for visibility and DPI change
        // notifications.
        ComPtr<IXamlRoot> m_xamlRoot;

        std::shared_ptr<adapter_t> m_adapter;
        std::unique_ptr<IRecreatableDeviceManager<TRAITS>> m_recreatableDeviceManager;
        std::atomic<int> m_loadedCount;
        bool m_isLoaded;
        bool m_isSuspended;
        bool m_isVisible;
        bool m_useSharedDevice;
        bool m_forceSoftwareRenderer;

        RegisteredEvent m_applicationSuspendingEventRegistration;
        RegisteredEvent m_applicationResumingEventRegistration;
        RegisteredEvent m_dpiChangedEventRegistration;
        RegisteredEvent m_windowVisibilityChangedEventRegistration;
        RegisteredEvent m_xamlRootChangedEventRegistration;
        RegisteredEvent m_deviceLostEventRegistration;

        std::mutex m_mutex;
        Size m_currentSize;       // protected by m_mutex
        Color m_clearColor;       // protected by m_mutex
        float m_logicalDpi;       // protected by m_mutex
        float m_customDpiScaling; // protected by m_mutex

        RenderTarget m_currentRenderTarget;

        WeakRef m_lastSeenParent;

        ComPtr<ICanvasDevice> m_customDevice;

    public:
        BaseControl(std::shared_ptr<adapter_t> adapter, bool useSharedDevice)
            : m_adapter(adapter)
            , m_recreatableDeviceManager(adapter->CreateRecreatableDeviceManager(GetControlInterface()))
            , m_loadedCount(0)
            , m_isLoaded(false)
            , m_isSuspended(false)
            , m_isVisible(true)
            , m_window(adapter->GetWindowOfCurrentThread())
            , m_logicalDpi(adapter->GetLogicalDpi())
            , m_customDpiScaling(1.0f)
            , m_useSharedDevice(useSharedDevice)
            , m_forceSoftwareRenderer(false)
            , m_currentSize{}
            , m_clearColor{}
            , m_currentRenderTarget{}
        {
            CreateBaseClass();
            RegisterEventHandlersOnSelf();
        }

        virtual ~BaseControl() = default;

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

                    CheckIsOnUIThread();

                    *token = m_recreatableDeviceManager->AddCreateResources(GetControl(), value);
                });
        }        
    
        IFACEMETHODIMP remove_CreateResources(
            EventRegistrationToken token) override
        {
            return ExceptionBoundary(
                [&]
                {
                    CheckIsOnUIThread();
                    m_recreatableDeviceManager->RemoveCreateResources(token);
                });
        }

        IFACEMETHODIMP get_ReadyToDraw(
            boolean* value) override
        {
            return ExceptionBoundary(
                [=]
                {
                    CheckIsOnUIThread();
                    *value = IsReadyToDraw();
                });
        }

        IFACEMETHODIMP get_Size(Size* value) override
        {
            return ExceptionBoundary(
                [&]
                {
                    CheckInPointer(value);

                    *value = GetCurrentSize();
                });
        }

        IFACEMETHODIMP RemoveFromVisualTree() override
        {
            return ExceptionBoundary(
                [&]
                {
                    auto control = As<IUIElement>(GetControl()); //Do this first because the unit tests expect a failure when this cast fails.
                    if (!m_lastSeenParent) {
                        return;
                    }
                    RemoveFromVisualTreeImpl(LockWeakRef<IDependencyObject>(m_lastSeenParent).Get(), control.Get());
                });
        }

        IFACEMETHODIMP get_UseSharedDevice(
            boolean* value) override
        {
            return ExceptionBoundary(
                [&]
                {
                    CheckInPointer(value);

                    auto lock = GetLock();

                    *value = m_useSharedDevice;
                });
        }

        IFACEMETHODIMP put_UseSharedDevice(
            boolean value) override
        {
            return ExceptionBoundary(
                [&]
                {
                    auto lock = GetLock();

                    if (m_useSharedDevice == !!value)
                        return;

                    m_useSharedDevice = !!value;

                    lock.unlock();
                    
                    Changed(ChangeReason::DeviceCreationOptions);
                });
        }

        IFACEMETHODIMP get_ForceSoftwareRenderer(
            boolean* value) override
        {
            return ExceptionBoundary(
                [&]
                {
                    CheckInPointer(value);

                    auto lock = GetLock();

                    *value = m_forceSoftwareRenderer;
                });
        }

        IFACEMETHODIMP put_ForceSoftwareRenderer(
            boolean value) override
        {
            return ExceptionBoundary(
                [&]
                {
                    auto lock = GetLock();

                    if (m_forceSoftwareRenderer == !!value)
                        return;

                    m_forceSoftwareRenderer = !!value;

                    lock.unlock();
                    
                    Changed(ChangeReason::DeviceCreationOptions);
                });
        }

        IFACEMETHODIMP get_CustomDevice(
            ICanvasDevice** value) override
        {
            return ExceptionBoundary(
                [&]
                {
                    CheckAndClearOutPointer(value);

                    auto lock = GetLock();

                    if (m_customDevice)
                    {
                        ThrowIfFailed(m_customDevice.CopyTo(value));
                    }
                });
        }

        IFACEMETHODIMP put_CustomDevice(
            ICanvasDevice* value) override
        {
            return ExceptionBoundary(
                [&]
                {
                    //
                    // Passing null is valid here, and will clear out the custom device.
                    //

                    auto lock = GetLock();

                    bool isChanged = !IsSameInstance(m_customDevice.Get(), value);

                    if (isChanged)
                    {
                        m_customDevice = value;

                        lock.unlock();

                        Changed(ChangeReason::DeviceCreationOptions);
                    }
                });
        }        

        IFACEMETHODIMP get_DpiScale(float* value) override
        {
            return ExceptionBoundary(
                [&]
                {
                    CheckInPointer(value);

                    auto lock = GetLock();

                    *value = m_customDpiScaling;
                });
        }

        IFACEMETHODIMP put_DpiScale(float value) override
        {
            return ExceptionBoundary(
                [&]
                {
                    if (value <= 0)
                        ThrowHR(E_INVALIDARG);

                    auto lock = GetLock();

                    if (value != m_customDpiScaling)
                    {
                        m_customDpiScaling = value;

                        lock.unlock();

                        m_recreatableDeviceManager->SetDpiChanged();
                    }
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
                        ThrowHR(E_INVALIDARG, Strings::CanvasDeviceGetDeviceWhenNotCreated);
                });
        }

        IFACEMETHODIMP get_Dpi(float* dpi) override
        {
            return ExceptionBoundary(
                [&]
                {
                    CheckInPointer(dpi);

                    *dpi = GetEffectiveDpi(GetLock());
                });
        }

        IFACEMETHODIMP ConvertPixelsToDips(int pixels, float* dips) override
        {
            return ExceptionBoundary(
                [&]
                {
                    CheckInPointer(dips);

                    *dips = PixelsToDips(pixels, GetEffectiveDpi(GetLock()));
                });
        }

        IFACEMETHODIMP ConvertDipsToPixels(float dips, CanvasDpiRounding dpiRounding, int* pixels) override
        {
            return ExceptionBoundary(
                [&]
                {
                    CheckInPointer(pixels);

                    *pixels = DipsToPixels(dips, GetEffectiveDpi(GetLock()), dpiRounding);
                });
        }

        virtual void CreateOrUpdateRenderTarget(
            ICanvasDevice* device,
            CanvasAlphaMode newAlphaMode,
            float newDpi,
            Size newSize,
            RenderTarget* renderTarget) = 0;

        virtual void Changed(ChangeReason reason) = 0;

        virtual void Loaded() = 0;
        virtual void Unloaded() = 0;

        virtual void ApplicationSuspending(ISuspendingEventArgs* args) = 0;
        virtual void ApplicationResuming() = 0;
        virtual void WindowVisibilityChanged() = 0;

        control_t* GetControl()
        {
            return static_cast<TRAITS::control_t*>(this);
        }

        typename TRAITS::controlInterface_t* GetControlInterface()
        {
            return this;    // Narrowing conversion disambiguates between multiple possible IInspectable implementations.
        }

        void CheckIsOnUIThread()
        {
            // 
            // Verifies execution on this control's window's UI thread.
            // Certain methods should not be executed on other threads-
            // they are not designed to have a locking mechanism to
            // protect them.
            //

            //
            // IWindow will not be available in Xaml island scenarios, so prefer
            // IDependencyObject for getting the dispatcher.
            //
            ComPtr<ICoreDispatcher> dispatcher;
            auto control = GetControl();
            if (auto dependencyObject = MaybeAs<IDependencyObject>(control))
            {
                ThrowIfFailed(dependencyObject->get_Dispatcher(&dispatcher));
            }
            else
            {
                ThrowIfFailed(GetWindow()->get_Dispatcher(&dispatcher));
            }

            //
            // get_Dispatcher may succeed but return null if we're running in
            // the designer.  In this case we assume that we must be running on
            // the correct thread.
            //
            if (!dispatcher)
            {
                return;
            }

            boolean hasAccess;
            ThrowIfFailed(dispatcher->get_HasThreadAccess(&hasAccess));

            if (!hasAccess)
            {
                ThrowHR(RPC_E_WRONG_THREAD);
            }
        }

        IWindow* GetWindow()
        { 
            return m_window.Get(); 
        }

        bool IsReadyToDraw() const
        {
            return m_recreatableDeviceManager->IsReadyToDraw();
        }

        bool IsLoaded() const
        {
            return m_isLoaded;
        }

        bool IsSuspended() const
        {
            return m_isSuspended;
        }

        bool IsVisible() const
        {
            return m_isVisible;
        }

        std::shared_ptr<adapter_t> GetAdapter()
        {
            return m_adapter;
        }

        void ResetRenderTarget()
        {
            m_currentRenderTarget = RenderTarget{};
            Changed(ChangeReason::Other);
        }

        RenderTarget* GetCurrentRenderTarget()
        {
            return &m_currentRenderTarget;
        }

        //
        // The entry point for rendering a single frame.  Ultimately, this
        // arranges for RenderWithTarget() to get called.
        //
        template<typename FN>
        void RunWithRenderTarget(FN&& fn)
        {
            auto lock = GetLock();
            auto clearColor = m_clearColor;
            auto renderTargetSize = m_currentSize;
            auto dpi = GetEffectiveDpi(lock);
            DeviceCreationOptions deviceCreationOptions{ m_useSharedDevice, m_forceSoftwareRenderer, m_customDevice };
            lock.unlock();
            
            m_recreatableDeviceManager->RunWithDevice(GetControl(), deviceCreationOptions,
                [&] (ICanvasDevice* device, RunWithDeviceFlags flags)
                {
                    RunWithRenderTarget(clearColor, renderTargetSize, dpi, device, flags, fn);
                });
        }

        //
        // Executes the given function using the current render target.  Does
        // not attempt to recreate or resize the target.
        //
        template<typename FN>
        void RunWithCurrentRenderTarget(FN&& fn)
        {
            auto lock = GetLock();
            auto clearColor = m_clearColor;
            DeviceCreationOptions deviceCreationOptions{ m_useSharedDevice, m_forceSoftwareRenderer, m_customDevice };
            lock.unlock();
            
            m_recreatableDeviceManager->RunWithDevice(GetControl(), deviceCreationOptions,
                [&] (ICanvasDevice* device, RunWithDeviceFlags flags)
                {
                    HandleNewlyCreatedDevice(device, flags);
                    
                    bool areResourcesCreated = !IsSet(flags, RunWithDeviceFlags::ResourcesNotCreated);

                    fn(m_currentRenderTarget.Target.Get(), clearColor, areResourcesCreated);
                });
        }
        

        HRESULT OnDeviceLost(ICanvasDevice*, IInspectable*)
        {
            return ExceptionBoundary(
                [&]
                {
                    //
                    // Avoids keeping a reference to the device alive longer
                    // than necessary. This event will be registered on 
                    // a different device, once rendering starts up again.
                    //
                    auto lock = GetLock();

                    m_deviceLostEventRegistration.Release();

                    lock.unlock();
                    
                    Changed(ChangeReason::DeviceLost);
                });
        }

        Color GetClearColor()
        {
            auto lock = GetLock();
            return m_clearColor;
        }

        Size GetCurrentSize()
        {
            auto lock = GetLock();
            return m_currentSize;
        }

        void GetClearColorSizeAndDpi(Color* clearColor, Size* currentSize, float* currentDpi)
        {
            auto lock = GetLock();
            *clearColor = m_clearColor;
            *currentSize = m_currentSize;
            *currentDpi = GetEffectiveDpi(lock);
        }

        static CanvasAlphaMode GetAlphaModeFromClearColor(Color const& clearColor)
        {
            return clearColor.A == 255 ? CanvasAlphaMode::Ignore : CanvasAlphaMode::Premultiplied;
        }

        void Trim()
        {
            auto& device = m_recreatableDeviceManager->GetDevice();
            if (!device)
                return;

            auto direct3DDevice = As<IDirect3DDevice>(device);
            ThrowIfFailed(direct3DDevice->Trim());
        }

    private:
        Lock GetLock()
        {
            return Lock(m_mutex);
        }

        float GetEffectiveDpi(Lock const& lock)
        {
            MustOwnLock(lock);
            return m_logicalDpi * m_customDpiScaling;
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

            m_clearColor = value;

            lock.unlock();
            
            Changed(ChangeReason::ClearColor);
        }

        template<typename FN>
        void RunWithRenderTarget(Color const& clearColor, Size const& renderTargetSize, float dpi, ICanvasDevice* device, RunWithDeviceFlags flags, FN&& fn)
        {
            HandleNewlyCreatedDevice(device, flags);

            bool areResourcesCreated = !IsSet(flags, RunWithDeviceFlags::ResourcesNotCreated);

            UpdateCurrentRenderTarget(device, renderTargetSize, flags, clearColor, dpi);
            fn(m_currentRenderTarget.Target.Get(), device, clearColor, areResourcesCreated);
        }

        void HandleNewlyCreatedDevice(ICanvasDevice* device, RunWithDeviceFlags flags)
        {
            if (IsSet(flags, RunWithDeviceFlags::NewlyCreatedDevice))
            {
                auto callback = Callback<DeviceLostHandlerType>(this, &BaseControl::OnDeviceLost);
                CheckMakeResult(callback);
                m_deviceLostEventRegistration = RegisteredEvent(
                    device,
                    &ICanvasDevice::add_DeviceLost,
                    &ICanvasDevice::remove_DeviceLost,
                    callback.Get());
            }
        }

        void UpdateCurrentRenderTarget(
            ICanvasDevice* device, 
            Size const& renderTargetSize, 
            RunWithDeviceFlags flags, 
            Color const& clearColor,
            float dpi)
        {
            if (IsSet(flags, RunWithDeviceFlags::NewlyCreatedDevice))
                m_currentRenderTarget = RenderTarget{};

            auto alphaMode = GetAlphaModeFromClearColor(clearColor);

            // CanvasControl will recreate its CanvasImageSource if any of these
            // properties are different.  CanvasAnimatedControl is able to
            // resize buffers / set SourceSize in some cases to avoid
            // recreating.
            GetControl()->CreateOrUpdateRenderTarget(
                device,
                alphaMode,
                dpi,
                renderTargetSize,
                &m_currentRenderTarget);
        }

        void CreateBaseClass()
        {
            auto base = GetAdapter()->CreateUserControl(As<IInspectable>(GetControl()).Get());

            ThrowIfFailed(GetControl()->SetComposableBasePointers(base.Get(), nullptr));
        }

        // These handlers never need to be unregistered, so can
        // be set up immediately when the object is constructed.
        void RegisterEventHandlersOnSelf()
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

            m_recreatableDeviceManager->SetChangedCallback(
                [=] (ChangeReason reason)
                {
                    Changed(reason);
                });
        }

        // These handlers have UI thread affinity, so must not be unregistered by a finalizer thread.
        // They are registered and unregistered when the control is Loaded or Unloaded.
        void RegisterEventHandlers()
        {
            m_applicationSuspendingEventRegistration = m_adapter->AddApplicationSuspendingCallback(
                this, 
                &BaseControl::OnApplicationSuspending);

            m_applicationResumingEventRegistration = m_adapter->AddApplicationResumingCallback(
                this,
                &BaseControl::OnApplicationResuming);

            auto frameworkElement = As<IFrameworkElement>(GetControl());
            if (auto elementAsUIE10 = MaybeAs<IUIElement10>(frameworkElement))
            {
                ThrowIfFailed(elementAsUIE10->get_XamlRoot(&m_xamlRoot));
            }

            // XamlRoot is a 19H1 (18362) API that provides visibility and DPI APIs for Xaml island
            // scenarios as well as CoreWindow scenarios. Use it if it's available, otherwise fall
            // back to the CoreWindow's visibility notifications.
            if (m_xamlRoot)
            {
                m_xamlRootChangedEventRegistration = m_adapter->AddXamlRootChangedCallback(
                    this,
                    &BaseControl::OnXamlRootChanged,
                    m_xamlRoot.Get());
            }
            else
            {
                m_dpiChangedEventRegistration = m_adapter->AddDpiChangedCallback(this, &BaseControl::OnDpiChanged);

                m_windowVisibilityChangedEventRegistration = m_adapter->AddVisibilityChangedCallback(
                    this,
                    &BaseControl::OnWindowVisibilityChanged,
                    GetWindow());
            }

            // Check if the DPI changed while we weren't listening for events.
            UpdateDpi();
        }

        void UnregisterEventHandlers()
        {
            m_applicationSuspendingEventRegistration.Release();
            m_applicationResumingEventRegistration.Release();
            m_dpiChangedEventRegistration.Release();
            m_windowVisibilityChangedEventRegistration.Release();
            m_xamlRootChangedEventRegistration.Release();
            m_deviceLostEventRegistration.Release();
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
                    // OnLoaded and OnUnloaded are fired from XAML asynchronously, and unfortunately they could
                    // be out of order. If the element is removed from tree A and added to tree B, we could get
                    // a Loaded event for tree B *before* we see the Unloaded event for tree A. To handle this:
                    //  * When we see a Loaded event when we're already loaded, just unregister event handlers
                    //      for the old tree and register for the new one.
                    //  * When we get an Unloaded event, check the loaded count. If the element has already been
                    //      added to another tree, the load count will be nonzero. In that case, we don't update
                    //      anything because the OnLoaded call has already taken care of things.

                    UnregisterEventHandlers();
                    RegisterEventHandlers();

                    UpdateIsVisible();
                    UpdateLastSeenParent();

                    if (++m_loadedCount == 1)
                    {
                        Loaded();

                        auto lock = GetLock();
                        m_isLoaded = true;
                        lock.unlock();
                    }

                    Changed(ChangeReason::Other);
                });
        }

        void UpdateIsVisible()
        {
            boolean isVisible;

            if (m_xamlRoot)
            {
                if (SUCCEEDED(m_xamlRoot->get_IsHostVisible(&isVisible)))
                {
                    m_isVisible = !!isVisible;
                }
            }
            // get_Visible fails when running in the designer, in which case we leave m_isVisible set to true.
            else if (SUCCEEDED(m_window->get_Visible(&isVisible)))
            {
                m_isVisible = !!isVisible;
            }
        }

        void UpdateDpi()
        {
            float newDpi = m_logicalDpi;

            if (m_xamlRoot)
            {
                double rasterizationScale;
                if (SUCCEEDED(m_xamlRoot->get_RasterizationScale(&rasterizationScale)))
                {
                    newDpi = static_cast<float>(rasterizationScale * 96.0f);
                }
            }
            else
            {
                newDpi = m_adapter->GetLogicalDpi();
            }

            if (newDpi != m_logicalDpi)
            {
                auto lock = GetLock();

                m_logicalDpi = newDpi;

                //
                // The recreatable device manager
                // may turn around and call Changed() on this 
                // control, which takes out the lock. In that
                // case, we want to make sure we don't hold the
                // lock.
                //
                lock.unlock();

                m_recreatableDeviceManager->SetDpiChanged();
            }
        }

        void UpdateLastSeenParent()
        {
            ComPtr<IDependencyObject> parent;
            As<IFrameworkElement>(GetControl()->GetComposableBase())->get_Parent(&parent);
            if (!parent) {
                m_lastSeenParent.Reset();
                return;
            }
            m_lastSeenParent = AsWeak(parent.Get());
        }

        HRESULT OnUnloaded(IInspectable*, IRoutedEventArgs*)
        {
            return ExceptionBoundary(
                [&]
                {
                    if (--m_loadedCount == 0)
                    {
                        auto lock = GetLock();
                        m_isLoaded = false;
                        lock.unlock();

                        Unloaded();
                        UnregisterEventHandlers();
                    }
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
                    Size newSize;
                    ThrowIfFailed(args->get_NewSize(&newSize));

                    auto lock = GetLock();
                    if (m_currentSize != newSize)
                    {
                        m_currentSize = newSize;
                        lock.unlock();
                        
                        Changed(ChangeReason::Size);
                    }
                });
        }

        HRESULT OnApplicationSuspending(IInspectable*, ISuspendingEventArgs* args)
        {
            return ExceptionBoundary(
                [&]
                {
                    m_isSuspended = true;
                    ApplicationSuspending(args);
                });
        }

        HRESULT OnApplicationResuming(IInspectable*, IInspectable*)
        {
            return ExceptionBoundary(
                [&]
                {
                    m_isSuspended = false;
                    ApplicationResuming();
                });
        }

        HRESULT OnDpiChanged(IDisplayInformation*, IInspectable*)
        {
            return ExceptionBoundary(
                [&]
                {
                    UpdateDpi();
                });
        }

        HRESULT OnWindowVisibilityChanged(IInspectable*, IVisibilityChangedEventArgs* args)
        {
            return ExceptionBoundary(
                [&]
                {
                    boolean isVisible;
                    ThrowIfFailed(args->get_Visible(&isVisible));

                    auto lock = GetLock();
                    m_isVisible = !!isVisible;
                    lock.unlock();
                    
                    WindowVisibilityChanged();
                });
        }

        HRESULT OnXamlRootChanged(IXamlRoot*, IXamlRootChangedEventArgs*)
        {
            return ExceptionBoundary(
                [&]
                {
                    auto lock = GetLock();
                    boolean wasVisible = m_isVisible;
                    UpdateIsVisible();
                    boolean isVisible = m_isVisible;
                    lock.unlock();

                    if (wasVisible != isVisible)
                    {
                        WindowVisibilityChanged();
                    }

                    UpdateDpi();
                });
        }
    };


    template<typename TRAITS>
    class BaseControlWithDrawHandler
        : public BaseControl<TRAITS>
    {
    public:
        typedef typename TRAITS::drawEventHandler_t drawEventHandler_t;
        typedef typename TRAITS::drawEventArgs_t    drawEventArgs_t;

    private:
        EventSource<drawEventHandler_t, InvokeModeOptions<StopOnFirstError>> m_drawEventList;

    public:
        BaseControlWithDrawHandler(std::shared_ptr<adapter_t> adapter, bool useSharedDevice)
            : BaseControl(adapter, useSharedDevice)
        {
        }

        IFACEMETHODIMP add_Draw(
            drawEventHandler_t* value,
            EventRegistrationToken* token) override
        {
            return ExceptionBoundary(
                [&]
                {
                    ThrowIfFailed(m_drawEventList.Add(value, token));
                    Changed(ChangeReason::Other);
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

    protected:
        virtual ComPtr<drawEventArgs_t> CreateDrawEventArgs(
            ICanvasDrawingSession* drawingSession,
            bool isRunningSlowly) = 0;

        //
        // Creates a drawing session, optionally calls the draw handlers and
        // finally closes the drawing session.
        //
        void Draw(renderTarget_t* target, Color const& clearColor, bool callDrawHandlers, bool isRunningSlowly)
        {
            ComPtr<ICanvasDrawingSession> drawingSession;
            ThrowIfFailed(target->CreateDrawingSession(clearColor, &drawingSession));
            if (callDrawHandlers)
            {
                auto drawEventArgs = GetControl()->CreateDrawEventArgs(drawingSession.Get(), isRunningSlowly);
                ThrowIfFailed(m_drawEventList.InvokeAll(GetControl(), drawEventArgs.Get()));
            }

            ThrowIfFailed(As<IClosable>(drawingSession)->Close());
        }
};

}}}}}}
