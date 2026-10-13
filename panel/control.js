// control.js
// Lifecast - Character Control: the things a player DOES to the character
// while editing, as opposed to the sliders that describe what it looks like.
//
// It stands where the Index section used to. The Index was a grid of tiles
// that jumped to a category, and it was cut in favour of this.
//
// The key is "control" and NOT "character": "character" was taken by
// the identity section, which is always open because it is identity and not
// a category to browse (D064). A name that reads alike is not a free key
// (D079).
//
// Undress (Fase 6.6, E5): a toggle, not a state readout. The DLL only ever
// unequips what is already worn and re-equips that same instance - the item
// never leaves the inventory (see Edit/Undress.h). Same shape as the light
// toggle this section was written to receive: the button says what pressing
// it does, and the label after a click is what the DLL confirms, not a
// guess of what happened.
//
// Light (Fase 6.6, E4b): same shape again, and for the same reason as
// Undress - the DLL never reads the stage light's state back (Edit/Light.h),
// so a label that claimed state would be an inference dressed as a
// measurement. Placement here is provisional (layout is Fase 7).
//
// Compare (Fase 6, E3b): the third one in the same shape, and it is the
// switch D104 asked for - the side by side costs a frame while it is on and
// nothing while it is off, so it is a decision and not a preference.
//
// Two things make it different from the other two, and neither is cosmetic:
//
//   the frame   the DLL has to be told where the character view's own edges
//               are, because the panel covers the left of the screen and the
//               middle of the WINDOW is not the middle of what the player can
//               see. Only the page knows that number. #stage belongs to
//               stage.js; this reads its rect and writes nothing.
//   the why     "compare" can come back with a reason it refused - the second
//               figure left in another room, no body loaded there. The reason
//               is shown and the rest of the panel is untouched: the
//               comparison is the only thing that goes away.
"use strict";

