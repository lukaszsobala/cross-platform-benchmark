"use strict";

// cpu-bench results hub, front end. No dependencies, no inline script (the
// server sends a CSP that forbids it). Every value that came from an upload is
// written with textContent, never innerHTML.

const state = {
  metrics: [],          // from /api/metrics
  rows: [],             // current leaderboard page: one row per upload
  children: new Map(),  // run id -> that upload's records, once expanded
  expanded: new Set(),  // run ids currently showing their records
  selected: new Map(),  // core id -> row; all of one scope, see retargetSelection
  params: new URLSearchParams(),   // the filters the current board was loaded with
  sort: "score",        // which metric the board is ranked by, set by its header
  order: "desc",
  search: "",           // the last submitted search, not what is in the box
  norm: "abs",
  detailed: false,      // every metric, rather than the headline set
  scopeNote: "",        // what changing scope did to the selection, if anything
};

// Which way a metric runs, in words and in one glyph. Every place a metric is
// named says this: a bar is drawn from the measurement itself, so nothing in
// the picture tells you which end of it wins.
const DIRECTIONS = {
  high: { text: "higher is better", mark: "↑" },
  low:  { text: "lower is better",  mark: "↓" },
  none: { text: "not a ranking",    mark: "" },
};

const SCOPE_LABELS = {
  total: "whole-machine",
  cpu: "per-core",
  thread: "per-thread",
};

function direction(metric) {
  return (metric && DIRECTIONS[metric.better]) || null;
}

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

// Per-GHz normalisation. Rates divide by clock and latencies in ns become
// cycles; `ratio` and `fixed` pass through untouched -- the first is already
// clock-independent, the second is not the core clock's to set (see METRICS in
// server.py). Nothing here invents a unit the metric table did not ask for.
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

// The columns a table shows. The metric being sorted on is always one of them —
// a board ordered by a number it does not print reads as arbitrary.
function shownMetrics() {
  if (state.detailed) return state.metrics;
  return state.metrics.filter((m) => m.headline || m.key === state.sort);
}

function machineName(row) {
  return row.cpu_models || row.machine || "unknown CPU";
}

// Machine and record together, for the compare chips: there a row has been
// lifted out of the board and has to identify itself on its own. The board
// itself names the machine only.
function coreName(row) {
  const model = machineName(row);
  if (row.scope === "total") return `${model} · all ${row.threads ?? "?"} threads`;
  return row.cpu === null || row.cpu === undefined ? model : `${model} · cpu${row.cpu}`;
}

function recordName(row) {
  if (row.scope === "total") return `all ${row.threads ?? "?"} threads`;
  const what = row.scope === "thread" ? "thread" : "cpu";
  return row.cpu === null || row.cpu === undefined ? what : `${what}${row.cpu}`;
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
  // The dropdowns reload the board the moment they change, so their live value
  // is always the applied one.
  for (const name of ["scope", "target", "vectorize", "fma"]) {
    const v = form.elements[name].value;
    if (v) p.set(name, v);
  }
  // The search box does not: half-typed text is not a filter anyone asked for,
  // so it applies on submit and is held here until then.
  if (state.search) p.set("q", state.search);
  // Sorting is not a filter either -- it lives on the column headers.
  p.set("sort", state.sort);
  p.set("order", state.order);
  p.set("limit", "100");
  return p;
}

// The direction a metric is worth reading in the first time you click it:
// down the rankings, whichever end of the scale that is.
function naturalOrder(key) {
  const m = state.metrics.find((x) => x.key === key);
  return m && m.better === "low" ? "asc" : "desc";
}

function sortBy(key) {
  if (state.sort === key) state.order = state.order === "desc" ? "asc" : "desc";
  else { state.sort = key; state.order = naturalOrder(key); }
  loadBoard().catch((e) => alert(e.message));
}

