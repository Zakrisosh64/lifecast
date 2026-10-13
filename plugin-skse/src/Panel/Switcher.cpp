// Switcher.cpp
// Lifecast

#include "Panel/Switcher.h"

#include "Edit/FilmPaint.h"
#include "Panel/Bridge.h"
#include "Panel/Panel.h"
#include "Panel/RaceMenuFilm.h"
#include "Panel/RaceMenuTabs.h"
#include "Panel/Stage.h"

namespace Lifecast::Panel::Switcher
{
	namespace
	{
		using Key = RE::BSKeyboardDevice::Key;
		using Clock = std::chrono::steady_clock;

		// F4 swaps the two editors - the key the players of the previous
		// RaceMenu front-end already know.
		constexpr std::uint32_t kSwapKey = Key::kF4;

		// How long the panel may stay on screen waiting for Meridian to grant
		// focus. Focus asked for before the page has loaded is applied at
		// load end, so this covers a slow first load; past it, something is
		// wrong and the player gets RaceMenu back rather than a panel that
		// does not answer.
		constexpr auto kFocusGrace = std::chrono::seconds(5);

		enum class Mode
		{
			kClosed,    // the RaceSex Menu is not open
			kLifecast,   // the panel is on screen and owns (or is getting) focus
			kRaceMenu,  // RaceMenu's film is on screen, the panel is hidden
		};

		Mode g_mode = Mode::kClosed;

		// Focus is applied asynchronously, so "not focused" right after the
		// request is normal. Losing focus only counts once it has been seen.
		bool              g_focusSeen = false;
		Clock::time_point g_shownAt{};

		// A native MessageBoxMenu is open over the Lifecast, and the panel has let go of
		// the focus so the box can be answered. A focused browser keeps the keyboard
		// from reaching a native menu and takes the mouse as well (measured in
		// Docs/MEDIDO-E23.md: Esc closes such a box with the panel released, never
		// with it focused). While this is set the focus watch does not count the
		// loss as the player leaving.
		bool g_boxSuspended = false;

		std::uint32_t g_opens = 0;
		std::uint32_t g_swaps = 0;

		const char* ModeName(Mode a_mode)
		{
			switch (a_mode) {
			case Mode::kClosed:
				return "closed";
			case Mode::kLifecast:
				return "lifecast";
			case Mode::kRaceMenu:
				return "racemenu";
			default:
				return "?";
			}
		}

		// RaceMenu's film is hidden and shown through the engine's movie API.
		// The film itself is never modified; it keeps running underneath and
		// keeps every value the player set there.
		void SetFilmVisible(bool a_visible)
		{
			auto* ui = RE::UI::GetSingleton();
			auto  menu = ui ? ui->GetMenu(RE::RaceSexMenu::MENU_NAME) : nullptr;
			auto* movie = menu ? menu->uiMovie.get() : nullptr;
			if (!movie) {
				spdlog::warn("[switch] RaceMenu film not reachable - cannot {} it",
					a_visible ? "show" : "hide");
				return;
			}

			movie->SetVisible(a_visible);
			spdlog::info("[switch] RaceMenu film visible = {} (read back {})",
				a_visible, movie->GetVisible());
		}

		// RaceMenu is usually opened from the console (showracemenu), which
		// stays open underneath. Once the panel owns the keyboard, the key
		// that would close the console never reaches the game, and the
		// console sits over the Lifecast until the player goes back to
		// RaceMenu. So it is closed before the panel asks for focus.
		void CloseConsole()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui || !ui->IsMenuOpen(RE::Console::MENU_NAME)) {
				return;
			}

			auto* queue = RE::UIMessageQueue::GetSingleton();
			if (!queue) {
				spdlog::warn("[switch] the console is open and there is no UI message queue to close it");
				return;
			}

			queue->AddMessage(RE::Console::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
			spdlog::info("[switch] the console was open - asked it to close");
		}

		void SetMode(Mode a_mode)
		{
			spdlog::info("[switch] {} -> {}", ModeName(g_mode), ModeName(a_mode));
			g_mode = a_mode;
		}

