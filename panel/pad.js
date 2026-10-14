// pad.js
// Lifecast - the controller, while the panel has the focus.
//
// The DLL sends every controller event as a "pad" message (Panel/Pad.h); this
// file makes the controller do what the mouse does, over a focus ring:
//
//   D-pad, left stick   the ring moves to the nearest visible control in that
//                       direction, by where things are on the screen
//   left / right        on a slider, one press of its < or > button
//   A                   the click (on a text field: the click that opens typing)
//   B                   back: leaves a text field, closes an open list or
//                       colour picker, clears the search
//   LB / RB             the previous / next category of the belt, wrapping
//   right stick         turns the character, as the drag on the stage does
//
// Every action goes through the path the mouse already uses - the button's
// own click, the slider's own < >, the stage's drag - so nothing here writes
// a value of its own. The ring is a class (.pad-focus), not the browser's
// focus: a button that took the real focus would keep it after the mouse
// came back, and a text field that took it would start typing. The ring goes
// away at the first mouse press or key, and comes back where it was at the
// next controller press.
"use strict";

const LifecastPad = (() => {
  const STICK_ON = 0.5;           // a left stick past this points somewhere
  const STICK_FIRST_MS = 380;     // a held left stick moves again after this,
  const STICK_EVERY_MS = 160;     // and then every this
  const TURN_PX_PER_S = 500;      // the right stick at full tilt, in the drag's own pixels a second

  const TARGETS = "button, input, select, textarea, .slider";
  const TEXT = "input:not([type]), input[type=text], input[type=search], textarea";

  let cur = null;
  let shown = false;
  let presses = 0;
  let last = "";
  const stick = { dir: null, timer: null };
  const turn = { x: 0, frame: null, at: 0 };

  function hidden(a_el) {
    if (a_el.disabled || a_el.closest("[hidden]")) {
      return true;
    }
    const r = a_el.getBoundingClientRect();
    if (r.width < 2 || r.height < 2) {
      return true;
    }
    const st = getComputedStyle(a_el);
    return st.visibility === "hidden" || st.pointerEvents === "none";
  }

  // What the ring can stop on: every visible control, a slider as one stop
  // (its < > and its range are the slider's, not stops of their own).
  function targets() {
    const out = [];
    for (const el of document.querySelectorAll(TARGETS)) {
      if (el.matches("input[type=range]")) {
        continue;
      }
      if (!el.classList.contains("slider") && el.closest(".slider") && !el.matches(TEXT)) {
        continue;
      }
      if (!hidden(el)) {
        out.push(el);
      }
    }
    return out;
  }

  function show() {
    if (cur) {
      cur.classList.add("pad-focus");
      cur.scrollIntoView({ block: "nearest", inline: "nearest" });
    }
    shown = true;
  }

  function hide() {
    if (cur) {
      cur.classList.remove("pad-focus");
    }
    shown = false;
  }

  function setCur(a_el) {
    if (cur && cur !== a_el) {
      cur.classList.remove("pad-focus");
    }
    cur = a_el;
    show();
  }

  function valid() {
    return cur && document.contains(cur) && !hidden(cur);
  }

  // The first stop when there is none: the top one of the open section, else
  // the top-left one of the page.
  function first(a_all) {
    const host = document.getElementById("sections");
    const inHost = a_all.filter((el) => host && host.contains(el));
    const pool = inHost.length ? inHost : a_all;
    let best = null;
    let bestKey = Infinity;
    for (const el of pool) {
      const r = el.getBoundingClientRect();
      const key = r.top * 4 + r.left;
      if (key < bestKey) {
        bestKey = key;
        best = el;
      }
    }
    return best;
  }

  // Where a stop lives: the panel's column (search, regions, sections), the
  // belt, the bars under the stage. A move looks inside the current zone
  // first and leaves it only when the zone has nothing more that way: the
  // Character bar sits beside the column's lower rows, and a plain nearest
  // stop crosses between the two halfway (D253).
  const ZONES = [
    ["column", "#finder, #regions, #sections"],
    ["belt", "#belt"],
    ["toolbar", "#toolbar"],
    ["footer", ".panel-footer"],
    ["control", "#control-bar"],
    ["stage", "#stage"],
  ];

  // Up out of a bar under the panel lands in the column, its last rows -
  // measured without this, up from the belt went to Undo and from Undo to
  // the column's top, skipping every row between (D253).
  const EXITS = {
    belt: { up: "column" },
    toolbar: { up: "column" },
    footer: { up: "column" },
    control: { up: "column", left: "column" },
  };

  function zone(a_el) {
    for (const [name, sel] of ZONES) {
      if (a_el.closest(sel)) {
        return name;
      }
    }
    return "page";
  }

  // The nearest stop in a direction: it has to be past the current one in
  // that direction, and the score is the distance along the direction plus
  // twice the distance across it, edge to edge - a stop straight below beats
  // a closer one off to the side.
  function nearest(a_dir) {
    const all = targets();
    if (!valid()) {
      return first(all);
    }
    const here = zone(cur);
    const inZone = (name) => all.filter((el) => zone(el) === name);
    const exit = EXITS[here] && EXITS[here][a_dir];
    return nearestIn(a_dir, inZone(here), 1) || (exit && nearestIn(a_dir, inZone(exit), ROW)) || nearestIn(a_dir, all, 1);
  }

  // Into another zone the nearest ROW wins first: the belt starts right of
  // the column, and with the plain score up from it picked the column's top
  // (Changed) over its bottom row (Male, Female) - measured, D253.
  const ROW = 50;

  function nearestIn(a_dir, a_all, a_along) {
    const a = cur.getBoundingClientRect();
    let best = null;
    let bestScore = Infinity;
    for (const el of a_all) {
      if (el === cur || el.contains(cur) || cur.contains(el)) {
        continue;
      }
      const b = el.getBoundingClientRect();
      let along;
      let across;
      if (a_dir === "up" || a_dir === "down") {
        along = a_dir === "down" ? b.top - a.bottom : a.top - b.bottom;
        if ((a_dir === "down" ? b.top + b.height / 2 <= a.top + a.height / 2 : b.top + b.height / 2 >= a.top + a.height / 2)) {
          continue;
        }
        across = Math.max(0, Math.max(a.left, b.left) - Math.min(a.right, b.right));
      } else {
        along = a_dir === "right" ? b.left - a.right : a.left - b.right;
        if ((a_dir === "right" ? b.left + b.width / 2 <= a.left + a.width / 2 : b.left + b.width / 2 >= a.left + a.width / 2)) {
          continue;
        }
        across = Math.max(0, Math.max(a.top, b.top) - Math.min(a.bottom, b.bottom));
      }
      const score = a_along * Math.max(0, along) + 2 * across;
      if (score < bestScore) {
        bestScore = score;
        best = el;
      }
    }
    return best;
  }

  function go(a_dir) {
    if (!shown && valid()) {
      show();
      last = "show";
      return;
    }
    // Left and right on a slider are its < and >: the same click the mouse makes.
    if (valid() && cur.classList.contains("slider") && (a_dir === "left" || a_dir === "right")) {
      const steps = cur.querySelectorAll(".step-button");
      const b = steps[a_dir === "left" ? 0 : steps.length - 1];
      if (b && !b.disabled) {
        b.click();
        last = "step " + a_dir;
        return;
      }
    }
    const next = nearest(a_dir);
    if (next) {
      setCur(next);
      last = "move " + a_dir;
    } else {
      show();
      last = "edge " + a_dir;
    }
  }

  function activate() {
    if (!valid()) {
      setCur(first(targets()));
      last = "first";
      return;
    }
    // A ring that was hidden by the mouse is shown first, never clicked unseen.
    if (!shown) {
      show();
      last = "show";
      return;
    }
    if (cur.classList.contains("slider")) {
      last = "slider";
      return;
    }
    if (cur.matches(TEXT)) {
      cur.focus();
    }
    cur.click();
    last = "click";
  }

  function back() {
    const active = document.activeElement;
    if (active && active.matches && active.matches(TEXT)) {
      active.blur();
      setCur(active);
      last = "leave field";
      return;
    }
    const open = Array.from(document.querySelectorAll(".listpop, .picker")).filter((w) => !w.hidden);
    if (open.length) {
      for (const w of open) {
        // The same as their own close: the list's close() and the picker's
        // toggle only flip hidden.
        w.hidden = true;
        if (cur && w.contains(cur)) {
          const owner = w.closest(".section") && w.closest(".section").querySelector(".list-chip, .swatch, .slider");
          setCur(owner && !hidden(owner) ? owner : first(targets()));
        }
      }
      last = "close";
      return;
    }
    const search = document.getElementById("search");
    if (search && search.value) {
      search.value = "";
      search.dispatchEvent(new Event("input", { bubbles: true }));
      last = "clear search";
      return;
    }
    last = "nothing to close";
  }

  // LB / RB: the belt's categories in their order on the screen, wrapping.
  function category(a_step) {
    const cats = Array.from(document.querySelectorAll(".belt-cat")).filter((b) => !b.hidden && !b.disabled);
    if (!cats.length) {
      return;
    }
    const at = cats.findIndex((b) => b.classList.contains("is-on"));
    const next = at < 0 ? (a_step > 0 ? cats[0] : cats[cats.length - 1])
      : cats[(at + a_step + cats.length) % cats.length];
    next.click();
    const stop = first(targets());
    setCur(stop || next);
    last = "category " + (next.dataset.cat || "");
  }

  // The right stick turns the character through the stage's drag (stage.js),
  // one frame at a time while it is off centre.
  function turnFrame(a_now) {
    const dt = turn.at ? Math.min(0.1, (a_now - turn.at) / 1000) : 0;
    turn.at = a_now;
    if (dt && turn.x) {
      document.dispatchEvent(new CustomEvent("lifecast-stage-drag", { detail: { dx: turn.x * TURN_PX_PER_S * dt } }));
    }
    turn.frame = turn.x ? requestAnimationFrame(turnFrame) : null;
    if (!turn.x) {
      turn.at = 0;
    }
  }

  function onRight(a_x) {
    turn.x = a_x;
    if (turn.x && turn.frame === null) {
      turn.at = 0;
      turn.frame = requestAnimationFrame(turnFrame);
    }
  }

  function stickDir(a_x, a_y) {
    if (Math.max(Math.abs(a_x), Math.abs(a_y)) < STICK_ON) {
      return null;
    }
    if (Math.abs(a_x) > Math.abs(a_y)) {
      return a_x > 0 ? "right" : "left";
    }
    return a_y > 0 ? "up" : "down";
  }

  function onLeft(a_x, a_y) {
    const dir = stickDir(a_x, a_y);
    if (dir === stick.dir) {
      return;
    }
    clearTimeout(stick.timer);
    stick.timer = null;
    stick.dir = dir;
    if (!dir) {
      return;
    }
    presses += 1;
    go(dir);
    const again = () => {
      go(stick.dir);
      stick.timer = setTimeout(again, STICK_EVERY_MS);
    };
    stick.timer = setTimeout(again, STICK_FIRST_MS);
  }

  function reset() {
    clearTimeout(stick.timer);
    stick.timer = null;
    stick.dir = null;
    turn.x = 0;
  }

  const DPAD = { DUp: "up", DDown: "down", DLeft: "left", DRight: "right" };

  function onPad(a_data) {
    if (!a_data) {
      return;
    }
    if (a_data.reset) {
      reset();
      return;
    }
    if (a_data.b === "LStick") {
      onLeft(Number(a_data.x) || 0, Number(a_data.y) || 0);
      return;
    }
    if (a_data.b === "RStick") {
      onRight(Number(a_data.x) || 0);
      return;
    }
    const down = a_data.phase === "down";
    if (!down && a_data.phase !== "repeat") {
      return;
    }
    if (DPAD[a_data.b]) {
      presses += 1;
      go(DPAD[a_data.b]);
    } else if (!down) {
      // Only the directions repeat; A, B and the bumpers act once per press.
    } else if (a_data.b === "A") {
      presses += 1;
      activate();
    } else if (a_data.b === "B") {
      presses += 1;
      back();
    } else if (a_data.b === "LB" || a_data.b === "RB") {
      presses += 1;
      category(a_data.b === "RB" ? 1 : -1);
    }
  }

  // The mouse or the keyboard back in the player's hand: the ring steps aside.
  document.addEventListener("pointerdown", hide, true);
  document.addEventListener("keydown", hide, true);

  LifecastBridge.on("pad", onPad);

  function describe(a_el) {
    if (!a_el) {
      return null;
    }
    const nameEl = a_el.classList.contains("slider") ? a_el.querySelector(".slider-name") : null;
    const range = a_el.classList.contains("slider") ? a_el.querySelector("input[type=range]") : null;
    const text = (nameEl ? nameEl.textContent : (a_el.getAttribute("aria-label") || a_el.title || a_el.textContent || a_el.placeholder || ""))
      .replace(/\s+/g, " ").trim().slice(0, 40);
    return {
      tag: a_el.tagName.toLowerCase(),
      id: a_el.id || "",
      cls: String(a_el.className).replace("pad-focus", "").trim(),
      text: text,
      value: range ? Number(range.value) : (a_el.matches(TEXT) ? a_el.value : null),
    };
  }

  // What the ring is on, for whoever is checking the page from outside.
  function state() {
    const on = document.querySelector(".belt-cat.is-on");
    const active = document.activeElement;
    return {
      presses: presses,
      last: last,
      shown: shown,
      focus: describe(valid() ? cur : null),
      category: on ? on.dataset.cat : null,
      typing: !!(active && active.matches && active.matches(TEXT)),
      open: Array.from(document.querySelectorAll(".listpop, .picker")).filter((w) => !w.hidden).length,
      stops: targets().length,
    };
  }

  return { state: state };
})();