async function loadBoard() {
  const form = $("#filters");
  state.norm = form.elements.norm.value;
  state.detailed = form.elements.detailed.checked;
  // Remember what was actually applied: expanding a row has to ask for the
  // same filters and the same order, not whatever the form says by then.
  state.params = filterParams();
  // Which record represents an upload depends on the sort metric, so a reload
  // invalidates anything already expanded.
  state.children.clear();
  state.expanded.clear();
  // Follow the scope being ranked, so a comparison is never half one kind of
  // record and half another. Uses the params just applied, so the record it
  // picks per run is the one the board is about to show.
  await retargetSelection(state.params.get("scope"));
  const { cores } = await api("/api/cores?" + state.params.toString());
  state.rows = cores;
  renderBoard();
  renderSelection();
}

// The records of one upload, in the board's own order. Fetched on demand: a
// per-core sweep of a 256-CPU machine is 256 rows nobody asked to see.
async function toggleExpand(row) {
  if (state.expanded.delete(row.run_id)) return renderBoard();
  if (!state.children.has(row.run_id)) {
    const p = new URLSearchParams(state.params);
    p.set("run", row.run_id);
    p.set("group", "none");
    p.set("limit", "1024");
    const { cores } = await api("/api/cores?" + p.toString());
    state.children.set(row.run_id, cores);
  }
  state.expanded.add(row.run_id);
  renderBoard();
}

function selectBox(row) {
  const box = el("td");
  const cb = el("input");
  cb.type = "checkbox";
  cb.checked = state.selected.has(row.id);
  cb.addEventListener("change", () =>
    toggleSelect(row, cb.checked).catch((e) => alert(e.message)));
  box.append(cb);
  return box;
}

// A column header that ranks the board by its own column. Clicking the one
// already sorted on turns it around; clicking another starts it at its natural
// end. Every metric the server will sort on has one, so the sort is wherever
// the number is rather than in a control away from it.
function sortHeader(label, unit, key) {
  const active = state.sort === key;
  // Spelling "higher is better" out fifteen times across one header row would
  // double the table's width, so a column carries the glyph and says the words
  // in its tooltip; the legend under the table gives both in full.
  const dir = direction(state.metrics.find((x) => x.key === key));
  const th = el("th", active ? "num sorted" : "num");
  th.setAttribute("aria-sort", !active ? "none"
    : state.order === "desc" ? "descending" : "ascending");
  const btn = el("button", "sorth");
  btn.type = "button";
  btn.title = dir ? `sort by ${label} — ${dir.text}` : `sort by ${label}`;
  const name = el("span", "mname", label);
  if (active) name.append(el("span", "arrow", state.order === "desc" ? "▾" : "▴"));
  btn.append(name, el("span", "munit", dir && dir.mark ? `${unit} ${dir.mark}` : unit));
  btn.addEventListener("click", () => sortBy(key));
  th.append(btn);
  return th;
}

function metricCells(tr, row) {
  tr.append(el("td", "num", row.mhz ? Math.round(row.mhz) : "—"));
  for (const m of shownMetrics()) tr.append(el("td", "num", fmt(value(row, m))));
}

function renderBoard() {
  const table = $("#board-table");
  const thead = table.tHead;
  const tbody = table.tBodies[0];
  thead.replaceChildren();
  tbody.replaceChildren();
  $("#board-empty").hidden = state.rows.length > 0;

  const hr = el("tr");
  hr.append(el("th", "num", "#"), el("th", "", ""), el("th", "wide", "machine"),
            el("th", "", "arch"), el("th", "", "build"),
            sortHeader("MHz", "", "mhz"));
  for (const m of shownMetrics()) hr.append(sortHeader(m.label, unitOf(m), m.key));
  thead.append(hr);

  state.rows.forEach((row, i) => {
    const open = state.expanded.has(row.run_id);
    const tr = el("tr", "run-row");
    tr.append(el("td", "num", i + 1), selectBox(row));

    const name = el("td", "wide");
    const head = el("div", "run-head");
    if ((row.records ?? 1) > 1) {
      const ex = el("button", "expander", open ? "▾" : "▸");
      ex.type = "button";
      ex.title = open ? "hide this run's records" : "show every record in this run";
      ex.setAttribute("aria-expanded", String(open));
      ex.addEventListener("click", () => toggleExpand(row).catch((e) => alert(e.message)));
      head.append(ex);
    } else {
      head.append(el("span", "expander blank", " "));
    }
    const link = el("button", "linkish", machineName(row));
    link.type = "button";
    link.addEventListener("click", () => showRun(row.run_id));
    head.append(link);
    name.append(head);
    if (row.label) name.append(el("span", "label", row.label));
    tr.append(name);

    tr.append(el("td", "", row.target || "—"), el("td", "flags", flagsText(row)));
    metricCells(tr, row);
    tbody.append(tr);

    if (!open) return;
    for (const c of state.children.get(row.run_id) || []) {
      const sub = el("tr", "child-row");
      sub.append(el("td", "num", ""), selectBox(c));
      const cname = el("td", "wide");
      cname.append(el("span", "childname", recordName(c)));
      sub.append(cname, el("td", "", ""), el("td", "", ""));
      metricCells(sub, c);
      tbody.append(sub);
    }
  });
}

