#include "pch.h"
#include "AudioPlaybackConnector.h"

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void SetupFlyout();
void SetupMenu();
winrt::fire_and_forget ConnectDevice(DevicePicker, std::wstring_view);
void SetupDevicePicker();
void SetupSvgIcon();
void UpdateNotifyIcon();
winrt::fire_and_forget SetupAudioRouting(std::wstring deviceId, std::wstring deviceName);
void StopAudioRouting(std::wstring deviceId);
winrt::fire_and_forget ListAudioDevices();

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(nCmdShow);

	g_hInst = hInstance;

	winrt::init_apartment();

	// Check for command-line arguments
	if (lpCmdLine && wcslen(lpCmdLine) > 0)
	{
		std::wstring cmdLine(lpCmdLine);
		if (cmdLine.find(L"--list-devices") != std::wstring::npos || 
		    cmdLine.find(L"-l") != std::wstring::npos)
		{
			// List devices and exit
			ListAudioDevices();
			// Wait for the message box to be shown and closed
			MSG msg;
			while (GetMessageW(&msg, nullptr, 0, 0))
			{
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
			return EXIT_SUCCESS;
		}
	}

	winrt::init_apartment();

	bool supported = false;
	try
	{
		using namespace winrt::Windows::Foundation::Metadata;

		supported = ApiInformation::IsTypePresent(winrt::name_of<DesktopWindowXamlSource>()) &&
			ApiInformation::IsTypePresent(winrt::name_of<AudioPlaybackConnection>());
	}
	catch (winrt::hresult_error const&)
	{
		supported = false;
		LOG_CAUGHT_EXCEPTION();
	}
	if (!supported)
	{
		TaskDialog(nullptr, nullptr, _(L"Unsupported Operating System"), nullptr, _(L"AudioPlaybackConnector is not supported on this operating system version."), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		return EXIT_FAILURE;
	}

	WNDCLASSEXW wcex = {
		.cbSize = sizeof(wcex),
		.lpfnWndProc = WndProc,
		.hInstance = hInstance,
		.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_AUDIOPLAYBACKCONNECTOR)),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = L"AudioPlaybackConnector",
		.hIconSm = wcex.hIcon
	};

	RegisterClassExW(&wcex);

	// When parent window size is 0x0 or invisible, the dpi scale of menu is incorrect. Here we set window size to 1x1 and use WS_EX_LAYERED to make window looks like invisible.
	g_hWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST, L"AudioPlaybackConnector", nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
	FAIL_FAST_LAST_ERROR_IF_NULL(g_hWnd);
	FAIL_FAST_IF_WIN32_BOOL_FALSE(SetLayeredWindowAttributes(g_hWnd, 0, 0, LWA_ALPHA));

	DesktopWindowXamlSource desktopSource;
	auto desktopSourceNative2 = desktopSource.as<IDesktopWindowXamlSourceNative2>();
	winrt::check_hresult(desktopSourceNative2->AttachToWindow(g_hWnd));
	winrt::check_hresult(desktopSourceNative2->get_WindowHandle(&g_hWndXaml));

	g_xamlCanvas = Canvas();
	desktopSource.Content(g_xamlCanvas);

	LoadSettings();
	SetupFlyout();
	SetupMenu();
	SetupDevicePicker();
	SetupSvgIcon();

	g_nid.hWnd = g_niid.hWnd = g_hWnd;
	wcscpy_s(g_nid.szTip, _(L"AudioPlaybackConnector"));
	UpdateNotifyIcon();

	WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
	LOG_LAST_ERROR_IF(WM_TASKBAR_CREATED == 0);

	PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		BOOL processed = FALSE;
		winrt::check_hresult(desktopSourceNative2->PreTranslateMessage(&msg, &processed));
		if (!processed)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_DESTROY:
		// Stop all audio graphs first
		for (auto& [deviceId, audioGraph] : g_audioGraphs)
		{
			auto& [graph, inputNode, outputNode] = audioGraph;
			graph.Stop();
			graph.Close();
		}
		g_audioGraphs.clear();
		
		for (const auto& connection : g_audioPlaybackConnections)
		{
			connection.second.second.Close();
			g_devicePicker.SetDisplayStatus(connection.second.first, {}, DevicePickerDisplayStatusOptions::None);
		}
		if (g_reconnect)
		{
			SaveSettings();
			g_audioPlaybackConnections.clear();
		}
		else
		{
			g_audioPlaybackConnections.clear();
			SaveSettings();
		}
		Shell_NotifyIconW(NIM_DELETE, &g_nid);
		PostQuitMessage(0);
		break;
	case WM_SETTINGCHANGE:
		if (lParam && CompareStringOrdinal(reinterpret_cast<LPCWCH>(lParam), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)
		{
			UpdateNotifyIcon();
		}
		break;
	case WM_NOTIFYICON:
		switch (LOWORD(lParam))
		{
		case NIN_SELECT:
		case NIN_KEYSELECT:
		{
			using namespace winrt::Windows::UI::Popups;

			RECT iconRect;
			auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
			if (FAILED(hr))
			{
				LOG_HR(hr);
				break;
			}

			auto dpi = GetDpiForWindow(hWnd);
			Rect rect = {
				static_cast<float>(iconRect.left * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(iconRect.top * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_HIDEWINDOW);
			SetForegroundWindow(hWnd);
			g_devicePicker.Show(rect, Placement::Above);
		}
		break;
		case WM_RBUTTONUP: // Menu activated by mouse click
			g_menuFocusState = FocusState::Pointer;
			break;
		case WM_CONTEXTMENU:
		{
			if (g_menuFocusState == FocusState::Unfocused)
				g_menuFocusState = FocusState::Keyboard;

			auto dpi = GetDpiForWindow(hWnd);
			Point point = {
				static_cast<float>(GET_X_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(GET_Y_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(g_hWndXaml, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
			SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
			SetForegroundWindow(hWnd);

			g_xamlMenu.ShowAt(g_xamlCanvas, point);
		}
		break;
		}
		break;
	case WM_CONNECTDEVICE:
		if (g_reconnect)
		{
			for (const auto& i : g_lastDevices)
			{
				ConnectDevice(g_devicePicker, i);
			}
			g_lastDevices.clear();
		}
		break;
	default:
		if (WM_TASKBAR_CREATED && message == WM_TASKBAR_CREATED)
		{
			UpdateNotifyIcon();
		}
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}
	return 0;
}

void SetupFlyout()
{
	TextBlock textBlock;
	textBlock.Text(_(L"All connections will be closed.\nExit anyway?"));
	textBlock.Margin({ 0, 0, 0, 12 });

	static CheckBox checkbox;
	checkbox.IsChecked(g_reconnect);
	checkbox.Content(winrt::box_value(_(L"Reconnect on next start")));

	Button button;
	button.Content(winrt::box_value(_(L"Exit")));
	button.HorizontalAlignment(HorizontalAlignment::Right);
	button.Click([](const auto&, const auto&) {
		g_reconnect = checkbox.IsChecked().Value();
		PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
	});

	StackPanel stackPanel;
	stackPanel.Children().Append(textBlock);
	stackPanel.Children().Append(checkbox);
	stackPanel.Children().Append(button);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(stackPanel);

	g_xamlFlyout = flyout;
}

void SetupMenu()
{
	// https://docs.microsoft.com/en-us/windows/uwp/design/style/segoe-ui-symbol-font
	FontIcon settingsIcon;
	settingsIcon.Glyph(L"\xE713");

	MenuFlyoutItem settingsItem;
	settingsItem.Text(_(L"Bluetooth Settings"));
	settingsItem.Icon(settingsIcon);
	settingsItem.Click([](const auto&, const auto&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	// Audio Devices menu item
	FontIcon audioIcon;
	audioIcon.Glyph(L"\xE7F5");  // Speaker icon

	MenuFlyoutItem audioDevicesItem;
	audioDevicesItem.Text(_(L"List Audio Devices"));
	audioDevicesItem.Icon(audioIcon);
	audioDevicesItem.Click([](const auto&, const auto&) {
		ListAudioDevices();
	});

	FontIcon closeIcon;
	closeIcon.Glyph(L"\xE8BB");

	MenuFlyoutItem exitItem;
	exitItem.Text(_(L"Exit"));
	exitItem.Icon(closeIcon);
	exitItem.Click([](const auto&, const auto&) {
		if (g_audioPlaybackConnections.size() == 0)
		{
			PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
			return;
		}

		RECT iconRect;
		auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
		if (FAILED(hr))
		{
			LOG_HR(hr);
			return;
		}

		auto dpi = GetDpiForWindow(g_hWnd);

		SetWindowPos(g_hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, 0, 0, SWP_HIDEWINDOW);
		g_xamlCanvas.Width(static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi));
		g_xamlCanvas.Height(static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi));

		g_xamlFlyout.ShowAt(g_xamlCanvas);
	});

	MenuFlyout menu;
	menu.Items().Append(settingsItem);
	menu.Items().Append(audioDevicesItem);
	menu.Items().Append(exitItem);
	menu.Opened([](const auto& sender, const auto&) {
		auto menuItems = sender.as<MenuFlyout>().Items();
		auto itemsCount = menuItems.Size();
		if (itemsCount > 0)
		{
			menuItems.GetAt(itemsCount - 1).Focus(g_menuFocusState);
		}
		g_menuFocusState = FocusState::Unfocused;
	});
	menu.Closed([](const auto&, const auto&) {
		ShowWindow(g_hWnd, SW_HIDE);
	});

	g_xamlMenu = menu;
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, DeviceInformation device)
{
	picker.SetDisplayStatus(device, _(L"Connecting"), DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);

	bool success = false;
	std::wstring errorMessage;

	try
	{
		auto connection = AudioPlaybackConnection::TryCreateFromId(device.Id());
		if (connection)
		{
			g_audioPlaybackConnections.emplace(device.Id(), std::pair(device, connection));

			connection.StateChanged([](const auto& sender, const auto&) {
				if (sender.State() == AudioPlaybackConnectionState::Closed)
				{
					std::wstring deviceId(sender.DeviceId());
					// Stop audio routing for this device
					StopAudioRouting(deviceId);
					
					auto it = g_audioPlaybackConnections.find(deviceId);
					if (it != g_audioPlaybackConnections.end())
					{
						g_devicePicker.SetDisplayStatus(it->second.first, {}, DevicePickerDisplayStatusOptions::None);
						g_audioPlaybackConnections.erase(it);
					}
					sender.Close();
				}
			});

			co_await connection.StartAsync();
			auto result = co_await connection.OpenAsync();

			switch (result.Status())
			{
			case AudioPlaybackConnectionOpenResultStatus::Success:
				success = true;
				break;
			case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
				success = false;
				errorMessage = _(L"The request timed out");
				break;
			case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
				success = false;
				errorMessage = _(L"The operation was denied by the system");
				break;
			case AudioPlaybackConnectionOpenResultStatus::UnknownFailure:
				success = false;
				winrt::throw_hresult(result.ExtendedError());
				break;
			}
		}
		else
		{
			success = false;
			errorMessage = _(L"Unknown error");
		}
	}
	catch (winrt::hresult_error const& ex)
	{
		success = false;
		errorMessage.resize(64);
		while (1)
		{
			auto result = swprintf(errorMessage.data(), errorMessage.size(), L"%s (0x%08X)", ex.message().c_str(), static_cast<uint32_t>(ex.code()));
			if (result < 0)
			{
				errorMessage.resize(errorMessage.size() * 2);
			}
			else
			{
				errorMessage.resize(result);
				break;
			}
		}
		LOG_CAUGHT_EXCEPTION();
	}

	if (success)
	{
		picker.SetDisplayStatus(device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
		// Set up audio routing to configured output device
		SetupAudioRouting(std::wstring(device.Id()), std::wstring(device.Name()));
	}
	else
	{
		auto it = g_audioPlaybackConnections.find(std::wstring(device.Id()));
		if (it != g_audioPlaybackConnections.end())
		{
			it->second.second.Close();
			g_audioPlaybackConnections.erase(it);
		}
		picker.SetDisplayStatus(device, errorMessage, DevicePickerDisplayStatusOptions::ShowRetryButton);
	}
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, std::wstring_view deviceId)
{
	auto device = co_await DeviceInformation::CreateFromIdAsync(deviceId);
	ConnectDevice(picker, device);
}

void SetupDevicePicker()
{
	g_devicePicker = DevicePicker();
	winrt::check_hresult(g_devicePicker.as<IInitializeWithWindow>()->Initialize(g_hWnd));

	g_devicePicker.Filter().SupportedDeviceSelectors().Append(AudioPlaybackConnection::GetDeviceSelector());
	g_devicePicker.DevicePickerDismissed([](const auto&, const auto&) {
		SetWindowPos(g_hWnd, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_HIDEWINDOW);
	});
	g_devicePicker.DeviceSelected([](const auto& sender, const auto& args) {
		ConnectDevice(sender, args.SelectedDevice());
	});
	g_devicePicker.DisconnectButtonClicked([](const auto& sender, const auto& args) {
		auto device = args.Device();
		std::wstring deviceId(device.Id());
		
		// Stop audio routing for this device
		StopAudioRouting(deviceId);
		
		auto it = g_audioPlaybackConnections.find(deviceId);
		if (it != g_audioPlaybackConnections.end())
		{
			it->second.second.Close();
			g_audioPlaybackConnections.erase(it);
		}
		sender.SetDisplayStatus(device, {}, DevicePickerDisplayStatusOptions::None);
	});
}

void SetupSvgIcon()
{
	auto hRes = FindResourceW(g_hInst, MAKEINTRESOURCEW(1), L"SVG");
	FAIL_FAST_LAST_ERROR_IF_NULL(hRes);

	auto size = SizeofResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF(size == 0);

	auto hResData = LoadResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF_NULL(hResData);

	auto svgData = reinterpret_cast<const char*>(LockResource(hResData));
	FAIL_FAST_IF_NULL_ALLOC(svgData);

	const std::string_view svg(svgData, size);
	const int width = GetSystemMetrics(SM_CXSMICON), height = GetSystemMetrics(SM_CYSMICON);

	g_hIconLight = SvgTohIcon(svg, width, height, { 0, 0, 0, 1 });
	g_hIconDark = SvgTohIcon(svg, width, height, { 1, 1, 1, 1 });
}

void UpdateNotifyIcon()
{
	DWORD value = 0, cbValue = sizeof(value);
	LOG_IF_WIN32_ERROR(RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &cbValue));
	g_nid.hIcon = value != 0 ? g_hIconLight : g_hIconDark;

	if (!Shell_NotifyIconW(NIM_MODIFY, &g_nid))
	{
		if (Shell_NotifyIconW(NIM_ADD, &g_nid))
		{
			FAIL_FAST_IF_WIN32_BOOL_FALSE(Shell_NotifyIconW(NIM_SETVERSION, &g_nid));
		}
		else
		{
			LOG_LAST_ERROR();
		}
	}
}

// Find a device by name (partial match allowed)
winrt::Windows::Foundation::IAsyncOperation<DeviceInformation> FindAudioRenderDevice(std::wstring_view deviceName)
{
	auto selector = winrt::Windows::Media::Devices::MediaDevice::GetAudioRenderSelector();
	auto devices = co_await DeviceInformation::FindAllAsync(selector);
	
	for (const auto& device : devices)
	{
		// Check if the device name contains the search string
		std::wstring name(device.Name());
		if (name.find(deviceName) != std::wstring::npos)
		{
			co_return device;
		}
	}
	co_return nullptr;
}

// Find the A2DP audio input device for a connected Bluetooth device
winrt::Windows::Foundation::IAsyncOperation<DeviceInformation> FindA2dpInputDevice(std::wstring_view bluetoothDeviceName)
{
	auto selector = winrt::Windows::Media::Devices::MediaDevice::GetAudioCaptureSelector();
	auto devices = co_await DeviceInformation::FindAllAsync(selector);
	
	for (const auto& device : devices)
	{
		std::wstring name(device.Name());
		// A2DP devices typically have "A2DP SNK" in their name
		if (name.find(L"A2DP") != std::wstring::npos || name.find(bluetoothDeviceName) != std::wstring::npos)
		{
			co_return device;
		}
	}
	co_return nullptr;
}

// Set up audio routing from A2DP input device to the configured output device
winrt::fire_and_forget SetupAudioRouting(std::wstring deviceId, std::wstring deviceName)
{
	// Only set up routing if a custom output device is configured
	if (g_audioOutputDevice.empty())
	{
		co_return;
	}

	try
	{
		// Small delay to allow the A2DP virtual device to become available
		co_await winrt::resume_after(std::chrono::milliseconds(1000));

		// Find the A2DP input device
		auto inputDevice = co_await FindA2dpInputDevice(deviceName);
		if (!inputDevice)
		{
			LOG_HR_MSG(E_FAIL, "Could not find A2DP input device for: %ls", deviceName.c_str());
			co_return;
		}

		// Find the configured output device
		auto outputDevice = co_await FindAudioRenderDevice(g_audioOutputDevice);
		if (!outputDevice)
		{
			LOG_HR_MSG(E_FAIL, "Could not find audio output device: %ls", g_audioOutputDevice.c_str());
			co_return;
		}

		// Create audio graph settings with the output device
		AudioGraphSettings settings(winrt::Windows::Media::Render::AudioRenderCategory::Media);
		settings.PrimaryRenderDevice(outputDevice);

		// Create the audio graph
		auto graphResult = co_await AudioGraph::CreateAsync(settings);
		if (graphResult.Status() != AudioGraphCreationStatus::Success)
		{
			LOG_HR_MSG(E_FAIL, "Failed to create AudioGraph: %d", static_cast<int>(graphResult.Status()));
			co_return;
		}

		auto graph = graphResult.Graph();

		// Create device input node from the A2DP device
		auto inputResult = co_await graph.CreateDeviceInputNodeAsync(
			winrt::Windows::Media::Capture::MediaCategory::Media,
			graph.EncodingProperties(),
			inputDevice);

		if (inputResult.Status() != AudioDeviceNodeCreationStatus::Success)
		{
			LOG_HR_MSG(E_FAIL, "Failed to create input node: %d", static_cast<int>(inputResult.Status()));
			graph.Close();
			co_return;
		}

		auto inputNode = inputResult.DeviceInputNode();

		// Create device output node
		auto outputResult = co_await graph.CreateDeviceOutputNodeAsync();
		if (outputResult.Status() != AudioDeviceNodeCreationStatus::Success)
		{
			LOG_HR_MSG(E_FAIL, "Failed to create output node: %d", static_cast<int>(outputResult.Status()));
			graph.Close();
			co_return;
		}

		auto outputNode = outputResult.DeviceOutputNode();

		// Connect input to output
		inputNode.AddOutgoingConnection(outputNode);

		// Store the graph for cleanup later
		g_audioGraphs.emplace(deviceId, std::make_tuple(graph, inputNode, outputNode));

		// Start the audio graph
		graph.Start();
	}
	catch (winrt::hresult_error const& ex)
	{
		LOG_HR_MSG(ex.code(), "Audio routing setup failed: %ls", ex.message().c_str());
	}
}

// Stop audio routing for a device
void StopAudioRouting(std::wstring deviceId)
{
	auto it = g_audioGraphs.find(deviceId);
	if (it != g_audioGraphs.end())
	{
		auto& [graph, inputNode, outputNode] = it->second;
		graph.Stop();
		graph.Close();
		g_audioGraphs.erase(it);
	}
}

// List all available audio devices (for config file reference)
winrt::fire_and_forget ListAudioDevices()
{
	try
	{
		std::wstring message = L"=== Audio Output Devices ===\n\n";

		auto renderSelector = winrt::Windows::Media::Devices::MediaDevice::GetAudioRenderSelector();
		auto renderDevices = co_await DeviceInformation::FindAllAsync(renderSelector);
		
		message += L"Output Devices (speakers/headphones):\n";
		for (const auto& device : renderDevices)
		{
			message += L"  • ";
			message += device.Name();
			message += L"\n";
		}

		message += L"\n=== Audio Input Devices ===\n\n";

		auto captureSelector = winrt::Windows::Media::Devices::MediaDevice::GetAudioCaptureSelector();
		auto captureDevices = co_await DeviceInformation::FindAllAsync(captureSelector);
		
		message += L"Input Devices (microphones/A2DP sources):\n";
		for (const auto& device : captureDevices)
		{
			message += L"  • ";
			message += device.Name();
			message += L"\n";
		}

		message += L"\nTo set output device, add to AudioPlaybackConnector.json:\n";
		message += L"\"audioOutputDevice\": \"<device name or partial name>\"\n";

		MessageBoxW(nullptr, message.c_str(), L"AudioPlaybackConnector - Available Audio Devices", MB_OK | MB_ICONINFORMATION);
	}
	CATCH_LOG();
}
