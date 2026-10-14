// Pad.h
// Lifecast - the controller, while the panel has the focus.
//
// A focused Meridian browser hears the keyboard and the mouse and never the
// controller (the page has no gamepad events of its own). This module is the
// controller's way in: an input sink that, while the panel holds Meridian's
// focus, turns every controller event into a "pad" message to the page, and
// the page (pad.js) moves a focus ring and clicks.
//
// Messages, dll -> page, type "pad":
//
//   { "b": "<button>", "id": <idCode>, "phase": "down" | "repeat" | "up" }
//   { "b": "LStick" | "RStick", "x": <-1..1>, "y": <-1..1> }
//   { "reset": true }
//
// Button names: A B X Y LB RB Back Start LS RS LT RT DUp DDown DLeft DRight.
// A held button repeats after kRepeatFirst and then every kRepeatEvery. A
// stick is sent only when its position, past the dead zone and rounded to a
// step, changes - holding it still sends nothing, and coming back inside the
// dead zone sends one (0, 0). "reset" goes out when the panel loses the focus
// with a button down or a stick off centre, so nothing stays held on the page.
//
// Nothing is consumed here: the sink always returns kContinue. With the panel
// focused the RaceSex Menu behind it does not react to the controller - that
// was measured before this file was written (D252), so there is nothing to
// block, and a sink that stopped the dispatch would also starve every sink
// after it, this plugin's own ticks included.

#pragma once

namespace Lifecast::Panel::Pad
{
	// Registers the input sink. Call once, at kDataLoaded. Safe to call twice.
	void Install();
}