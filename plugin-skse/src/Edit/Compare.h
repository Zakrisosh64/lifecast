// Compare.h
// Lifecast - the side by side: the character as she was, next to the
// character as she is being edited. Fase 6, E3b.
//
// The second figure is ONE duplicate of the player's own actor base - the
// twin - and she is the "before". The player's own character is the "after",
// and it already draws without us.
//
// WHAT IS MEASURED AND WHAT IS NOT. This module is written on top of a phase
// of measurement in the game itself, boot by boot, and the rounds that shaped
// it are named here because each one closed a door:
//
//   boot 80   a twin born WITH the RaceSex Menu already open does not draw.
//             Her birth therefore cannot wait for the button, and this module
//             makes her on a tick with the menu CLOSED.
//   boot 92   a twin moved to a guard cell and brought back does NOT draw
//             again - four reconstruction paths were tried and none restored
//             her. So she is never sent to another cell: she waits in the
//             player's own cell, culled.
//   boot 92   no field read so far separates "drawn" from "present and not
//             culled" - Is3DLoaded, node present and GetAppCulled gave the
//             same three values with her visible and with her invisible. So
//             nothing here reports that she is ON SCREEN. It reports what was
//             called and what was read back, and the eye does the rest.
//   boot 91   CreateDuplicateForm does NOT carry what the player is wearing:
//             the twin was born in her base's default outfit, helmet and all,
//             with no equipped FormID in common with the player. Copying the
//             clothes is therefore explicit work, and it is below.
//   boot 92   the twin takes no pose from the behavior graph: idles are
//             accepted and nothing moves, with the AI enabled and the player
//             moving to the same calls in the same seconds. She stands and
//             does not animate. WHAT SHAPE she rests in was never measured,
//             and a still frame of a standing figure does not settle it - an
//             earlier reading of this comment said bind pose, which was an
//             inference and was withdrawn. NOT this module's to fix (D103
//             lost its object by that route) - stated here so that nobody
//             reads the still figure as a defect of this file.
//
// WHAT COSTS, AND WHEN. Culled, the root node is not drawn and the subgraph
// is not walked - animation, skinning and SMP all stop there (D104). The
// measured cost of the comparison (-2,4% to +2,1% of a frame, D101) is paid
// only while the button is on, which is what makes the button a design
// decision and not a preference.
//
// SHE IS NOT DROPPED AND NOT DELETED, ever. SetDelete poisons the next
// duplicate - measured in V099, and it is what crashed boots 76 and 77. One
// twin per game session, made once, reused by every opening in that session.
//
// WHAT THIS MODULE DOES NOT DO, declared rather than left to be discovered:
//
//   across cells   she is made beside the player and stays in that cell. A
//                  player who walks away and opens the menu elsewhere has no
//                  twin there, and the button says so instead of moving her:
//                  moving her between cells is the path boot 92 measured as
//                  not coming back.
//   across saves   she is not persisted (PlaceObjectAtMe with forcePersist
//                  false) and there is no co-save, so nothing of her is
//                  written to the player's save and nothing of her survives
//                  a load. D100 accepts a resident twin; the mechanism that
//                  would make one is the guard cell, and boot 92 left it
//                  OPEN - neither freed nor condemned.
//   her collision  she stands in the world while she waits, culled. Culling
//                  removes the drawing, not the body. Whether the player can
//                  walk into her is NOT measured, and nothing here tries to
//                  turn it off. TESObjectREFR::SetCollision(bool) exists, but
//                  it is not a call into the engine: it flips the
//                  kCollisionsDisabled bit of formFlags and nothing else, the
//                  header marks that bit "?", and the bit shares its value
//                  (1 << 4) with kIsGroundPiece in the same enum. Setting an
//                  ambiguous flag on a live reference to fix something nobody
//                  has measured is not a trade this module makes - it goes up
//                  as a candidate instead.
//
// Messages (see Panel/Bridge.h for the envelope). The prefix is "compare-"
// and NOT "twin-": "twin-" belongs to the Fase 6 probe, and a production
// handler under that name would be answered by the probe when it is linked
// back in.
//
//   page -> dll  "compare-get"  {}
//   page -> dll  "compare-set"  { "on": bool, "frame": { "left": 0..1,
//                                 "right": 0..1 } }
//   page -> dll  "compare-turn" { "deg": number }  a DELTA, |deg| <= 180; ignored
//                                 unless the comparison is on. She turns about her
//                                 own spot; the total is kept until the editor
//                                 closes, and Place() gives it back.
//   page -> dll  "compare-move" { "across": number, "depth": number }  a DELTA in
//                                 world units, either key may be left out; ignored
//                                 unless the comparison is on. across is screen
//                                 right, depth away from the camera. The total is
//                                 kept by Stage (PairShift), cut to its limits,
//                                 and kept like the turn.
//   dll -> page  "compare"      { "on": bool, "why": text when it refused }
//
// "frame" is the stage element's own left and right edge as a fraction of
// the window width, and the page is the only side that knows it: the panel
// covers the left of the screen, so the middle of the WINDOW is not the
// middle of what the player can see. Without it the rule centres the pair on
// the whole window and the panel eats the left half of the result.

#pragma once

namespace Lifecast::Edit::Compare
{
	// Registers the bridge handlers and the session callbacks. Call once, at
	// kDataLoaded, after Session::Install. Safe to call twice.
	void Install();

	// The player's worn items changed under this module's feet - Undress ran.
	// Does nothing unless the comparison is on; when it is, the twin is
	// dressed again from what the player wears NOW.
	//
	// The twin mirrors what is CONTEXT and freezes what is under comparison
	// (D106), and clothes are context: comparing a dressed "before" against
	// an undressed "after" compares the clothes.
	// E36: no figure of this module may be in a save. Called on kSaveGame.
	void BeforeSave();

	// A game is loading or a new one starts: forget the old game's figure.
	void GameChanged();

	void MirrorWorn();
}