// The record that stands for one run at the board's metric, in a given scope --
// the same pick the leaderboard makes, asked for one run at a time.
async function representative(runId, scope) {
  // Built from the ranking alone, not from state.params: the board's filters
  // narrow what is ranked, and a ticked run must not vanish because the search
  // box no longer matches it.
  const p = new URLSearchParams();
  p.set("run", String(runId));
  p.set("scope", scope);
  p.set("group", "run");
  p.set("sort", state.sort);
  p.set("order", state.order);
  p.set("limit", "1");
  const { cores } = await api("/api/cores?" + p.toString());
  return cores[0] || null;
}

// One comparison, one scope. A 'total' row is the sum over every thread, so
// setting it beside a single core's numbers compares a machine with a part of
// one -- the bars would be true and the reading of them false. Changing scope
// therefore carries the selection across rather than dropping it: the same
// machines stay ticked, as their record in the scope now being ranked, and the
// rows they were ticked on lose their tick. A run with nothing in the new scope
// (a --per-core upload has no whole-machine total) has to leave, and says so.
async function retargetSelection(scope) {
  const rows = [...state.selected.values()];
  if (!scope || !rows.length || rows.every((r) => r.scope === scope)) return;
  const next = new Map();
  const lost = [];
  for (const r of rows) {
    if (r.scope === scope) {
      next.set(r.id, r);
      continue;
    }
    const rec = await representative(r.run_id, scope);
    if (rec) next.set(rec.id, rec);
    else lost.push(machineName(r));
  }
  state.selected = next;
  state.scopeNote = lost.length
    ? `${lost.join(", ")} has no ${SCOPE_LABELS[scope] || scope} record, so it ` +
      `left the comparison when the ranking changed.`
    : "";
  syncHash();
}

async function toggleSelect(row, on) {
  if (on) {
    // Ticking across scopes moves the whole comparison to the new one rather
    // than mixing the two.
    await retargetSelection(row.scope);
    state.selected.set(row.id, row);
  } else {
    state.selected.delete(row.id);
    if (!state.selected.size) state.scopeNote = "";
  }
  syncHash();
  renderSelection();
  // A record can appear twice -- as a run's representative and in its expanded
  // list -- so redraw to keep both boxes agreeing.
  renderBoard();
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
  if (state.scopeNote) warn.append(el("p", "warn", state.scopeNote));
  if (!n) return;

  const rows = [...state.selected.values()];

  const builds = new Set(rows.map((r) => `${r.vectorize}/${r.fma}`));
  if (builds.size > 1) {
    warn.append(el("p", "warn",
      "These rows were built with different vectorize/FMA flags. A vectorised " +
      "or FMA-contracted build does more work per instruction, so the compute " +
      "numbers are not comparable with a scalar one."));
  }
  // The backstop. retargetSelection keeps the selection to one scope at every
  // door into it, so this should not fire -- but a mixed comparison reads as a
  // result rather than as a mistake, and that is not a thing to leave to an
  // invariant holding.
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
    const dir = direction(m);

    const block = el("div", "cmp-metric");
    const h = el("div", "cmp-title");
    h.append(el("strong", null, m.label), el("span", "munit", unitOf(m)));
    if (dir) h.append(el("span", "hint", dir.text));
    block.append(h);

    rows.forEach((r, i) => {
      const v = vals[i];
      const line = el("div", "bar-row");
      const bar = el("div", "bar");
      const fill = el("div", `fill c${i % 6}`);
      // The bar is the measurement, not a verdict: its length is the value
      // against the largest in the selection, whichever way the metric runs.
      // Which end wins is said in words above the group, so a 384 ns latency
      // draws the long bar its number deserves and the "best" mark beside the
      // 109 ns one says who took it.
      const frac = v === null || !max ? 0 : v / max;
      fill.style.width = frac ? `${Math.max(1, frac * 100)}%` : "0%";
      bar.append(fill);
      line.append(bar, el("span", "bar-val", fmt(v)));
      // Always present, even when empty: a missing cell would let one row's
      // bar run wider than the rest and break the shared scale by eye.
      const rel = v !== null && best && m.better !== "none"
        ? (m.better === "low" ? best / v : v / best) : null;
      line.append(el("span", "bar-rel", rel === null ? ""
        : rel >= 0.999 ? "best" : `${(rel * 100).toFixed(0)}%`));
      block.append(line);
    });
    body.append(block);
  }
}

