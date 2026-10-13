// Undress.cpp
// Lifecast

#include "Edit/Undress.h"

#include "Edit/Compare.h"
#include "Edit/Session.h"
#include "Panel/Bridge.h"

#include <atomic>
#include <chrono>
#include <thread>

// Windows.h turns GetObject into GetObjectA, which hides
// BGSDefaultObjectManager::GetObject.
#undef GetObject

namespace Lifecast::Edit::Undress
{
	namespace
	{
		bool g_on = false;

		// Every worn (object, exact worn extraList) pair, as found the moment
		// undress ran. The extraList is only a key to look for at re-dress,
		// never a pointer to use: it may be freed by then - see Undress.h.
		// `left` is set for a weapon or torch carried in the left hand: the
		// engine puts a hand item with no slot in the right hand, so one that
		// came off the left has to be told where to go.
		struct Worn
		{
			RE::TESBoundObject* object = nullptr;
			RE::ExtraDataList*  extraList = nullptr;
			bool                left = false;
		};
		std::vector<Worn> g_worn;

		// Bumped on every undress and dress; the result read back after a
		// dress is dropped when another one started in the meantime.
		std::atomic<std::uint32_t> g_generation{ 0 };

		// Carried in a hand (as opposed to worn on the body): a weapon or a
		// torch. A shield is armour and is worn, whichever hand it sits in.
		bool IsHandItem(const RE::TESForm* a_object)
		{
			return a_object && (a_object->IsWeapon() || a_object->As<RE::TESObjectLIGH>());
		}

		// A two-hander (or a bow) fills both hands, so it reads as held in
		// the left hand too; it is one item and goes back with no slot.
		bool FillsBothHands(const RE::TESForm* a_object)
		{
			const auto* weapon = a_object ? a_object->As<RE::TESObjectWEAP>() : nullptr;
			return !weapon || weapon->IsTwoHandedSword() || weapon->IsTwoHandedAxe() || weapon->IsBow() ||
			       weapon->IsCrossbow();
		}

		// Which entry of `a_found` is the one in the left hand. The extra
		// list cannot say: GetWorn() is true for either hand and the library
		// has no left-hand variant, so the engine's own hand slots are read.
		// Same weapon in both hands: one entry per hand, and a missing second
		// one is added (it goes back through the engine's pick of the stack).
		void MarkLeftHand(RE::Actor* a_actor, std::vector<Worn>& a_found)
		{
			auto* inLeft = a_actor->GetEquippedObject(true);
			auto* inRight = a_actor->GetEquippedObject(false);
			if (!IsHandItem(inLeft)) {
				return;
			}
			if (inLeft == inRight) {
				if (FillsBothHands(inLeft)) {
					return;
				}
				std::size_t same = 0;
				for (auto& w : a_found) {
					same += (w.object == inLeft);
				}
				if (same == 0) {
					return;
				}
				if (same == 1) {
					a_found.push_back({ static_cast<RE::TESBoundObject*>(inLeft), nullptr, true });
					return;
				}
				for (auto it = a_found.rbegin(); it != a_found.rend(); ++it) {
					if (it->object == inLeft) {
						it->left = true;
						return;
					}
				}
				return;
			}
			for (auto& w : a_found) {
				if (w.object == inLeft) {
					w.left = true;
				}
			}
		}

		// Every ExtraDataList* actually worn right now, one entry per stack
		// that GetWorn() finds true - an object can have more than one stack
		// and only the worn one is ever touched (commonlib-scout, 2026-09-29).
		std::vector<Worn> CollectWorn(RE::Actor* a_actor)
		{
			std::vector<Worn> found;
			for (auto& [object, pair] : a_actor->GetInventory()) {
				auto* entryData = pair.second.get();
				if (!entryData || !entryData->extraLists) {
					continue;
				}
				for (auto* xList : *entryData->extraLists) {
					if (xList && xList->GetWorn()) {
						found.push_back({ object, xList, false });
					}
				}
			}
			MarkLeftHand(a_actor, found);
			return found;
		}

		void PushUndress(const char* a_why)
		{
			spdlog::info("[undress] {}: on = {}", a_why, g_on);
			Panel::Bridge::Send("undress", { { "on", g_on } });
		}