(() => {
  // Always open and always on screen (D127): not a
  // section of the list but a fixed bar of its own over the stage's corner,
  // so no category, search or collapse can take it away.
  const section = { body: document.getElementById("control-bar") };
  if (!section.body) {
    return;
  }
  // The title goes first: undo and redo are already in the bar (index.html,
  // D130), and the buttons below go after them.
  section.body.insertBefore(LifecastControls.el("span", "stage-group-title", "Character"), section.body.firstChild);

  const button = LifecastControls.el("button", "panel-button control-button", "Undress");
  button.type = "button";
  const note = LifecastControls.el("span", "control-note", "");
  section.body.appendChild(button);
  section.body.appendChild(note);

  let on = false;   // this feature's own belief; the DLL's "undress" reply is the truth
  let busy = false;

  function render() {
    button.textContent = on ? "Dress" : "Undress";
    button.disabled = busy;
  }

  button.addEventListener("click", () => {
    if (busy) {
      return;
    }
    busy = true;
    note.textContent = "";
    render();
    if (!LifecastBridge.send("undress-set", { on: !on })) {
      busy = false;
      note.textContent = "No bridge to the game - nothing was done.";
      render();
    }
  });

  LifecastBridge.on("undress", (data) => {
    on = !!(data && data.on);
    busy = false;
    render();
  });

  LifecastBridge.whenAttached((ok) => {
    if (ok) {
      LifecastBridge.send("undress-get");
    }
  });

  render();

  const lightButton = LifecastControls.el("button", "panel-button control-button", "Light Off");
  lightButton.type = "button";
  const lightNote = LifecastControls.el("span", "control-note", "");
  section.body.appendChild(lightButton);
  section.body.appendChild(lightNote);

  let lightOn = true;   // this feature's own belief; the DLL's "light" reply is the truth
  let lightBusy = false;

  function renderLight() {
    lightButton.textContent = lightOn ? "Light Off" : "Light On";
    lightButton.disabled = lightBusy;
  }

  lightButton.addEventListener("click", () => {
    if (lightBusy) {
      return;
    }
    lightBusy = true;
    lightNote.textContent = "";
    renderLight();
    if (!LifecastBridge.send("light-set", { on: !lightOn })) {
      lightBusy = false;
      lightNote.textContent = "No bridge to the game - nothing was done.";
      renderLight();
    }
  });

  LifecastBridge.on("light", (data) => {
    lightOn = !!(data && data.on);
    lightBusy = false;
    renderLight();
  });

  LifecastBridge.whenAttached((ok) => {
    if (ok) {
      LifecastBridge.send("light-get");
    }
  });

  renderLight();

  // Freeze (Fase 8, E12): same shape. The label says what pressing does; the
  // DLL's "freeze" reply is the state. It is undone by the DLL when the menu
  // closes, when a pose is played and when a game loads.
  const freezeButton = LifecastControls.el("button", "panel-button control-button", "Freeze");
  freezeButton.type = "button";
  section.body.appendChild(freezeButton);

  let freezeOn = false;
  let freezeBusy = false;

  function renderFreeze() {
    freezeButton.textContent = freezeOn ? "Unfreeze" : "Freeze";
    freezeButton.disabled = freezeBusy;
  }

  freezeButton.addEventListener("click", () => {
    if (freezeBusy) {
      return;
    }
    freezeBusy = true;
    renderFreeze();
    if (!LifecastBridge.send("freeze-set", { on: !freezeOn })) {
      freezeBusy = false;
      poseNote.textContent = "No bridge to the game - nothing was done.";
      renderFreeze();
    }
  });

  LifecastBridge.on("freeze", (data) => {
    freezeOn = !!(data && data.on);
    freezeBusy = false;
    renderFreeze();
  });

  // Pose: the list comes from the DLL (it owns the events). Choosing one
  // plays it and shows what the graph answered; the first option stops.
  const poseSelect = LifecastControls.el("select", "panel-button control-button", "");
  const poseNote = LifecastControls.el("span", "control-note", "");
  // Stop: the select does not fire "change" when the item already shown is
  // chosen again, so after a long pose the first option is not a reachable
  // command. This sends the same empty event the first option sends.
  const poseStop = LifecastControls.el("button", "panel-button control-button", "Stop pose");
  poseStop.type = "button";
  section.body.appendChild(poseSelect);
  section.body.appendChild(poseStop);
  section.body.appendChild(poseNote);

  function fillPoses(poses) {
    poseSelect.textContent = "";
    const first = LifecastControls.el("option", "", "Pose");
    first.value = "";
    poseSelect.appendChild(first);
    for (const pose of poses) {
      const option = LifecastControls.el("option", "", String(pose.label));
      option.value = String(pose.event);
      poseSelect.appendChild(option);
    }
  }
  fillPoses([]);

  poseSelect.addEventListener("change", () => {
    poseNote.textContent = "";
    if (!LifecastBridge.send("pose-play", { event: poseSelect.value })) {
      poseNote.textContent = "No bridge to the game - nothing was done.";
    }
  });

  poseStop.addEventListener("click", () => {
    poseNote.textContent = "";
    poseSelect.value = "";
    if (!LifecastBridge.send("pose-play", { event: "" })) {
      poseNote.textContent = "No bridge to the game - nothing was done.";
    }
  });

  LifecastBridge.on("poses", (data) => {
    fillPoses(data && Array.isArray(data.poses) ? data.poses : []);
  });

  LifecastBridge.on("pose", (data) => {
    // The DLL's answer is the truth: a refused event says so.
    poseNote.textContent = (data && data.accepted === false && typeof data.why === "string") ? data.why : "";
    if (data && data.accepted === false) {
      poseSelect.value = "";
    }
  });

  LifecastBridge.whenAttached((ok) => {
    if (ok) {
      LifecastBridge.send("freeze-get");
      LifecastBridge.send("pose-list");
    }
  });

  renderFreeze();

  // The character view's edges, as a fraction of the window width. #stage is
  // laid out from the panel's right edge to the window's right edge, so this
  // is exactly the region the camera has to fit the pair into. Read fresh at
  // every click rather than kept: the panel scale is a setting the player can
  // change while the editor is open.
  function frame() {
    const stage = document.getElementById("stage");
    const width = window.innerWidth;
    if (!stage || !(width > 0)) {
      return null;
    }
    const rect = stage.getBoundingClientRect();
    if (!(rect.width > 0)) {
      return null;
    }
    return { left: rect.left / width, right: rect.right / width };
  }

  const compareButton = LifecastControls.el("button", "panel-button control-button", "Compare");
  compareButton.type = "button";
  const updateButton = LifecastControls.el("button", "panel-button control-button", "Update copy");
  updateButton.type = "button";
  const compareNote = LifecastControls.el("span", "control-note", "");
  section.body.appendChild(compareButton);
  section.body.appendChild(updateButton);
  section.body.appendChild(compareNote);

  let compareOn = false;   // this feature's own belief; the DLL's "compare" reply is the truth
  let compareBusy = false;

  // Hidden until the DLL says the figure will draw this opening; an
  // unavailable Compare is not shown rather than shown inert.
  let compareAvailable = false;

  function renderCompare() {
    compareButton.style.display = compareAvailable ? "" : "none";
    compareNote.style.display = compareAvailable ? "" : "none";
    compareButton.textContent = compareOn ? "Compare Off" : "Compare";
    compareButton.disabled = compareBusy;
    // Only with the comparison on: it makes the copy again from the character as
    // she is now. Nothing refreshes it by itself.
    updateButton.style.display = compareAvailable ? "" : "none";
    updateButton.disabled = compareBusy || !compareOn;
  }

  updateButton.addEventListener("click", () => {
    if (compareBusy || !compareOn) {
      return;
    }
    const where = frame();
    compareBusy = true;
    compareNote.textContent = "";
    renderCompare();
    const message = {};
    if (where) {
      message.frame = where;
    }
    if (!LifecastBridge.send("compare-update", message)) {
      compareBusy = false;
      compareNote.textContent = "No bridge to the game - nothing was done.";
      renderCompare();
    }
  });

  compareButton.addEventListener("click", () => {
    if (compareBusy) {
      return;
    }
    const where = frame();
    compareBusy = true;
    compareNote.textContent = "";
    renderCompare();
    const message = { on: !compareOn };
    if (where) {
      message.frame = where;
    }
    if (!LifecastBridge.send("compare-set", message)) {
      compareBusy = false;
      compareNote.textContent = "No bridge to the game - nothing was done.";
      renderCompare();
    }
  });

  LifecastBridge.on("compare", (data) => {
    compareOn = !!(data && data.on);
    compareAvailable = !!(data && data.available === true);
    compareBusy = false;
    // The DLL's own words when it refused. Empty when it did not, so a
    // successful toggle clears whatever the last refusal said.
    compareNote.textContent = (data && typeof data.why === "string") ? data.why : "";
    renderCompare();
    // The bridge keeps one handler per message type and this file owns "compare", so stage.js (Shift + drag turns
    // the copy) is told through the document, as look.js does with "lifecast-look".
    document.dispatchEvent(new CustomEvent("lifecast-compare", { detail: { on: compareOn } }));
  });

  LifecastBridge.whenAttached((ok) => {
    if (ok) {
      LifecastBridge.send("compare-get");
    }
  });

  renderCompare();

  // ----- out of the stage bar's way --------------------------------------
  // This bar sits at the stage's left edge and the camera bar at the stage's
  // middle, on the same line. With the panel open and a large panel scale,
  // or on a narrow screen, the stage is too narrow for both and this one
  // covered the camera bar (measured in the E5 photos: 1920 at 160%, 1280
  // at 100%). Then this bar goes up, over the camera bar, by just enough
  // (--control-lift), and centres on it (--control-shift), so the two read
  // as one stack (D135). Both are zero otherwise. Read from the rects, so it
  // follows the scale, the open panel and the window without knowing any of
  // them.
  const stageBar = document.getElementById("stage-bar");

  function placeBar() {
    const style = section.body.style;
    // Measure from the bar's own place: drop the lift and shift first.
    style.setProperty("--control-lift", "0px");
    style.setProperty("--control-shift", "0px");
    if (!stageBar || stageBar.hidden || stageBar.offsetParent === null) {
      return;
    }
    const c = section.body.getBoundingClientRect();
    const s = stageBar.getBoundingClientRect();
    const gap = 8;
    if (c.right + gap > s.left && c.left < s.right + gap) {
      style.setProperty("--control-lift", Math.ceil(c.bottom - s.top + gap) + "px");
      style.setProperty("--control-shift", Math.round(s.left + s.width / 2 - (c.left + c.width / 2)) + "px");
    }
  }

  window.addEventListener("resize", placeBar);
  if (typeof ResizeObserver !== "undefined") {
    const watch = new ResizeObserver(placeBar);
    watch.observe(section.body);
    if (stageBar) {
      watch.observe(stageBar);
    }
    watch.observe(document.getElementById("stage"));
  }
  // The open panel and the scale change the stage's width through
  // attributes and the style on <html>; the stage bar shows and hides.
  const changes = new MutationObserver(placeBar);
  changes.observe(document.documentElement, { attributes: true, attributeFilter: ["style", "data-belt-open"] });
  if (stageBar) {
    changes.observe(stageBar, { attributes: true, attributeFilter: ["hidden"] });
  }
  placeBar();
})();
