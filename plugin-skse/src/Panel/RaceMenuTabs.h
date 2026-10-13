// RaceMenuTabs.h
// Lifecast - opening RaceMenu on the category a delegated control lives in.
//
// The panel's "Open" buttons hand a control back to RaceMenu. Without this
// RaceMenu comes back on whatever category it was on, which after a fresh
// open is its first one. Here the film is asked, through the engine's movie
// API, to press the matching entry of its own category list - the same thing
// a click on that entry does. The film is never modified.
//
// Best effort: the category list is found by name, its entries are read
// back and logged, and when nothing matches RaceMenu is simply left where
// it is. Presets and Sculpt are RaceMenu modes, not categories: for those the
// matching button of the film's mode selector is pressed instead.

#pragma once

namespace Lifecast::Panel::RaceMenuTabs
{
	// a_for is the key the page sends with "show-racemenu". Queued on the
	// UI thread; returns at once.
	//
	// "category:<label>" presses the entry whose label is exactly <label>,
	// which is how a category this build has never heard of is still reachable:
	// the page reads the live list through Report() and hands a label back.
	void Select(const std::string& a_for);

	// Puts the film back on its Sliders mode if it is anywhere else, so that a
	// window another mod opens on the Presets mode closes when the Lifecast comes
	// back in front. Queued on the UI thread; returns at once.
	void LeavePresets();

	// The whole category list as the film holds it now - index, label and flag
	// of every entry - pushed to the page as "racemenu-categories". Queued on
	// the UI thread; returns at once.
	//
	// The list is the player's, not ours: which categories exist depends on
	// which mods inject them, so the page decides what it covers by matching
	// labels against its own list rather than trusting a count.
	void Report();

	// Once the page has asked through Report(), the list is watched while the
	// menu is open and pushed again whenever it changes: a mod's tabs arrive
	// after the menu opens. Call after Edit::Session::Install().
	void Install();
}
