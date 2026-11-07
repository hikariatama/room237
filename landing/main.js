// Download button: name the visitor's OS. Package names carry the version, so the link opens the latest release.
(() => {
  const platform = navigator.userAgentData?.platform ?? navigator.platform ?? "";
  const ua = navigator.userAgent;
  const link = document.getElementById("download");
  if (/iPhone|iPad|Android/i.test(ua)) return;
  if (/Mac/i.test(platform)) link.textContent = "Download for macOS";
  else if (/Win/i.test(platform)) link.textContent = "Download for Windows";
  else if (/Linux/i.test(platform)) link.textContent = "Download for Linux";
})();

// Demo: the app window on the left, the folder it manages on the right.
// Loops through importing a photo, moving it to another album and creating an album.
(() => {
  const $ = (id) => document.getElementById(id);
  const stage = $("stage");
  const albumsEl = $("albums");
  const gridEl = $("grid");
  const treeEl = $("tree");
  const cursor = $("cursor");
  const newAlbum = $("new-album");
  const reduced = matchMedia("(prefers-reduced-motion: reduce)").matches;
  const narrow = matchMedia("(max-width: 820px)");

  // Height / width of each photo in photos/, used to lay out the masonry.
  const ratio = {
    "0102": 0.667, "0107": 0.667, "0115": 1.506, "0311": 0.667, "0318": 0.667,
    "0342": 0.667, "0357": 0.667, "0366": 0.667, "0371": 0.667, "0380": 0.667,
    "0384": 0.667, "1180": 0.562, "1184": 0.562, "1191": 1.5, "1203": 1.5,
    "1220": 1.5, "1236": 0.667, "1251": 1.392, "1262": 0.667, "1270": 0.667,
    "1288": 0.667, "1294": 0.685, "1301": 1.5, "2041": 1.333,
  };
  const name = (n) => `IMG_${n}.jpg`;
  const src = (file) => `photos/${file}`;
  const aspect = (file) => ratio[file.slice(4, 8)] ?? 1;

  // Newest first, as with the app's default EXIF date sort.
  const initial = () => ({
    albums: [
      {
        name: "home",
        files: ["1301", "1180", "1191", "1262", "1203", "1236", "1270", "1220", "1184", "1288", "1251", "1294"].map(name),
      },
      { name: "naps", files: ["0384", "0380", "0371", "0366", "0357", "0342", "0318", "0311"].map(name) },
      { name: "outside", files: ["0115", "0107", "0102"].map(name) },
    ],
    open: "home",
    fresh: new Set(),
  });

  const album = (s, n) => s.albums.find((a) => a.name === n);
  const imported = name("2041");

  const scenes = [
    // Drop a photo onto the window: it is copied into the album folder.
    {
      apply(s) {
        album(s, "home").files.unshift(imported);
        s.fresh.add("home/" + imported);
      },
      async play(run) {
        // Start just outside the window, as if dragging from the desktop.
        await move(run, rel(gridEl, narrow.matches ? 0.85 : 1.02, 0.85));
        grab(imported);
        await pause(run, 250);
        await move(run, rel(gridEl, 0.2, 0.25));
        await drop(run);
      },
    },
    // Drag it onto another album: the file moves to that folder.
    {
      apply(s) {
        const home = album(s, "home");
        home.files = home.files.filter((f) => f !== imported);
        album(s, "naps").files.unshift(imported);
        s.fresh.add("naps/" + imported);
      },
      async play(run) {
        const tile = gridEl.querySelector(`[data-name="${imported}"]`);
        const row = albumsEl.querySelector('[data-name="naps"]');
        await move(run, rel(tile));
        await pause(run, 200);
        tile.classList.add("lifted");
        grab(imported);
        await move(run, rel(row, 0.45));
        row.classList.add("highlighted");
        await pause(run, 300);
        await drop(run);
      },
    },
    // A new album is a new folder.
    {
      apply(s) {
        s.albums.push({ name: "zoo", files: [] });
        s.fresh.add("zoo");
      },
      async play(run) {
        await move(run, rel(newAlbum));
        newAlbum.classList.add("pressed");
        await pause(run, 250);
        newAlbum.classList.remove("pressed");
      },
    },
  ];

  // Rendering

  const el = (tag, cls, text) => {
    const node = document.createElement(tag);
    if (cls) node.className = cls;
    if (text != null) node.textContent = text;
    return node;
  };
  const byName = (x, y) => x.localeCompare(y);

  function render(s) {
    albumsEl.replaceChildren(
      ...s.albums.map((a) => {
        const row = el("div", "album");
        row.dataset.name = a.name;
        row.classList.toggle("active", a.name === s.open);
        row.classList.toggle("fresh", s.fresh.has(a.name));
        const thumb = el("span", "thumb");
        if (a.files[0]) thumb.style.backgroundImage = `url(${src(a.files[0])})`;
        row.append(thumb, el("span", "name", a.name), el("span", "count", a.files.length));
        return row;
      }),
    );

    // Masonry: each photo goes to the currently shortest column.
    const count = narrow.matches ? 3 : 4;
    const columns = Array.from({ length: count }, () => el("div", "column"));
    const heights = new Array(count).fill(0);
    for (const file of album(s, s.open).files) {
      const i = heights.indexOf(Math.min(...heights));
      heights[i] += aspect(file) + 0.06;
      const tile = el("div", "tile");
      tile.dataset.name = file;
      if (s.fresh.has(s.open + "/" + file)) tile.classList.add("fresh");
      const img = el("img");
      img.src = src(file);
      img.alt = "";
      img.style.aspectRatio = String(1 / aspect(file));
      tile.append(img);
      columns[i].append(tile);
    }
    gridEl.replaceChildren(...columns);

    const lines = [];
    for (const a of [...s.albums].sort((x, y) => byName(x.name, y.name))) {
      const collapsed = a.name !== s.open && a.name !== "naps";
      const folder = el("li", "folder", a.name + "/");
      if (collapsed) {
        folder.classList.add("collapsed");
        if (a.files.length) folder.append(el("span", null, `  ${a.files.length} items`));
      }
      if (s.fresh.has(a.name)) folder.classList.add("fresh");
      lines.push(folder);
      if (collapsed) continue;
      for (const f of [...a.files].sort(byName)) {
        const line = el("li", "file", f);
        if (s.fresh.has(a.name + "/" + f)) line.classList.add("fresh");
        lines.push(line);
      }
    }
    treeEl.replaceChildren(...lines);

    s.fresh.clear();
  }

  // Cursor and the photo it carries

  const CANCELLED = Symbol("cancelled");
  let current = 0;
  let at = { x: 0, y: 0 };
  let ghost = null;
  const running = new Set();

  function rel(target, ax = 0.5, ay = 0.5) {
    const box = target.getBoundingClientRect();
    const origin = stage.getBoundingClientRect();
    return {
      x: box.left - origin.left + box.width * ax,
      y: box.top - origin.top + box.height * ay,
    };
  }

  const place = (p) => `translate(${p.x}px, ${p.y}px)`;
  const placeGhost = (p) => `translate(${p.x - 24}px, ${p.y - 20}px)`;

  async function guard(run, promise) {
    await promise;
    if (run !== current) throw CANCELLED;
  }

  function animate(node, frames, duration) {
    const a = node.animate(frames, {
      duration: reduced ? 0 : duration,
      easing: "cubic-bezier(.45,0,.2,1)",
      fill: "forwards",
    });
    running.add(a);
    // Keep the end state inline so the next animation starts from it.
    // A cancelled animation rejects; callers check the run instead.
    return a.finished
      .then(() => {
        if (!node.isConnected) return;
        a.commitStyles();
        a.cancel();
      })
      .catch(() => {})
      .finally(() => running.delete(a));
  }

  const pause = (run, ms) =>
    guard(run, new Promise((r) => setTimeout(r, reduced ? Math.min(ms, 150) : ms)));

  function grab(file) {
    ghost?.remove();
    ghost = el("div", "drag-ghost");
    ghost.style.transform = placeGhost(at);
    const img = el("img");
    img.src = src(file);
    img.alt = "";
    ghost.append(img, el("span", null, file));
    stage.append(ghost);
    animate(ghost, [{ opacity: 0 }, { opacity: 1 }], 200);
  }

  function park() {
    at = rel(gridEl, 0.6, 0.55);
    cursor.style.transform = place(at);
  }

  async function move(run, to) {
    const from = at;
    at = to;
    const duration = Math.min(1300, Math.max(600, Math.hypot(to.x - from.x, to.y - from.y) * 2));
    if (ghost) animate(ghost, [{ transform: placeGhost(from) }, { transform: placeGhost(to) }], duration);
    await guard(
      run,
      animate(cursor, [{ transform: place(from), opacity: 1 }, { transform: place(to), opacity: 1 }], duration),
    );
  }

  async function drop(run) {
    if (ghost) {
      const g = ghost;
      ghost = null;
      animate(g, [{ opacity: 1 }, { opacity: 0, transform: placeGhost(at) + " scale(.6)" }], 200).then(() =>
        g.remove(),
      );
    }
    await pause(run, 150);
  }

  function reset() {
    for (const a of running) a.cancel();
    running.clear();
    ghost?.remove();
    ghost = null;
  }

  // Playback

  let timer = 0;

  async function play(index) {
    const run = ++current;
    clearTimeout(timer);
    reset();

    const state = initial();
    for (const scene of scenes.slice(0, index)) scene.apply(state);
    state.fresh.clear();
    render(state);

    try {
      await pause(run, 700);
      await scenes[index].play(run);
      scenes[index].apply(state);
      render(state);
    } catch (e) {
      if (e === CANCELLED) return;
      throw e;
    }
    const last = index === scenes.length - 1;
    timer = setTimeout(() => play(last ? 0 : index + 1), last ? 3000 : 2000);
  }

  // Positions depend on layout, so restart the loop after a resize.
  let resizeTimer = 0;
  let size = innerWidth + "x" + innerHeight;
  addEventListener("resize", () => {
    if (innerWidth + "x" + innerHeight === size) return;
    size = innerWidth + "x" + innerHeight;
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(() => {
      reset();
      park();
      play(0);
    }, 300);
  });

  render(initial());
  park();
  addEventListener("load", () => play(0));
})();
