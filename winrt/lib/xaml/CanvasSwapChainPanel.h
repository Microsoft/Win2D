// Copyright (c) Microsoft Corporation. All rights reserved.
//
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.

#pragma once

namespace ABI { namespace Microsoft { namespace Graphics { namespace Canvas { namespace UI { namespace Xaml
{
    using namespace ABI::Windows::Foundation;
    using namespace ::Microsoft::WRL;
    using namespace ABI::Windows::UI::Xaml::Controls;

    class ICanvasSwapChainPanelAdapter
    {
    public:
        virtual ~ICanvasSwapChainPanelAdapter() = default;

        virtual ComPtr<IInspectable> CreateSwapChainPanel(IInspectable* canvasSwapChainPanel) = 0;
    };
    
    class CanvasSwapChainPanelAdapter : public ICanvasSwapChainPanelAdapter,
                                        private LifespanTracker<CanvasSwapChainPanelAdapter>
    {
        ComPtr<ISwapChainPanelFactory> m_swapChainPanelFactory;

    public:
        CanvasSwapChainPanelAdapter();

        virtual ComPtr<IInspectable> CreateSwapChainPanel(IInspectable* canvasSwapChain) override;
    };

    class CanvasSwapChainPanel : public RuntimeClass<
        ICanvasSwapChainPanel,
        ComposableBase<>>,
        private LifespanTracker<CanvasSwapChainPanel>
    {
        InspectableClass(RuntimeClass_Microsoft_Graphics_Canvas_UI_Xaml_CanvasSwapChainPanel, BaseTrust);

        std::shared_ptr<ICanvasSwapChainPanelAdapter> m_adapter;

        ComPtr<ICanvasSwapChain> m_canvasSwapChain;
        WeakRef m_lastSeenParent;

    public:
        CanvasSwapChainPanel(std::shared_ptr<ICanvasSwapChainPanelAdapter> adapter);
        ~CanvasSwapChainPanel();

        // ICanvasSwapChainPanel

        IFACEMETHOD(get_SwapChain)(ICanvasSwapChain** value) override;

        IFACEMETHOD(put_SwapChain)(ICanvasSwapChain* value) override;

        IFACEMETHOD(RemoveFromVisualTree)() override;

    private:
        HRESULT OnLoaded(IInspectable*, IRoutedEventArgs*);
    };

}}}}}}
