#include "Host.h"

#include "UWPUtils.h"

#include <windows.h>
#include <iostream>

#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/windows.graphics.display.core.h>

#include <Gamingdeviceinformation.h>

#include "UICommon/UICommon.h"
#include "UICommon/GameFile.h"
#include "UICommon/ImGuiMenu/ImGuiFrontend.h"
#include "UICommon/ImGuiMenu/ImGuiNetplay.h"
#include "UICommon/ImGuiMenu/WinRTKeyboard.h"

#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/Present.h"

#include "Common/WindowSystemInfo.h"

#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Core.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/Host.h"
#include "Core/IOS/STM/STM.h"
#include "Core/HotkeyManager.h"

#define SDL_MAIN_HANDLED

using namespace winrt;

using namespace Windows;
using namespace Windows::Storage;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Foundation::Numerics;
using namespace Windows::UI::Composition;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Pickers;
using namespace Windows::Graphics::Display::Core;

using winrt::Windows::UI::Core::BackRequestedEventArgs;
using winrt::Windows::UI::Core::CoreProcessEventsOption;
using winrt::Windows::UI::Core::CoreWindow;

std::shared_ptr<NetPlay::NetPlayClient> g_netplay_client = nullptr;
std::shared_ptr<NetPlay::NetPlayServer> g_netplay_server = nullptr;
std::shared_ptr<ImGuiFrontend::ImGuiNetPlay> g_netplay_dialog = nullptr;

namespace UWP
{
Common::Flag m_running{false};
winrt::hstring m_launchOnExit;
Common::Flag g_shutdown_requested {false};
Common::Flag g_tried_graceful_shutdown {false};

struct App : implements<App, IFrameworkViewSource, IFrameworkView>
{
  IFrameworkView CreateView() { return *this; }

  void Initialize(CoreApplicationView const& v)
  {
    v.Activated({this, &App::OnActivate});
    CoreApplication::EnteredBackground({this, &App::EnteredBackground});
    CoreApplication::Suspending({this, &App::Suspending});
   
  }

  void Load(hstring const&) {}

  void Uninitialize() {}

  void Run()
  {
    CoreWindow::GetForCurrentThread().Dispatcher().ProcessEvents(
        winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);

    auto navigation = winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView();
    navigation.BackRequested(
        [](const winrt::Windows::Foundation::IInspectable&,
           const winrt::Windows::UI::Core::BackRequestedEventArgs& args) { args.Handled(true); });

    while (true)
    {
      // ImGUI frontend
      if (!m_running.IsSet())
      {
        auto frontend = new ImGuiFrontend::ImGuiFrontend();
        auto result = frontend->RunUntilSelection();
        auto game = result.game_result;

        if (game)
        {
          InitializeDolphinFromFile(game->GetFilePath());
        }
        else if (result.netplay)
        {
          InitializeDolphinForNetplay();
        }
      }

      g_tried_graceful_shutdown.Clear();

      // Dolphin loop
      while (Core::GetState() != Core::State::Stopping)
      {
        if (g_shutdown_requested.TestAndClear())
        {
          UICommon::TriggerSTMPowerEvent();

          if (Core::GetState() == Core::State::Paused)
            Core::SetState(Core::State::Running);

          if (NetPlay::IsNetPlayRunning())
            NetPlay::SendPowerButtonEvent();

          Core::Stop();
          Core::Shutdown();

          break;
        }

        ::Core::HostDispatchJobs();

        if (Core::IsRunningAndStarted())
        {
          Core::UpdateInputGate(false);
          HotkeyManagerEmu::GetStatus(false);
          ControlReference::SetInputGate(true);

          HotkeyManagerEmu::GetStatus(true);

          if (HotkeyManagerEmu::IsPressed(HK_TOGGLE_ONSCREEN_MENU, false))
          {
            OSD::ToggleShowSettings();
            Core::SetState(Core::GetState() == Core::State::Paused ? Core::State::Running :
                                                                      Core::State::Paused);
          }

          if (Core::GetState() == Core::State::Paused)
          {
            g_presenter->Present();
          }
        }

        CoreWindow::GetForCurrentThread().Dispatcher().ProcessEvents(
            CoreProcessEventsOption::ProcessAllIfPresent);

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }

      // Make sure we've shut down properly.

      m_running.Clear();

      // If there's another frontend, boot to that.
      if (!m_launchOnExit.empty())
      {
        winrt::Windows::Foundation::Uri m_uri{m_launchOnExit};
        auto asyncOperation = winrt::Windows::System::Launcher::LaunchUriAsync(m_uri);
        asyncOperation.Completed([](winrt::Windows::Foundation::IAsyncOperation<bool> const& sender,
                                    winrt::Windows::Foundation::AsyncStatus const asyncStatus) {
          CoreApplication::Exit();
          return;
        });
      }

      // Continue looping to open the frontend again
    }
  }

