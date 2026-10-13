// Stage.cpp
// Lifecast

#include "Panel/Stage.h"

#include "Edit/Session.h"
#include "Panel/Bridge.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

namespace Lifecast::Panel::Stage
{
	namespace
	{
		using json = nlohmann::json;
		using Rot = std::array<double, 9>;  // flat, row after row - RaceMenu's order
		using Clock = std::chrono::steady_clock;

		constexpr auto   kCharGen = "_global.skse.plugins.CharGen";
		constexpr double kPi = 3.14159265358979323846;

		// How much of the gap that is left the current position covers in one
		// tick, and how close to the target counts as arrived.
		//
		// WORKING VALUES, not chosen ones. The only evidence that exists about
		// what this camera sliding should feel like is the engine's own zoom
		// delegate, and that is one grandeza, one boot, and a movement with no
		// value to read - not a population to derive a rate from. The number
		// is picked once it has been seen moving in game.
		//
		// The snap epsilons are in the units each grandeza is measured in:
		// game units for distance and height, degrees for turn and tilt.
		//
		// kEase is one of the seven the panel can retune (Stage.h); it stays in
		// this file rather than moving to the header because nothing outside
		// reads it, and LIFECAST_TUNABLE works just as well on a name with
		// internal linkage. The two snap epsilons are NOT tunable: they decide
		// when a slide has ARRIVED, which is correctness and not taste.
		LIFECAST_TUNABLE kEase = 0.25;
#ifdef LIFECAST_TUNING
		// What the DLL did with each number the page last sent, in words, one entry
		// per key. A number that is accepted and then changes nothing - a limit the
		// camera is already inside, a pair that is off - looks exactly like a number
		// that was ignored; this is what tells the two apart. Read by State().
		nlohmann::json g_tuneSaid = nlohmann::json::object();

		// The seven as they were COMPILED, taken at load and never written again.
		// "Reset" on the page sends these back through the same stage-tune path
		// every typed number takes, so a reset is a number like any other and the
		// DLL has no second way to set one. By design (2026-10-06): reset
		// returns the compiled default, not the value the box opened with.
		const nlohmann::json g_tuneCompiled = { { "maxDistance", kMaxDistance }, { "liftUp", kLiftUp },
			{ "panMax", kPanMax }, { "tiltMax", kTiltMax }, { "pairApart", kPairApart }, { "pairSpan", kPairSpan },
			{ "ease", kEase } };

		// What the last SolvePair wanted against what it got, for the same
		// "A LIMIT WAS HIT" the log carries. Written by SolvePair, read by OnTune
		// right after it, so the box can say the pair was cut and by how much.
		struct PairCut
		{
			double wantedDistance = 0.0;
			double distance = 0.0;
			double wantedPan = 0.0;
			double pan = 0.0;
		} g_pairCut;
#endif
		constexpr double kSnapUnits = 0.05;
		constexpr double kSnapDegrees = 0.05;

		// THE SUBJECT DOES NOT HOLD STILL, and that is measured rather than
		// supposed: with the camera parked to the third decimal, the head bone
		// sweeps 190 to 230 px of 2560 in 13 s (Fase 7, E4d, boot 137). Worse
		// for a rule that aims once, the sweep is SLOW and not stationary - the
		// mean of one half of a 13 s record differs from the other half by up
		// to 6.1% of the stage, and even a 10 s rolling mean moves 2.0 to 2.5%
		// between neighbouring windows, which is the whole tolerance the rule
		// is judged by. So there is no short window to average over: no STATIC
		// aim, however well chosen, can promise where the subject will be for
		// the next ten seconds.
		//
		// Hence the rule re-aims. kTrackMs is how often it looks, and
		// kTrackBand is how far off centre it tolerates before it moves - a
		// dead band, so that a camera chasing the last pixel is not what the
		// player watches. The band is HALF the frame check's own tolerance
		// (1% of the character view), so correcting inside it would be the
		// rule fighting its own audit.
		//
		// Neither is tunable and neither is taste: they are what makes the rule
		// track a measured movement, and the seven tuning knobs are
		// elsewhere (Stage.h).
		constexpr int    kTrackMs = 100;
		constexpr double kTrackBand = 0.005;  // of the character view's width

		// The solve reads the projection curve instead of extrapolating a
		// slope: kSolveSteps caps the secant, kSolveEps is how close the
		// PREDICTED landing has to be before it stops. A tenth of the frame
		// check's tolerance, so a residual in the log is the camera disagreeing
		// with the model and never the solve stopping early.
		constexpr int    kSolveSteps = 8;
		constexpr double kSolveEps = 0.001;  // of the character view's width

		// The five RaceMenu calls this module needs; any one missing and the
		// view is refused, since a half-working view is worse than none.
		constexpr std::array<const char*, 5> kCalls{ "GetRaceSexCameraPos", "SetRaceSexCameraPos", "GetPlayerPosition",
			"GetPlayerRotation", "SetPlayerRotation" };

		// ----- what the menu showed when it opened (UI thread only) -----------

		struct Frame
		{
			bool          have{ false };
			RE::NiPoint3  camera;     // camera position, as RaceMenu reports it
			RE::NiPoint3  player;     // player position, as RaceMenu reports it
			Rot           body{};     // the body's rotation
			bool          node{ false };
			RE::NiMatrix3 cameraRot;  // the camera node's local rotation
			double        distance{ 0.0 };
			double        dirX{ 0.0 };  // unit vector, player to camera, ground plane
			double        dirY{ 0.0 };
			// Where the camera REALLY looks, ground plane, unit. The line player-to-camera above is not it: RaceMenu
			// leaves the figure off to one side, so the two differ by tens of degrees (47 at the IM02 opening).
			double        rightX{ 0.0 };
			double        rightY{ 0.0 };
		};

		struct At
		{
			double turn{ 0.0 };
			double distance{ 0.0 };
			double lift{ 0.0 };
			double pan{ 0.0 };
			double tilt{ 0.0 };
		};

		Frame       g_frame;
		At          g_target;  // where the page asked the view to be
		At          g_at;      // where it is now, which is what was last written
		std::string g_why;     // why the view is not ready, when it is not

		// Read by the tick on the game thread, written by the work on the UI
		// thread. A stale read costs one frame of the slide and nothing else,
		// so it is relaxed.
		std::atomic<bool> g_moving{ false };

		// The slide under way, for the log: this is the measurement the change
		// exists to produce, and there is nowhere else to take it from.
		std::uint32_t     g_ticks = 0;
		Clock::time_point g_slideAt{};
		double            g_workUs = 0.0;
		double            g_worstUs = 0.0;

		// ----- requests from the page (game thread), folded ----------------------

		struct Pending
		{
			double turn{ 0.0 };
			double distance{ 0.0 };
			double lift{ 0.0 };
			double pan{ 0.0 };
			double tilt{ 0.0 };
			bool   reset{ false };
			bool   any{ false };
			// 0 nothing, 1 standard, 2 free; the last one wins. And how many
			// far <-> near swaps were asked (they add up: two are none).
			int    mode{ 0 };
			int    zoom{ 0 };
			// The lock on the character (Acabamento 9): 0 nothing, 1 on, 2 off.
			// The last one wins, like the mode.
			int    lock{ 0 };
			// The framing rule: 0 nothing, 1 frame the pair, 2 stop framing
			// it. Not a bool, because "no request" and "turn it off" are
			// different things and folding them loses the second.
			int    pair{ 0 };
			double frameLeft{ 0.0 };
			double frameRight{ 1.0 };
			// The single-figure rule, the same three fields and for the same
			// reason. Its own, not shared with the pair's: a message for one
			// must never be read as a message for the other.
			int    one{ 0 };
			double oneLeft{ 0.0 };
			double oneRight{ 1.0 };
			// Whether the page is waiting for an answer. A tick is not: the
			// state it would send is the target, and the target is exactly
			// what a tick does not change.
			bool answer{ false };
		};

		bool g_installed = false;

		std::mutex g_lock;
		Pending    g_pending;
		bool       g_queued = false;

		// The framing rule, while it is on (UI thread). The edges are kept
		// because the audit after the slide needs the same frame the solve
		// used, and the page is not asked twice.
		bool   g_pairOn = false;
		double g_pairLeft = 0.0;
		double g_pairRight = 1.0;
		int    g_pairSolves = 0;  // how many times the rule has solved for this pairing

		// Where the player moved the second subject (PairShift, Stage.h). Written
		// on the game thread by Edit/Compare, read here on the UI thread.
		std::atomic<double> g_shiftAcross{ 0.0 };
		std::atomic<double> g_shiftDepth{ 0.0 };

		// The single-figure framing rule (UI thread), kept apart from the pair's
		// state rather than folded into it: the two rules answer different
		// questions, only one may own the camera at a time, and sharing the
		// fields would make "which rule is on" a thing to infer instead of a
		// thing to read.
		bool   g_oneOn = false;
		double g_oneLeft = 0.0;
		double g_oneRight = 1.0;
		int    g_oneSolves = 0;  // how many times it has solved for this view

		// Where the last solve PREDICTED the aim would land, so the check after
		// the slide compares the model with the camera instead of only with the
		// centre. A model that is right and a camera that moved look the same in
		// a residual; these two numbers tell them apart.
		double g_onePredicted = 0.0;

		// The re-aim: when it last looked, and how many times it has moved the
		// camera this opening. The count is the measurement of how much the
		// subject drifted, and a count of zero with a wide band is a tracker
		// that was written and never acted.
		int               g_oneTracks = 0;
		Clock::time_point g_trackAt{};
		Clock::time_point g_holdAt{};

		// g_oneOn, as an atomic, for Tick() alone. Tick() runs on the INPUT
		// thread - that is why g_moving is an atomic and not a bool - and it
		// used to return whenever nothing was moving. With the tracker living
		// inside Work(), that gate meant the tracker only ever got a look while
		// a slide was already under way: it closed its own slide and then went
		// quiet, which is how boot 138 re-aimed 3 times in 60 seconds instead
		// of once every 100 ms. Reading g_oneOn there would be a data race, so
		// the gate reads this and every write to g_oneOn writes it too.
		std::atomic<bool> g_oneTracking{ false };

		// The price of the gate above: with the rule on, Work() now runs on
		// every input tick and not only during a slide. These two say what that
		// costs, measured rather than assumed, and they are reported when the
		// rule goes off - a tick budget nobody ever reads is not a budget.
		int    g_restTicks = 0;
		double g_restWorstUs = 0.0;

		// THE LOCK ON THE CHARACTER (Acabamento 9). ON, the default, is the rule above
		// as it always was: the camera keeps the subject framed, and re-aims as she
		// drifts. OFF, nothing of ours aims the camera any more - no solve on any
		// trigger, no re-aim - and it stays where it is until the player moves it. The
		// edges the page sent are kept either way, so turning it back on frames at
		// once. Session only, back to ON when the menu closes (D157). UI thread.
		// The player's own sideways travel, in world units along the camera's REAL right, from where the rule (or
		// a reset) last left it. Zero whenever the rule solves. The old limit was kept on the pan axis, which runs
		// along the player-camera perpendicular and not along the screen: a drag to one side also moved the camera
		// in depth, so it came out of the frame long before the limit (D158, redone in D159).
		double g_lat = 0.0;
		double PanLo() { return -kPanMax; }
		double PanHi() { return kPanMax; }

		// Born OFF (E26, F14/F30: by design the lock is switched on by hand). It was born ON in every mode, the first
		// "stage" payload of a boot already said lock:true with free:false.
		bool g_lockOn = false;

		// THE VERTICAL HALF OF THE LOCK (Fase 8, E21, F30). The rule above solved PAN alone, which keeps the
		// figure centred sideways and says nothing of up and down: lifting or lowering the camera, or tilting it,
		// took the head out of the window with the lock on exactly as with it off (boot E21, 206 steps: the head's
		// y was the same within 0.02 either way while its x stayed at 0.50 only with the lock on). So the lock now
		// also holds the height the head landed at when the free camera took over, by TILT - the one axis that
		// turns the view without moving the camera. g_oneY0 is that height, 0..1 of the window; it is read the
		// first time the aim is measured with the camera still at the opening (lift 0, tilt 0) and dropped with
		// every new reading of the frame. The band is of the window's height, as kTrackBand is of the view's width.
		bool   g_oneY0Have = false;
		double g_oneY0 = 0.0;
		constexpr double kTrackBandY = 0.01;

		void SetOneOn(bool a_on)
		{
			g_oneOn = a_on;
			g_oneTracking.store(a_on && g_lockOn, std::memory_order_relaxed);
		}

		// THE MODE (Acabamento 4, D152). Standard is what the menu opens in: RaceMenu's
		// own two camera positions, swapped by LAlt, and the only thing of ours on it
		// is the turn of the body: no write to the camera at all. Free is the camera
		// of before, all five moves, with the framing rule. g_freeBefore is what the comparison gives back when it closes. All UI thread.
		bool g_free = false;
		bool g_freeBefore = false;
		bool g_pairForced = false;  // the comparison is what made it free

		// Whether the kept frame has been read a SECOND time this opening, and
		// what the first reading said. See Refresh().
		bool   g_reread = false;
		double g_firstDistance = 0.0;

		// ----- small helpers ------------------------------------------------------