		// Runs on every input tick. Puts RaceMenu back when the panel is on
		// screen but no longer owns focus, or never got it.
		void Watch()
		{
			if (g_mode != Mode::kLifecast || g_boxSuspended) {
				return;
			}

			const bool focused = Panel::IsFocused();

			if (focused) {
				if (!g_focusSeen) {
					g_focusSeen = true;
					const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
						Clock::now() - g_shownAt);
					spdlog::info("[switch] focus granted, {} ms after the panel was shown", ms.count());
				}
				return;
			}

			if (g_focusSeen) {
				spdlog::info("[switch] the panel lost focus - back to RaceMenu");
				ShowRaceMenu();
				return;
			}

			if (Clock::now() - g_shownAt > kFocusGrace) {
				spdlog::warn("[switch] focus was never granted within {} s - back to RaceMenu",
					kFocusGrace.count());
				ShowRaceMenu();
			}
		}

		void OnMenuOpened()
		{
			++g_opens;
			spdlog::info("[switch] RaceSex Menu opened (#{})", g_opens);

			if (!Panel::Ensure()) {
				// No panel this time. RaceMenu stays exactly as it was, and
				// the player loses nothing but the Lifecast.
				SetMode(Mode::kRaceMenu);
				return;
			}

			SetMode(Mode::kRaceMenu);
			ShowLifecast();
		}

		// A message box that is not the Lifecast's own (the Lifecast answers its own the
		// instant the game queues it, see RaceMenuFilm) opens over the panel: give the
		// focus up while it is there and take it back when it is gone.
		void OnBoxOpened()
		{
			if (g_mode != Mode::kLifecast) {
				return;
			}

			// The Lifecast answers the game's own box the instant it is queued, and the
			// event for it can still arrive after the answer: an empty queue is that.
			const auto* box = RE::MessageBoxMenu::GetCurrentMessageBoxData();
			if (!box) {
				spdlog::info("[switch] a message box opened and its queue is empty - answered already, focus kept");
				return;
			}
			spdlog::info("[switch] a message box opened over the Lifecast ('{}') - the panel lets go of the focus",
				box ? box->bodyText.c_str() : "?");
			g_boxSuspended = true;
			Panel::ReleaseFocus();
		}

		void OnBoxClosed()
		{
			if (!g_boxSuspended) {
				return;
			}

			g_boxSuspended = false;
			if (g_mode != Mode::kLifecast) {
				return;
			}

			spdlog::info("[switch] the message box closed - the panel takes the focus back");
			g_focusSeen = false;
			g_shownAt = Clock::now();
			Panel::RequestFocus();
		}

		void OnMenuClosed()
		{
			spdlog::info("[switch] RaceSex Menu closed");
			g_boxSuspended = false;

			// What the page shows for a moment - a refusal, a save preview, a
			// way to RaceMenu - belongs to the menu session it was shown in,
			// and the page is told that session ended. The hidden panel is not
			// that signal: F4 hides it too, and the player comes back to it.
			Bridge::Send("menu-closed");

			// The film goes away with the menu. The panel is hidden, not
			// released: re-creating it would make a new browser instance.
			Panel::DisarmFocusChord();
			Panel::SetVisible(false);
			SetMode(Mode::kClosed);
		}

		// "show-racemenu" { "for": what }, where "for" is optional: the
		// control the page hands over to RaceMenu, logged so a round shows
		// which way the player went.
		void OnPageShowRaceMenu(const nlohmann::json& a_data)
		{
			const bool hasFor = a_data.is_object() && a_data.contains("for") && a_data["for"].is_string();
			spdlog::info("[switch] the page asked for RaceMenu{}{}", hasFor ? " - for " : "",
				hasFor ? a_data["for"].get<std::string>() : std::string());
			ShowRaceMenu();
			if (hasFor) {
				RaceMenuTabs::Select(a_data["for"].get<std::string>());
			}
		}

		// "racemenu-categories-get" with no data: the page asking what
		// categories RaceMenu actually has on this install. The answer travels
		// back as its own "racemenu-categories" message - the bridge is
		// fire-and-forget both ways.
		void OnPageCategories(const nlohmann::json&)
		{
			RaceMenuTabs::Report();
		}

		class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static MenuSink* GetSingleton()
			{
				static MenuSink s_instance;
				return &s_instance;
			}

			RE::BSEventNotifyControl ProcessEvent(
				const RE::MenuOpenCloseEvent* a_event,
				RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && a_event->menuName == RE::RaceSexMenu::MENU_NAME) {
					if (a_event->opening) {
						OnMenuOpened();
					} else {
						OnMenuClosed();
					}
				} else if (a_event && a_event->menuName == RE::MessageBoxMenu::MENU_NAME) {
					if (a_event->opening) {
						OnBoxOpened();
					} else {
						OnBoxClosed();
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			MenuSink() = default;
		};

		class InputSink : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			static InputSink* GetSingleton()
			{
				static InputSink s_instance;
				return &s_instance;
			}

			RE::BSEventNotifyControl ProcessEvent(
				RE::InputEvent* const* a_event,
				RE::BSTEventSource<RE::InputEvent*>*) override
			{
				Watch();

				// This is the input tick that drives the stage, not a frame
				// tick: it fires on input dispatch, measured between 56.7
				// and 64.6 a second whether the panel holds focus or not,
				// so it is also where the character view slides. A second
				// tick of its own would buy nothing extra; the stage reads
				// two atomics and returns unless something is moving or the
				// one-figure rule is on and watching its subject drift.
				Stage::Tick();

				if (!a_event || g_mode != Mode::kRaceMenu) {
					// With the Lifecast on screen, F4 never gets here: it is
					// the panel's focus chord and Meridian consumes it.
					return RE::BSEventNotifyControl::kContinue;
				}

				for (auto* event = *a_event; event; event = event->next) {
					if (event->GetDevice() != RE::INPUT_DEVICES::kKeyboard) {
						continue;
					}

					const auto* button = event->AsButtonEvent();
					if (button && button->IsDown() && button->GetIDCode() == kSwapKey) {
						spdlog::info("[switch] F4 from the game");
						ShowLifecast();
						break;
					}
				}

				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			InputSink() = default;
		};

		bool g_installed = false;
	}

	void Install()
	{
		if (g_installed) {
			return;
		}

		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			spdlog::error("[switch] no UI singleton - the Lifecast will never open");
			return;
		}
		ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::GetSingleton());

		auto* input = RE::BSInputDeviceManager::GetSingleton();
		if (input) {
			input->AddEventSink(InputSink::GetSingleton());
		} else {
			// The panel still opens with the menu and the page can still send
			// the player back, but F4 from RaceMenu is dead and so is the
			// focus watch. Said here so it is not discovered by pressing F4.
			spdlog::error("[switch] no BSInputDeviceManager - F4 from RaceMenu will not work");
		}

		Bridge::On("show-racemenu", &OnPageShowRaceMenu);
		Bridge::On("racemenu-categories-get", &OnPageCategories);

		g_installed = true;
		spdlog::info("[switch] installed - F4 swaps the Lifecast and RaceMenu");
	}

	void ShowLifecast()
	{
		if (g_mode != Mode::kRaceMenu || !Panel::Browser()) {
			return;
		}

		++g_swaps;
		spdlog::info("[switch] show the Lifecast (swap #{})", g_swaps);

		// The order is the point. The way out exists before the panel can
		// take the keyboard; focus is the very last thing asked for.
		CloseConsole();
		Panel::ArmFocusChord(kSwapKey, 0);
		Panel::SetVisible(true);
		SetFilmVisible(false);
		RaceMenuTabs::LeavePresets();

		g_focusSeen = false;
		g_shownAt = Clock::now();
		SetMode(Mode::kLifecast);

		Panel::RequestFocus();
	}

	void ShowRaceMenu()
	{
		if (g_mode != Mode::kLifecast) {
			return;
		}

		++g_swaps;
		spdlog::info("[switch] show RaceMenu (swap #{})", g_swaps);

		// The panel gives its focus up and shows only its card. The chord comes
		// off with it, so F4 reaches the game again and brings the Lifecast back.
		Panel::ShowCard("F4");
		Panel::DisarmFocusChord();
		SetFilmVisible(true);
		RaceMenuFilm::Sync();
		Edit::FilmPaint::Sync("switch to RaceMenu");

		SetMode(Mode::kRaceMenu);
	}
}
