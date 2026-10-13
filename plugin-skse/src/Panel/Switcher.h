// Switcher.h
// Lifecast
//
// Which of the two editors the player is looking at while the RaceSex Menu is
// open: the Lifecast panel or RaceMenu's own film. Only one is on screen at a
// time, and F4 swaps them.
//
//   RaceSex Menu opens   -> the Lifecast, every time, whatever opened the menu
//   F4, Lifecast showing  -> RaceMenu
//   F4, RaceMenu showing -> the Lifecast
//   RaceSex Menu closes  -> the panel is hidden, never destroyed
//
// Showing the Lifecast means taking Meridian's focus, because an unfocused
// browser receives no input at all. A focused browser swallows every key and
// mouse event, this plugin's own included - so the ways back are armed BEFORE
// focus is asked for, every single time:
//
//   1. F4, registered with Meridian as the panel's focus chord. Meridian
//      evaluates chords before a focused browser can swallow the key, so this
//      works even when the page is broken or frozen. It is armed only while
//      the panel is shown: a chord left armed on a hidden browser would hand
//      focus to something invisible.
//   2. A control inside the page, which sends "show-racemenu" through the
//      bridge. It works even if the chord does not, because the page is
//      exactly what holds the keyboard and the mouse.
//
// Whichever way focus goes away - F4, the page, or another Meridian consumer
// claiming it - the switcher notices on its next input tick and puts
// RaceMenu back on screen, so the player is never left with a visible panel
// that cannot be used and a film that cannot be seen. If focus is never
// granted at all, the same thing happens after a short grace period.

#pragma once

namespace Lifecast::Panel::Switcher
{
	// Registers the menu and input sinks and the page's "show-racemenu"
	// message. Call once, at kDataLoaded. Safe to call twice.
	void Install();

	// The two directions. Both do nothing while the RaceSex Menu is closed.
	void ShowLifecast();
	void ShowRaceMenu();

	// A window another mod hangs on RaceMenu's Presets mode, in front of the
	// scene with the film still hidden: the panel gives its focus up and shows
	// its card, and F4 brings the Lifecast back. Only from the Lifecast.
	void ShowPortraits();
}