// ---------------------------------------------------------------------------
// Run detail
// ---------------------------------------------------------------------------

function metaList(pairs) {
  const dl = el("dl", "meta");
  for (const [k, v] of pairs) {
    if (!v) continue;
    dl.append(el("dt", null, k), el("dd", null, v));
  }
  return dl;
}

async function showRun(id) {
  const run = await api(`/api/runs/${id}`);
  const rank = await api(`/api/runs/${id}/rank`).catch(() => ({}));
  const box = $("#detail-body");
  box.replaceChildren();

  box.append(el("h2", null, run.cpu_models || run.machine || `run ${run.id}`));
  if (run.label) box.append(el("p", "label big", run.label));
  if (run.notes) box.append(el("p", "notes", run.notes));

  // What identifies the run, then everything that qualifies it. The second set
  // is what you go looking for once a number surprises you, not what you read
  // on the way in, so it starts folded.
  box.append(metaList([
    ["uploaded", run.created_at],
    ["system", [run.sysname, run.os_release, run.machine].filter(Boolean).join(" ")],
  ]));

  const rest = metaList([
    ["mode", run.mode],
    ["arch", run.target],
    ["build", `${run.compiler || "?"}${run.compiler_version && run.compiler_version !== run.compiler ? ` (${run.compiler_version})` : ""}` +
              ` · ${run.vectorize ? "vectorize on" : "vectorize off"} · ${run.fma ? "FMA on" : "FMA off"}`],
    ["flags", run.build_flags ? `${run.cc || "cc"} ${run.build_flags}` : null],
    ["config", `${run.threads ?? "?"} threads · ${run.seconds ?? "?"}s/phase × ${run.reps ?? "?"} reps · ` +
                `${run.mem_bytes ? (run.mem_bytes / 1048576).toFixed(0) + " MiB/thread" : "no memory phases"} · ` +
                `pin ${run.pin ? "on" : "off"}`],
    ["DRAM", run.dram_name ? `${run.dram_name} at ${fmt(run.dram_mhz_min, 0)}–${fmt(run.dram_mhz_max, 0)} MHz` : null],
    ["checksum", run.checksum],
  ]);
  if (rest.childElementCount) {
    const more = el("details", "more");
    more.append(el("summary", null, "Build, config and checksum"), rest);
    box.append(more);
  }

  if (rank.metrics && Object.keys(rank.metrics).length) {
    box.append(el("h3", null, `Best results`));
    const list = el("div", "percentiles");
    for (const m of shownMetrics()) {
      const r = rank.metrics[m.key];
      if (!r || r.percentile === null) continue;
      const dir = direction(m);
      const line = el("div", "pct-row");
      line.append(el("span", "pct-name", m.label));
      // These bars are a standing, not a measurement, and a standing has only
      // one good end however the metric runs -- so the direction is said here
      // too, to keep it from being read as one of the value bars in Compare.
      line.append(el("span", "pct-dir", dir ? dir.text : ""));
      const bar = el("div", "bar");
      const fill = el("div", "fill c0");
      fill.style.width = `${Math.max(1, r.percentile)}%`;
      bar.append(fill);
      line.append(bar);
      line.append(el("span", "pct-val",
        `beats ${r.percentile.toFixed(0)}% of ${r.population}`));
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
  for (const m of shownMetrics()) {
    const dir = direction(m);
    const th = el("th", "num", dir && dir.mark ? `${m.label} ${dir.mark}` : m.label);
    if (dir) th.title = `${m.label} — ${dir.text}`;
    hr.append(th);
  }
  thead.append(hr);
  const tbody = el("tbody");
  for (const c of run.cores) {
    const tr = el("tr");
    tr.append(el("td", null, c.scope),
              el("td", "num", c.cpu ?? "—"),
              el("td", "num", c.mhz ? Math.round(c.mhz) : "—"));
    for (const m of shownMetrics()) tr.append(el("td", "num", fmt(c[m.key])));
    tbody.append(tr);
  }
  table.append(thead, tbody);
  scroller.append(table);
  box.append(scroller);
  box.append(el("p", "note legend", "↑ higher is better · ↓ lower is better"));

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
        state.scopeNote = "";
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
    out.append(el("p", "ok",
      `Stored run ${res.id} — one leaderboard row, holding all ${res.records} ` +
      `records. Expand it to see every core.`));
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

  $("#filters").addEventListener("submit", (e) => {
    e.preventDefault();
    // Submitting the form is the search: it is the one filter that waits to be
    // asked for, since a board that reshuffles on every keystroke is unusable.
    state.search = $("#filters").elements.q.value.trim();
    loadBoard().catch((err) => alert(err.message));
  });
  // Picking from a dropdown is the whole gesture -- there is nothing to confirm
  // afterwards, so each one reloads the board itself.
  for (const name of ["scope", "target", "vectorize", "fma"]) {
    $("#filters").elements[name].addEventListener("change", () => {
      loadBoard().catch((err) => alert(err.message));
    });
  }
  // These two change how the rows are drawn, not which rows they are, so
  // neither refetches.
  const redraw = () => { renderBoard(); renderSelection(); };
  $("#filters").elements.norm.addEventListener("change", (e) => {
    state.norm = e.target.value;
    redraw();
  });
  $("#filters").elements.detailed.addEventListener("change", (e) => {
    state.detailed = e.target.checked;
    redraw();
  });
  $("#uploadform").addEventListener("submit", doUpload);
  $("#clearsel").addEventListener("click", () => {
    state.selected.clear();
    state.scopeNote = "";
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

  renderMine();
  renderSelection();
  await loadBoard();

  // Deep links: #run=N opens a run, #compare=1,2,3 restores a shared comparison.
  const cmp = location.hash.match(/compare=([\d,]+)/);
  if (cmp) {
    const { cores } = await api("/api/cores?limit=64&ids=" + cmp[1]);
    // A link names records directly, so it is the one way a mixed selection can
    // still arrive. Take the scope of the first and keep the board on it; the
    // rest of that scope come along, anything else is left behind rather than
    // silently compared against a whole machine.
    const scope = cores.length ? cores[0].scope : null;
    const kept = cores.filter((c) => c.scope === scope);
    for (const row of kept) state.selected.set(row.id, row);
    state.scopeNote = kept.length < cores.length
      ? `${cores.length - kept.length} row(s) in that link were a different ` +
        `scope and were left out: one comparison holds one kind of record.`
      : "";
    if (scope && $("#filters").elements.scope.value !== scope) {
      $("#filters").elements.scope.value = scope;
      await loadBoard();
    }
    syncHash();
    renderSelection();
    renderBoard();
    if (kept.length) showTab("compare");
  }
  const m = location.hash.match(/run=(\d+)/);
  if (m) showRun(Number(m[1])).catch(() => {});
}

boot().catch((e) => {
  document.body.prepend(el("p", "warn", `Could not start: ${e.message}`));
});