  void InitializeDolphinForNetplay() {
    m_running.Set();
  }

  winrt::fire_and_forget InitializeDolphinFromFile(std::string path)
  {
    if (path.empty())
    {
      FileOpenPicker openPicker;
      openPicker.ViewMode(PickerViewMode::List);
      openPicker.SuggestedStartLocation(PickerLocationId::HomeGroup);
      openPicker.FileTypeFilter().Append(L".iso");
      openPicker.FileTypeFilter().Append(L".ciso");
      openPicker.FileTypeFilter().Append(L".rvz");
      openPicker.FileTypeFilter().Append(L".wbfs");
      openPicker.FileTypeFilter().Append(L".gcm");
      openPicker.FileTypeFilter().Append(L".gcz");
      openPicker.FileTypeFilter().Append(L".json");
      openPicker.FileTypeFilter().Append(L".elf");
      openPicker.FileTypeFilter().Append(L".dol");
      openPicker.FileTypeFilter().Append(L".tgc");
      openPicker.FileTypeFilter().Append(L".wad");

      auto file = co_await openPicker.PickSingleFileAsync();
      if (file)
      {
        path = winrt::to_string(file.Path().data());
      }
    }

    CoreWindow window = CoreWindow::GetForCurrentThread();
    void* abi = winrt::get_abi(window);

    WindowSystemInfo wsi;
    wsi.type = WindowSystemType::Windows;
    wsi.render_surface = abi;
    wsi.render_width = window.Bounds().Width;
    wsi.render_height = window.Bounds().Height;

    GAMING_DEVICE_MODEL_INFORMATION info = {};
    GetGamingDeviceModelInformation(&info);
    if (info.vendorId == GAMING_DEVICE_VENDOR_ID_MICROSOFT)
    {
      HdmiDisplayInformation hdi = HdmiDisplayInformation::GetForCurrentView();
      if (hdi)
      {
        wsi.render_width = hdi.GetCurrentDisplayMode().ResolutionWidthInRawPixels();
        wsi.render_height = hdi.GetCurrentDisplayMode().ResolutionHeightInRawPixels();
        // Our UI is based on 1080p, and we're adding a modifier to zoom in by 80%
        wsi.render_surface_scale = ((float) wsi.render_width / 1920.0f) * 1.8f;
      }
    }

    std::unique_ptr<BootParameters> boot =
        BootParameters::GenerateFromFile(path, BootSessionData("", DeleteSavestateAfterBoot::No));

    if (!BootManager::BootCore(std::move(boot), wsi))
    {
      fprintf(stderr, "Could not boot the specified file\n");
    }

    m_running.Set();
  }

  void OnClosed(const IInspectable&, const winrt::Windows::UI::Core::CoreWindowEventArgs& args)
  {
    g_shutdown_requested.Set();
  }

