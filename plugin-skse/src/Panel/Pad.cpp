// Pad.cpp
// Lifecast

#include "Panel/Pad.h"

#include "Panel/Bridge.h"
#include "Panel/Panel.h"

#include <array>
#include <cmath>

namespace Lifecast::Panel::Pad
{
	namespace
	{
		using json = nlohmann::json;

		// Seconds a button has to be held before it repeats, and between repeats.
		constexpr float kRepeatFirst = 0.40f;
		constexpr float kRepeatEvery = 0.10f;

		// A stick inside this radius is at rest; outside it, its position is
		// rounded to kStickStep before it is compared with the last one sent.
		constexpr float kDeadZone = 0.25f;
		constexpr float kStickStep = 0.05f;

		struct Name
		{
			std::uint32_t id;
			const char*   name;
		};

		// The controller's idCodes as the game reports them (the same codes on
		// every input context; the user event a button maps to is not).
		constexpr std::array<Name, 16> kNames{ {
			{ 0x0001, "DUp" },
			{ 0x0002, "DDown" },
			{ 0x0004, "DLeft" },
			{ 0x0008, "DRight" },
			{ 0x0010, "Start" },
			{ 0x0020, "Back" },
			{ 0x0040, "LS" },
			{ 0x0080, "RS" },
			{ 0x0100, "LB" },
			{ 0x0200, "RB" },
			{ 0x1000, "A" },
			{ 0x2000, "B" },
			{ 0x4000, "X" },
			{ 0x8000, "Y" },
			{ 0x0009, "LT" },
			{ 0x000A, "RT" },
		} };

		const char* NameOf(std::uint32_t a_id)
		{
			for (const auto& n : kNames) {
				if (n.id == a_id) {
					return n.name;
				}
			}
			return nullptr;
		}

		struct Held
		{
			bool  down = false;
			float next = kRepeatFirst;
		};

		struct Stick
		{
			float x = 0.0f;
			float y = 0.0f;
		};

		std::array<Held, kNames.size()> g_held{};
		std::array<Stick, 2>            g_sticks{};  // left, right
		bool                            g_active = false;

		Held* HeldOf(std::uint32_t a_id)
		{
			for (std::size_t i = 0; i < kNames.size(); ++i) {
				if (kNames[i].id == a_id) {
					return &g_held[i];
				}
			}
			return nullptr;
		}

		float Round(float a_v)
		{
			return std::round(a_v / kStickStep) * kStickStep;
		}

		bool AnythingHeld()
		{
			for (const auto& h : g_held) {
				if (h.down) {
					return true;
				}
			}
			for (const auto& s : g_sticks) {
				if (s.x != 0.0f || s.y != 0.0f) {
					return true;
				}
			}
			return false;
		}

		void Clear()
		{
			g_held.fill(Held{});
			g_sticks.fill(Stick{});
		}

		void OnButton(const RE::ButtonEvent& a_button)
		{
			const auto  id = a_button.GetIDCode();
			const auto* name = NameOf(id);
			auto*       held = HeldOf(id);
			if (!name || !held) {
				return;
			}

			const char* phase = nullptr;
			if (a_button.IsDown()) {
				held->down = true;
				held->next = kRepeatFirst;
				phase = "down";
			} else if (a_button.IsUp()) {
				// An up whose down was never seen (pressed before the panel took
				// the focus) still goes out: the page ignores what it did not see
				// go down.
				held->down = false;
				phase = "up";
			} else if (a_button.IsHeld() && held->down && a_button.HeldDuration() >= held->next) {
				held->next = a_button.HeldDuration() + kRepeatEvery;
				phase = "repeat";
			}

			if (phase) {
				Bridge::Send("pad", json{ { "b", name }, { "id", id }, { "phase", phase } });
			}
		}

		void OnStick(const RE::ThumbstickEvent& a_stick)
		{
			const bool left = a_stick.IsLeft();
			if (!left && !a_stick.IsRight()) {
				return;
			}
			float x = a_stick.xValue;
			float y = a_stick.yValue;
			if (std::sqrt(x * x + y * y) < kDeadZone) {
				x = 0.0f;
				y = 0.0f;
			} else {
				x = Round(x);
				y = Round(y);
			}

			auto& last = g_sticks[left ? 0 : 1];
			if (x == last.x && y == last.y) {
				return;
			}
			last.x = x;
			last.y = y;
			Bridge::Send("pad", json{ { "b", left ? "LStick" : "RStick" }, { "x", x }, { "y", y } });
		}

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
				// The panel has to hold Meridian's focus: shown and focused is
				// the only state in which the page is what the player is using.
				// With RaceMenu's film in front, or another mod's window, the
				// focus is given up and the controller is left to the game.
				const bool active = Panel::IsFocused();
				if (!active) {
					if (g_active) {
						if (AnythingHeld()) {
							Bridge::Send("pad", json{ { "reset", true } });
						}
						Clear();
						spdlog::info("[pad] the panel lost the focus; the controller goes back to the game");
					}
					g_active = false;
					return RE::BSEventNotifyControl::kContinue;
				}
				if (!g_active) {
					g_active = true;
					Clear();
					spdlog::info("[pad] the panel has the focus; the controller goes to the page");
				}

				if (!a_event) {
					return RE::BSEventNotifyControl::kContinue;
				}
				for (auto* event = *a_event; event; event = event->next) {
					if (event->GetDevice() != RE::INPUT_DEVICES::kGamepad) {
						continue;
					}
					if (const auto* button = event->AsButtonEvent()) {
						OnButton(*button);
					} else if (const auto* stick = event->AsThumbstickEvent()) {
						OnStick(*stick);
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
		auto* input = RE::BSInputDeviceManager::GetSingleton();
		if (!input) {
			spdlog::error("[pad] no BSInputDeviceManager - the controller will not reach the panel");
			return;
		}
		input->AddEventSink(InputSink::GetSingleton());
		g_installed = true;
		spdlog::info("[pad] installed");
	}
}