		void DoUndress()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* equipManager = RE::ActorEquipManager::GetSingleton();
			if (!player || !equipManager) {
				return;
			}
			++g_generation;
			g_worn = CollectWorn(player);
			for (auto& worn : g_worn) {
				equipManager->UnequipObject(player, worn.object, worn.extraList);
			}
			spdlog::info("[undress] unequipped {} worn item(s)", g_worn.size());
		}

		// What was asked of one item at re-dress, to be checked against what the
		// engine holds a moment later. `kept` is the hand it came off (L or R)
		// for a weapon or torch, W for anything worn on the body.
		struct Asked
		{
			RE::TESBoundObject* object = nullptr;
			char                kept = 'W';
			bool                viaSlot = false;
		};

		// Reads the player's hands and inventory and says, per item, whether it is
		// on - in the hand it came off - and not only whether EquipObject was
		// called. Runs on the game thread, some time after the dress.
		void ReportResult(const std::vector<Asked>& a_items, std::size_t a_gone, std::uint32_t a_generation)
		{
			if (a_generation != g_generation.load()) {
				spdlog::info("[undress] result: superseded by a later undress/dress - not judged");
				return;
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}
			const auto* inLeft = player->GetEquippedObject(true);
			const auto* inRight = player->GetEquippedObject(false);
			const auto  inventory = player->GetInventory();
			std::size_t ok = 0;
			std::size_t notOn = 0;
			std::size_t otherHand = 0;
			for (const auto& item : a_items) {
				char now = '-';
				if (item.kept == 'W') {
					const auto it = inventory.find(item.object);
					const auto* entry = it != inventory.end() ? it->second.second.get() : nullptr;
					if (entry && (entry->IsWorn(false) || entry->IsWorn(true))) {
						now = 'W';
					}
				} else {
					const bool wantLeft = item.kept == 'L';
					const auto* here = wantLeft ? inLeft : inRight;
					const auto* there = wantLeft ? inRight : inLeft;
					if (here == item.object) {
						now = item.kept;
					} else if (there == item.object) {
						now = wantLeft ? 'R' : 'L';
					}
				}
				const char* verdict = "ok";
				if (now == item.kept) {
					++ok;
				} else if (now == '-') {
					verdict = "NOT ON";
					++notOn;
				} else {
					verdict = "WRONG HAND";
					++otherHand;
				}
				const char* name = item.object ? item.object->GetName() : nullptr;
				spdlog::info("[undress] item {:08X} '{}': kept {}, asked {}, now {}: {}",
					item.object ? item.object->GetFormID() : 0, name ? name : "", item.kept,
					item.viaSlot ? "the left-hand slot" : "the engine's pick", now, verdict);
			}
			spdlog::info("[undress] result: {} of {} item(s) on as they were, {} not on, {} in the other hand; {} no longer "
						 "in the inventory",
				ok, a_items.size(), notOn, otherHand, a_gone);
		}

		void DoDress()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* equipManager = RE::ActorEquipManager::GetSingleton();
			if (!player || !equipManager) {
				g_worn.clear();
				return;
			}
			const std::uint32_t generation = ++g_generation;
			// The pointer kept at undress is never handed back as it is: a list
			// that held nothing but the worn flag is freed by the engine the
			// moment the item comes off, and EquipObject would then read freed
			// memory - see Undress.h. It is looked for among the object's lists
			// NOW, by address only (never dereferenced): found, the item goes
			// back on that same instance, temper and enchantment included; not
			// found, it goes back with no list and the engine picks the stack.
			const auto  wornNow = CollectWorn(player);
			auto        inventory = player->GetInventory();
			std::size_t sameList = 0;
			std::size_t engineList = 0;
			std::size_t gone = 0;
			std::vector<Asked> asked;

			// With no slot the engine puts a weapon in the right hand, and the one
			// put on after it takes its place. What came off the left hand is put
			// on last and asked for the left-hand slot by name; everything else
			// goes back as it always did.
			const RE::BGSEquipSlot* leftSlot = nullptr;
			if (auto* defaults = RE::BGSDefaultObjectManager::GetSingleton()) {
				leftSlot = defaults->GetObject<RE::BGSEquipSlot>(RE::DEFAULT_OBJECT::kLeftHandEquip);
			}
			if (!leftSlot) {
				spdlog::warn("[undress] the left-hand equip slot is not available - the left hand goes back with no slot");
			}