		std::string P(const RE::NiPoint3& a_p)
		{
			return fmt::format("({:.2f}, {:.2f}, {:.2f})", a_p.x, a_p.y, a_p.z);
		}

		double Clamp(double a_v, double a_lo, double a_hi)
		{
			return a_v < a_lo ? a_lo : (a_v > a_hi ? a_hi : a_v);
		}

		double Wrap(double a_deg)
		{
			double d = std::fmod(a_deg, 360.0);
			if (d > 180.0) {
				d -= 360.0;
			} else if (d <= -180.0) {
				d += 360.0;
			}
			return d;
		}

		// Clamps, and says so. A target that is silently clipped leaves the
		// page showing a value nobody asked for with nothing in the log to
		// explain where it came from.
		double ClampSaid(double a_v, double a_lo, double a_hi, const char* a_what)
		{
			const double c = Clamp(a_v, a_lo, a_hi);
			if (c != a_v) {
				spdlog::info("[stage] {} target {:.2f} is outside {:.2f} .. {:.2f} - clamped to {:.2f}", a_what, a_v,
					a_lo, a_hi, c);
			}
			return c;
		}

		// One step of the slide. Returns whether the value moved, which is
		// what decides if this grandeza gets written this tick.
		bool Step(double& a_cur, double a_target, double a_snap)
		{
			const double gap = a_target - a_cur;
			if (std::abs(gap) <= a_snap) {
				if (a_cur == a_target) {
					return false;
				}
				a_cur = a_target;  // exactly, so arrival is a comparison and not a guess
				return true;
			}
			a_cur += gap * kEase;
			return true;
		}

		// The same, the short way round: turn lives on a circle, so a target
		// just past 180 degrees is a small move and not an almost whole turn.
		bool StepAngle(double& a_cur, double a_target, double a_snap)
		{
			const double gap = Wrap(a_target - a_cur);
			if (std::abs(gap) <= a_snap) {
				if (a_cur == a_target) {
					return false;
				}
				a_cur = a_target;
				return true;
			}
			a_cur = Wrap(a_cur + gap * kEase);
			return true;
		}

		RE::GPtr<RE::RaceSexMenu> Menu()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui ? ui->GetMenu<RE::RaceSexMenu>() : RE::GPtr<RE::RaceSexMenu>();
		}

		bool CharGen(RE::GFxMovieView* a_movie, RE::GFxValue& a_out)
		{
			return a_movie && a_movie->GetVariable(&a_out, kCharGen) && a_out.IsObject();
		}

		bool GetPoint(RE::GFxValue& a_cg, const char* a_fn, RE::NiPoint3& a_out)
		{
			RE::GFxValue result;
			if (!a_cg.Invoke(a_fn, &result) || !result.IsObject()) {
				spdlog::warn("[stage] CharGen.{} gave no object", a_fn);
				return false;
			}
			const std::array<const char*, 3> names{ "x", "y", "z" };
			std::array<double, 3>            xyz{};
			for (std::size_t i = 0; i < names.size(); ++i) {
				RE::GFxValue v;
				if (!result.GetMember(names[i], &v) || !v.IsNumber()) {
					spdlog::warn("[stage] CharGen.{}: no number '{}'", a_fn, names[i]);
					return false;
				}
				xyz[i] = v.GetNumber();
			}
			a_out = RE::NiPoint3(static_cast<float>(xyz[0]), static_cast<float>(xyz[1]), static_cast<float>(xyz[2]));
			return true;
		}

		bool SetCameraPos(RE::GFxMovieView* a_movie, RE::GFxValue& a_cg, const RE::NiPoint3& a_p)
		{
			RE::GFxValue obj;
			a_movie->CreateObject(&obj);
			if (!obj.IsObject()) {
				spdlog::warn("[stage] CreateObject failed - SetRaceSexCameraPos not called");
				return false;
			}
			obj.SetMember("x", RE::GFxValue(static_cast<double>(a_p.x)));
			obj.SetMember("y", RE::GFxValue(static_cast<double>(a_p.y)));
			obj.SetMember("z", RE::GFxValue(static_cast<double>(a_p.z)));
			return a_cg.Invoke("SetRaceSexCameraPos", nullptr, &obj, 1);
		}

		bool GetBody(RE::GFxValue& a_cg, Rot& a_out)
		{
			RE::GFxValue result;
			if (!a_cg.Invoke("GetPlayerRotation", &result) || !result.IsArray() || result.GetArraySize() != 9) {
				spdlog::warn("[stage] CharGen.GetPlayerRotation gave no array of 9");
				return false;
			}
			for (std::uint32_t i = 0; i < 9; ++i) {
				RE::GFxValue v;
				if (!result.GetElement(i, &v) || !v.IsNumber()) {
					spdlog::warn("[stage] CharGen.GetPlayerRotation: element {} is not a number", i);
					return false;
				}
				a_out[i] = v.GetNumber();
			}
			return true;
		}

		bool SetBody(RE::GFxMovieView* a_movie, RE::GFxValue& a_cg, const Rot& a_r)
		{
			RE::GFxValue arr;
			a_movie->CreateArray(&arr);
			if (!arr.IsArray()) {
				spdlog::warn("[stage] CreateArray failed - SetPlayerRotation not called");
				return false;
			}
			for (const double v : a_r) {
				arr.PushBack(RE::GFxValue(v));
			}
			// RaceMenu asserts exactly 9, and its assert does not stop a
			// release build: refuse here instead of there.
			if (arr.GetArraySize() != 9) {
				spdlog::warn("[stage] rotation array has {} elements - SetPlayerRotation not called", arr.GetArraySize());
				return false;
			}
			return a_cg.Invoke("SetPlayerRotation", nullptr, &arr, 1);
		}

		// Rz(a_deg) * a_m, both flat row after row.
		Rot TurnedBy(const Rot& a_m, double a_deg)
		{
			const double                c = std::cos(a_deg * kPi / 180.0);
			const double                s = std::sin(a_deg * kPi / 180.0);
			const std::array<double, 9> z{ c, -s, 0.0, s, c, 0.0, 0.0, 0.0, 1.0 };
			Rot                         out{};
			for (int r = 0; r < 3; ++r) {
				for (int col = 0; col < 3; ++col) {
					double sum = 0.0;
					for (int k = 0; k < 3; ++k) {
						sum += z[r * 3 + k] * a_m[k * 3 + col];
					}
					out[r * 3 + col] = sum;
				}
			}
			return out;
		}

		// A rotation of a_deg about the unit axis (a_x, a_y, a_z), column
		// vectors (Rodrigues).
		RE::NiMatrix3 AxisAngle(double a_x, double a_y, double a_z, double a_deg)
		{
			const double  t = a_deg * kPi / 180.0;
			const double  c = std::cos(t);
			const double  s = std::sin(t);
			const double  k = 1.0 - c;
			RE::NiMatrix3 m;
			m.entry[0][0] = static_cast<float>(c + a_x * a_x * k);
			m.entry[0][1] = static_cast<float>(a_x * a_y * k - a_z * s);
			m.entry[0][2] = static_cast<float>(a_x * a_z * k + a_y * s);
			m.entry[1][0] = static_cast<float>(a_y * a_x * k + a_z * s);
			m.entry[1][1] = static_cast<float>(c + a_y * a_y * k);
			m.entry[1][2] = static_cast<float>(a_y * a_z * k - a_x * s);
			m.entry[2][0] = static_cast<float>(a_z * a_x * k - a_y * s);
			m.entry[2][1] = static_cast<float>(a_z * a_y * k + a_x * s);
			m.entry[2][2] = static_cast<float>(c + a_z * a_z * k);
			return m;
		}

		double LiftMin()
		{
			const double room = g_frame.camera.z - g_frame.player.z - kFloorMargin;
			return room > 0.0 ? -room : 0.0;
		}

		double DistanceMin()
		{
			// Never force the camera further away than the menu itself put it.
			return g_frame.distance < kMinDistance ? g_frame.distance : kMinDistance;
		}

		// ----- the framing rule (UI thread, with the kept frame) -----------------

