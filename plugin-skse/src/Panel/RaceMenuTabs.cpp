// RaceMenuTabs.cpp
// Lifecast

#include "Panel/RaceMenuTabs.h"

#include "Edit/Session.h"
#include "Panel/Bridge.h"

#include <thread>

namespace Lifecast::Panel::RaceMenuTabs
{
	namespace
	{
		constexpr auto kPanels = "_root.RaceSexMenuBaseInstance.RaceSexPanelsInstance";

		// RaceMenu's category flags (racemenubase.psc): RACE 2, FACE 16.
		// Read from the game, the whole list: $ALL 2044, RACE 2, BODY 4, HEAD 8,
		// FACE 16, EYES 32, BROW 64, MOUTH 128, HAIR 256, $EXTRA 512,
		// $EXPRESSIONS 1024, $COLORS 8192, $MAKEUP 16384, $BODY PAINT 32768,
		// $HAND PAINT 65536, $FOOT PAINT 131072, $FACE PAINT 262144, and 0 for
		// the body scale and morph pages.
		constexpr std::uint32_t kFlagRace = 2;
		constexpr std::uint32_t kFlagFace = 16;
			constexpr std::uint32_t kFlagExtra = 512;

		// What each delegated control looks for: a category flag, or failing
		// that a piece of the entry's label (lower case).
		struct Target
		{
			const char*                    key;
			std::uint32_t                  flag;
			std::initializer_list<const char*> words;
		};

		const Target kTargets[] = {
			{ "race-sex", kFlagRace, {} },
			{ "face-sliders", kFlagFace, {} },
			{ "extra", kFlagExtra, {} },
			{ "expressions", 0, { "express" } },
			{ "paints", 0, { "paint", "overlay", "makeup" } },
		};