  void OnActivate(const winrt::Windows::ApplicationModel::Core::CoreApplicationView&,
                  const winrt::Windows::ApplicationModel::Activation::IActivatedEventArgs& args)
  {
    std::stringstream filePath;

    if (args.Kind() == Windows::ApplicationModel::Activation::ActivationKind::Protocol)
    {
      auto protocolActivatedEventArgs{
          args.as<Windows::ApplicationModel::Activation::ProtocolActivatedEventArgs>()};
      auto query = protocolActivatedEventArgs.Uri().QueryParsed();

      for (uint32_t i = 0; i < query.Size(); i++)
      {
        auto arg = query.GetAt(i);

        // parse command line string
        if (arg.Name() == winrt::hstring(L"cmd"))
        {
          std::string argVal = winrt::to_string(arg.Value());

          // Strip the executable from the cmd argument
          if (argVal.starts_with("dolphin.exe"))
          {
            argVal = argVal.substr(11, argVal.length());
          }

          std::istringstream iss(argVal);
          std::string s;

          // Maintain slashes while reading the quotes
          while (iss >> std::quoted(s, '"', (char)0))
          {
            filePath << s;
          }
        }
        else if (arg.Name() == winrt::hstring(L"launchOnExit"))
        {
          // For if we want to return to a frontend
          m_launchOnExit = arg.Value();
        }
      }
    }

    UICommon::SetUserDirectory(UWP::GetUserLocation());
    UICommon::CreateDirectories();
    UICommon::Init();
    UICommon::InitControllers({});

    std::string gamePath = filePath.str();
    if (!gamePath.empty() && gamePath != "")
    {
      InitializeDolphinFromFile(gamePath);
    }

    CoreWindow window = CoreWindow::GetForCurrentThread();
    window.Activate();
  }

    void EnteredBackground(const IInspectable&,
                           const winrt::Windows::ApplicationModel::EnteredBackgroundEventArgs& args)
    {
    }

    void Suspending(const IInspectable&,
                    const winrt::Windows::ApplicationModel::SuspendingEventArgs& args)
    {
      // The Series S/X quits fast, so let's immediately shutdown to ensure all the caches save.
      Core::Stop();
      Core::Shutdown();
      UICommon::Shutdown();

      if (!m_launchOnExit.empty())
      {
        winrt::Windows::Foundation::Uri m_uri{m_launchOnExit};
        auto asyncOperation = winrt::Windows::System::Launcher::LaunchUriAsync(m_uri);
        asyncOperation.Completed([](winrt::Windows::Foundation::IAsyncOperation<bool> const& sender,
                                    winrt::Windows::Foundation::AsyncStatus const asyncStatus) {
          CoreApplication::Exit();
        });
      }
    }

    void OnCharacterReceived(winrt::Windows::UI::Core::CoreWindow const& /* sender */,
                        winrt::Windows::UI::Core::CharacterReceivedEventArgs const& e /* args */)
    {
      UWP::HandleCharacter(e.KeyCode());
    }

    void SetWindow(CoreWindow const& w)
    {
      w.Closed({this, &App::OnClosed});
      w.CharacterReceived({this, &App::OnCharacterReceived});
    }
  };
}

int WINAPIV WinMain()
{
  winrt::init_apartment();

  CoreApplication::Run(make<UWP::App>());

  winrt::uninit_apartment();

  return 0;
}

std::vector<std::string> Host_GetPreferredLocales()
{
  return {};
}

void Host_NotifyMapLoaded()
{
}

void Host_RefreshDSPDebuggerWindow()
{
}

bool Host_UIBlocksControllerState()
{
  return false;
}

void Host_Message(HostMessageID id)
{
}

void Host_UpdateTitle(const std::string& title)
{
}

void Host_UpdateDisasmDialog()
{
}

void Host_UpdateMainFrame()
{
}

void Host_RequestRenderWindowSize(int width, int height)
{
}

bool Host_RendererHasFocus()
{
  return true;
}

bool Host_RendererHasFullFocus()
{
  // Mouse capturing isn't implemented
  return Host_RendererHasFocus();
}

bool Host_RendererIsFullscreen()
{
  return true;
}

void Host_YieldToUI()
{
}

void Host_TitleChanged()
{
}

void Host_UpdateDiscordClientID(const std::string& client_id)
{
}

bool Host_UpdateDiscordPresenceRaw(const std::string& details, const std::string& state,
                                   const std::string& large_image_key,
                                   const std::string& large_image_text,
                                   const std::string& small_image_key,
                                   const std::string& small_image_text,
                                   const int64_t start_timestamp, const int64_t end_timestamp,
                                   const int party_size, const int party_max)
{
  return false;
}

std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core> core)
{
  return nullptr;
}