		// Where the two subjects are aimed at: the player's own look-at point,
		// and the same point kPairApart to the side, across the line of sight.
		//
		// The second one is where the twin's head is BY CONSTRUCTION and not by
		// measurement of her: Edit/Compare places her at the player's position
		// plus that same offset, and she is a duplicate of the player's base,
		// so she is the same height. Projecting a point rather than her head
		// node is also what lets the rule be solved BEFORE she is there.
		bool Aims(RE::NiPoint3& a_player, RE::NiPoint3& a_other)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return false;
			}
			a_player = player->GetLookingAtLocation();
			// (-dirY, dirX) is the player-camera direction turned 90 degrees in
			// the ground plane - the same perpendicular pan moves along, and the
			// same one the probe measured as the axis that actually separates
			// two figures on screen. Pushing the other subject TOWARDS the
			// camera does not make her bigger: past a point she disappears
			// (V107), so this offset has no component along the line of sight.
			//
			// The player's own shift of her (E14) is added on the same two
			// axes: across on the perpendicular, depth along -dir, which is
			// away from the camera. Both are zero until she is moved.
			const double perpX = -g_frame.dirY;
			const double perpY = g_frame.dirX;
			const double across = kPairApart + g_shiftAcross.load(std::memory_order_relaxed);
			const double depth = g_shiftDepth.load(std::memory_order_relaxed);
			a_other = RE::NiPoint3(static_cast<float>(a_player.x + perpX * across - g_frame.dirX * depth),
				static_cast<float>(a_player.y + perpY * across - g_frame.dirY * depth), a_player.z);
			return true;
		}

		// Asks the engine's own camera where a world point lands, as a fraction
		// of the window's width. Nothing here assumes a field of view, a
		// resolution or where RaceMenu aims: all three come out of this call.
		//
		// The port is read and divided by rather than trusted to be 0..1. If it
		// is the normalized viewport its width is 1 and the division changes
		// nothing; if it is in pixels the division is what makes the number
		// comparable with the edges the page sends. Both are logged, so which
		// one it is stops being a guess after the first run.
		bool ScreenX(RE::NiCamera* a_cam, const RE::NiPoint3& a_point, double& a_x, double& a_y, double& a_depth)
		{
			if (!a_cam) {
				return false;
			}
			float x = 0.0f;
			float y = 0.0f;
			float z = 0.0f;
			if (!a_cam->WorldPtToScreenPt3(a_point, x, y, z, 1e-5f)) {
				return false;
			}
			const auto&  port = a_cam->GetRuntimeData2().port;
			const double w = port.GetWidth();
			const double h = port.GetHeight();
			a_x = w > 0.0 ? x / w : x;
			a_y = h > 0.0 ? y / h : y;
			a_depth = z;
			return true;
		}

		// Projects both aims and reports where they landed. Returns false, with
		// the reason logged, when any step of it is missing - and a rule that
		// cannot measure does not guess: it leaves the camera alone.
		bool Measure(double& a_x1, double& a_x2, const char* a_when)
		{
			auto* cam = RE::Main::WorldRootCamera();
			if (!cam) {
				spdlog::warn("[stage] pair {}: there is no world camera to ask - the rule does not move anything", a_when);
				return false;
			}
			RE::NiPoint3 one;
			RE::NiPoint3 two;
			if (!Aims(one, two)) {
				spdlog::warn("[stage] pair {}: no player to aim at", a_when);
				return false;
			}
			double y1 = 0.0;
			double y2 = 0.0;
			double d1 = 0.0;
			double d2 = 0.0;
			if (!ScreenX(cam, one, a_x1, y1, d1) || !ScreenX(cam, two, a_x2, y2, d2)) {
				spdlog::warn("[stage] pair {}: the camera would not project one of the two aims", a_when);
				return false;
			}
			const auto& port = cam->GetRuntimeData2().port;
			spdlog::info("[stage] pair {}: player head at x {:.4f} y {:.4f} depth {:.1f} | the other at x {:.4f} y "
						 "{:.4f} depth {:.1f} | apart {:.4f} of the window | port {:.1f} x {:.1f}",
				a_when, a_x1, y1, d1, a_x2, y2, d2, a_x2 - a_x1, port.GetWidth(), port.GetHeight());
			return true;
		}

		// Solves for distance and pan, and writes them on the target. Returns
		// whether it solved; a refusal leaves the view exactly as it was.
		bool SolvePair(const char* a_when)
		{
			double x1 = 0.0;
			double x2 = 0.0;
			if (!Measure(x1, x2, a_when)) {
				return false;
			}
			const double span = g_pairRight - g_pairLeft;
			const double want = span * kPairSpan;
			const double sepNow = x2 - x1;
			if (span <= 0.0 || want <= 0.0) {
				spdlog::warn("[stage] pair {}: the page gave a character view {:.4f} wide - nothing to frame into",
					a_when, span);
				return false;
			}
			if (std::abs(sepNow) < 1e-6) {
				spdlog::warn("[stage] pair {}: the two aims project to the same point ({:.6f} apart) - the camera "
							 "cannot be solved from that",
					a_when, sepNow);
				return false;
			}

			// x = x0 + k * lateral / depth, with lateral measured across the
			// line of sight and depth along it. Two points at the same depth
			// give k, and k with either point gives x0. Both are properties of
			// THIS opening, not of this file.
			//
			// The second aim is `apart` to the side and `deeper` further away
			// once the player has moved her (E14); with both at their resting
			// values (kPairApart, 0) this is the same k as before. With depth,
			// the two points no longer share one, so k takes each at its own;
			// the distance and pan below stay the same-depth model, and the
			// audit after the slide is what measures how far that is off.
			const double now = g_at.distance;
			const double apart = kPairApart + g_shiftAcross.load(std::memory_order_relaxed);
			const double deeper = g_shiftDepth.load(std::memory_order_relaxed);
			const double per = (apart - g_at.pan) / (now + deeper) + g_at.pan / now;
			if (!(now + deeper > 1.0) || std::abs(per) < 1e-6) {
				spdlog::warn("[stage] pair {}: the second aim is {:.1f} deeper at distance {:.2f} - no camera scale can "
							 "be read from that",
					a_when, deeper, now);
				return false;
			}
			const double k = sepNow / per;
			const double x0 = x1 + k * g_at.pan / now;

			// Distance so the pair spans what was asked: the separation on
			// screen falls as 1/depth, so the distance scales by exactly how
			// much too wide the pair is now.
			const double wantedDistance = now * std::abs(sepNow) / want;
			const double distance = ClampSaid(wantedDistance, DistanceMin(), kMaxDistance, "distance");

			// Pan so the pair's middle lands on the middle of the character
			// view. The middle of the pair is half the separation to the side
			// of the player, so the camera has to go the same way.
			const double centre = (g_pairLeft + g_pairRight) / 2.0;
			const double wantedPan = apart / 2.0 - (centre - x0) * distance / k;
			const double pan = ClampSaid(wantedPan, -kPanMax, kPanMax, "pan");

			spdlog::info("[stage] pair {}: view {:.3f}..{:.3f}, asking for {:.3f} of the window between the heads. "
						 "Measured k {:.4f} and centre-of-view {:.4f} at distance {:.2f}, pan {:.2f}; the second aim "
						 "{:.1f} across, {:+.1f} deep",
				a_when, g_pairLeft, g_pairRight, want, k, x0, now, g_at.pan, apart, deeper);
			spdlog::info("[stage] pair {}: solved distance {:.2f} (wanted {:.2f}) and pan {:.2f} (wanted {:.2f}){}",
				a_when, distance, wantedDistance, pan, wantedPan,
				(distance != wantedDistance || pan != wantedPan)
					? " - A LIMIT WAS HIT, and these two limits are ours: the pair comes out closer together than "
					  "asked for, and the number the rule would have needed is the 'wanted' above"
					: "");

#ifdef LIFECAST_TUNING
			g_pairCut = { wantedDistance, distance, wantedPan, pan };
#endif
			g_target.distance = distance;
			g_target.pan = pan;
			g_lat = 0.0;
			++g_pairSolves;
			return true;
		}

		// What the slide actually produced, projected again at the camera it
		// arrived at. Everything SolvePair does is a model of a camera, and
		// the model is not the camera: this is where the two are compared.
		//
		// At most ONE correction follows, and the cap is deliberate. A rule
		// that keeps correcting itself towards a target it never reaches is
		// not converging, it is hunting - and the third small variation of the
		// same attempt is where a wrong premise stops being worth another try.
		// A second miss is left standing in the log, which is the measurement.
		void AuditPair()
		{
			if (!g_pairOn || g_pairSolves < 1 || g_pairSolves > 2) {
				return;
			}
			double x1 = 0.0;
			double x2 = 0.0;
			if (!Measure(x1, x2, g_pairSolves == 1 ? "arrived" : "arrived after the correction")) {
				g_pairSolves = 3;
				return;
			}
			const double got = std::abs(x2 - x1);
			const double want = (g_pairRight - g_pairLeft) * kPairSpan;
			const double middle = (x1 + x2) / 2.0;
			const double centre = (g_pairLeft + g_pairRight) / 2.0;
			const double sepOff = want > 0.0 ? (got - want) / want : 0.0;
			spdlog::info("[stage] pair check: the heads came out {:.4f} of the window apart against {:.4f} asked "
						 "({:+.1f}%), and their middle at {:.4f} against {:.4f} ({:+.4f})",
				got, want, sepOff * 100.0, middle, centre, middle - centre);

			// The tolerances are working values, and they are about what an eye
			// can see rather than what a float can measure: a tenth of the
			// separation, and a fiftieth of the window off centre.
			constexpr double kSepTolerance = 0.10;
			constexpr double kCentreTolerance = 0.02;
			const bool       wide = std::abs(sepOff) > kSepTolerance;
			const bool       offCentre = std::abs(middle - centre) > kCentreTolerance;
			if (g_pairSolves == 1 && (wide || offCentre)) {
				spdlog::info("[stage] pair check: outside tolerance ({}{}{}) - solving once more, and only once",
					wide ? "separation" : "", (wide && offCentre) ? " and " : "", offCentre ? "centre" : "");
				if (SolvePair("correction")) {
					// The tick stopped when the slide arrived; this target is
					// new, so it has to be told to start again.
					g_moving.store(true, std::memory_order_relaxed);
					return;
				}
			}
			if (g_pairSolves == 2 && (wide || offCentre)) {
				spdlog::warn("[stage] pair check: still outside tolerance after the one correction. The rule stops "
							 "here on purpose - this line is the measurement, not a failure to retry");
			}
			g_pairSolves = 3;
		}

		// ----- the framing rule for ONE figure (UI thread, with the kept frame) ---

		// Where the subject is aimed at, and the two probe points either side of
		// her across the line of sight. NOTHING stands at the probes - they are
		// projected to measure the camera's lateral scale and no more, which is
		// also why this rule needs no second actor and reopens nothing of the
		// paused comparison.
		//
		// They straddle the aim rather than reaching away from it on one side,
		// so what comes out is the slope AT the subject. The pair rule's single
		// far probe is what the archived logs show going wrong: a chord to a
		// point with half the subject's depth is not the slope anywhere near her.
		// WHERE THE FACE IS, and it is a node rather than a number. E4d measured
		// that GetLookingAtLocation() returns the HEAD BONE's own position -
		// A minus the head node's world.translate was (0,0,0) in all 29 readings
		// - so today's aim is already a point of the body, and not the one that
		// wants framing: the head bone sits at the base of the skull, and the
		// capture's bilateral-symmetry axis is read at the FACE. E4d measured
		// the gap as +24 to +76 px, growing with the head's yaw.
		//
		// NPCEyeBone closes it without a constant of ours. Read from the
		// skeleton this instance actually loads, its translate relative to
		// "NPC Head [Head]" is (-0.082, +8.096, +3.712): on the head's midline
		// - the very quantity the symmetry detector measures - 8.1 units
		// forward and 3.7 up. That is the same magnitude E4d's yaw fit implied
		// for the face, arrived at from the mesh instead of from a correlation.
		//
		// Falls back to the head node and then to GetLookingAtLocation(), so a
		// skeleton without the node frames worse rather than not at all. Which
		// one answered is logged once per measurement, because a fallback that
		// is silent is a fallback nobody knows they are measuring.
		const char* AimNode(RE::PlayerCharacter* a_player, RE::NiPoint3& a_out)
		{
			for (const char* name : { "NPCEyeBone", "NPC Head [Head]" }) {
				if (auto* node = a_player->GetNodeByName(RE::BSFixedString(name))) {
					a_out = node->world.translate;
					return name;
				}
			}
			a_out = a_player->GetLookingAtLocation();
			return "GetLookingAtLocation";
		}

		bool AimOne(RE::NiPoint3& a_aim, RE::NiPoint3& a_left, RE::NiPoint3& a_right, const char*& a_from)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return false;
			}
			a_from = AimNode(player, a_aim);
			// (-dirY, dirX) is the player-camera direction turned 90 degrees in
			// the ground plane: the very axis pan moves along, so a world offset
			// measured here is already in the quantity the solve has to answer in.
			const double perpX = -g_frame.dirY;
			const double perpY = g_frame.dirX;
			const double half = kProbeStep / 2.0;
			a_left = RE::NiPoint3(static_cast<float>(a_aim.x - perpX * half),
				static_cast<float>(a_aim.y - perpY * half), a_aim.z);
			a_right = RE::NiPoint3(static_cast<float>(a_aim.x + perpX * half),
				static_cast<float>(a_aim.y + perpY * half), a_aim.z);
			return true;
		}

		// One look at the camera: where the subject lands, how much of the window
		// a world unit across the line of sight is worth there, and the camera
		// and aim point themselves, so the solve can go on projecting without
		// asking again. Nothing here is a constant of this file: field of view,
		// resolution, the player's height and wherever RaceMenu left the camera
		// all move these, and all of them come out of this one call.
		struct Look
		{
			RE::NiCamera* cam{ nullptr };
			RE::NiPoint3  aim;
			const char*   from{ "" };
			double        perpX{ 0.0 };
			double        perpY{ 0.0 };
			double        aimX{ 0.0 };
			double        aimY{ 0.0 };  // where the aim lands vertically, 0..1 of the window (E21: the lock holds it)
			RE::NiPoint3  camPos;       // the world camera's position, the pivot a tilt turns about
			double        perUnit{ 0.0 };
		};

		// a_quiet is for the tracker, which looks ten times a second: the line
		// below is worth one per solve and not ten per second, and a log that
		// scrolls is a log nobody reads the solve out of.
		bool MeasureOne(Look& a_look, const char* a_when, bool a_quiet = false)
		{
			auto* cam = RE::Main::WorldRootCamera();
			if (!cam) {
				spdlog::warn("[stage] frame {}: there is no world camera to ask - the rule does not move anything",
					a_when);
				return false;
			}
			RE::NiPoint3 aim;
			RE::NiPoint3 left;
			RE::NiPoint3 right;
			const char*  from = "";
			if (!AimOne(aim, left, right, from)) {
				spdlog::warn("[stage] frame {}: no player to aim at", a_when);
				return false;
			}
			double ax = 0.0;
			double ay = 0.0;
			double ad = 0.0;
			double lx = 0.0;
			double ly = 0.0;
			double ld = 0.0;
			double rx = 0.0;
			double ry = 0.0;
			double rd = 0.0;
			if (!ScreenX(cam, aim, ax, ay, ad) || !ScreenX(cam, left, lx, ly, ld) ||
				!ScreenX(cam, right, rx, ry, rd)) {
				spdlog::warn("[stage] frame {}: the camera would not project the aim or one of the two probes",
					a_when);
				return false;
			}
			const double sep = rx - lx;
			const auto&  port = cam->GetRuntimeData2().port;
			if (!a_quiet) {
				spdlog::info("[stage] frame {}: aim at x {:.4f} y {:.4f} depth {:.3f} (from {}) | probes at x {:.4f} and "
							 "{:.4f} (depth {:.3f} and {:.3f}) | {:.4f} of the window for {:.1f} units | port {:.1f} x "
							 "{:.1f}",
					a_when, ax, ay, ad, from, lx, rx, ld, rd, sep, kProbeStep, port.GetWidth(), port.GetHeight());
			}
			if (std::abs(sep) < 1e-6) {
				spdlog::warn("[stage] frame {}: the two probes project to the same point ({:.8f} apart) - there is no "
							 "lateral scale to solve with, and a rule that cannot measure leaves the camera alone",
					a_when, sep);
				return false;
			}
			a_look.cam = cam;
			a_look.aim = aim;
			a_look.from = from;
			a_look.perpX = -g_frame.dirY;
			a_look.perpY = g_frame.dirX;
			a_look.aimX = ax;
			a_look.aimY = ay;
			a_look.camPos = cam->world.translate;
			a_look.perUnit = sep / kProbeStep;
			return true;
		}

		// WHERE THE AIM WOULD LAND if pan moved by a_by from where the camera
		// stands now - read, not extrapolated.
		//
		// Moving the camera +d along perp and moving the AIM -d along perp are
		// the same projection, exactly and not to first order: what the camera
		// sees depends on (aim - camera) in the camera's own axes, both change
		// that vector by the same -d * perp, and pan does not rotate anything
		// (WriteCameraPos writes a position; the rotation comes from the kept
		// frame and from tilt). So the whole curve x(pan) can be sampled by
		// projecting points, without the camera moving and without a slide to
		// wait for.
		//
		// That is what E4d's measurement needed. The scale the rule reads FALLS
		// BY HALF between the measurement and the arrival (0.0166 -> 0.0085 of
		// the window per unit closed, 0.0189 -> 0.0103 open), because perp is
		// not perpendicular to the line of sight - the RaceSex camera does not
		// look at the player, and E4d projected the aim at x 1.0277, off the
		// right of the screen. So translating along perp changes the aim's
		// DEPTH (0.647 -> 0.741 -> 0.775 in that log), and a slope measured at
		// one pan is not the slope 40 units later. The slope is right where it
		// is measured; extrapolating it is what was wrong.
		bool LandingAt(const Look& a_look, double a_by, double& a_x)
		{
			const RE::NiPoint3 moved(static_cast<float>(a_look.aim.x - a_look.perpX * a_by),
				static_cast<float>(a_look.aim.y - a_look.perpY * a_by), a_look.aim.z);
			double y = 0.0;
			double d = 0.0;
			return ScreenX(a_look.cam, moved, a_x, y, d);
		}

		// The pan that puts the aim on a_centre, solved against the projection
		// itself. The secant starts from the straight-line guess the slope gives
		// - which is the whole of what the rule used to do - and then walks the
		// real curve.
		//
		// a_steps and a_predicted come back out because they are the measurement
		// of this solve: how many evaluations it took, and where it says the aim
		// will land. The frame check after the slide compares that prediction
		// with the camera, and a disagreement there is the model being wrong
		// rather than the solve stopping early.
		double SolveFor(const Look& a_look, double a_centre, double a_eps, int& a_steps, double& a_predicted)
		{
			double d0 = 0.0;
			double f0 = a_look.aimX - a_centre;
			double d1 = f0 / a_look.perUnit;
			a_steps = 0;
			a_predicted = a_look.aimX;
			for (int i = 0; i < kSolveSteps; ++i) {
				double x = 0.0;
				if (!LandingAt(a_look, d1, x)) {
					// Off the projectable world. The last pan that did project
					// is the answer; a rule that cannot measure does not guess.
					return d0;
				}
				++a_steps;
				a_predicted = x;
				const double f1 = x - a_centre;
				if (std::abs(f1) <= a_eps) {
					return d1;
				}
				const double slope = f1 - f0;
				if (std::abs(slope) < 1e-9) {
					return d1;
				}
				const double next = d1 - f1 * (d1 - d0) / slope;
				d0 = d1;
				f0 = f1;
				d1 = next;
			}
			return d1;
		}

		// Solves for PAN alone and writes it on the target. Stage.h sets out why
		// pan alone, and why the camera distance is absent from the arithmetic.
		// Takes a look that has already been made rather than making its own, so
		// the tracker - which has to read the offset before it decides whether to
		// move at all - does not project the same camera twice a tenth of a
		// second apart and then solve from the second one.
		bool PanFrom(const Look& a_look, const char* a_when, bool a_quiet)
		{
			const double span = g_oneRight - g_oneLeft;
			const double centre = (g_oneLeft + g_oneRight) / 2.0;
			const Look&  look = a_look;
			const double off = look.aimX - centre;
			// The straight-line step the slope gives, kept only to be LOGGED
			// against the solved one: the difference between the two is the
			// nonlinearity E4d measured, in units, in every round from now on.
			const double straight = g_at.pan + off / look.perUnit;
			int          steps = 0;
			double       predicted = 0.0;
			const double wantedPan = g_at.pan + SolveFor(look, centre, span * kSolveEps, steps, predicted);
			const double pan = ClampSaid(wantedPan, -kPanMax, kPanMax, "pan");

			if (!a_quiet) {
				spdlog::info("[stage] frame {}: view {:.4f}..{:.4f}, centre {:.4f}; the aim sits at {:.4f}, off by "
							 "{:+.4f} of the window - {:+.2f}% of the character view. Scale {:.6f} of the window per "
							 "unit, so {:.2f} units to the fraction",
					a_when, g_oneLeft, g_oneRight, centre, look.aimX, off, off / span * 100.0, look.perUnit,
					1.0 / look.perUnit);
				spdlog::info("[stage] frame {}: pan {:.2f} -> {:.2f} (wanted {:.2f}; the straight line off the slope "
							 "would have asked {:.2f}, {:+.2f} units out). Solved in {} projections, predicting the aim "
							 "lands at {:.4f} ({:+.2f}% of the view). Distance is NOT touched and stays {:.2f} - this "
							 "rule REMIRA and never recua (V099){}",
					a_when, g_at.pan, pan, wantedPan, straight, straight - wantedPan, steps, predicted,
					span > 0.0 ? (predicted - centre) / span * 100.0 : 0.0, g_target.distance,
					pan != wantedPan
						? " - THE PAN LIMIT WAS HIT, so the figure comes out short of the centre. kPanMax is the "
						  "number set in review (E4, Fase 6.5) and is not retuned here; the 'wanted' above is what the rule "
						  "would have needed"
						: "");
			}

			g_target.pan = pan;
			g_lat = 0.0;
			g_onePredicted = predicted;
			++g_oneSolves;
			return true;
		}

		// WHERE THE AIM WOULD LAND vertically if the camera pitched a_by degrees more than it is now, read from the
		// projection and not extrapolated: a pitch of the camera about its own position is the same as turning the
		// world the other way about that position, so the aim is rotated with the transpose of the very matrix
		// WriteTilt turns the camera by (same axis, same sign), about the camera's world position, and projected
		// with the camera as it stands. Nothing moves to take the sample.
		bool LandingYAt(const Look& a_look, double a_by, double& a_y)
		{
			const RE::NiMatrix3 r = AxisAngle(g_frame.rightX, g_frame.rightY, 0.0, a_by);
			const RE::NiPoint3  moved = a_look.camPos + r.Transpose() * (a_look.aim - a_look.camPos);
			double              x = 0.0;
			double              d = 0.0;
			return ScreenX(a_look.cam, moved, x, a_y, d);
		}

		// The tilt that puts the aim back at the height it had at the opening, by the same secant the pan uses.
		// Writes the TARGET only. Returns whether the target moved. At the limit it stops at kTiltMax and says so:
		// the figure then comes out short of where it was, and that is the truth of the limit, not a failure to solve.
		bool TiltFrom(const Look& a_look, const char* a_when, bool a_quiet)
		{
			if (!g_frame.node || !g_oneY0Have) {
				return false;
			}
			double d0 = 0.0;
			double f0 = a_look.aimY - g_oneY0;
			double y1 = 0.0;
			double d1 = f0 > 0.0 ? 1.0 : -1.0;
			if (!LandingYAt(a_look, d1, y1)) {
				return false;
			}
			double f1 = y1 - g_oneY0;
			for (int i = 0; i < kSolveSteps && std::abs(f1) > 0.002; ++i) {
				const double slope = f1 - f0;
				if (std::abs(slope) < 1e-9) {
					break;
				}
				const double next = d1 - f1 * (d1 - d0) / slope;
				double       y = 0.0;
				if (!LandingYAt(a_look, next, y)) {
					break;
				}
				d0 = d1;
				f0 = f1;
				d1 = next;
				f1 = y - g_oneY0;
			}
			const double wanted = g_at.tilt + d1;
			const double tilt = Clamp(wanted, -kTiltMax, kTiltMax);  // silent: the tracker asks every 100 ms
			const bool   moved = std::abs(tilt - g_target.tilt) > 0.01;
			if (moved && !a_quiet) {
				spdlog::info("[stage] frame {}: the head sits at y {:.4f}, it was {:.4f} at the opening - tilt {:.2f} -> {:.2f} "
							 "(wanted {:.2f}){}",
					a_when, a_look.aimY, g_oneY0, g_at.tilt, tilt, wanted,
					tilt != wanted ? " - THE TILT LIMIT WAS HIT, so the head comes out short of its height" : "");
			}
			if (moved) {
				g_target.tilt = tilt;
			}
			return moved;
		}

		bool SolveOne(const char* a_when, bool a_quiet = false)
		{
			const double span = g_oneRight - g_oneLeft;
			if (span <= 0.0) {
				spdlog::warn("[stage] frame {}: the page gave a character view {:.4f} wide - nothing to frame into",
					a_when, span);
				return false;
			}
			Look look;
			if (!MeasureOne(look, a_when, a_quiet)) {
				return false;
			}
			const bool panned = PanFrom(look, a_when, a_quiet);
			// The height of the opening is the first one read with the camera still there; after that the lock
			// brings the head back to it.
			if (!g_oneY0Have && g_at.lift == 0.0 && g_at.tilt == 0.0 && g_target.lift == 0.0 && g_target.tilt == 0.0) {
				g_oneY0 = look.aimY;
				g_oneY0Have = true;
				spdlog::info("[stage] frame {}: the head lands at y {:.4f} at the opening - the lock holds it by tilt (E21)",
					a_when, g_oneY0);
			} else if (g_oneY0Have) {
				TiltFrom(look, a_when, a_quiet);
			}
			return panned;
		}

		// What the slide actually produced, projected again at the camera it
		// arrived at. Everything SolveOne does is a model of a camera, and the
		// model is not the camera: this is where the two are compared.
		//
		// IT NO LONGER CORRECTS, and that is the point of the new solve. The
		// old shape here was one linear step and then at most one correction,
		// capped because a rule that keeps correcting towards a target it never
		// reaches is hunting rather than converging. E4d measured that cap
		// stopping at +2.12% closed and +3.70% open, outside the rule's own 1%:
		// the cap was not the defect, the extrapolation was. SolveOne now reads
		// the projection curve instead, so the residual printed below is the
		// camera disagreeing with the model - and the aim having MOVED while the
		// slide ran, which is what the tracker answers.
		//
		// So this is an instrument now and not a controller: it says what was
		// asked, what was predicted and what came out, and it never writes.
		void AuditOne()
		{
			if (!g_oneOn || g_oneSolves < 1) {
				return;
			}
			Look look;
			if (!MeasureOne(look, "arrived")) {
				return;
			}
			const double span = g_oneRight - g_oneLeft;
			const double centre = (g_oneLeft + g_oneRight) / 2.0;
			const double off = look.aimX - centre;
			// The criterion this rule is judged by is 2% of the character view
			// (Fase 7, E4c). The rule holds itself to ONE per cent - half of it -
			// so that it is not a rule tuned to stop exactly where the criterion
			// starts failing. The two numbers are independent on purpose: this
			// one is the rule's, the other is the judge's.
			const double tolerance = span * 0.01;
			spdlog::info("[stage] frame check: the aim came out at {:.4f} against {:.4f} asked - {:+.4f} of the "
						 "window, {:+.2f}% of the character view, against a tolerance of {:.2f}%. The solve had "
						 "predicted {:.4f}, so the MODEL is {:+.4f} off the camera ({:+.2f}% of the view){}",
				look.aimX, centre, off, span > 0.0 ? off / span * 100.0 : 0.0, 1.0, g_onePredicted,
				look.aimX - g_onePredicted, span > 0.0 ? (look.aimX - g_onePredicted) / span * 100.0 : 0.0,
				std::abs(off) > tolerance ? " - OUTSIDE the tolerance; the tracker below is what closes it" : "");
		}

		// THE RULE RE-AIMS, because the subject does not hold still and no
		// static aim can promise where she will be. The constants above carry
		// the measurement; this is the loop.
		//
		// It is deliberately NOT a second tick: it runs inside the one the
		// Switcher already drives, on the UI thread, and it only looks every
		// kTrackMs. Inside the dead band it does nothing at all - no projection
		// wasted, no camera written - so a settled view costs one comparison
		// per look.
		//
		// Stands down while a slide is under way: the aim it would read is a
		// camera in motion, and solving from that is solving for a frame that
		// has already gone.
		void TrackOne(Clock::time_point a_now)
		{
			if (!g_oneOn || !g_lockOn || !g_free || g_pairOn || g_moving.load(std::memory_order_relaxed)) {
				return;
			}
			if (std::chrono::duration_cast<std::chrono::milliseconds>(a_now - g_trackAt).count() < kTrackMs) {
				return;
			}
			g_trackAt = a_now;
			const double span = g_oneRight - g_oneLeft;
			if (span <= 0.0) {
				return;
			}
			Look look;
			if (!MeasureOne(look, "track", true)) {
				return;
			}
			const double centre = (g_oneLeft + g_oneRight) / 2.0;
			const double off = look.aimX - centre;
			const bool   xOut = std::abs(off) > span * kTrackBand;
			const bool   yOut = g_frame.node && g_oneY0Have && std::abs(look.aimY - g_oneY0) > kTrackBandY;
			if (!xOut && !yOut) {
				return;
			}
			bool did = false;
			if (xOut) {
				const double was = g_target.pan;
				if (PanFrom(look, "track", true)) {
					++g_oneTracks;
					did = true;
					// One line per correction, not one per look: this is the record of
					// how much the figure moved and how often, and it is also how a
					// tracker that was written and never acted is told apart from one
					// that works - P6 of the E4e prediction.
					spdlog::info("[stage] frame track #{}: the aim had drifted to {:+.2f}% of the view (aim from {}), pan "
								 "{:.2f} -> {:.2f}",
						g_oneTracks, off / span * 100.0, look.from, was, g_target.pan);
				}
			}
			if (yOut) {
				const double was = g_target.tilt;
				if (TiltFrom(look, "track", true)) {
					++g_oneTracks;
					did = true;
					spdlog::info("[stage] frame track #{}: the head had drifted to y {:.4f} from {:.4f} (aim from {}), tilt "
								 "{:.2f} -> {:.2f}",
						g_oneTracks, look.aimY, g_oneY0, look.from, was, g_target.tilt);
				}
			}
			if (!did) {
				return;
			}
			g_moving.store(true, std::memory_order_relaxed);
		}

		// ----- the work (UI thread) -------------------------------------------------

		bool Capture(RE::RaceSexMenu* a_menu, RE::GFxValue& a_cg)
		{
			for (const char* fn : kCalls) {
				if (!a_cg.HasMember(fn)) {
					g_why = fmt::format("RaceMenu's CharGen.{} is missing", fn);
					return false;
				}
			}
			Frame f;
			if (!GetPoint(a_cg, "GetRaceSexCameraPos", f.camera) || !GetPoint(a_cg, "GetPlayerPosition", f.player) ||
				!GetBody(a_cg, f.body)) {
				g_why = "RaceMenu did not report the camera, the player or the body";
				return false;
			}
			const double dx = static_cast<double>(f.camera.x) - f.player.x;
			const double dy = static_cast<double>(f.camera.y) - f.player.y;
			f.distance = std::sqrt(dx * dx + dy * dy);
			if (f.distance < 1.0) {
				g_why = fmt::format("the camera is {:.2f} from the player in the ground plane - no direction to move along",
					f.distance);
				return false;
			}
			f.dirX = dx / f.distance;
			f.dirY = dy / f.distance;
			// The world camera's forward is its first column (measured: the pose lines of the band probe, a camera
			// looking down the room reads (-0.016, -0.995, -0.101)). Screen right is that turned a quarter, flat.
			f.rightX = -f.dirY;
			f.rightY = f.dirX;
			double offLine = 0.0;
			if (auto* wc = RE::Main::WorldRootCamera()) {
				const double fx = wc->world.rotate.entry[0][0];
				const double fy = wc->world.rotate.entry[1][0];
				const double fl = std::sqrt(fx * fx + fy * fy);
				if (fl > 0.05) {
					f.rightX = fy / fl;
					f.rightY = -fx / fl;
					// Between where it looks and the line to the player: 0 when it looks straight at her.
					offLine = std::acos(std::clamp(-(fx * f.dirX + fy * f.dirY) / fl, -1.0, 1.0)) * 57.29577951308232;
				}
			}
			if (auto* node = a_menu->GetRuntimeData().camera.cameraRoot.get()) {
				f.node = true;
				f.cameraRot = node->local.rotate;
			}
			f.have = true;
			if (g_firstDistance == 0.0) {
				g_firstDistance = f.distance;
			}
			g_frame = f;
			g_oneY0Have = false;
			g_at = At{};
			g_at.distance = f.distance;
			g_target = g_at;
			g_moving.store(false, std::memory_order_relaxed);
			g_why.clear();
			spdlog::info("[stage] frame kept: camera {} player {} - {:.2f} apart in the ground plane, {:.2f} above; "
						 "camera node {}; it looks {:.1f} deg off the line to the player, screen right is ({:+.3f}, {:+.3f})",
				P(f.camera), P(f.player), f.distance, static_cast<double>(f.camera.z) - f.player.z,
				f.node ? "present" : "ABSENT (no tilt)", offLine, f.rightX, f.rightY);
			return true;
		}

		// V099's countermeasure, and the reason it lives here rather than inside
		// Capture: RaceMenu's FIRST GetRaceSexCameraPos of an opening is STALE.
		// Measured 2026-09-30 - it reported distance 229 with the camera at 38 -
		// and the archived pair logs show the same number, 228.77, being solved
		// with in boots 93 to 134 while the camera was really an order of
		// magnitude closer.
		//
		// It never bit before because a move that changes nothing writes nothing,
		// and until now only the player moved this view. A framing rule DOES
		// write: it moves pan, and WriteCameraPos writes the whole position,
		// distance included - so a stale distance becomes a real 190-unit shove
		// backwards the first time the rule fires. Which is precisely the
		// "recuar" V099 says does not frame, arrived by accident.
		//
		// So the frame is read a SECOND time before the first solve, and both
		// readings go in the log: that pair of numbers is what the criterion
		// asks for, and it is also the check on this very countermeasure - if
		// the second reading comes back equal to the first, the staleness is
		// not what this assumes and the line says so instead of hiding it.
		//
		// Only while the view is still exactly where the frame put it. A refresh
		// that threw away a move the player had already made would be a worse
		// defect than the one it repairs.
		void Refresh(RE::RaceSexMenu* a_menu, RE::GFxValue& a_cg, const char* a_why)
		{
			if (g_reread || !g_frame.have) {
				return;
			}
			const bool untouched = g_at.turn == 0.0 && g_at.lift == 0.0 && g_at.pan == 0.0 && g_at.tilt == 0.0 &&
			                       g_at.distance == g_frame.distance && g_target.distance == g_frame.distance &&
			                       g_target.turn == 0.0 && g_target.lift == 0.0 && g_target.pan == 0.0 &&
			                       g_target.tilt == 0.0;
			if (!untouched) {
				g_reread = true;
				spdlog::info("[stage] {}: the view has already been moved, so the kept frame is NOT read again - the "
							 "first reading stands at distance {:.2f}, and if it was stale that error is now part of "
							 "where the player put the camera",
					a_why, g_frame.distance);
				return;
			}
			const double first = g_frame.distance;
			if (!Capture(a_menu, a_cg)) {
				spdlog::warn("[stage] {}: the second reading of the frame FAILED - the first stands at {:.2f}", a_why,
					first);
				return;
			}
			g_reread = true;
			const double now = g_frame.distance;
			spdlog::info("[stage] {}: the frame was read AGAIN - distance FIRST {:.2f}, NOW {:.2f} ({:+.2f}). This is "
						 "the number that gets PLACED, and the rule is about to write the whole camera position. {}",
				a_why, first, now, now - first,
				std::abs(now - first) < 1.0
					? "The two readings agree, so this opening had no stale first value - V099's defect did not "
					  "appear here and nothing was corrected"
					: "The two DISAGREE, which is V099's stale first reading caught in the act; the second is the "
					  "one used");
		}

		bool WriteCameraPos(RE::GFxMovieView* a_movie, RE::GFxValue& a_cg)
		{
			// pan moves the camera along the perpendicular of the player-camera
			// line, in the ground plane - the same kind of offset lift already
			// is on the vertical axis, just sideways instead of up (E4, Fase
			// 6.5). (-dirY, dirX) is dirX,dirY turned 90 degrees.
			const double perpX = -g_frame.dirY;
			const double perpY = g_frame.dirX;
			const RE::NiPoint3 p(
				static_cast<float>(g_frame.player.x + g_frame.dirX * g_at.distance + perpX * g_at.pan),
				static_cast<float>(g_frame.player.y + g_frame.dirY * g_at.distance + perpY * g_at.pan),
				static_cast<float>(g_frame.camera.z + g_at.lift));
			return SetCameraPos(a_movie, a_cg, p);
		}

		bool WriteTilt(RE::RaceSexMenu* a_menu)
		{
			auto* node = a_menu->GetRuntimeData().camera.cameraRoot.get();
			if (!node || !g_frame.node) {
				return false;
			}
			// A world-space rotation about the horizontal axis across the line
			// of sight, brought into the node's parent space. Which way a
			// positive angle turns the view is read on screen, not assumed.
			const RE::NiMatrix3 world = AxisAngle(g_frame.rightX, g_frame.rightY, 0.0, g_at.tilt);
			RE::NiMatrix3       local;
			if (auto* parent = node->parent) {
				const RE::NiMatrix3 p = parent->world.rotate;
				local = p.Transpose() * (world * (p * g_frame.cameraRot));
			} else {
				local = world * g_frame.cameraRot;
			}
			node->local.rotate = local;
			RE::NiUpdateData data{};
			node->Update(data);
			return true;
		}

		// The TARGET, not where the camera has got to. The page disables a
		// button at a limit from this, and the readout is a destination: both
		// have to answer the click that was just made, not the frame the
		// slide happens to be in.
		json State()
		{
			if (!g_frame.have) {
				return { { "ready", false }, { "why", g_why.empty() ? std::string("not measured yet") : g_why } };
			}
			json state = { { "ready", true }, { "tilt", g_frame.node }, { "turn", g_target.turn },
				{ "distance", g_target.distance }, { "lift", g_target.lift }, { "pan", g_lat }, { "panAxis", g_target.pan },
				{ "tiltDeg", g_target.tilt }, { "free", g_free }, { "forced", g_pairForced }, { "lock", g_lockOn },
				// What the camera is framing into, so the page - and the bench
				// driving it - can read the rule's state instead of assuming it.
				{ "frame", { { "on", g_oneOn }, { "left", g_oneLeft }, { "right", g_oneRight } } },
				{ "limits", { { "minDistance", DistanceMin() }, { "maxDistance", kMaxDistance }, { "liftMin", LiftMin() },
								{ "liftMax", kLiftUp }, { "panMin", PanLo() }, { "panMax", PanHi() },
								{ "tiltMax", kTiltMax } } } };
#ifdef LIFECAST_TUNING
			// Deliberately NOT folded into "limits" - see the note in Stage.h.
			// Four of these are bounds a button stops at and three are not,
			// and the page reads "limits" to decide what to disable.
			state["tuning"] = { { "maxDistance", kMaxDistance }, { "liftUp", kLiftUp }, { "panMax", kPanMax },
				{ "tiltMax", kTiltMax }, { "pairApart", kPairApart }, { "pairSpan", kPairSpan }, { "ease", kEase } };
			state["tuningDefaults"] = g_tuneCompiled;
			state["tuneSaid"] = g_tuneSaid;
#endif
			return state;
		}

		void Answer(json a_state)
		{
			auto* tasks = SKSE::GetTaskInterface();
			if (!tasks) {
				return;
			}
			tasks->AddTask([state = std::move(a_state)]() { Panel::Bridge::Send("stage", state); });
		}

		void Refuse(std::string a_why, bool a_answer)
		{
			g_frame = Frame{};
			g_why = std::move(a_why);
			g_moving.store(false, std::memory_order_relaxed);
			if (a_answer) {
				Answer(State());
			}
		}

		// Where RaceMenu's own camera stood when standard was left for the free one.
		// Handing the camera back puts it exactly there: the opening frame is NOT
		// RaceMenu's far position (boot 155 measured the engine at 55.40 / 0 / 0 for
		// three seconds after a comparison closed, against 31.40 / 9.20 / 19.92 before
		// it), and a standard camera that is not RaceMenu's breaks D152's rule.
		RE::NiPoint3 g_native{};
		bool         g_haveNative = false;

		void KeepNative(RE::GFxValue& a_cg)
		{
			g_haveNative = GetPoint(a_cg, "GetRaceSexCameraPos", g_native);
			spdlog::info("[stage] RaceMenu's own camera kept before the free one: {} ({:.2f}, {:.2f}, {:.2f})",
				g_haveNative ? "read" : "NOT readable", g_native.x, g_native.y, g_native.z);
		}

		// A sideways move of a_by world units along the camera's REAL right (screen left/right, flat), written as
		// the two numbers the camera is built from: the pan axis is the player-camera perpendicular, and when the
		// camera does not look at the player along that line one unit sideways is a unit of pan AND some distance.
		// The travel is limited on the screen axis, from where the rule last left it, and a limit that distance
		// would cross stops the move there instead of bending it.
		void MoveSideways(double a_by)
		{
			const double a = g_frame.rightX * g_frame.dirX + g_frame.rightY * g_frame.dirY;
			const double b = -g_frame.rightX * g_frame.dirY + g_frame.rightY * g_frame.dirX;
			double       to = g_lat + a_by;
			if (to < PanLo() || to > PanHi()) {
				const double cut = std::clamp(to, PanLo(), PanHi());
				spdlog::info("[stage] pan target {:.2f} is outside {:.2f} .. {:.2f} - clamped to {:.2f}", to, PanLo(), PanHi(), cut);
				to = cut;
			}
			double by = to - g_lat;
			// The guard is on how far the camera really is from the character on the ground, not on the distance
			// coordinate: that one runs along the player-camera line, and a move to the right of a camera that is not
			// looking along that line shortens it without bringing the camera any nearer (boot 173: the right side
			// stopped after 17 units, against 80 on the left). A move may not end nearer than the nearest zoom
			// allows, nor farther than the farthest, unless it is already out there and moving back in.
			const double now = std::hypot(g_target.distance, g_target.pan);
			const auto   fine = [&](double a_by) {
				const double g = std::hypot(g_target.distance + a * a_by, g_target.pan + b * a_by);
				return (g >= DistanceMin() || g >= now) && (g <= kMaxDistance || g <= now);
			};
			if (!fine(by)) {
				double lo = 0.0;
				double hi = 1.0;
				for (int i = 0; i < 24; ++i) {
					const double mid = (lo + hi) / 2.0;
					(fine(by * mid) ? lo : hi) = mid;
				}
				spdlog::info("[stage] sideways: {:.2f} units would take the camera {:.2f} from the character, outside {:.2f} .. {:.2f} - "
							 "the move stops at {:.2f}",
					by, std::hypot(g_target.distance + a * by, g_target.pan + b * by), DistanceMin(), kMaxDistance, by * lo);
				by *= lo;
			}
			g_target.distance += a * by;
			g_target.pan += b * by;
			g_lat += by;
		}

		// The frame is read once when the menu opens, and that reading can be stale (V099) or simply a camera that
		// RaceMenu has moved since: its own zoom, and above all a game camera that was turned to the side before
		// the menu opened (the in-game test of 2026-10-06 - the kept frame was the side view, the camera RaceMenu
		// actually had was 100 units away on the other side, and everything the free camera was built on, the
		// direction, the height and the rotation the tilt starts from, was the old one). So it is read again at the
		// moment the free camera takes over. The body's turn is the body's and is kept as it was.
		void FrameAgain(RE::RaceSexMenu* a_menu, RE::GFxValue& a_cg, const char* a_why)
		{
			const Frame before = g_frame;
			const At    atBefore = g_at;
			const At    targetBefore = g_target;
			if (!Capture(a_menu, a_cg)) {
				spdlog::warn("[stage] {}: the frame could not be read again - the one from the opening stays", a_why);
				return;
			}
			g_frame.body = before.body;
			g_at.turn = atBefore.turn;
			g_target.turn = targetBefore.turn;
			g_lat = 0.0;
			g_reread = true;
			spdlog::info("[stage] {}: the frame was read again as the free camera takes over - camera ({:.2f}, {:.2f}) was ({:.2f}, "
						 "{:.2f}), {:.2f} from the player was {:.2f}",
				a_why, g_frame.camera.x, g_frame.camera.y, before.camera.x, before.camera.y, g_frame.distance, before.distance);
		}

		// Handing the camera back to RaceMenu from the free one, the one write the
		// standard camera is allowed. After it, standard writes NOTHING to the camera
		// (D152): the turn of the body is the only thing of ours that moves, and the
		// two zoom positions are RaceMenu's own, asked for in OnZoom.
		void RestoreOpening(const char* a_why, RE::GFxMovieView* a_movie, RE::GFxValue& a_cg)
		{
			g_target.distance = g_frame.distance;
			g_target.lift = 0.0;
			g_target.pan = 0.0;
			g_target.tilt = 0.0;
			if (g_haveNative && a_movie) {
				// What is written already equals the target, so the tick below does not
				// walk the camera toward the opening frame; tilt still steps back to 0.
				g_at.distance = g_target.distance;
				g_at.lift = g_target.lift;
				g_at.pan = g_target.pan;
				const bool ok = SetCameraPos(a_movie, a_cg, g_native);
				spdlog::info("[stage] standard ({}): the camera goes back to where RaceMenu had it ({:.2f}, {:.2f}, {:.2f}) - {}",
					a_why, g_native.x, g_native.y, g_native.z, ok ? "written" : "NOT written");
			} else {
				spdlog::warn("[stage] standard ({}): RaceMenu's own position was not kept - the camera is left where it is", a_why);
			}
		}

		// V166's instrument, and it reports a VALUE, never a count of writes: once
		// a second in the standard camera, with the camera the ENGINE reports
		// (not the one we wrote) against the target, in the quantities the
		// limiars are stated in. A line that goes silent when nothing is wrong
		// is the trap that made the Acabamento 3 limiar 4 measure nothing, so
		// this one is written whether or not anything moved.
		void LogHold(RE::GFxValue& a_cg, Clock::time_point a_now)
		{
			if (g_free || g_pairOn || g_moving.load(std::memory_order_relaxed)) {
				return;
			}
			if (std::chrono::duration_cast<std::chrono::milliseconds>(a_now - g_holdAt).count() < 1000) {
				return;
			}
			g_holdAt = a_now;
			RE::NiPoint3 cam;
			if (!GetPoint(a_cg, "GetRaceSexCameraPos", cam)) {
				return;
			}
			const double perpX = -g_frame.dirY;
			const double perpY = g_frame.dirX;
			const double dx = cam.x - g_frame.player.x;
			const double dy = cam.y - g_frame.player.y;
			const double distance = dx * g_frame.dirX + dy * g_frame.dirY;
			const double pan = dx * perpX + dy * perpY;
			const double lift = cam.z - g_frame.camera.z;
			double       aim = 0.0;
			bool         haveAim = false;
			if (g_oneOn && g_oneRight > g_oneLeft) {
				Look look;
				if (MeasureOne(look, "hold", true)) {
					aim = (look.aimX - (g_oneLeft + g_oneRight) / 2.0) / (g_oneRight - g_oneLeft) * 100.0;
					haveAim = true;
				}
			}
			spdlog::info("[stage] hold {}: engine distance {:.2f} lift {:.2f} pan {:.2f} | target distance {:.2f} lift {:.2f} "
						 "pan {:.2f} tilt {:.2f} | aim {}{:+.2f}% of the view",
				"standard", distance, lift, pan, g_target.distance, g_target.lift, g_target.pan, g_target.tilt,
				haveAim ? "" : "n/a ", aim);
		}

		void Work()
		{
			const auto startedAt = Clock::now();

			Pending p;
			{
				std::scoped_lock lock(g_lock);
				p = g_pending;
				g_pending = Pending{};
				g_queued = false;
			}

			auto         menu = Menu();
			auto*        movie = menu ? menu->uiMovie.get() : nullptr;
			RE::GFxValue cg;
			if (!movie) {
				Refuse("the RaceSex Menu is not open", p.answer);
				return;
			}
			if (!CharGen(movie, cg)) {
				Refuse("RaceMenu's CharGen object is not there", p.answer);
				spdlog::warn("[stage] {} - the view is refused", g_why);
				return;
			}
			if (!g_frame.have && !Capture(menu.get(), cg)) {
				spdlog::warn("[stage] {} - the view is refused", g_why);
				g_moving.store(false, std::memory_order_relaxed);
				if (p.answer) {
					Answer(State());
				}
				return;
			}

			// The comparison forces the free camera and gives the mode back. Said
			// before the gate below, so the very pass that opens it is not gated.
			if (p.pair == 1 && !g_pairForced) {
				g_pairForced = true;
				g_freeBefore = g_free;
				if (!g_free) {
					FrameAgain(menu.get(), cg, "comparison opens");
					KeepNative(cg);
				}
				g_free = true;
				spdlog::info("[stage] comparison opens: the free camera is forced on (was {})", g_freeBefore ? "free" : "standard");
			}
			if (p.mode != 0) {
				if (g_pairForced) {
					spdlog::info("[stage] mode change ignored: the comparison holds the free camera");
				} else if ((p.mode == 2) != g_free) {
					if (p.mode == 2) {
						FrameAgain(menu.get(), cg, "free camera on");
						KeepNative(cg);
					}
					g_free = p.mode == 2;
					if (!g_free) {
						RestoreOpening("mode", movie, cg);
					} else if (g_oneOn && g_lockOn && !g_pairOn) {
						g_oneSolves = 0;
						SolveOne("free on");
					}
					spdlog::info("[stage] mode: {}", g_free ? "free" : "standard");
				}
			}
			// The lock. Off: the rule stops aiming and the camera stays; the target is
			// not touched, so what the page reads is where the camera is. On: it frames
			// once, from where the camera is now, exactly as turning the free camera on
			// does. The comparison owns the camera while it is open, so there is
			// nothing for the lock to do then, and it is said.
			if (p.lock != 0) {
				const bool want = p.lock == 1;
				if (want != g_lockOn) {
					g_lockOn = want;
					SetOneOn(g_oneOn);
					if (!g_lockOn) {
						// "Stays where it is" means where it IS: a slide that the rule had
						// already started toward its last aim stops here instead of running
						// out its easing, so the pose at the instant of the switch is the
						// pose it keeps. The turn is the body's and is not touched.
						g_target.distance = g_at.distance;
						g_target.lift = g_at.lift;
						g_target.pan = g_at.pan;
						g_target.tilt = g_at.tilt;
					}
					spdlog::info("[stage] lock on the character: {}{}", g_lockOn ? "ON" : "OFF",
						g_pairOn ? " (the comparison is framing the camera; this takes effect when it closes)" : "");
					if (g_lockOn && g_free && g_oneOn && !g_pairOn) {
						g_oneSolves = 0;
						g_trackAt = Clock::now();
						Refresh(menu.get(), cg, "lock on");
						SolveOne("lock on");
					}
				}
			}

			// STANDARD drops every move the page could still send except the
			// turn, and the tracker stands down (TrackOne): the camera is
			// RaceMenu's and nothing of ours writes to it.
			if (!g_free) {
				p.distance = 0.0;
				p.lift = 0.0;
				p.pan = 0.0;
				p.tilt = 0.0;
			}

			// The move goes on the target, and the clamp with it.
			if (p.reset) {
				g_target = At{}; g_lat = 0.0;
				g_target.distance = g_frame.distance;
			}
			if (p.turn != 0.0) {
				g_target.turn = Wrap(g_target.turn + p.turn);
			}
			if (p.distance != 0.0) {
				g_target.distance = ClampSaid(g_target.distance + p.distance, DistanceMin(), kMaxDistance, "distance");
			}
			if (p.lift != 0.0) {
				g_target.lift = ClampSaid(g_target.lift + p.lift, LiftMin(), kLiftUp, "height");
			}
			if (p.pan != 0.0) {
				MoveSideways(p.pan);
			}
			if (p.tilt != 0.0 && g_frame.node) {
				g_target.tilt = ClampSaid(g_target.tilt + p.tilt, -kTiltMax, kTiltMax, "tilt");
			}

			// The rule sets the target outright rather than by a delta: it
			// solves for where the camera has to BE, and a delta would make
			// that depend on where it happened to be left.
			if (p.pair == 1) {
				g_pairLeft = p.frameLeft;
				g_pairRight = p.frameRight;
				g_pairSolves = 0;
				g_pairOn = SolvePair("on");
			} else if (p.pair == 2) {
				if (g_pairOn) {
					spdlog::info("[stage] pair off: back to the view the menu opened with");
				}
				g_pairOn = false;
				g_pairSolves = 0;
				g_target = At{}; g_lat = 0.0;
				g_target.distance = g_frame.distance;
				if (g_pairForced) {
					g_pairForced = false;
					g_free = g_freeBefore;
					spdlog::info("[stage] comparison closes: back to the {} camera", g_free ? "free" : "standard");
				}
				if (!g_free) {
					RestoreOpening("comparison closed", movie, cg);
				}
			}

			// The single-figure rule, after the pair's: if both arrive in one
			// pass the pair owns the camera, and standing down is said out loud
			// rather than left for a reader to work out from the order.
			if (p.one == 1) {
				if (g_pairOn) {
					spdlog::warn("[stage] frame on: the PAIR rule is already framing this camera - the "
								 "single-figure rule stands down rather than fight it for the same axis");
				} else {
					const bool again = g_oneOn;
					const bool moved = p.oneLeft != g_oneLeft || p.oneRight != g_oneRight;
					g_oneLeft = p.oneLeft;
					g_oneRight = p.oneRight;
					g_restTicks = 0;
					g_restWorstUs = 0.0;
					g_trackAt = Clock::now();
					g_oneSolves = 0;
					g_oneTracks = 0;
					if (!g_free || !g_lockOn) {
						// Standard, or the lock is off: the edges are kept for the day the free
						// camera is switched on or the lock is turned on, and nothing is solved or
						// written.
						SetOneOn(true);
					} else {
						// Before the measurement, because the measurement is of the
						// camera the write is about to be based on.
						Refresh(menu.get(), cg, "frame on");
						SetOneOn(SolveOne(!again ? "on" : (moved ? "the view resized" : "again")));
					}
				}
			} else if (p.one == 2) {
				if (g_oneOn) {
					spdlog::info("[stage] frame off after {} re-aims: back to the view the menu opened with. This is "
								 "also how the negative control of the measurement is taken - same DLL, same boot, "
								 "same room. {} ticks of it were at rest, worst one {:.0f} us",
						g_oneTracks, g_restTicks, g_restWorstUs);
				}
				SetOneOn(false);
				g_oneSolves = 0;
				g_oneTracks = 0;
				if (g_free) {
					g_target = At{}; g_lat = 0.0;
					g_target.distance = g_frame.distance;
				}
			}

			// And the tick moves what is written, one step closer.
			const bool turnMoved = StepAngle(g_at.turn, g_target.turn, kSnapDegrees);
			const bool distanceMoved = Step(g_at.distance, g_target.distance, kSnapUnits);
			const bool liftMoved = Step(g_at.lift, g_target.lift, kSnapUnits);
			const bool panMoved = Step(g_at.pan, g_target.pan, kSnapUnits);
			const bool tiltMoved = g_frame.node && Step(g_at.tilt, g_target.tilt, kSnapDegrees);

			// A reframe writes all three even when nothing moved: RaceMenu's
			// own tabs move this camera, and the kept frame is what the
			// player asked to see again.
			const bool body = turnMoved || p.reset;
			const bool camera = distanceMoved || liftMoved || panMoved || p.reset;
			const bool tilt = g_frame.node && (tiltMoved || p.reset);

			const bool bodyOk = !body || SetBody(movie, cg, TurnedBy(g_frame.body, g_at.turn));
			const bool cameraOk = !camera || WriteCameraPos(movie, cg);
			const bool tiltOk = !tilt || WriteTilt(menu.get());

			const bool wasMoving = g_moving.load(std::memory_order_relaxed);
			const bool moving = g_at.turn != g_target.turn || g_at.distance != g_target.distance ||
			                    g_at.lift != g_target.lift || g_at.pan != g_target.pan ||
			                    g_at.tilt != g_target.tilt;
			g_moving.store(moving, std::memory_order_relaxed);

			if (p.any) {
				spdlog::info(
					"[stage] {}target turn {:.1f} deg, distance {:.2f}, lift {:.2f}, pan {:.2f}, tilt {:.1f} deg{}{}{}",
					p.reset ? "reframe: " : "", g_target.turn, g_target.distance, g_target.lift, g_target.pan,
					g_target.tilt, bodyOk ? "" : " - body NOT written", cameraOk ? "" : " - camera NOT written",
					tiltOk ? "" : " - tilt NOT written");
			}

			// Set when a slide ends, so the page is told where the camera
			// actually stopped. A tick carries no question of its own
			// (Pending::answer is false on it), and that was measured as a
			// defect in boot 93: the pair rule solves once, the audit
			// solves AGAIN from the arrived frame, and the page kept showing
			// the FIRST target for the rest of the opening. Three frames of
			// that round read "distance 320 | pan 27" while the camera was
			// at 116.27 / 61.14, and the readout is the player's only view
			// of where the rule put the camera - a readout that names a
			// target which is no longer the target is worse than none.
			bool arrived = false;

			// The slide, tick by tick. At trace because it is one line a
			// frame while something is moving: raising the level silences it
			// without touching this file.
			if (wasMoving || moving) {
				const double us = std::chrono::duration<double, std::micro>(Clock::now() - startedAt).count();
				if (!wasMoving) {
					g_ticks = 0;
					g_workUs = 0.0;
					g_worstUs = 0.0;
					g_slideAt = startedAt;
				}
				++g_ticks;
				g_workUs += us;
				if (us > g_worstUs) {
					g_worstUs = us;
				}
				spdlog::trace(
					"[stage] tick {}: at turn {:.2f} distance {:.2f} lift {:.2f} pan {:.2f} tilt {:.2f} -> target "
					"{:.2f} {:.2f} {:.2f} {:.2f} {:.2f} - {:.0f} us",
					g_ticks, g_at.turn, g_at.distance, g_at.lift, g_at.pan, g_at.tilt, g_target.turn,
					g_target.distance, g_target.lift, g_target.pan, g_target.tilt, us);
				if (!moving) {
					const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_slideAt);
					spdlog::info("[stage] arrived after {} ticks in {} ms - {:.0f} us of work in all, worst tick "
								 "{:.0f} us",
						g_ticks, ms.count(), g_workUs, g_worstUs);
					AuditPair();
					AuditOne();
					// The arrival is also when the tracker's clock should start:
					// re-aiming is about drift AFTER the view settled, and the
					// first look should be kTrackMs from here and not from
					// whenever the last one happened to be.
					g_trackAt = Clock::now();
					// After the audit on purpose: when it corrects, it has
					// already written the new target, and that is the number
					// the page should be showing.
					arrived = true;
				}
			}

			// After the slide, never during it: the rule re-aims at a camera
			// that has stopped. It writes the TARGET only, so the easing above
			// is what moves - the camera glides at the speed the subject drifts
			// (~17 px/s on screen, E4d) instead of stepping.
			TrackOne(startedAt);
			LogHold(cg, startedAt);

			if (!wasMoving && !moving) {
				++g_restTicks;
				const double us = std::chrono::duration<double, std::micro>(Clock::now() - startedAt).count();
				if (us > g_restWorstUs) {
					g_restWorstUs = us;
				}
			}

			if (p.answer || arrived) {
				Answer(State());
			}
		}

		void Queue(const Pending& a_add)
		{
			bool queue = false;
			{
				std::scoped_lock lock(g_lock);
				g_pending.turn += a_add.turn;
				g_pending.distance += a_add.distance;
				g_pending.lift += a_add.lift;
				g_pending.pan += a_add.pan;
				g_pending.tilt += a_add.tilt;
				g_pending.reset = g_pending.reset || a_add.reset;
				if (a_add.mode != 0) {
					g_pending.mode = a_add.mode;
				}
				g_pending.zoom += a_add.zoom;
				if (a_add.lock != 0) {
					g_pending.lock = a_add.lock;
				}
				// A toggle does not add up: two presses folded into one pass
				// are the last one, not both.
				if (a_add.pair != 0) {
					g_pending.pair = a_add.pair;
					g_pending.frameLeft = a_add.frameLeft;
					g_pending.frameRight = a_add.frameRight;
				}
				// Same folding, same reason - and the LAST edges win, which is
				// what a burst of resize messages during a panel transition
				// should collapse to.
				if (a_add.one != 0) {
					g_pending.one = a_add.one;
					g_pending.oneLeft = a_add.oneLeft;
					g_pending.oneRight = a_add.oneRight;
				}
				g_pending.any = g_pending.any || a_add.any;
				g_pending.answer = g_pending.answer || a_add.answer;
				if (!g_queued) {
					g_queued = true;
					queue = true;
				}
			}
			if (!queue) {
				return;
			}
			if (auto* tasks = SKSE::GetTaskInterface()) {
				tasks->AddUITask([]() { Work(); });
			} else {
				std::scoped_lock lock(g_lock);
				g_queued = false;
			}
		}

		double Number(const json& a_data, const char* a_key)
		{
			if (!a_data.is_object() || !a_data.contains(a_key) || !a_data[a_key].is_number()) {
				return 0.0;
			}
			const double v = a_data[a_key].get<double>();
			return std::isfinite(v) ? v : 0.0;
		}

		// ----- bridge and session (game thread) -----------------------------------

		Pending Asked()
		{
			Pending p;
			p.answer = true;
			return p;
		}

		void OnGet(const json&)
		{
			Queue(Asked());
		}

		void OnMove(const json& a_data)
		{
			Move(Number(a_data, "turn"), Number(a_data, "distance"), Number(a_data, "lift"), Number(a_data, "pan"),
				Number(a_data, "tilt"));
		}

		void OnReset(const json&)
		{
			Reset();
		}

		// "free": bool. Absent is standard, the safe side.
		void OnMode(const json& a_data)
		{
			bool free = false;
			if (a_data.is_object() && a_data.contains("free") && a_data["free"].is_boolean()) {
				free = a_data["free"].get<bool>();
			}
			Pending p;
			p.mode = free ? 2 : 1;
			p.any = true;
			p.answer = true;
			Queue(p);
		}

		// "on": bool. Absent is ON, the safe side: ON is what the view always did.
		void OnLock(const json& a_data)
		{
			bool on = true;
			if (a_data.is_object() && a_data.contains("on") && a_data["on"].is_boolean()) {
				on = a_data["on"].get<bool>();
			}
			Pending p;
			p.lock = on ? 1 : 2;
			p.any = true;
			p.answer = true;
			Queue(p);
		}

		// The standard camera's zoom is RaceMenu's own, and ours only in that we
		// ask for it. Two ways failed at boot 153: LAlt in the input queue (the panel
		// holds the keyboard and Meridian echoed the key back to the page, which asked
		// again - hundreds of messages) and a "Zoom In/Out" ButtonEvent handed to the
		// menu (CanProcess false). This one runs the menu's own Scaleform handler, so
		// RaceMenu keeps its zoom state. Game thread. D152's probe.
		Clock::time_point g_zoomAt{};

		void OnZoom(const json&)
		{
			if (g_free) {
				return;
			}
			const auto now = Clock::now();
			if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_zoomAt).count() < 400) {
				return;  // a swap takes longer than that, and a loop would show here
			}
			g_zoomAt = now;
			auto menu = Menu();
			auto* movie = menu ? menu->uiMovie.get() : nullptr;
			if (!movie) {
				spdlog::warn("[stage] zoom: no RaceSex menu movie - nothing was sent");
				return;
			}
			// Read from the installed racesex_menu.swf (read only): the bottom bar's
			// "Zoom" button runs onZoomClicked, which flips bPlayerZoom, swaps the
			// label and calls GameDelegate.call("ZoomPC"). So the handler is invoked
			// by name on whichever candidate object owns it; which one does is logged.
			static constexpr const char* kPaths[] = { "_root.RaceSexMenuBaseInstance", "_root", "_root.RaceSexMenuBaseInstance.RaceSexPanelsInstance" };
			bool                         called = false;
			for (const char* path : kPaths) {
				RE::GFxValue obj;
				const bool   got = movie->GetVariable(&obj, path) && obj.IsObject();
				const bool   has = got && obj.HasMember("onZoomClicked");
				spdlog::info("[stage] zoom: candidate {} - object {}, onZoomClicked {}", path, got, has);
				if (has) {
					called = obj.Invoke("onZoomClicked", nullptr, nullptr, 0);
					spdlog::info("[stage] zoom: {}.onZoomClicked() invoked - {}", path, called);
					break;
				}
			}
			if (!called) {
				spdlog::warn("[stage] zoom: no candidate owned onZoomClicked - nothing was sent");
			}
		}

		// Absent "on" means TRUE: the page sends this every time the character
		// view is resized, and the common message should not have to carry the
		// flag. Only the negative control, and whoever wants the camera left
		// alone, says false. Number() cannot be used for the two edges - it
		// answers 0.0 for a missing key, and 0.0 is a meaningful left edge.
		void OnFrame(const json& a_data)
		{
			bool   on = true;
			double left = 0.0;
			double right = 1.0;
			if (a_data.is_object()) {
				if (a_data.contains("on") && a_data["on"].is_boolean()) {
					on = a_data["on"].get<bool>();
				}
				if (a_data.contains("left") && a_data["left"].is_number()) {
					left = a_data["left"].get<double>();
				}
				if (a_data.contains("right") && a_data["right"].is_number()) {
					right = a_data["right"].get<double>();
				}
			}
			FrameOne(on, left, right);
		}