			for (const bool leftPass : { false, true }) {
				for (auto& kept : g_worn) {
					if (kept.left != leftPass) {
						continue;
					}
					const auto it = inventory.find(kept.object);
					if (it == inventory.end() || it->second.first <= 0) {
						spdlog::info("[undress] {:08X} is no longer in the inventory - not re-equipped",
							kept.object ? kept.object->GetFormID() : 0);
						++gone;
						continue;
					}
					RE::ExtraDataList* list = nullptr;
					if (auto* entry = it->second.second.get(); entry && entry->extraLists) {
						for (auto* xList : *entry->extraLists) {
							if (xList == kept.extraList) {
								list = xList;
								break;
							}
						}
					}
					const bool alreadyWorn = std::any_of(wornNow.begin(), wornNow.end(), [&](const Worn& w) {
						return w.object == kept.object && (!list || w.extraList == list);
					});
					if (alreadyWorn) {
						continue;
					}
					const RE::BGSEquipSlot* slot = kept.left ? leftSlot : nullptr;
					equipManager->EquipObject(player, kept.object, list, 1, slot);
					++(list ? sameList : engineList);
					asked.push_back({ kept.object, IsHandItem(kept.object) ? (kept.left ? 'L' : 'R') : 'W', slot != nullptr });
				}
			}
			spdlog::info("[undress] re-equipped {} item(s): {} on the list they came off, {} on a list the engine "
						 "picks; {} no longer in the inventory",
				sameList + engineList, sameList, engineList, gone);
			g_worn.clear();

			// EquipObject is a request; what the engine did with it is read back
			// once it has had time to apply.
			if (!asked.empty()) {
				std::thread([asked = std::move(asked), gone, generation]() {
					std::this_thread::sleep_for(std::chrono::milliseconds(700));
					if (auto* tasks = SKSE::GetTaskInterface()) {
						tasks->AddTask([asked, gone, generation]() { ReportResult(asked, gone, generation); });
					}
				}).detach();
			}
		}

		void OnUndressGet(const nlohmann::json&) { PushUndress("the page asked"); }

		void OnUndressSet(const nlohmann::json& a_data)
		{
			if (!Session::MenuOpen()) {
				spdlog::warn("[undress] undress-set with the RaceSex Menu closed - ignored");
				return;
			}
			if (!a_data.is_object() || !a_data.contains("on") || !a_data["on"].is_boolean()) {
				spdlog::warn("[undress] undress-set without a boolean \"on\" - ignored");
				return;
			}
			const bool wantOn = a_data["on"].get<bool>();
			if (wantOn == g_on) {
				PushUndress("undress-set, already there");
				return;
			}
			if (wantOn) {
				DoUndress();
			} else {
				DoDress();
			}
			g_on = wantOn;
			// The second figure wears what the player wears. Clothes are
			// CONTEXT and not what is under comparison (D106): a dressed
			// "before" beside an undressed "after" compares the clothes. Does
			// nothing when the comparison is off, which is the common case.
			Compare::MirrorWorn();
			Session::RequestRedraw(true);
			PushUndress("after undress-set");
		}

		void OnOpened()
		{
			g_on = false;
			g_worn.clear();
			// Pushed, not just reset: the page cannot be trusted to ask again
			// on its own (it did not, across a Finish/reopen, and that is the
			// defect found in game - EMENDA de 2026-09-29). The label is
			// fixed to what this call says, never to what the character is
			// actually wearing - by design, so no dressed/undressed detection
			// is needed (see Undress.h).
			PushUndress("menu opened");
		}

		void OnClosed()
		{
			if (g_on) {
				DoDress();
				spdlog::info("[undress] the RaceSex Menu closed while undressed - dressed back");
			}
			g_on = false;
		}

		bool g_installed = false;
	}

	void Install()
	{
		if (g_installed) {
			return;
		}
		Session::OnMenuOpened(&OnOpened);
		Session::OnMenuClosed(&OnClosed);
		Panel::Bridge::On("undress-get", &OnUndressGet);
		Panel::Bridge::On("undress-set", &OnUndressSet);
		g_installed = true;
		spdlog::info("[undress] installed");
	}
}