		std::string Lower(std::string a_s)
		{
			for (auto& c : a_s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return a_s;
		}

		std::string Text(const RE::GFxValue& a_entry)
		{
			RE::GFxValue text;
			if (a_entry.GetMember("text", &text) && text.IsString()) {
				return text.GetString();
			}
			return {};
		}

		// The key a mod registered its category under (AddCategory's keyName,
		// which the film keeps as textFilter). Only a mod's category has one:
		// the film builds its own categories in place and tells them apart by
		// flag. So a non-empty key means "some mod added this tab", which is
		// exactly the set Edit/BodyMorphs builds a section for.
		std::string Key(const RE::GFxValue& a_entry)
		{
			RE::GFxValue key;
			if (a_entry.GetMember("textFilter", &key) && key.IsString()) {
				return key.GetString();
			}
			return {};
		}

		std::uint32_t Flag(const RE::GFxValue& a_entry)
		{
			RE::GFxValue flag;
			if (a_entry.GetMember("flag", &flag) && flag.IsNumber()) {
				return static_cast<std::uint32_t>(flag.GetNumber());
			}
			return 0;
		}

		// A member under its public name, or the underscored field behind it:
		// the game showed the list's entries at "_entryList" and its selection
		// at "_selectedIndex", with no member of the plain names.
		bool Member(const RE::GFxValue& a_obj, const char* a_name, RE::GFxValue& a_out)
		{
			if (a_obj.GetMember(a_name, &a_out) && !a_out.IsUndefined()) {
				return true;
			}
			const auto hidden = std::string("_") + a_name;
			return a_obj.GetMember(hidden.c_str(), &a_out) && !a_out.IsUndefined();
		}

		// The category list: at the known path, or as a member of one of the
		// panels object's direct members.
		bool FindCategoryList(RE::GFxMovieView* a_movie, RE::GFxValue& a_out, std::string& a_where)
		{
			a_where = std::string(kPanels) + ".categoryList";
			if (a_movie->GetVariable(&a_out, a_where.c_str()) && a_out.IsObject()) {
				return true;
			}
			RE::GFxValue panels;
			if (!a_movie->GetVariable(&panels, kPanels) || !panels.IsObject()) {
				a_where = kPanels;
				return false;
			}
			bool found = false;
			panels.VisitMembers([&](const char* a_name, const RE::GFxValue& a_value) {
				if (found || !a_value.IsObject()) {
					return;
				}
				RE::GFxValue list;
				if (a_value.GetMember("categoryList", &list) && list.IsObject()) {
					a_out = list;
					a_where = fmt::format("{}.{}.categoryList", kPanels, a_name);
					found = true;
				}
			});
			return found;
		}

		// RaceMenu's modes - the Sliders / Presets / Camera / Sculpt buttons at
		// the top - are not categories. A dump of the film showed them: four
		// Scaleform RadioButtons, each with its caption at textField.text, an
		// onRelease handler, and a shared group at
		// modeSelect.tabContainer._buttonGroup_buttonGroup, which holds them in
		// "children" (in screen order) and the current one in "selectedButton".
		// Pressing one is its onRelease, the handler a click runs.
		struct Mode
		{
			const char* key;
			const char* word;
		};

		const Mode kModes[] = {
			{ "presets", "preset" },
			{ "sculpt", "sculpt" },
		};

		std::string Caption(const RE::GFxValue& a_button)
		{
			RE::GFxValue field;
			RE::GFxValue text;
			if (a_button.GetMember("textField", &field) && field.IsObject() && field.GetMember("text", &text) &&
				text.IsString()) {
				return text.GetString();
			}
			return {};
		}

		std::string Selected(const RE::GFxValue& a_group)
		{
			RE::GFxValue selected;
			if (a_group.GetMember("selectedButton", &selected) && selected.IsObject()) {
				return Caption(selected);
			}
			return {};
		}

		enum class Pressed
		{
			kNo,       // no group, or no button matches
			kAlready,  // the mode was already the selected one
			kYes,
		};

		Pressed SelectMode(RE::GFxMovieView* a_movie, const std::string& a_for, const char* a_word)
		{
			const auto   where = std::string(kPanels) + ".modeSelect.tabContainer._buttonGroup_buttonGroup";
			RE::GFxValue group;
			RE::GFxValue children;
			if (!a_movie->GetVariable(&group, where.c_str()) || !group.IsObject() ||
				!group.GetMember("children", &children) || !children.IsArray()) {
				spdlog::warn("[tabs] {}: no mode button group with children at {} - RaceMenu left as it is", a_for,
					where);
				return Pressed::kNo;
			}

			const auto   n = children.GetArraySize();
			RE::GFxValue chosen;
			int          at = -1;
			std::string  captions;
			for (std::uint32_t i = 0; i < n; ++i) {
				RE::GFxValue button;
				if (!children.GetElement(i, &button) || !button.IsObject()) {
					captions += fmt::format("{}{}=?", captions.empty() ? "" : ", ", i);
					continue;
				}
				const auto caption = Caption(button);
				captions += fmt::format("{}{}='{}'", captions.empty() ? "" : ", ", i, caption);
				if (at < 0 && Lower(caption).find(a_word) != std::string::npos) {
					at = static_cast<int>(i);
					chosen = button;
				}
			}
			spdlog::info("[tabs] {}: {} mode buttons, '{}' selected: {}", a_for, n, Selected(group), captions);
			if (at < 0) {
				spdlog::info("[tabs] {}: no mode button matches '{}' - RaceMenu left as it is", a_for, a_word);
				return Pressed::kNo;
			}

			const auto want = Caption(chosen);
			if (Lower(Selected(group)).find(a_word) != std::string::npos) {
				spdlog::info("[tabs] {}: mode '{}' already selected", a_for, want);
				return Pressed::kAlready;
			}
			const std::array<RE::GFxValue, 1> zero{ RE::GFxValue(0.0) };
			const bool released = chosen.Invoke("onRelease", nullptr, zero);
			spdlog::info("[tabs] {}: pressed mode button {} '{}' - onRelease {}, selected now '{}'", a_for, at, want,
				released ? "ok" : "failed", Selected(group));
			return Pressed::kYes;
		}

		// The page's way of reaching a category this build does not know by
		// name: it read the live list through Report() and hands back a label.
		constexpr std::string_view kByLabel = "category:";

		void SelectNow(const std::string& a_for, bool a_modeChecked = false)
		{
			std::string wantLabel;
			if (a_for.starts_with(kByLabel)) {
				wantLabel = a_for.substr(kByLabel.size());
			}

			const Target* target = nullptr;
			if (wantLabel.empty()) {
				for (const auto& t : kTargets) {
					if (a_for == t.key) {
						target = &t;
					}
				}
			}
			auto* ui = RE::UI::GetSingleton();
			auto  menu = ui ? ui->GetMenu(RE::RaceSexMenu::MENU_NAME) : nullptr;
			auto* movie = menu ? menu->uiMovie.get() : nullptr;
			if (!movie) {
				spdlog::warn("[tabs] {}: RaceMenu film not reachable", a_for);
				return;
			}
			if (!target && wantLabel.empty()) {
				for (const auto& m : kModes) {
					if (a_for == m.key) {
						SelectMode(movie, a_for, m.word);
						return;
					}
				}
				spdlog::info("[tabs] {}: not a category nor a mode - RaceMenu left as it is", a_for);
				return;
			}

			// Categories live in the Sliders mode. Measured: after Sculpt and
			// then Presets, every category press landed while Presets stayed
			// on screen. Back to Sliders first; the category is pressed a
			// moment later, once the film has swapped its panels.
			if (!a_modeChecked && SelectMode(movie, a_for, "slider") == Pressed::kYes) {
				std::thread([a_for]() {
					std::this_thread::sleep_for(std::chrono::milliseconds(150));
					if (auto* tasks = SKSE::GetTaskInterface()) {
						tasks->AddUITask([a_for]() { SelectNow(a_for, true); });
					}
				}).detach();
				return;
			}

			RE::GFxValue list;
			std::string  where;
			if (!FindCategoryList(movie, list, where)) {
				spdlog::warn("[tabs] {}: no category list found (looked at {})", a_for, where);
				return;
			}
			RE::GFxValue entries;
			if (!Member(list, "entryList", entries) || !entries.IsArray()) {
				spdlog::warn("[tabs] {}: {} has no entryList array", a_for, where);
				return;
			}

			const auto n = entries.GetArraySize();
			int        chosen = -1;
			std::string labels;
			for (std::uint32_t i = 0; i < n; ++i) {
				RE::GFxValue entry;
				if (!entries.GetElement(i, &entry) || !entry.IsObject()) {
					continue;
				}
				const auto text = Text(entry);
				const auto flag = Flag(entry);
				labels += fmt::format("{}{}='{}'/{}", labels.empty() ? "" : ", ", i, text, flag);
				if (chosen >= 0) {
					continue;
				}
				// By label, and exact. This comment used to say the label was
				// the only identifier an entry carries, listing the members of
				// entry 0 to prove it - but entry 0 is "$ALL", one of the
				// film's own, and a category a MOD registered does carry its
				// key as textFilter (measured 2026-09-30 by reading the film;
				// Key() above reads it). The label stays the identifier here
				// because it is the one thing every entry has, mod-made or
				// not, and it is what the page hands back. A substring would
				// let one category's name match another that contains it.
				if (!wantLabel.empty()) {
					if (text == wantLabel) {
						chosen = static_cast<int>(i);
					}
					continue;
				}
				// Exact flag: "$ALL" carries every category bit (2044 in the
				// game) and matched FACE before FACE did.
				if (target->flag != 0 && flag == target->flag) {
					chosen = static_cast<int>(i);
				}
				const auto lower = Lower(text);
				for (const char* w : target->words) {
					if (lower.find(w) != std::string::npos) {
						chosen = static_cast<int>(i);
					}
				}
			}
			spdlog::info("[tabs] {}: {} entries at {}: {}", a_for, n, where, labels);
			RE::GFxValue first;
			if (n > 0 && entries.GetElement(0, &first) && first.IsObject()) {
				std::string members;
				first.VisitMembers([&](const char* a_name, const RE::GFxValue& a_value) {
					members += fmt::format("{}{}:{}", members.empty() ? "" : ", ", a_name,
						static_cast<int>(a_value.GetType()));
				});
				spdlog::info("[tabs] {}: members of entry 0: {}", a_for, members);
			}
			if (chosen < 0) {
				spdlog::info("[tabs] {}: no entry matches - RaceMenu left as it is", a_for);
				return;
			}

			// The same call a click on the entry makes: index, and 0 for "by
			// mouse".
			const std::array<RE::GFxValue, 2> args{ RE::GFxValue(static_cast<double>(chosen)), RE::GFxValue(0.0) };
			const bool ok = list.Invoke("onItemPress", args);
			RE::GFxValue selected;
			const bool   read = Member(list, "selectedIndex", selected) && selected.IsNumber();
			spdlog::info("[tabs] {}: pressed entry {} - invoke {}, selectedIndex now {}", a_for, chosen,
				ok ? "ok" : "failed", read ? fmt::format("{}", selected.GetNumber()) : std::string("unreadable"));
		}

		// What the category list looks like right now: label, flag and key of
		// every entry. Empty when the list cannot be read.
		std::string Signature()
		{
			auto*        ui = RE::UI::GetSingleton();
			auto         menu = ui ? ui->GetMenu(RE::RaceSexMenu::MENU_NAME) : nullptr;
			auto*        movie = menu ? menu->uiMovie.get() : nullptr;
			RE::GFxValue list;
			RE::GFxValue entries;
			std::string  where;
			if (!movie || !FindCategoryList(movie, list, where) || !Member(list, "entryList", entries) ||
				!entries.IsArray()) {
				return {};
			}
			std::string out;
			const auto  n = entries.GetArraySize();
			for (std::uint32_t i = 0; i < n; ++i) {
				RE::GFxValue entry;
				if (!entries.GetElement(i, &entry) || !entry.IsObject()) {
					continue;
				}
				out += fmt::format("{}/{}/{};", Text(entry), Flag(entry), Key(entry));
			}
			return out;
		}

		// Set once the page has asked for the list; the watch below stays
		// silent before that.
		bool                  g_wanted = false;
		std::string           g_signature;
		std::chrono::steady_clock::time_point g_looked{};

		void ReportNow(const char* a_why = "asked for");

		// The category list is not final when the panel comes on screen: a mod
		// adds its tabs from Papyrus after the menu opens (17 entries at the
		// instant, 20 five seconds later - Docs\VERIFY.md V095, D091). One read
		// at a moment we pick would hand the page a list with the mod tabs
		// missing, and a check that every entry has a home would pass over 17 of
		// 20. So the list is watched while the menu is open, and pushed again
		// whenever it changes.
		void OnTick()
		{
			if (!g_wanted) {
				return;
			}
			if (!Edit::Session::MenuOpen()) {
				g_signature.clear();
				return;
			}
			const auto now = std::chrono::steady_clock::now();
			if (now - g_looked < std::chrono::seconds(1)) {
				return;
			}
			g_looked = now;
			auto* tasks = SKSE::GetTaskInterface();
			if (!tasks) {
				return;
			}
			tasks->AddUITask([]() {
				const auto signature = Signature();
				if (signature.empty() || signature == g_signature) {
					return;
				}
				const bool first = g_signature.empty();
				g_signature = signature;
				ReportNow(first ? "the film was read" : "the film changed");
			});
		}

		void OnOpened() { g_signature.clear(); }

		void ReportNow(const char* a_why)
		{
			auto         out = nlohmann::json::array();
			bool         read = false;
			auto*        ui = RE::UI::GetSingleton();
			auto         menu = ui ? ui->GetMenu(RE::RaceSexMenu::MENU_NAME) : nullptr;
			auto*        movie = menu ? menu->uiMovie.get() : nullptr;
			RE::GFxValue list;
			RE::GFxValue entries;
			std::string  where;
			if (!movie) {
				spdlog::warn("[tabs] categories: RaceMenu film not reachable");
			} else if (!FindCategoryList(movie, list, where)) {
				spdlog::warn("[tabs] categories: no category list found (looked at {})", where);
			} else if (!Member(list, "entryList", entries) || !entries.IsArray()) {
				spdlog::warn("[tabs] categories: {} has no entryList array", where);
			} else {
				read = true;
				const auto n = entries.GetArraySize();
				for (std::uint32_t i = 0; i < n; ++i) {
					RE::GFxValue entry;
					if (!entries.GetElement(i, &entry) || !entry.IsObject()) {
						continue;
					}
					out.push_back(
						{ { "index", i }, { "text", Text(entry) }, { "flag", Flag(entry) }, { "key", Key(entry) } });
				}
				spdlog::info("[tabs] categories: sending {} of {} entries from {} ({})", out.size(), n, where, a_why);
			}
			// "ready" says the list was READ, not that it has entries. An empty
			// list and an unreachable film are different answers, and a page
			// that cannot tell them apart would drop every door it should be
			// putting up on a bad read.
			//
			// Logged whole: this line is the page's test case - a cut one does
			// not parse.
			Bridge::Send("racemenu-categories", { { "ready", read }, { "entries", std::move(out) } }, true);
		}
	}