#ifdef LIFECAST_TUNING
		// Reads one tunable, and ABSENT is not zero: the page sends the single
		// field the user just typed, so every number it leaves out has to
		// stay where it is. Number() above cannot be used for this - it answers
		// 0.0 for a missing key, which here would mean "set it to zero".
		//
		// A value that is not finite is dropped rather than clamped: there is
		// no sane bound to clamp a tuning knob to, and silently substituting a
		// number the user did not type is worse than ignoring the input.
		bool Tuned(const json& a_data, const char* a_key, double& a_out)
		{
			if (!a_data.is_object() || !a_data.contains(a_key)) {
				return false;
			}
			if (!a_data[a_key].is_number()) {
				g_tuneSaid[a_key] = "not a number - ignored, nothing changed";
				return false;
			}
			const double v = a_data[a_key].get<double>();
			if (!std::isfinite(v)) {
				spdlog::warn("[stage] tune: \"{}\" is not a finite number - ignored", a_key);
				g_tuneSaid[a_key] = "not a finite number - ignored, nothing changed";
				return false;
			}
			spdlog::info("[stage] tune: {} {:.4f} -> {:.4f}", a_key, a_out, v);
			a_out = v;
			return true;
		}

		// True while the entry for a key still says only "stored": the number was
		// accepted and nothing has described its effect yet. A rejected number keeps
		// its own message and must not be overwritten by a later, rosier one.
		bool Stored(const char* a_key)
		{
			return g_tuneSaid.contains(a_key) && g_tuneSaid[a_key] == "stored";
		}

		// One line for the box: the number is stored, and then what it did to the
		// camera - the TARGET before and after the same ClampSaid every move uses.
		void SaidLimit(const char* a_key, double a_before, double a_after)
		{
			if (!Stored(a_key)) {
				return;
			}
			g_tuneSaid[a_key] = a_after != a_before
				? fmt::format("limit stored - the camera was at {:.2f}, outside it, and was moved to {:.2f}", a_before, a_after)
				: fmt::format("limit stored - the camera is at {:.2f}, inside it, so nothing moves; it only stops "
							  "the camera when it would go past", a_before);
		}

		// maxDistance has the one floor that has a number behind it: the camera
		// cannot come nearer than DistanceMin(), so a limit below it leaves the
		// target nowhere to be - Clamp(v, DistanceMin(), limit) hands back the
		// floor for a v under it and the limit for a v at the floor, and the
		// target flips between the two on every tune (Acabamento 1: 32 and 33.56,
		// 2130 "clamped" lines in 281 s). The other six have no such number and
		// no floor; that is a decision (Acabamento 5), not an omission.
		void FloorMaxDistance()
		{
			const double lowest = g_frame.have ? DistanceMin() : kMinDistance;
			if (kMaxDistance >= lowest) {
				return;
			}
			spdlog::info("[stage] tune: maxDistance {:.2f} is below the nearest the camera can go ({:.2f}) - raised "
						 "to {:.2f}",
				kMaxDistance, lowest, lowest);
			g_tuneSaid["maxDistance"] = fmt::format(
				"raised to {:.2f}: that is the nearest the camera can go, and a limit under it has no place to put the "
				"camera",
				lowest);
			kMaxDistance = lowest;
		}

		// The page asked for a different number. UI thread, because these are
		// read by the slide and by the framing rule, which both run there.
		//
		// Lowering a limit can leave the TARGET outside it. The target is then
		// re-clamped by the same ClampSaid every move uses - this changes WHAT
		// the limit is, never WHERE it is applied, which is the rule the design
		// note at the top of Stage.h sets out. The slide carries the camera in.
		void OnTune(const json& a_data)
		{
			auto* tasks = SKSE::GetTaskInterface();
			if (!tasks) {
				return;
			}
			const json data = a_data;
			tasks->AddUITask([data]() {
				// A fresh answer replaces the old one for the keys it carries; the others
				// keep what they last said.
				for (const char* key : { "maxDistance", "liftUp", "panMax", "tiltMax", "pairApart", "pairSpan", "ease" }) {
					if (data.is_object() && data.contains(key)) {
						g_tuneSaid.erase(key);
					}
				}
				bool any = Tuned(data, "maxDistance", kMaxDistance);
				if (any) {
					FloorMaxDistance();
				}
				any = Tuned(data, "liftUp", kLiftUp) || any;
				any = Tuned(data, "panMax", kPanMax) || any;
				any = Tuned(data, "tiltMax", kTiltMax) || any;
				any = Tuned(data, "pairApart", kPairApart) || any;
				any = Tuned(data, "pairSpan", kPairSpan) || any;
				any = Tuned(data, "ease", kEase) || any;
				if (!any) {
					spdlog::warn("[stage] tune: no usable number in the message - nothing changed");
					g_tuneSaid["none"] = "no usable number in the message - nothing changed";
					Queue(Asked());
					return;
				}
				g_tuneSaid.erase("none");
				// "stored" is the base answer for a number that was accepted.
				for (const char* key : { "maxDistance", "liftUp", "panMax", "tiltMax", "pairApart", "pairSpan", "ease" }) {
					if (data.is_object() && data.contains(key) && !g_tuneSaid.contains(key)) {
						g_tuneSaid[key] = "stored";
					}
				}
				if (g_frame.have) {
					const At before = g_target;
					g_target.distance = ClampSaid(g_target.distance, DistanceMin(), kMaxDistance, "distance");
					g_target.lift = ClampSaid(g_target.lift, LiftMin(), kLiftUp, "height");
					const double latBefore = g_lat;
					if (g_lat < PanLo() || g_lat > PanHi()) {
						MoveSideways(std::clamp(g_lat, PanLo(), PanHi()) - g_lat);
					}
					g_target.tilt = ClampSaid(g_target.tilt, -kTiltMax, kTiltMax, "tilt");
					SaidLimit("maxDistance", before.distance, g_target.distance);
					SaidLimit("liftUp", before.lift, g_target.lift);
					SaidLimit("panMax", latBefore, g_lat);
					SaidLimit("tiltMax", before.tilt, g_target.tilt);
					// A pairing already on the screen was solved with the OLD
					// numbers, so it is solved again. The twin herself does not
					// move: Edit/Compare placed her at the old kPairApart and
					// only a fresh placement puts her anywhere else, which is
					// why the user is told to toggle the comparison off and
					// on. Said here so the log shows the gap rather than
					// leaving the rule aiming at a figure that is not there.
					if (g_pairOn) {
						spdlog::info("[stage] tune: the pair is on - solving again. The second figure standing in the "
									 "world was placed at the previous separation and does NOT move; toggle "
									 "the comparison off and on to place her at the new one");
						g_pairSolves = 0;
						g_pairCut = {};
						g_pairOn = SolvePair("after tune");
						// The same test the log line applies. A limit that cut the pair is said
						// first, with the number the rule wanted, because "solved again" alone
						// reads as "did what you asked".
						std::string cut;
						if (g_pairCut.distance != g_pairCut.wantedDistance) {
							cut += fmt::format(" A LIMIT CUT THE PAIR: the rule wanted distance {:.2f} and the camera got {:.2f}.",
								g_pairCut.wantedDistance, g_pairCut.distance);
						}
						if (g_pairCut.pan != g_pairCut.wantedPan) {
							cut += fmt::format(" A LIMIT CUT THE PAIR: the rule wanted pan {:.2f} and the camera got {:.2f}.",
								g_pairCut.wantedPan, g_pairCut.pan);
						}
						if (Stored("pairApart")) {
							g_tuneSaid["pairApart"] = "stored - the second figure does NOT move: turn the comparison off and on "
													  "to place her at the new separation." + cut;
						}
						if (Stored("pairSpan")) {
							g_tuneSaid["pairSpan"] = "stored - the pair is on, and the framing was solved again with it." + cut;
						}
					} else {
						if (Stored("pairApart")) {
							g_tuneSaid["pairApart"] = "stored - the comparison is off; it shows when the comparison is turned on";
						}
						if (Stored("pairSpan")) {
							g_tuneSaid["pairSpan"] = "stored - the comparison is off; it frames the pair when it is turned on";
						}
					}
				} else {
					for (const char* key : { "maxDistance", "liftUp", "panMax", "tiltMax" }) {
						if (Stored(key)) {
							g_tuneSaid[key] = "stored - there is no view yet, so it limits the next move";
						}
					}
				}
				if (Stored("ease")) {
					g_tuneSaid["ease"] = "stored - it changes how fast the camera slides, so it shows only while the "
										 "camera is moving";
				}
				g_moving.store(true, std::memory_order_relaxed);
				Queue(Asked());
			});
		}
