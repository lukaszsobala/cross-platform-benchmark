"use strict";

// cpu-bench results hub, front end. No dependencies, no inline script (the
// server sends a CSP that forbids it). Every value that came from an upload is
// written with textContent, never innerHTML.

const state = {
  metrics: [],          // from /api/metrics
  rows: [],             // current leaderboard page
  selected: new Map(),  // core id -> row
  norm: "abs",
};

const $ = (sel) => document.querySelector(sel);
const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text !== undefined && text !== null) n.textContent = String(text);
  return n;
};

async function api(path, opts) {
  const r = await fetch(path, opts);
  const body = await r.json().catch(() => ({ error: `HTTP ${r.status}` }));
  if (!r.ok) throw new Error(body.error || `HTTP ${r.status}`);
  return body;
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

function fmt(v, digits) {
  if (v === null || v === undefined) return "—";
  const a = Math.abs(v);
  if (a === 0) return "0";
  if (a >= 10000) return v.toFixed(0);
  if (a >= 100) return v.toFixed(digits ?? 1);
  if (a >= 1) return v.toFixed(digits ?? 2);
  return v.toFixed(3);
}

// Per-GHz normalisation. Rates divide by clock, latencies in ns become cycles,
// ratios and lengths are already clock-independent.
function value(row, metric) {
  const raw = row[metric.key];
  if (raw === null || raw === undefined) return null;
  if (state.norm === "abs" || !row.mhz) return raw;
  const ghz = row.mhz / 1000;
  if (!ghz) return raw;
  if (metric.kind === "rate") return raw / ghz;
  if (metric.kind === "time") return raw * ghz;
  return raw;
}

function unitOf(metric) {
  if (state.norm === "abs") return metric.unit;
  if (metric.kind === "rate") return metric.unit + "/GHz";
  if (metric.kind === "time") return "cycles";
  return metric.unit;
}

function coreName(row) {
  const model = row.cpu_models || row.machine || "unknown CPU";
  if (row.scope === "total") return `${model} · all ${row.threads ?? "?"} threads`;
  return row.cpu === null || row.cpu === undefined ? model : `${model} · cpu${row.cpu}`;
}

function flagsText(row) {
  const v = row.vectorize ? "vec" : "scalar";
  const f = row.fma ? "fma" : "no-fma";
  return `${v}, ${f}`;
}

// ---------------------------------------------------------------------------
// Leaderboard
// ---------------------------------------------------------------------------

function filterParams() {
  const form = $("#filters");
  const p = new URLSearchParams();
  for (const name of ["scope", "target", "vectorize", "fma", "sort", "q"]) {
    const v = form.elements[name].value;
    if (v) p.set(name, v);
  }
  const metric = state.metrics.find((m) => m.key === p.get("sort"));
  p.set("order", metric && metric.better === "low" ? "asc" : "desc");
  p.set("limit", "100");
  return p;
}

async function loadBoard() {
  state.norm = $("#filters").elements.norm.value;
  const { cores } = await api("/api/cores?" + filterParams().toString());
  state.rows = cores;
  renderBoard();
}

function renderBoard() {
  const table = $("#board-table");
  const thead = table.tHead;
  const tbody = table.tBodies[0];
  thead.replaceChildren();
  tbody.replaceChildren();
  $("#board-empty").hidden = state.rows.length > 0;

  const hr = el("tr");
  hr.append(el("th", "num", "#"), el("th", "", ""), el("th", "wide", "core"),
            el("th", "", "arch"), el("th", "", "build"), el("th", "num", "MHz"));
  for (const m of state.metrics) {
    const th = el("th", "num");
    th.append(el("span", "mname", m.label), el("span", "munit", unitOf(m)));
    hr.append(th);
  }
  thead.append(hr);

  state.rows.forEach((row, i) => {
    const tr = el("tr");
    tr.append(el("td", "num", i + 1));

    const box = el("td");
    const cb = el("input");
    cb.type = "checkbox";
    cb.checked = state.selected.has(row.id);
    cb.addEventListener("change", () => toggleSelect(row, cb.checked));
    box.append(cb);
    tr.append(box);

    const name = el("td", "wide");
    const link = el("button", "linkish", coreName(row));
    link.type = "button";
    link.addEventListener("click", () => showRun(row.run_id));
    name.append(link);
    if (row.label) name.append(el("span", "label", row.label));
    tr.append(name);

    tr.append(el("td", "", row.target || "—"),
              el("td", "flags", flagsText(row)),
              el("td", "num", row.mhz ? Math.round(row.mhz) : "—"));
    for (const m of state.metrics) {
      tr.append(el("td", "num", fmt(value(row, m))));
    }
    tbody.append(tr);
  });
}

function toggleSelect(row, on) {
  if (on) state.selected.set(row.id, row);
  else state.selected.delete(row.id);
  syncHash();
  renderSelection();
}

// Keep the selection in the URL so a comparison can be shared as a link.
function syncHash() {
  const ids = [...state.selected.keys()];
  const want = ids.length ? `#compare=${ids.join(",")}` : "";
  if (location.hash !== want) {
    history.replaceState(null, "", location.pathname + want);
  }
}

function showTab(name) {
  for (const b of document.querySelectorAll("#tabs button")) {
    b.classList.toggle("active", b.dataset.tab === name);
  }
  for (const s of document.querySelectorAll("main .tab")) {
    s.classList.toggle("active", s.id === name);
  }
}

// ---------------------------------------------------------------------------
// Compare
// ---------------------------------------------------------------------------

function renderSelection() {
  const n = state.selected.size;
  $("#selcount").textContent = n ? `(${n})` : "";
  $("#clearsel").hidden = n === 0;
  $("#compare-note").hidden = n > 0;

  const body = $("#compare-body");
  const warn = $("#compare-warn");
  body.replaceChildren();
  warn.replaceChildren();
  if (!n) return;

  const rows = [...state.selected.values()];

  const builds = new Set(rows.map((r) => `${r.vectorize}/${r.fma}`));
  if (builds.size > 1) {
    warn.append(el("p", "warn",
      "These rows were built with different vectorize/FMA flags. A vectorised " +
      "or FMA-contracted build does more work per instruction, so the compute " +
      "numbers are not comparable with a scalar one."));
  }
  const scopes = new Set(rows.map((r) => r.scope));
  if (scopes.size > 1) {
    warn.append(el("p", "warn",
      "Mixing per-core, per-thread and whole-machine records: a 'total' row is " +
      "the sum over every thread, so it will tower over single-core rows."));
  }

  const head = el("div", "cmp-head");
  rows.forEach((r, i) => {
    const chip = el("div", `chip c${i % 6}`);
    chip.append(el("strong", null, coreName(r)));
    chip.append(el("span", null,
      `${r.target || "?"} · ${flagsText(r)} · ${r.mhz ? Math.round(r.mhz) + " MHz" : "clock unknown"}`));
    head.append(chip);
  });
  body.append(head);

  for (const m of state.metrics) {
    const vals = rows.map((r) => value(r, m));
    if (vals.every((v) => v === null)) continue;
    const finite = vals.filter((v) => v !== null);
    const best = m.better === "low" ? Math.min(...finite) : Math.max(...finite);
    const max = Math.max(...finite);

    const block = el("div", "cmp-metric");
    const h = el("div", "cmp-title");
    h.append(el("strong", null, m.label), el("span", "munit", unitOf(m)));
    if (m.better === "low") h.append(el("span", "hint", "lower is better"));
    if (m.better === "none") h.append(el("span", "hint", "not a ranking"));
    block.append(h);

    rows.forEach((r, i) => {
      const v = vals[i];
      const line = el("div", "bar-row");
      const bar = el("div", "bar");
      const fill = el("div", `fill c${i % 6}`);
      // A longer bar always means a better result, so a lower-is-better metric
      // is drawn as best/value rather than value/max -- otherwise the slowest
      // memory latency in the group would draw the most impressive bar.
      const frac = v === null ? 0
                 : m.better === "low" ? (v ? best / v : 0)
                 : (max ? v / max : 0);
      fill.style.width = frac ? `${Math.max(1, frac * 100)}%` : "0%";
      bar.append(fill);
      line.append(bar, el("span", "bar-val", fmt(v)));
      if (v !== null && best && m.better !== "none") {
        const rel = m.better === "low" ? best / v : v / best;
        line.append(el("span", "bar-rel", rel >= 0.999 ? "best" : `${(rel * 100).toFixed(0)}%`));
      }
      block.append(line);
    });
    body.append(block);
  }
}

// ---------------------------------------------------------------------------
// Run detail
// ---------------------------------------------------------------------------

async function showRun(id) {
  const run = await api(`/api/runs/${id}`);
  const rank = await api(`/api/runs/${id}/rank`).catch(() => ({}));
  const box = $("#detail-body");
  box.replaceChildren();

  box.append(el("h2", null, run.cpu_models || run.machine || `run ${run.id}`));
  if (run.label) box.append(el("p", "label big", run.label));
  if (run.notes) box.append(el("p", "notes", run.notes));

  const meta = el("dl", "meta");
  const pairs = [
    ["uploaded", run.created_at],
    ["mode", run.mode],
    ["arch", run.target],
    ["build", `${run.compiler || "?"}${run.compiler_version && run.compiler_version !== run.compiler ? ` (${run.compiler_version})` : ""}` +
              ` · ${run.vectorize ? "vectorize on" : "vectorize off"} · ${run.fma ? "FMA on" : "FMA off"}`],
    ["flags", run.build_flags ? `${run.cc || "cc"} ${run.build_flags}` : null],
    ["system", [run.sysname, run.os_release, run.machine].filter(Boolean).join(" ")],
    ["config", `${run.threads ?? "?"} threads · ${run.seconds ?? "?"}s/phase × ${run.reps ?? "?"} reps · ` +
                `${run.mem_bytes ? (run.mem_bytes / 1048576).toFixed(0) + " MiB/thread" : "no memory phases"} · ` +
                `pin ${run.pin ? "on" : "off"}`],
    ["DRAM", run.dram_name ? `${run.dram_name} at ${fmt(run.dram_mhz_min, 0)}–${fmt(run.dram_mhz_max, 0)} MHz` : null],
    ["checksum", run.checksum],
  ];
  for (const [k, v] of pairs) {
    if (!v) continue;
    meta.append(el("dt", null, k), el("dd", null, v));
  }
  box.append(meta);

  if (rank.metrics && Object.keys(rank.metrics).length) {
    box.append(el("h3", null, `Where this run's best ${rank.scope} values land`));
    const list = el("div", "percentiles");
    for (const m of state.metrics) {
      const r = rank.metrics[m.key];
      if (!r || r.percentile === null) continue;
      const line = el("div", "pct-row");
      line.append(el("span", "pct-name", m.label));
      const bar = el("div", "bar");
      const fill = el("div", "fill c0");
      fill.style.width = `${Math.max(1, r.percentile)}%`;
      bar.append(fill);
      line.append(bar);
      line.append(el("span", "pct-val",
        `${r.percentile.toFixed(0)}th of ${r.population}`));
      list.append(line);
    }
    box.append(list);
  }

  box.append(el("h3", null, "Records"));
  const scroller = el("div", "scroller");
  const table = el("table");
  const thead = el("thead");
  const hr = el("tr");
  hr.append(el("th", null, "scope"), el("th", "num", "cpu"), el("th", "num", "MHz"));
  for (const m of state.metrics) hr.append(el("th", "num", m.label));
  thead.append(hr);
  const tbody = el("tbody");
  for (const c of run.cores) {
    const tr = el("tr");
    tr.append(el("td", null, c.scope),
              el("td", "num", c.cpu ?? "—"),
              el("td", "num", c.mhz ? Math.round(c.mhz) : "—"));
    for (const m of state.metrics) tr.append(el("td", "num", fmt(c[m.key])));
    tbody.append(tr);
  }
  table.append(thead, tbody);
  scroller.append(table);
  box.append(scroller);

  const dl = el("a", "linkish", "download the original JSON");
  dl.href = `/api/runs/${run.id}/raw`;
  dl.setAttribute("download", `cpu-bench-run-${run.id}.json`);
  const dlp = el("p");
  dlp.append(dl);
  box.append(dlp);

  $("#detail").hidden = false;
}

// ---------------------------------------------------------------------------
// Upload
// ---------------------------------------------------------------------------

function myRuns() {
  try {
    return JSON.parse(localStorage.getItem("cpu-bench-uploads") || "[]");
  } catch {
    return [];
  }
}

function rememberRun(entry) {
  const all = myRuns();
  all.unshift(entry);
  localStorage.setItem("cpu-bench-uploads", JSON.stringify(all.slice(0, 100)));
  renderMine();
}

function renderMine() {
  const box = $("#minelist");
  box.replaceChildren();
  const all = myRuns();
  if (!all.length) {
    box.append(el("p", "empty", "No uploads from this browser yet."));
    return;
  }
  for (const entry of all) {
    const card = el("div", "mine-card");
    const title = el("button", "linkish", entry.name || `run ${entry.id}`);
    title.type = "button";
    title.addEventListener("click", () => showRun(entry.id));
    card.append(title);
    card.append(el("span", "muted", entry.at || ""));
    const tok = el("code", "token", entry.token);
    card.append(tok);
    const del = el("button", "danger", "Delete");
    del.type = "button";
    del.addEventListener("click", async () => {
      if (!confirm(`Delete run ${entry.id} from the leaderboard?`)) return;
      try {
        await api(`/api/runs/${entry.id}`, {
          method: "DELETE",
          headers: { "X-Delete-Token": entry.token },
        });
        localStorage.setItem("cpu-bench-uploads",
          JSON.stringify(myRuns().filter((e) => e.id !== entry.id)));
        state.selected.clear();
        renderSelection();
        renderMine();
        loadBoard().catch(() => {});
      } catch (e) {
        alert(e.message);
      }
    });
    card.append(del);
    box.append(card);
  }
}

async function doUpload(ev) {
  ev.preventDefault();
  const out = $("#uploadresult");
  out.replaceChildren();

  let text = $("#paste").value.trim();
  const file = $("#file").files[0];
  if (file) text = (await file.text()).trim();
  if (!text) {
    out.append(el("p", "warn", "Choose a file or paste the JSON first."));
    return;
  }

  const p = new URLSearchParams();
  if ($("#label").value.trim()) p.set("label", $("#label").value.trim());
  if ($("#notes").value.trim()) p.set("notes", $("#notes").value.trim());

  try {
    const res = await api("/api/runs?" + p.toString(), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: text,
    });
    let name = `run ${res.id}`;
    try {
      const doc = JSON.parse(text);
      name = (doc.system && doc.system.cpu_models) || name;
    } catch { /* the server already validated it */ }
    rememberRun({ id: res.id, token: res.delete_token, name,
                  at: new Date().toISOString().slice(0, 16).replace("T", " ") });
    out.append(el("p", "ok", `Stored ${res.records} records as run ${res.id}.`));
    const p2 = el("p");
    const link = el("button", "linkish", "See where it lands");
    link.type = "button";
    link.addEventListener("click", () => showRun(res.id));
    p2.append(link);
    out.append(p2);
    out.append(el("p", "muted", "Delete token (keep it to withdraw this run):"));
    out.append(el("code", "token", res.delete_token));
    $("#paste").value = "";
    $("#file").value = "";
    loadBoard().catch(() => {});
  } catch (e) {
    out.append(el("p", "warn", e.message));
  }
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------

function setupTabs() {
  for (const b of document.querySelectorAll("#tabs button")) {
    b.addEventListener("click", () => showTab(b.dataset.tab));
  }
}

async function boot() {
  setupTabs();
  $("#curl").textContent =
    `./cpu-bench --per-core --json \\\n` +
    `  | curl -fsS -X POST "${location.origin}/api/runs?label=my+box" \\\n` +
    `         -H 'Content-Type: application/json' --data-binary @-`;

  const meta = await api("/api/metrics");
  state.metrics = meta.metrics;
  const sortsel = $("#sortsel");
  for (const m of state.metrics) {
    const o = el("option", null, m.label);
    o.value = m.key;
    sortsel.append(o);
  }
  sortsel.value = "score";

  $("#filters").addEventListener("submit", (e) => {
    e.preventDefault();
    loadBoard().catch((err) => alert(err.message));
  });
  $("#filters").elements.norm.addEventListener("change", () => {
    state.norm = $("#filters").elements.norm.value;
    renderBoard();
    renderSelection();
  });
  $("#uploadform").addEventListener("submit", doUpload);
  $("#clearsel").addEventListener("click", () => {
    state.selected.clear();
    syncHash();
    renderSelection();
    renderBoard();
  });
  $("#detail-close").addEventListener("click", () => { $("#detail").hidden = true; });
  $("#detail").addEventListener("click", (e) => {
    if (e.target.id === "detail") $("#detail").hidden = true;
  });
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") $("#detail").hidden = true;
  });

  const stats = await api("/api/stats").catch(() => null);
  if (stats) {
    const parts = [`${stats.runs} runs`, `${stats.records} records`];
    for (const t of stats.targets) parts.push(`${t.target}: ${t.n}`);
    $("#stats").textContent = parts.join(" · ");
  }

  renderMine();
  renderSelection();
  await loadBoard();

  // Deep links: #run=N opens a run, #compare=1,2,3 restores a shared comparison.
  const cmp = location.hash.match(/compare=([\d,]+)/);
  if (cmp) {
    const { cores } = await api("/api/cores?limit=64&ids=" + cmp[1]);
    for (const row of cores) state.selected.set(row.id, row);
    renderSelection();
    renderBoard();
    if (cores.length) showTab("compare");
  }
  const m = location.hash.match(/run=(\d+)/);
  if (m) showRun(Number(m[1])).catch(() => {});
}

boot().catch((e) => {
  document.body.prepend(el("p", "warn", `Could not start: ${e.message}`));
});
