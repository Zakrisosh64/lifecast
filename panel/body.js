// body.js
// Lifecast - the body section: OBody's body presets, one click to apply.
//
// The section exists only while OBody answers. Without OBody the DLL says
// "not ready" and nothing is built: no empty section, no disabled one, no
// note - the section is not in the page at all. If OBody withdraws while the
// panel is open, the section is removed the same way.
//
// Every "body" message is OBody's answer at that moment, read by the DLL:
// the list for the character's sex and the preset the character carries.
// The page keeps nothing between two of them, so after a preset load (which
// can replace the body) what shows is what OBody says, not what was clicked.
"use strict";

(() => {
  // Where this section lands among the others is declared once, in
  // groups.js, not here: mount() only takes a key, and arrange() places it
  // (E2 item 1 - the DOM-order shortcut this file used to take broke the
  // OBody shortcut button, the group index, and the placeholder cleanup;
  // see groups.js and controls.js).

  let section = null;
  let status = null;
  let list = null;
  let pending = null;  // the name asked for, until the next "body" message

  function build() {
    section = LifecastControls.section("OBody");
    status = LifecastControls.el("div", "preset-status", "--");
    list = LifecastControls.el("ul", "preset-list");
    section.body.appendChild(status);
    section.body.appendChild(list);
    LifecastControls.mount(section, "obody");
  }

  function remove() {
    if (section) {
      section.remove();
      section = null;
      status = null;
      list = null;
    }
    pending = null;
  }

  function render(data) {
    const current = typeof data.current === "string" ? data.current : "";
    const names = data.presets
      .filter((n) => typeof n === "string")
      .sort((a, b) => a.localeCompare(b, undefined, { sensitivity: "base" }));
    status.textContent = current ? "Current: " + current : "Current: none";
    list.replaceChildren();
    for (const name of names) {
      const li = LifecastControls.el("li", "preset-row pad-row");
      // The controller's ring stops on a row and keeps its place through the
      // re-render that follows a choice (pad.js, data-pad-key).
      li.dataset.padKey = "obody:" + name;
      li.appendChild(LifecastControls.el("span", "preset-name", name));
      if (name === current) {
        li.classList.add("is-selected");
      }
      li.addEventListener("click", () => {
        if (name === current || pending !== null) {
          return;
        }
        pending = name;
        status.textContent = "Applying " + name + "...";
        LifecastBridge.send("body-set", { name: name });
      });
      list.appendChild(li);
    }
    // The rows are new elements: the controller's ring, if it was on one,
    // moves to the new row of the same name (pad.js).
    document.dispatchEvent(new CustomEvent("lifecast-pad-refresh"));
  }

  LifecastBridge.on("body", (data) => {
    pending = null;
    const ready = data && data.ready === true && Array.isArray(data.presets) && data.presets.length > 0;
    if (!ready) {
      remove();
      return;
    }
    if (!section) {
      build();
    }
    render(data);
  });

  LifecastBridge.whenAttached((ok) => {
    if (ok) {
      LifecastBridge.send("body-get");
    }
  });
})();