	void Select(const std::string& a_for)
	{
		auto* tasks = SKSE::GetTaskInterface();
		if (!tasks) {
			spdlog::warn("[tabs] {}: no task interface - RaceMenu left as it is", a_for);
			return;
		}
		tasks->AddUITask([a_for]() { SelectNow(a_for); });
	}

	void LeavePresets()
	{
		auto* tasks = SKSE::GetTaskInterface();
		if (!tasks) {
			return;
		}
		tasks->AddUITask([]() {
			auto* ui = RE::UI::GetSingleton();
			auto  menu = ui ? ui->GetMenu(RE::RaceSexMenu::MENU_NAME) : nullptr;
			auto* movie = menu ? menu->uiMovie.get() : nullptr;
			if (!movie) {
				return;
			}
			// A mod that hangs a window on the Presets mode keeps it open for as
			// long as the film stays there; the Sliders mode is where it closes.
			SelectMode(movie, "leave-presets", "slider");
		});
	}

	void Report()
	{
		auto* tasks = SKSE::GetTaskInterface();
		if (!tasks) {
			spdlog::warn("[tabs] categories: no task interface - the page gets no list");
			return;
		}
		g_wanted = true;
		tasks->AddUITask([]() { ReportNow(); });
	}

	void Install()
	{
		// The page's own check that every entry of the live list has a home,
		// run by the page and only recorded here.
		Bridge::On("categories-checked", [](const nlohmann::json& a_data) {
			const auto total = a_data.value("total", 0);
			std::string orphans;
			if (a_data.contains("orphans") && a_data["orphans"].is_array()) {
				for (const auto& o : a_data["orphans"]) {
					orphans += fmt::format("{}'{}'", orphans.empty() ? "" : ", ", o.is_string() ? o.get<std::string>() : "?");
				}
			}
			const auto n = a_data.contains("orphans") && a_data["orphans"].is_array() ? a_data["orphans"].size() : 0;
			spdlog::info("[tabs] home check: {} entries, {} orphans{}{}", total, n, n ? ": " : "", orphans);
		});
		Edit::Session::OnTick(&OnTick);
		Edit::Session::OnMenuOpened(&OnOpened);
	}
}