#endif

		void OnShown()
		{
			Queue(Asked());
		}

		void OnMenuClosed()
		{
			if (auto* tasks = SKSE::GetTaskInterface()) {
				tasks->AddUITask([]() {
					if (g_frame.have) {
						spdlog::info("[stage] the menu closed - the kept frame is dropped");
					}
					g_frame = Frame{};
					g_at = At{};
					g_target = At{}; g_lat = 0.0;
					g_moving.store(false, std::memory_order_relaxed);
					g_why.clear();
					g_pairOn = false;
					g_pairSolves = 0;
					g_free = false;
					g_freeBefore = false;
					g_pairForced = false;
					g_lockOn = false;
					g_oneY0Have = false;
					SetOneOn(false);
					g_oneSolves = 0;
					g_oneTracks = 0;
					// The next opening gets its own first reading, and its own
					// second one: staleness is a property of an opening.
					g_reread = false;
					g_firstDistance = 0.0;
				});
			}
		}
	}

	void Move(double a_turn, double a_distance, double a_lift, double a_pan, double a_tilt)
	{
		Pending p;
		p.turn = a_turn;
		p.distance = a_distance;
		p.lift = a_lift;
		p.pan = a_pan;
		p.tilt = a_tilt;
		p.any = p.turn != 0.0 || p.distance != 0.0 || p.lift != 0.0 || p.pan != 0.0 || p.tilt != 0.0;
		p.answer = true;
		Queue(p);
	}

	void Reset()
	{
		Pending p;
		p.reset = true;
		p.any = true;
		p.answer = true;
		Queue(p);
	}

	namespace
	{
		// Screen right and the line of sight away from the camera, in the
		// ground plane, for PairSpot and PairReads alike.
		//
		// Which way is screen right is read NOW, from the camera that is drawing, and not from the frame kept when
		// the menu opened: RaceMenu moves its camera after that (and a game camera turned to the side before the
		// menu opened leaves it tens of degrees off the line to the player), so the opening frame's perpendicular
		// put the copy partly ALONG the line of sight - in front of her and huge, or behind her (boot 176: 27 units
		// along, 54 across, against 11 and 59 with this read). Two things change together, the point (the camera's
		// real right, not the player-camera perpendicular) and the instant of the read (now, not the opening); the
		// kept frame is the fallback when the camera gives no usable forward.
		void PairAxes(double& a_rightX, double& a_rightY, double& a_awayX, double& a_awayY)
		{
			a_rightX = g_frame.rightX;
			a_rightY = g_frame.rightY;
			if (auto* wc = RE::Main::WorldRootCamera()) {
				const double fx = wc->world.rotate.entry[0][0];
				const double fy = wc->world.rotate.entry[1][0];
				const double fl = std::sqrt(fx * fx + fy * fy);
				if (fl > 0.05) {
					a_rightX = fy / fl;
					a_rightY = -fx / fl;
				}
			}
			// Away from the camera is screen right turned back a quarter: right
			// is (fy, -fx) of the forward (fx, fy), so the forward is (-ry, rx).
			a_awayX = -a_rightY;
			a_awayY = a_rightX;
		}
	}

	bool PairSpot(RE::NiPoint3& a_out)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !g_frame.have) {
			return false;
		}
		double rightX = 0.0;
		double rightY = 0.0;
		double awayX = 0.0;
		double awayY = 0.0;
		PairAxes(rightX, rightY, awayX, awayY);
		const double       across = kPairApart + g_shiftAcross.load(std::memory_order_relaxed);
		const double       depth = g_shiftDepth.load(std::memory_order_relaxed);
		const RE::NiPoint3 at = player->GetPosition();
		a_out = RE::NiPoint3(static_cast<float>(at.x + rightX * across + awayX * depth),
			static_cast<float>(at.y + rightY * across + awayY * depth), at.z);
		return true;
	}

	PairShift ShiftPair(double a_across, double a_depth, bool& a_cut)
	{
		const double acrossAsked = g_shiftAcross.load(std::memory_order_relaxed) + a_across;
		const double depthAsked = g_shiftDepth.load(std::memory_order_relaxed) + a_depth;
		// The limits are on where she ends up, so the across one is on the
		// whole distance to the side and the shift is what is left of it.
		const double across = std::clamp(acrossAsked, -0.5 * kPairApart, 2.0 * kPairApart);
		const double depth = std::clamp(depthAsked, -0.5 * kPairApart, 2.0 * kPairApart);
		a_cut = across != acrossAsked || depth != depthAsked;
		g_shiftAcross.store(across, std::memory_order_relaxed);
		g_shiftDepth.store(depth, std::memory_order_relaxed);
		return { across, depth };
	}

	PairShift PairShiftNow()
	{
		return { g_shiftAcross.load(std::memory_order_relaxed), g_shiftDepth.load(std::memory_order_relaxed) };
	}

	void ClearPairShift()
	{
		g_shiftAcross.store(0.0, std::memory_order_relaxed);
		g_shiftDepth.store(0.0, std::memory_order_relaxed);
	}

	bool PairReads(const RE::NiPoint3& a_at, double& a_across, double& a_depth)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !g_frame.have) {
			return false;
		}
		double rightX = 0.0;
		double rightY = 0.0;
		double awayX = 0.0;
		double awayY = 0.0;
		PairAxes(rightX, rightY, awayX, awayY);
		const RE::NiPoint3 at = player->GetPosition();
		const double       dx = a_at.x - at.x;
		const double       dy = a_at.y - at.y;
		a_across = dx * rightX + dy * rightY;
		a_depth = dx * awayX + dy * awayY;
		return true;
	}

	void FramePair(bool a_on, double a_frameLeft, double a_frameRight)
	{
		Pending p;
		p.pair = a_on ? 1 : 2;
		// A frame the page did not send, or sent inside out, is the whole
		// window: the rule still frames, it just has no way to know that the
		// panel is covering part of its result.
		const bool usable = std::isfinite(a_frameLeft) && std::isfinite(a_frameRight) && a_frameRight > a_frameLeft;
		p.frameLeft = usable ? Clamp(a_frameLeft, 0.0, 1.0) : 0.0;
		p.frameRight = usable ? Clamp(a_frameRight, 0.0, 1.0) : 1.0;
		p.any = true;
		p.answer = true;
		Queue(p);
	}

	void FrameOne(bool a_on, double a_frameLeft, double a_frameRight)
	{
		Pending p;
		p.one = a_on ? 1 : 2;
		// A view the page did not send, or sent inside out, is the whole window:
		// the rule still frames, it just has no way to know that the panel is
		// covering part of its result. Said out loud, because a camera centring
		// on the window instead of the stage is exactly the failure this etapa
		// exists to remove, and it must not look like success.
		const bool usable = std::isfinite(a_frameLeft) && std::isfinite(a_frameRight) && a_frameRight > a_frameLeft;
		if (!usable && a_on) {
			spdlog::warn("[stage] frame: {:.4f}..{:.4f} is not a usable character view - falling back to the WHOLE "
						 "WINDOW, which centres the subject behind the panel rather than beside it",
				a_frameLeft, a_frameRight);
		}
		p.oneLeft = usable ? Clamp(a_frameLeft, 0.0, 1.0) : 0.0;
		p.oneRight = usable ? Clamp(a_frameRight, 0.0, 1.0) : 1.0;
		p.any = true;
		p.answer = true;
		Queue(p);
	}

	void Tick()
	{
		// Two atomics, not one. Moving means there is a slide to step; tracking
		// means the one-figure rule is on and the subject it aims at may have
		// drifted since the camera stopped - and a rule that only looks while
		// it is already moving cannot notice that (boot 138: 3 re-aims in 60 s,
		// every one of them on the tail of a slide already under way).
		// TrackOne's own 100 ms gate is what keeps the cost of the second
		// condition to one projection every six input ticks.
		if (!g_moving.load(std::memory_order_relaxed) && !g_oneTracking.load(std::memory_order_relaxed)) {
			return;
		}
		Queue(Pending{});
	}

	void Install()
	{
		if (g_installed) {
			return;
		}
		Edit::Session::OnPanelShown(&OnShown);
		Edit::Session::OnMenuClosed(&OnMenuClosed);
		Panel::Bridge::On("stage-get", &OnGet);
		Panel::Bridge::On("stage-move", &OnMove);
		Panel::Bridge::On("stage-reset", &OnReset);
		Panel::Bridge::On("stage-mode", &OnMode);
		Panel::Bridge::On("stage-lock", &OnLock);
		Panel::Bridge::On("stage-zoom", &OnZoom);
		Panel::Bridge::On("stage-frame", &OnFrame);
#ifdef LIFECAST_TUNING
		Panel::Bridge::On("stage-tune", &OnTune);
#endif
		g_installed = true;
#ifdef LIFECAST_TUNING
		spdlog::warn("[stage] installed WITH LIVE TUNING - this is a development build, and the seven numbers "
					 "of the view can be changed from the panel. A release build has none of this");
#else
		spdlog::info("[stage] installed");
#endif
	}
}
