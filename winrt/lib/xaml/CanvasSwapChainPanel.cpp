// Copyright (c) Microsoft Corporation. All rights reserved.
//
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.

#include "pch.h"

#include "CanvasSwapChainPanel.h"
#include "RemoveFromVisualTree.h"

using namespace ABI::Microsoft::Graphics::Canvas::UI::Xaml;
using namespace ABI::Microsoft::Graphics::Canvas;
using namespace ABI::Windows::UI::Xaml::Controls;

CanvasSwapChainPanelAdapter::CanvasSwapChainPanelAdapter()
{
    ThrowIfFailed(GetActivationFactory(
        HStringReference(RuntimeClass_Windows_UI_Xaml_Controls_SwapChainPanel).Get(),
        &m_swapChainPanelFactory));
}

ComPtr<IInspectable> CanvasSwapChainPanelAdapter::CreateSwapChainPanel(IInspectable* canvasSwapChainPanel)
{
    ComPtr<IInspectable> swapChainPanelInspectable;
    ComPtr<ISwapChainPanel> swapChainPanel;

    // Instantiates the SwapChainPanel XAML object.
    ThrowIfFailed(m_swapChainPanelFactory->CreateInstance(
        canvasSwapChainPanel,
        &swapChainPanelInspectable,
        &swapChainPanel));

    return swapChainPanelInspectable;
}

class CanvasSwapChainPanelFactory : public AgileActivationFactory<>,
                                    private LifespanTracker<CanvasSwapChainPanelFactory>
{
    //
    // While CanvasSwapChainPanel publically has only the default 
    // constructor, its implementing class has only a non-default
    // constructor, and so this activation factory is used to
    // pass in the adapter.
    //
    std::weak_ptr<CanvasSwapChainPanelAdapter> m_adapter;

public:
    IFACEMETHODIMP ActivateInstance(IInspectable** obj) override
    {
        return ExceptionBoundary(
            [&]
            {
                CheckAndClearOutPointer(obj);

                auto adapter = m_adapter.lock();

                if (!adapter)
                    m_adapter = adapter = std::make_shared<CanvasSwapChainPanelAdapter>();

                auto swapChainPanel = Make<CanvasSwapChainPanel>(adapter);
                CheckMakeResult(swapChainPanel);

                ThrowIfFailed(swapChainPanel.CopyTo(obj));
            });
    }
};

CanvasSwapChainPanel::CanvasSwapChainPanel(std::shared_ptr<ICanvasSwapChainPanelAdapter> adapter)
    : m_adapter(adapter)
{
    auto base = m_adapter->CreateSwapChainPanel(static_cast<ICanvasSwapChainPanel*>(this));
    ThrowIfFailed(SetComposableBasePointers(base.Get(), nullptr));

    auto frameworkElement = As<IFrameworkElement>(this);

    EventRegistrationToken tokenThatIsThrownAway{};
    auto callback = Callback<IRoutedEventHandler>(this, &CanvasSwapChainPanel::OnLoaded);
    CheckMakeResult(callback);

    ThrowIfFailed(frameworkElement->add_Loaded(callback.Get(), &tokenThatIsThrownAway));
}

CanvasSwapChainPanel::~CanvasSwapChainPanel()
{
}

HRESULT CanvasSwapChainPanel::OnLoaded(IInspectable*, IRoutedEventArgs*)
{
    return ExceptionBoundary(
        [&]
        {
            ComPtr<IDependencyObject> parent;
            As<IFrameworkElement>(GetComposableBase())->get_Parent(&parent);
            if (!parent) {
                m_lastSeenParent.Reset();
                return;
            }
            m_lastSeenParent = AsWeak(parent.Get());
        });
}

IFACEMETHODIMP CanvasSwapChainPanel::get_SwapChain(ICanvasSwapChain** value)
{
    return ExceptionBoundary(
        [&]
        {
            CheckAndClearOutPointer(value);

            ThrowIfFailed(m_canvasSwapChain.CopyTo(value));
        });
}

IFACEMETHODIMP CanvasSwapChainPanel::put_SwapChain(ICanvasSwapChain* value)
{
    return ExceptionBoundary(
        [&]
        {
            ComPtr<ISwapChainPanelNative> swapChainPanelNative = As<ISwapChainPanelNative>(this);

            ComPtr<IDXGISwapChain2> dxgiSwapChain;

            if (value)
            {
                auto swapChainResourceWrapper = As<ICanvasResourceWrapperNative>(value);

                ThrowIfFailed(swapChainResourceWrapper->GetNativeResource(nullptr, 0, IID_PPV_ARGS(&dxgiSwapChain)));
            }

            ThrowIfFailed(swapChainPanelNative->SetSwapChain(dxgiSwapChain.Get()));

            m_canvasSwapChain = value;
        });
}

IFACEMETHODIMP CanvasSwapChainPanel::RemoveFromVisualTree()
{
    return ExceptionBoundary(
        [&]
        {
            auto control = As<IUIElement>(this); //Do this first because the unit tests expect a failure when this cast fails.
            if (!m_lastSeenParent) {
                return;
            }
            RemoveFromVisualTreeImpl(LockWeakRef<IDependencyObject>(m_lastSeenParent).Get(), control.Get());
        });
}

ActivatableClassWithFactory(CanvasSwapChainPanel, CanvasSwapChainPanelFactory);
