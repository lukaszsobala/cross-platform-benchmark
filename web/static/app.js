"use strict";

// cpcpub results hub, front end. No dependencies, no inline script (the
// server sends a CSP that forbids it). Every value that came from an upload is
// written with textContent, never innerHTML.

const state = {
  metrics: [],          // from /api/metrics
  limit: 100,           // rows the board asks for, from its Show picker
  offset: 0,            // how far down the board this page starts
  total: 0,             // rows the board has in all, past this page
  mineLimit: 50,        // the same, for either list under My uploads
  mineOffset: 0,        // where the account half of My uploads starts
  mineTotal: 0,
  localOffset: 0,       // and where this browser's own list starts
  accountRuns: null,    // this account's uploads, once fetched; null = not yet
  accountError: "",     // why that listing is missing, if it is
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
  user: null,           // the signed-in account, from /api/auth/me
  submitter: "",        // board narrowed to one account's uploads, by name
  files: [],            // result documents chosen but not yet uploaded
  release: null,        // the newest release this hub verifies against, if any
  signed: null,         // {ci, attested}: how many runs carry an outside signature
};

// How many results a list may show at once. The server clamps every listing to
// MAX_PAGE as well, so asking for more by hand gets the same answer as asking
// for the cap; the picker offers the sizes worth choosing between.
const MAX_PAGE = 500;
const PAGE_SIZES = [25, 50, 100, 250, MAX_PAGE];

// The page reads this one to prove a write came from the page rather than from
// another site that merely knows the session cookie exists. The session cookie
// itself is HttpOnly and unreadable here, which is the point.
const CSRF_COOKIE = "cpb_csrf";

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

// A page size that came from a picker, a stored preference or a stale saved
// value, made into one this page will actually ask for.
function pageSize(v, fallback) {
  const n = Number.parseInt(v, 10);
  if (!Number.isFinite(n) || n < 1) return fallback;
  return Math.min(n, MAX_PAGE);
}

// A page size chosen once is the one you meant on the next visit too. Kept in
// local storage, where a browser that refuses it costs the preference and
// nothing else.
function storedPage(key, fallback) {
  try {
    const n = pageSize(localStorage.getItem(key), fallback);
    // A size the picker does not offer -- an older build's, or a hand-edited
    // one -- would leave the picker showing nothing at all, so it is not kept.
    return PAGE_SIZES.includes(n) ? n : fallback;
  } catch {
    return fallback;
  }
}

function rememberPage(key, n) {
  try {
    localStorage.setItem(key, String(n));
  } catch {
    /* a browser with storage denied still gets the size it picked, once */
  }
}

// A page size is a window on a listing, not a cap on it: every list here can
// be walked from end to end, so the size only decides how much of it is on
// screen at a time. One pager draws that for all three.
//
// `go(offset)` is handed the first row of the page asked for; whoever owns the
// list refetches or redraws from it.
// Where the last page of a listing starts. Zero for a listing that has no
// rows at all, which is also where a page that has outlived its rows belongs.
function lastPage(total, limit) {
  return total > 0 ? Math.floor((total - 1) / limit) * limit : 0;
}

function pager(where, { offset, limit, total }, go) {
  if (total <= limit && offset === 0) return;      // one page holds the lot
  const bar = el("nav", "pager");
  bar.setAttribute("aria-label", "pages");
  const last = lastPage(total, limit);

  const step = (label, to, disabled, title) => {
    const b = el("button", "page", label);
    b.type = "button";
    b.disabled = disabled;
    if (title) b.title = title;
    b.addEventListener("click", () => go(to));
    return b;
  };
  bar.append(step("« first", 0, offset <= 0, "the first page"),
             step("‹ prev", Math.max(0, offset - limit), offset <= 0, null));
  // Where you are, in rows rather than in page numbers: the rows are what the
  // reader is looking at, and the count is the answer to "is there more".
  const from = total ? offset + 1 : 0;
  const to = Math.min(offset + limit, total);
  bar.append(el("span", "pagecount",
    `${from.toLocaleString()}–${to.toLocaleString()} of ${total.toLocaleString()}`));
  bar.append(step("next ›", offset + limit, offset + limit >= total, null),
             step("last »", last, offset >= last, "the last page"));
  where.append(bar);
}

// Both pickers are filled from PAGE_SIZES rather than written out in the
// markup, so the cap is stated once and cannot drift from the server's.
function fillPageSizes(select, current) {
  select.replaceChildren();
  for (const n of PAGE_SIZES) {
    const opt = el("option", null, String(n));
    opt.value = String(n);
    select.append(opt);
  }
  select.value = String(current);
}

function cookie(name) {
  for (const part of document.cookie.split(";")) {
    const [k, ...v] = part.trim().split("=");
    if (k === name) return decodeURIComponent(v.join("="));
  }
  return "";
}

async function api(path, opts) {
  const o = { ...(opts || {}) };
  // Anything that changes something carries the CSRF token. Reads do not need
  // it, and neither does a request with no session behind it.
  if (o.method && o.method !== "GET") {
    const csrf = cookie(CSRF_COOKIE);
    if (csrf) o.headers = { ...(o.headers || {}), "X-CSRF-Token": csrf };
  }
  const r = await fetch(path, o);
  const body = await r.json().catch(() => ({ error: `HTTP ${r.status}` }));
  if (!r.ok) throw new Error(body.error || `HTTP ${r.status}`);
  return body;
}

// A button that puts a block of shell on the clipboard, for the machine that
// is not this one. Falls back to selecting the text when the clipboard is not
// available (it is not, over plain http on some browsers), so the gesture is
// never simply dead.
function copyButton(btn) {
  const pre = document.getElementById(btn.dataset.copy);
  if (!pre) return;
  const done = (msg) => {
    btn.textContent = msg;
    setTimeout(() => { btn.textContent = "Copy"; }, 1500);
  };
  const select = () => {
    const range = document.createRange();
    range.selectNodeContents(pre);
    const sel = window.getSelection();
    sel.removeAllRanges();
    sel.addRange(range);
    done("selected");
  };
  if (!navigator.clipboard) return select();
  navigator.clipboard.writeText(pre.textContent).then(() => done("copied"), select);
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

// What to call a machine whose run never said. uname's `machine` is the
// instruction set, not a CPU: printing it bare made a row read as a box called
// "aarch64", which is both untrue and already the target column's job. Runs
// uploaded by older binaries, before the benchmark could read a MIDR, are
// permanently in that state — nothing can go back and name them — so the label
// says what it knows and admits the rest.
function machineName(row, unknown = "unknown CPU") {
  if (row.cpu_models) return row.cpu_models;
  return row.machine ? `unnamed ${row.machine} CPU` : unknown;
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
  for (const name of ["scope", "target", "vectorize", "fma", "norm"]) {
    const v = form.elements[name].value;
    if (v) p.set(name, v);
  }
  // The search box does not: half-typed text is not a filter anyone asked for,
  // so it applies on submit and is held here until then.
  if (state.search) p.set("q", state.search);
  // Set by clicking a submitter's name rather than by a control in the form,
  // and shown as a chip above the table so it is never a filter you cannot see.
  if (state.submitter) p.set("user", state.submitter);
  if (form.elements.verified.value) p.set("verified", form.elements.verified.value);
  // Sorting is not a filter either -- it lives on the column headers.
  p.set("sort", state.sort);
  p.set("order", state.order);
  p.set("limit", String(state.limit));
  p.set("offset", String(state.offset));
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

// Bumped per load, so a reply that was overtaken -- two filters changed in
// quick succession, a withdrawal refreshing the board while a search is still
// in flight -- is dropped rather than drawn over the newer one.
let boardGen = 0;

// `keepPage` is for the reloads that are not a new question: a withdrawal
// refreshing the board, or paging itself. Everything else -- a filter, a
// search, a different sort, a different page size -- asks something new, and
// the answer to something new starts at its first page.
async function loadBoard({ keepPage = false } = {}) {
  const gen = ++boardGen;
  const form = $("#filters");
  state.norm = form.elements.norm.value;
  state.detailed = form.elements.detailed.checked;
  const size = pageSize(form.elements.limit.value, state.limit);
  if (size !== state.limit) state.offset = 0;   // a resized page starts again
  state.limit = size;
  rememberPage("cpcpub-page-board", state.limit);
  if (!keepPage) state.offset = 0;
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
  const page = await api("/api/cores?" + state.params.toString());
  if (gen !== boardGen) return;        // a newer load is already on its way
  state.rows = page.cores;
  state.total = page.total ?? page.cores.length;
  // Withdrawing the last rows of the last page, or a filter narrowing under
  // you, can leave the board standing past the end of its own listing. Step
  // back to where the rows now stop rather than showing an empty board with a
  // pager saying there is more above it. Only ever backwards, so a count and a
  // page that disagree cost one retry and not an endless run of them.
  const back = lastPage(state.total, state.limit);
  if (!state.rows.length && back < state.offset) {
    state.offset = back;
    return loadBoard({ keepPage: true });
  }
  renderBoard();
  renderSelection();
}

// Paging is a reload of the same question at a different depth, so it keeps
// the filters, the sort and the ticks, and only moves the window.
function goToPage(offset) {
  state.offset = Math.max(0, offset);
  loadBoard({ keepPage: true })
    .then(() => $("#board-table").scrollIntoView({ block: "start" }))
    .catch((e) => alert(e.message));
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
    p.delete("offset");        // the board's page is not this run's records

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

// Three different claims, and the display has to keep them apart or the
// strongest one is worth nothing.
//
//   ci         GitHub signed a token over these exact result bytes, from a job
//              on a GitHub-hosted runner running our pinned workflow. Nothing
//              in that chain is the submitter's.
//   attested   the same signed chain, on the submitter's own runner. It rules
//              out editing the file; it does not rule out the machine's owner.
//   release    the document says it came from a published binary. The document
//              is the submitter's, so this is a claim, not a check -- which is
//              why it is grey text rather than a mark.
//
// The mark says who signed, not how good the result is. "verified" read as a
// verdict on the numbers, which is the one thing no signature here speaks to --
// so each label now names the signer, and there is nothing left for a reader to
// infer. The wire values (`ci`, `attested`) are the API's and do not move.
const TRUST = {
  ci: {
    label: "github-signed",
    cls: "badge",
    tip: (r) => `GitHub signed these exact result bytes as measured on a ` +
                `GitHub-hosted runner by ` +
                `${r.attest_workflow || "the pinned workflow"} — no part of ` +
                `that chain belongs to the submitter`,
  },
  attested: {
    label: "signed, own machine",
    cls: "badge weak",
    tip: () => "signed by GitHub as a run of the pinned workflow, on a " +
               "self-hosted runner — the chain holds, the machine belongs to " +
               "the submitter",
  },
};

function trustBadge(row) {
  const kind = TRUST[row.attest_tier];
  if (kind) {
    const mark = el("span", kind.cls, kind.label);
    mark.title = kind.tip(row);
    return mark;
  }
  if (row.release_build) {
    // Not a badge, and not a demotion either: this is what an ordinary good
    // result looks like. The digest matches a binary that release published, so
    // the row is comparable with every other row claiming it -- which is the
    // question the mark answers. Who ran it is still the submitter's own word.
    //
    // The tag is the newest release those exact bytes appear in, which is the
    // hub's answer and not the run's: a binary unchanged across four releases
    // is one binary, and labelling it with the oldest of them would split one
    // population of comparable runs into four that look like different builds.
    const mark = el("span", "claim", `${row.release_build} build`);
    mark.title = `the digest in this result matches a binary published in ` +
                 `${row.release_build}, so it compares directly with other ` +
                 `${row.release_build} runs — the digest is the run's own ` +
                 `word, the release is the newest one publishing those bytes`;
    return mark;
  }
  return null;
}

// The legend under the board. Written from what this hub actually holds, for one
// reason: the old one was three fixed sentences, two of which described marks no
// run here had ever earned, and it led with them. A reader whose every row said
// "claims a release build" was being told, at length, about two better things
// they could not have -- so the ordinary case read as a failing grade when it is
// in fact how the board is meant to be used.
//
// So the badges are explained only where some row carries one, and otherwise the
// space goes to what earning one takes. The release name comes from the hub, not
// from a tag typed into the HTML.
function renderBoardLegend() {
  const box = $("#board-legend");
  if (!box) return;
  box.replaceChildren();
  const p = el("p", "note");
  const say = (text) => p.append(el("span", null, text));
  const badge = (tier) => p.append(
    el("span", TRUST[tier].cls, TRUST[tier].label));

  // Present tense only for marks that are on the board right now. `known` is
  // whether /api/stats answered at all: with no counts in hand the legend still
  // has to read as a sentence, but it must not claim a number it never saw.
  const known = !!state.signed;
  const signed = state.signed || {};
  const ci = signed.ci || 0;
  const attested = signed.attested || 0;

  if (ci) {
    badge("ci");
    say(" GitHub signed these exact numbers, measured on its own runner — " +
        "nothing in that chain belongs to the submitter. ");
  }
  if (attested) {
    badge("attested");
    say(" the same signed workflow on the submitter's own machine: the " +
        "signatures hold, the hardware is theirs. ");
  }

  const rel = state.release && state.release.release;
  if (rel) {
    p.append(el("span", "claim", `${rel} build`));
    say(` the run reports one of the binaries ${rel} published, matched by ` +
        `digest — which is what makes two results comparable. `);
  }
  // "Everything else" needs something to be else than, so it is only said where
  // a mark was actually explained above it.
  say(ci || attested
      ? "Everything else is self-reported: the normal case, and it ranks " +
        "exactly the same."
      : "Every result here is self-reported: the normal case, and they all " +
        "rank exactly the same.");

  box.append(p);

  // What a signature costs, said once, where it is relevant -- and only when
  // there is none to point at, so it reads as an invitation rather than as a
  // standing complaint about everyone's rows.
  if (!ci && !attested) {
    const how = el("p", "note");
    how.append(el("span", null,
      (known ? "No run here carries an outside signature yet. It takes the "
             : "A mark takes the ") +
      "measure workflow: GitHub signs the result bytes it produced, on its runners or " +
      "on your own. A digest inside a document you wrote cannot substitute, " +
      "however honest the run behind it — see "));
    // A button, not an anchor: the tabs are not hash-routed, so a link to
    // "#upload" would set a hash that syncHash() then wipes and switch nothing.
    const go = el("button", "linkish", "Submit a result");
    go.type = "button";
    go.addEventListener("click", () => showTab("upload"));
    how.append(go, el("span", null, "."));
    box.append(how);
  }
}

function filterBySubmitter(name) {
  state.submitter = name || "";
  showTab("board");
  syncHash();
  loadBoard().catch((e) => alert(e.message));
}

function renderSubmitterChip() {
  const box = $("#board-chip");
  box.replaceChildren();
  box.hidden = !state.submitter;
  if (!state.submitter) return;
  box.append(el("span", null, `Showing uploads by ${state.submitter}`));
  const clear = el("button", "linkish", "show everyone");
  clear.type = "button";
  clear.addEventListener("click", () => filterBySubmitter(""));
  box.append(clear);
}

function renderBoard() {
  renderSubmitterChip();
  const table = $("#board-table");
  const thead = table.tHead;
  const tbody = table.tBodies[0];
  thead.replaceChildren();
  tbody.replaceChildren();
  $("#board-empty").hidden = state.rows.length > 0;
  const box = $("#board-pager");
  box.replaceChildren();
  pager(box, { offset: state.offset, limit: state.limit, total: state.total },
        goToPage);

  const hr = el("tr");
  hr.append(el("th", "num", "#"), el("th", "", ""), el("th", "wide", "machine"),
            el("th", "", "arch"), el("th", "", "build"),
            sortHeader("MHz", "", "mhz"));
  for (const m of shownMetrics()) hr.append(sortHeader(m.label, unitOf(m), m.key));
  thead.append(hr);

  state.rows.forEach((row, i) => {
    const open = state.expanded.has(row.run_id);
    const tr = el("tr", "run-row");
    // Its place in the whole ranking: on page three of a hundred-row board the
    // top row is 201st, and numbering it 1 would say the opposite.
    tr.append(el("td", "num", state.offset + i + 1), selectBox(row));

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
    const mark = trustBadge(row);
    if (mark) head.append(mark);
    name.append(head);
    if (row.label) name.append(el("span", "label", row.label));
    // Who uploaded it, when anyone signed in did. Clicking narrows the board to
    // that account -- one machine's owner usually has several, and reading them
    // together is the reason to have accounts at all.
    if (row.user) {
      const by = el("button", "by", `by ${row.user}`);
      by.type = "button";
      by.title = `show only ${row.user}'s uploads`;
      by.addEventListener("click", () => filterBySubmitter(row.user));
      name.append(by);
    }
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
  // Per GHz a run is stood for by its most efficient record rather than its
  // fastest, so the pick has to be asked for the same way the board asked.
  p.set("norm", state.norm);
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

// Keep what the page is showing in the URL, so a comparison or one
// submitter's board can be shared as a link.
function syncHash() {
  const parts = [];
  if (state.submitter) parts.push(`user=${state.submitter}`);
  const ids = [...state.selected.keys()];
  if (ids.length) parts.push(`compare=${ids.join(",")}`);
  const want = parts.length ? "#" + parts.join("&") : "";
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

  const title = el("h2", null, machineName(run, `run ${run.id}`));
  const mark = trustBadge(run);
  if (mark) title.append(mark);
  box.append(title);
  if (run.label) box.append(el("p", "label big", run.label));
  if (run.notes) box.append(el("p", "notes", run.notes));

  // What identifies the run, then everything that qualifies it. The second set
  // is what you go looking for once a number surprises you, not what you read
  // on the way in, so it starts folded.
  box.append(metaList([
    ["uploaded", run.created_at],
    ["submitted by", run.user || "anonymous"],
    ["system", [run.sysname, run.os_release, run.machine].filter(Boolean).join(" ")],
  ]));

  const rest = metaList([
    ["mode", run.mode],
    ["arch", run.target],
    ["build", `${run.compiler || "?"}${run.compiler_version && run.compiler_version !== run.compiler ? ` (${run.compiler_version})` : ""}` +
              ` · ${run.vectorize ? "vectorize on" : "vectorize off"} · ${run.fma ? "FMA on" : "FMA off"}`],
    ["flags", run.build_flags ? `${run.cc || "cc"} ${run.build_flags}` : null],
    // What the run says about itself, then what someone else signed about it.
    // Kept as two lines because they are two different kinds of statement.
    ["binary", run.binary_sha256
      ? `sha256:${run.binary_sha256}` +
        (run.release_build
          ? ` — matches a ${run.release_build} binary, as reported by the run`
          : " — not a published build, or a local one")
      : null],
    ["attested", run.attest_tier
      ? `${run.attest_tier === "ci" ? "GitHub-hosted runner" : "self-hosted runner"}` +
        ` · ${run.attest_workflow || ""}` +
        (run.attest_repo ? ` · called from ${run.attest_repo}` : "")
      : null],
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
  dl.setAttribute("download", `cpcpub-run-${run.id}.json`);
  const dlp = el("p");
  dlp.append(dl);
  // The public log of the job that produced it. The signature is the proof;
  // this is how a reader goes and looks at what was signed.
  if (run.attest_run_url) {
    const job = el("a", "linkish", "the CI run that produced it");
    job.href = run.attest_run_url;
    job.rel = "noopener noreferrer";
    job.target = "_blank";
    dlp.append(el("span", "muted", " · "), job);
  }
  box.append(dlp);

  $("#detail").hidden = false;
}

// ---------------------------------------------------------------------------
// The registration puzzle
// ---------------------------------------------------------------------------
//
// Creating an account costs a small proof of work, so that a script cannot
// take every name on the hub overnight. The page has to hash to solve it, and
// crypto.subtle is unavailable on a hub served over plain http -- it needs a
// secure context, which http://a-machine-on-my-network:8782 is not. So SHA-256
// is here, in about forty lines, and works wherever the page loads.

const SHA_K = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
  0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
  0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
  0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
  0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
  0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

const rotr = (x, n) => ((x >>> n) | (x << (32 - n))) >>> 0;

// The digest of `bytes`, as eight 32-bit words. Words rather than bytes
// because the only question asked of it is how many leading zero bits it has.
function sha256(bytes) {
  const h = new Uint32Array([
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
  const total = ((bytes.length + 9 + 63) >> 6) << 6;
  const buf = new Uint8Array(total);
  buf.set(bytes);
  buf[bytes.length] = 0x80;
  const view = new DataView(buf.buffer);
  view.setUint32(total - 8, Math.floor((bytes.length * 8) / 4294967296));
  view.setUint32(total - 4, (bytes.length * 8) >>> 0);

  const w = new Uint32Array(64);
  for (let off = 0; off < total; off += 64) {
    for (let i = 0; i < 16; i++) w[i] = view.getUint32(off + i * 4);
    for (let i = 16; i < 64; i++) {
      const a = w[i - 15], b = w[i - 2];
      const s0 = rotr(a, 7) ^ rotr(a, 18) ^ (a >>> 3);
      const s1 = rotr(b, 17) ^ rotr(b, 19) ^ (b >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
    }
    let [a, b, c, d, e, f, g, hh] = h;
    for (let i = 0; i < 64; i++) {
      const t1 = (hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) +
                  ((e & f) ^ (~e & g)) + SHA_K[i] + w[i]) >>> 0;
      const t2 = ((rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) +
                  ((a & b) ^ (a & c) ^ (b & c))) >>> 0;
      hh = g; g = f; f = e; e = (d + t1) >>> 0;
      d = c; c = b; b = a; a = (t1 + t2) >>> 0;
    }
    const next = [a, b, c, d, e, f, g, hh];
    for (let i = 0; i < 8; i++) h[i] = (h[i] + next[i]) >>> 0;
  }
  return h;
}

function leadingZeros(words, bits) {
  let i = 0, left = bits;
  while (left >= 32) {
    if (words[i++] !== 0) return false;
    left -= 32;
  }
  return left === 0 || (words[i] >>> (32 - left)) === 0;
}

// Find a nonce the hub will accept, without freezing the tab: a slice of
// attempts, then back to the event loop so the progress line paints and the
// page stays answerable.
async function solveChallenge(challenge, bits, progress) {
  const enc = new TextEncoder();
  const limit = 1 << 26;                  // ~67M: far past any sane difficulty
  for (let n = 0; n < limit;) {
    for (let i = 0; i < 4096; i++, n++) {
      if (leadingZeros(sha256(enc.encode(`${challenge}:${n}`)), bits)) {
        return String(n);
      }
    }
    if (progress) progress(n);
    await new Promise((done) => setTimeout(done, 0));
  }
  throw new Error("could not solve the registration challenge");
}

// ---------------------------------------------------------------------------
// Accounts
// ---------------------------------------------------------------------------
//
// An account does three things and claims nothing else: it puts a name on a
// run, it lets that run be withdrawn from any browser rather than only from
// the one holding a delete token, and it carries an upload token so the
// machine that ran the benchmark can post its own result. Uploading without
// one still works everywhere it did.

async function loadUser() {
  try {
    const { user } = await api("/api/auth/me");
    state.user = user;
  } catch {
    state.user = null;          // a hub that has not been signed into is fine
  }
  renderWhoami();
  renderAccount();
  renderCommands();
  renderMine();
}

function renderWhoami() {
  const box = $("#whoami");
  box.replaceChildren();
  if (state.user) {
    const me = el("button", "linkish", state.user.name);
    me.type = "button";
    me.title = "your account";
    me.addEventListener("click", () => showTab("account"));
    box.append(el("span", "muted", "signed in as "), me);
    const out = el("button", "linkish", "sign out");
    out.type = "button";
    out.addEventListener("click", () => signOut().catch((e) => alert(e.message)));
    box.append(el("span", "muted", " · "), out);
    return;
  }
  const go = el("button", "linkish", "Sign in or create an account");
  go.type = "button";
  go.addEventListener("click", () => showTab("account"));
  box.append(go);
  box.append(el("span", "muted", " — optional; uploading works without one"));
}

async function signOut() {
  await api("/api/auth/logout", { method: "POST" });
  await loadUser();
  await loadBoard();
}

// One form for both doors. Registering and signing in ask for exactly the same
// two things, so they are the same fields with a different button rather than
// two forms side by side that a reader has to tell apart.
function authForm() {
  const wrap = el("div", "panel");
  wrap.append(el("h2", null, "Sign in"));
  wrap.append(el("p", "note",
    "An account is a name and a password — no email is asked for, and so " +
    "there is no password reset. It puts your name on the runs you upload, " +
    "lets you withdraw them from any browser, and gives you an upload token " +
    "for submitting straight from the machine you measured."));

  const form = el("form", "authform");
  const name = el("input");
  name.type = "text";
  name.id = "auth-name";
  name.autocomplete = "username";
  name.maxLength = 32;
  name.placeholder = "3–32 characters: letters, digits, . _ -";
  const nameLabel = el("label", null, "Name");
  nameLabel.append(name);

  const pw = el("input");
  pw.type = "password";
  pw.id = "auth-password";
  pw.autocomplete = "current-password";
  pw.placeholder = "at least 8 characters";
  const pwLabel = el("label", null, "Password");
  pwLabel.append(pw);

  const row = el("div", "buttons");
  const signin = el("button", null, "Sign in");
  signin.type = "submit";
  const register = el("button", "secondary", "Create account");
  register.type = "button";
  row.append(signin, register);

  // Only drawn once the hub says it wants one. Most do not.
  const invite = el("input");
  invite.type = "text";
  invite.id = "auth-invite";
  invite.maxLength = 128;
  const inviteLabel = el("label", null, "Invite code");
  inviteLabel.append(invite);
  inviteLabel.hidden = true;

  const msg = el("p", "authmsg");
  const signedIn = async (out) => {
    state.user = out.user;
    pw.value = "";
    renderWhoami();
    renderAccount();
    renderCommands();
    renderMine();
    await loadBoard();
  };
  const say = (text, warn) => {
    msg.className = warn ? "authmsg warn" : "authmsg";
    msg.textContent = text;
  };

  const signIn = async () => {
    say("");
    try {
      await signedIn(await api("/api/auth/login", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ name: name.value.trim(), password: pw.value }),
      }));
    } catch (e) {
      say(e.message, true);
    }
  };

  // Registering is the same two fields plus a puzzle: ask the hub for one,
  // solve it here, and send the answer with the form. The challenge is spent
  // whatever the outcome, so each attempt fetches its own.
  const createAccount = async () => {
    say("");
    register.disabled = true;
    try {
      const policy = await api("/api/auth/challenge");
      if (!policy.open) throw new Error("this hub is not taking new accounts");
      const body = { name: name.value.trim(), password: pw.value };
      if (policy.invite_required) body.invite = invite.value.trim();
      if (policy.bits > 0) {
        say("proving you are not a script…");
        body.nonce = await solveChallenge(
          policy.challenge, policy.bits,
          (n) => say(`proving you are not a script… ${(n / 1000) | 0}k tries`));
        body.challenge = policy.challenge;
      }
      await signedIn(await api("/api/auth/register", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      }));
      say("");
    } catch (e) {
      say(e.message, true);
    } finally {
      register.disabled = false;
    }
  };

  form.addEventListener("submit", (e) => { e.preventDefault(); signIn(); });
  register.addEventListener("click", createAccount);

  form.append(nameLabel, pwLabel, inviteLabel, row, msg);
  wrap.append(form);

  // What this hub asks of a new account, filled in when it answers. A closed
  // hub says so on the button rather than on the failure.
  //
  // The policy rather than a challenge: this runs for every visitor who draws
  // the form, and most of them are here to read the board. A puzzle fetched
  // here would be one nobody solves -- createAccount asks for its own -- and
  // paid for out of the same limit that registering needs.
  api("/api/auth/policy").then((policy) => {
    inviteLabel.hidden = !policy.invite_required;
    if (!policy.open) {
      register.disabled = true;
      register.title = "this hub is not taking new accounts";
      wrap.append(el("p", "note",
        "This hub is not taking new accounts. Uploading without one still " +
        "works and always will."));
    }
  }).catch(() => {});
  return wrap;
}

function accountPanel() {
  const u = state.user;
  const wrap = el("div", "panel");
  wrap.append(el("h2", null, u.name));
  wrap.append(el("p", "note",
    `Account since ${(u.created_at || "").slice(0, 10)} · ` +
    `${u.runs} run${u.runs === 1 ? "" : "s"} uploaded.`));

  const seeMine = el("button", "linkish", "See them on the leaderboard");
  seeMine.type = "button";
  seeMine.addEventListener("click", () => filterBySubmitter(u.name));
  const p = el("p");
  p.append(seeMine);
  wrap.append(p);

  wrap.append(el("h3", null, "Upload token"));
  wrap.append(el("p", "note",
    "Send this from the machine that ran the benchmark and the result lands " +
    "on this account with nothing to copy back. It grants uploading and " +
    "withdrawing as you and nothing else — it cannot change your password or " +
    "read anything the board does not already show."));
  wrap.append(el("code", "token", u.api_token));

  const rotate = el("button", "danger", "Issue a new token");
  rotate.type = "button";
  rotate.addEventListener("click", async () => {
    if (!confirm("Issue a new token? Anything still using the old one will " +
                 "start being refused.")) return;
    try {
      const out = await api("/api/auth/token", { method: "POST" });
      state.user.api_token = out.api_token;
      renderAccount();
      renderCommands();
    } catch (e) {
      alert(e.message);
    }
  });
  const rp = el("p");
  rp.append(rotate);
  wrap.append(rp);

  wrap.append(el("h3", null, "Change password"));
  const pwForm = el("form", "authform");
  const cur = el("input");
  cur.type = "password";
  cur.autocomplete = "current-password";
  const curLabel = el("label", null, "Current password");
  curLabel.append(cur);
  const next = el("input");
  next.type = "password";
  next.autocomplete = "new-password";
  next.placeholder = "at least 8 characters";
  const nextLabel = el("label", null, "New password");
  nextLabel.append(next);
  const pwMsg = el("p", "authmsg");
  const pwGo = el("button", null, "Change password");
  pwGo.type = "submit";
  pwForm.addEventListener("submit", async (e) => {
    e.preventDefault();
    pwMsg.className = "authmsg";
    try {
      await api("/api/auth/password", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ current: cur.value, password: next.value }),
      });
      cur.value = next.value = "";
      pwMsg.className = "authmsg ok";
      pwMsg.textContent = "Password changed. Other browsers have been signed out.";
    } catch (err) {
      pwMsg.className = "authmsg warn";
      pwMsg.textContent = err.message;
    }
  });
  pwForm.append(curLabel, nextLabel, pwGo, pwMsg);
  wrap.append(pwForm);

  const danger = el("details", "more");
  danger.append(el("summary", null, "Close this account"));
  danger.append(el("p", "note",
    "Closing the account deletes every run uploaded under it — they are not " +
    "left on the board without a name, because a run nobody can withdraw is " +
    "worse than no run. This cannot be undone."));
  const closeForm = el("form", "authform");
  const closePw = el("input");
  closePw.type = "password";
  closePw.autocomplete = "current-password";
  const closeLabel = el("label", null, "Password");
  closeLabel.append(closePw);
  const closeGo = el("button", "danger", "Close account and delete my runs");
  closeGo.type = "submit";
  const closeMsg = el("p", "authmsg");
  closeForm.addEventListener("submit", async (e) => {
    e.preventDefault();
    if (!confirm(`Delete the account "${u.name}" and all ${u.runs} of its runs?`)) return;
    try {
      const out = await api("/api/auth/close", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ password: closePw.value }),
      });
      state.user = null;
      state.submitter = "";
      renderWhoami();
      renderAccount();
      renderCommands();
      renderMine();
      await loadBoard();
      alert(`Account closed. ${out.runs_deleted} run(s) removed.`);
    } catch (err) {
      closeMsg.className = "authmsg warn";
      closeMsg.textContent = err.message;
    }
  });
  closeForm.append(closeLabel, closeGo, closeMsg);
  danger.append(closeForm);
  wrap.append(danger);
  return wrap;
}

function renderAccount() {
  const box = $("#account-body");
  box.replaceChildren();
  box.append(state.user ? accountPanel() : authForm());
}

// ---------------------------------------------------------------------------
// Submitting
// ---------------------------------------------------------------------------

// The two shell blocks on the Submit tab. Both carry this hub's address, and
// both carry the token when there is one to carry -- a command you have to
// edit before it works is a command most people will get wrong once.
// What a mark actually costs, said where someone is about to produce a result
// rather than only where they read one. The honest version: uploading from here
// cannot earn one, because nothing about a file you hand a web page is
// checkable. Saying so is better than implying the badge is within reach.
function renderVerifiedNote() {
  const box = $("#verified-note");
  box.replaceChildren();
  const line = (...kids) => { const p = el("p", "note"); p.append(...kids); return p; };

  box.append(line(
    el("span", null, "Uploading a file — from here or from a shell — is "),
    el("strong", null, "self-reported"),
    el("span", null, ". That is the normal way to use this board and the " +
                     "numbers rank the same. It simply cannot be checked: a " +
                     "digest inside a document you wrote is a claim about " +
                     "which binary you ran, not evidence of it.")));

  // The badge text comes from TRUST, so this paragraph cannot drift out of step
  // with what the board actually prints on a row.
  const mark = (tier) => el("span", TRUST[tier].cls, TRUST[tier].label);
  const how = el("p", "note");
  how.append(el("span", null, "For either mark, the measurement has to be " +
    "signed by someone who is not you: run the measure workflow from your own " +
    "repository and GitHub signs the exact result bytes it produced. On its " +
    "own runners that is "));
  how.append(mark("ci"));
  how.append(el("span", null, "; on a self-hosted runner — your own hardware, " +
    "which is the only way an interesting machine gets a mark — it is "));
  how.append(mark("attested"));
  how.append(el("span", null, ", because the signatures hold but the machine is yours."));
  box.append(how);

  if (state.release) {
    const p = el("p", "note");
    p.append(el("span", null, "Published binaries and the workflow to call: "));
    const link = el("a", "linkish", state.release.release);
    if (state.release.url) {
      link.href = state.release.url;
      link.rel = "noopener noreferrer";
      link.target = "_blank";
    }
    p.append(link, el("span", null, "."));
    box.append(p);
  }
}

function renderCommands() {
  const token = state.user && state.user.api_token;
  $("#cmd-submit").textContent =
    "make\n" +
    `bench/cpcpub --full --submit ${location.origin}` +
    (token ? ` --token ${token}` : "") + " --label \"my box\"";

  $("#shell-note").textContent = token
    ? "This carries your upload token, so the run lands on your account. The " +
      "token identifies you: $CPCPUB_TOKEN keeps it out of shell history and " +
      "out of the process list on a shared machine."
    : "Sign in first and this carries your upload token, so the result lands " +
      "on your account. Without one it still works — the run goes up " +
      "anonymously and the reply carries a delete token to keep.";

  $("#cmd-curl").textContent =
    `curl -fsS -X POST "${location.origin}/api/runs?label=my+box" \\\n` +
    (token ? `     -H "Authorization: Bearer ${token}" \\\n` : "") +
    "     -H 'Content-Type: application/json' --data-binary @run.json";
}

// What was dropped or chosen, before anything is sent. Parsed here so a file
// that is not a result says so immediately rather than after a round trip, and
// so a --variants array can be seen for what it is: several runs in one file.
function takeFiles(files) {
  const out = $("#uploadresult");
  out.replaceChildren();
  state.files = [];
  const jobs = [...files].map(async (f) => {
    const text = await f.text();
    let doc;
    try {
      doc = JSON.parse(text);
    } catch (e) {
      return { name: f.name, error: `not JSON: ${e.message}` };
    }
    return { name: f.name, docs: Array.isArray(doc) ? doc : [doc] };
  });
  return Promise.all(jobs).then((picked) => {
    state.files = picked;
    renderPicked();
  });
}

// One document per upload, in the order they will be sent, each with the label
// it will carry. A --variants run is an array of four builds; the hub keys a
// run on the two build flags, so they go up as four runs -- which is what makes
// them comparable with each other at all.
function pending() {
  const base = $("#label").value.trim();
  const out = [];
  for (const f of state.files) {
    if (!f.docs) continue;
    const many = f.docs.length > 1;
    f.docs.forEach((doc, i) => {
      const build = (doc && doc.build) || {};
      const variant = `${build.vectorize ? "vector" : "scalar"}-${build.fma ? "fma" : "nofma"}`;
      out.push({
        doc,
        source: f.name,
        variant: many ? variant : null,
        label: many ? `${base ? base + " " : ""}(${variant})` : base,
        machine: (doc && doc.system && doc.system.cpu_models) || f.name,
      });
    });
  }
  return out;
}

function renderPicked() {
  const box = $("#picked");
  box.replaceChildren();
  const errors = state.files.filter((f) => f.error);
  const jobs = pending();
  box.hidden = !state.files.length;
  for (const e of errors) {
    box.append(el("p", "warn", `${e.name}: ${e.error}`));
  }
  if (jobs.length) {
    box.append(el("p", "ok", jobs.length === 1
      ? `Ready: ${jobs[0].machine}`
      : `Ready: ${jobs.length} runs, uploaded one after another.`));
    const ul = el("ul", "picked-list");
    for (const j of jobs) {
      const li = el("li", null, j.machine);
      if (j.variant) li.append(el("span", "muted", ` · ${j.variant}`));
      ul.append(li);
    }
    box.append(ul);
    if (jobs.length > 1) {
      box.append(el("p", "note",
        "That file holds one document per build variant. Each goes up as its " +
        "own run, because results are only comparable between builds with " +
        "matching vectorize and FMA flags."));
    }
  }
  $("#uploadgo").textContent = jobs.length > 1 ? `Upload ${jobs.length} runs` : "Upload";
}

// Everything chosen, in order, reporting each as it lands. Sequential rather
// than parallel: the rate limiter counts uploads, and a four-variant run that
// half-succeeded because four requests raced is a confusing thing to explain.
async function doUpload(ev) {
  ev.preventDefault();
  const out = $("#uploadresult");
  out.replaceChildren();

  let jobs = pending();
  const pasted = $("#paste").value.trim();
  if (!jobs.length && pasted) {
    let doc;
    try {
      doc = JSON.parse(pasted);
    } catch (e) {
      out.append(el("p", "warn", `That is not JSON: ${e.message}`));
      return;
    }
    state.files = [{ name: "pasted", docs: Array.isArray(doc) ? doc : [doc] }];
    jobs = pending();
  }
  if (!jobs.length) {
    out.append(el("p", "warn", "Drop a result file, choose one, or paste the JSON first."));
    return;
  }

  const notes = $("#notes").value.trim();
  const go = $("#uploadgo");
  go.disabled = true;
  let stored = 0;
  for (const job of jobs) {
    const p = new URLSearchParams();
    if (job.label) p.set("label", job.label);
    if (notes) p.set("notes", notes);
    try {
      const res = await api("/api/runs?" + p.toString(), {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(job.doc),
      });
      stored++;
      out.append(uploadReceipt(job, res));
      if (!res.user) {
        rememberRun({ id: res.id, token: res.delete_token, name: job.machine,
                      at: new Date().toISOString().slice(0, 16).replace("T", " ") });
      }
    } catch (e) {
      out.append(el("p", "warn", `${job.machine}: ${e.message}`));
    }
  }
  go.disabled = false;
  if (stored) {
    state.files = [];
    $("#paste").value = "";
    $("#file").value = "";
    renderPicked();
    $("#picked").hidden = true;
    if (state.user) await loadUser();
    loadBoard().catch(() => {});
  }
}

// What one stored run says back. On an account that is a link and nothing to
// keep; anonymous it is a link and a token that is the only way back, so the
// token is shown as prominently as the success.
function uploadReceipt(job, res) {
  const card = el("div", "receipt");
  card.append(el("p", "ok",
    `Stored run ${res.id}${job.variant ? ` (${job.variant})` : ""} — one ` +
    `leaderboard row holding all ${res.records} records.`));
  const line = el("p");
  const link = el("button", "linkish", "See where it lands");
  link.type = "button";
  link.addEventListener("click", () => showRun(res.id));
  line.append(link);
  card.append(line);
  if (res.user) {
    card.append(el("p", "muted",
      `On your account as ${res.user}. Withdraw it any time from My uploads, ` +
      `in this browser or another.`));
  } else {
    card.append(el("p", "muted",
      "Uploaded anonymously. This delete token is the only way to withdraw " +
      "it — it is saved in this browser, under My uploads:"));
    card.append(el("code", "token", res.delete_token));
  }
  return card;
}

// ---------------------------------------------------------------------------
// My uploads
// ---------------------------------------------------------------------------

function myRuns() {
  try {
    return JSON.parse(localStorage.getItem("cpcpub-uploads") || "[]");
  } catch {
    return [];
  }
}

function rememberRun(entry) {
  const all = myRuns();
  all.unshift(entry);
  localStorage.setItem("cpcpub-uploads", JSON.stringify(all.slice(0, 100)));
  renderMine();
}

function forgetRun(id) {
  localStorage.setItem("cpcpub-uploads",
    JSON.stringify(myRuns().filter((e) => e.id !== id)));
}

// After anything leaves the board: a withdrawn run may still be ticked into a
// comparison, and a comparison of rows that no longer exist is worse than an
// empty one.
async function afterDelete() {
  state.selected.clear();
  state.scopeNote = "";
  renderSelection();
  // loadUser() redraws My uploads itself, so signed in this is one redraw and
  // not two -- two of them racing is what used to list every upload twice.
  if (state.user) await loadUser();
  else renderMine();
  // The board is being refreshed, not re-asked: whoever was reading page four
  // of it stays on page four (or on the last one, if that page has just gone).
  loadBoard({ keepPage: true }).catch(() => {});
}

// Which rows are ticked in each half, by run id. Held outside the lists so a
// redraw -- a refetch finishing, another withdrawal landing -- keeps the ticks
// that were made before it.
const minePicked = { account: new Set(), local: new Set() };

function runCard(entry, picked, onPick) {
  const card = el("div", "mine-card");
  const cb = el("input");
  cb.type = "checkbox";
  cb.className = "pick";
  cb.checked = picked.has(entry.id);
  cb.setAttribute("aria-label", `select run ${entry.id}`);
  cb.addEventListener("change", () => onPick(cb.checked));
  card.append(cb, entry.title, el("span", "muted", entry.at || ""));
  if (entry.token) card.append(el("code", "token", entry.token));
  return card;
}

// Withdrawing whatever is ticked. One confirmation for the lot and then one
// request each -- there is no batch endpoint, and a run that will not go
// (someone else withdrew it, a token no longer matches) has to be named rather
// than folded into one word about all of them.
async function withdrawPicked(entries, picked, remove, redraw) {
  const chosen = entries.filter((e) => picked.has(e.id));
  // Both ways out redraw: the button was disabled on the way in, and a list
  // left with a dead button after a cancelled confirmation is worse than the
  // wasted redraw.
  if (!chosen.length) return redraw();
  if (!confirm(chosen.length === 1
      ? `Withdraw run ${chosen[0].id} from the leaderboard?`
      : `Withdraw these ${chosen.length} runs from the leaderboard?`)) {
    return redraw();
  }
  const failed = [];
  for (const e of chosen) {
    try {
      await remove(e);
      picked.delete(e.id);
      forgetRun(e.id);
    } catch (err) {
      failed.push(`run ${e.id}: ${err.message}`);
    }
  }
  if (failed.length) {
    alert(`${chosen.length - failed.length} of ${chosen.length} withdrawn. ` +
          `Could not withdraw:\n${failed.join("\n")}`);
  }
  redraw();
  await afterDelete();
}

// One half of My uploads: a bar that acts on whatever is ticked, then a card
// per upload. Both halves are the same list with a different way of proving
// the run is yours, so one function draws them both.
function renderRunList(box, entries, picked, remove, redraw) {
  const ids = new Set(entries.map((e) => e.id));
  // Ticks for rows that are no longer here -- withdrawn in another tab, or off
  // the end of a smaller page -- would otherwise be counted forever.
  for (const id of [...picked]) if (!ids.has(id)) picked.delete(id);

  const bar = el("div", "batch");
  const all = el("label", "check");
  const allBox = el("input");
  allBox.type = "checkbox";
  allBox.checked = entries.length > 0 && picked.size === entries.length;
  allBox.indeterminate = picked.size > 0 && picked.size < entries.length;
  allBox.addEventListener("change", () => {
    picked.clear();
    if (allBox.checked) for (const e of entries) picked.add(e.id);
    redraw();
  });
  all.append(allBox, el("span", null, "Select all"));

  const go = el("button", "danger", picked.size > 1
    ? `Withdraw ${picked.size} results` : "Withdraw selected");
  go.type = "button";
  go.disabled = !picked.size;
  go.addEventListener("click", () => {
    go.disabled = true;                 // no second click while the first runs
    withdrawPicked(entries, picked, remove, redraw).catch((e) => alert(e.message));
  });
  bar.append(all, go);
  box.append(bar);

  for (const e of entries) {
    box.append(runCard(e, picked, (on) => {
      if (on) picked.add(e.id);
      else picked.delete(e.id);
      redraw();
    }));
  }
}

function runTitle(id, name) {
  const title = el("button", "linkish", name || `run ${id}`);
  title.type = "button";
  title.addEventListener("click", () => showRun(id));
  return title;
}

// The account half is fetched, so its drawing is kept apart from its loading:
// ticking a box or withdrawing redraws from the last listing, and only a real
// change asks the server for a new one.
let accountGen = 0;

async function loadAccountRuns() {
  const gen = ++accountGen;             // a reply from an older load is stale
  if (!state.user) {
    state.accountRuns = null;
    state.accountError = "";
    state.mineOffset = 0;
    state.mineTotal = 0;
    minePicked.account.clear();   // whoever signs in next ticked none of these
    return drawAccountRuns();
  }
  try {
    const page = await api(`/api/runs?user=me&limit=${state.mineLimit}` +
                           `&offset=${state.mineOffset}`);
    if (gen !== accountGen) return;     // a newer load is already on its way
    state.accountRuns = page.runs;
    state.mineTotal = page.total ?? page.runs.length;
    state.accountError = "";
    // Withdrawing a whole page leaves this one standing past the end of the
    // listing; step back to where the uploads now stop, and only backwards.
    const back = lastPage(state.mineTotal, state.mineLimit);
    if (!page.runs.length && back < state.mineOffset) {
      state.mineOffset = back;
      return loadAccountRuns();
    }
  } catch (e) {
    if (gen !== accountGen) return;
    state.accountRuns = [];
    state.accountError = e.message;
  }
  drawAccountRuns();
}

function drawAccountRuns() {
  const box = $("#mine-account");
  box.replaceChildren();
  if (!state.user) {
    box.append(el("p", "note",
      "Sign in and your uploads are listed here on the account instead, " +
      "withdrawable from any browser without a token."));
    return;
  }
  box.append(el("h2", null, `On your account (${state.user.name})`));
  if (state.accountError) {
    box.append(el("p", "warn", state.accountError));
    return;
  }
  if (state.accountRuns === null) {
    box.append(el("p", "empty", "Loading…"));
    return;
  }
  if (!state.accountRuns.length) {
    box.append(el("p", "empty", "Nothing uploaded to this account yet."));
    return;
  }
  const entries = state.accountRuns.map((run) => ({
    id: run.id,
    at: run.created_at,
    title: runTitle(run.id, machineName(run, `run ${run.id}`)),
  }));
  renderRunList(box, entries, minePicked.account,
                // Taken out of the listing as it goes rather than waiting for
                // the refetch, so a withdrawn run is never redrawn as present.
                (e) => api(`/api/runs/${e.id}`, { method: "DELETE" }).then(() => {
                  state.accountRuns = state.accountRuns.filter((r) => r.id !== e.id);
                  state.mineTotal = Math.max(0, state.mineTotal - 1);
                }),
                drawAccountRuns);
  pager(box, { offset: state.mineOffset, limit: state.mineLimit,
               total: state.mineTotal },
        (offset) => {
          state.mineOffset = Math.max(0, offset);
          loadAccountRuns().catch(() => {});
        });
}

function drawLocalRuns() {
  const box = $("#minelist");
  box.replaceChildren();
  const all = myRuns();
  if (!all.length) {
    box.append(el("p", "empty", "No anonymous uploads from this browser."));
    return;
  }
  if (state.localOffset >= all.length) {
    state.localOffset = lastPage(all.length, state.mineLimit);
  }
  const shown = all.slice(state.localOffset, state.localOffset + state.mineLimit);
  const entries = shown.map((entry) => ({
    id: entry.id,
    at: entry.at,
    token: entry.token,
    title: runTitle(entry.id, entry.name),
  }));
  renderRunList(box, entries, minePicked.local,
                (e) => api(`/api/runs/${e.id}`, {
                  method: "DELETE",
                  headers: { "X-Delete-Token": e.token },
                }),
                drawLocalRuns);
  pager(box, { offset: state.localOffset, limit: state.mineLimit,
               total: all.length },
        (offset) => {
          state.localOffset = Math.max(0, offset);
          drawLocalRuns();
        });
}

function renderMine() {
  loadAccountRuns().catch(() => {});
  drawLocalRuns();
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------

function setupTabs() {
  for (const b of document.querySelectorAll("#tabs button")) {
    b.addEventListener("click", () => showTab(b.dataset.tab));
  }
}

// How many results each list shows. Read back from local storage before the
// first load, so the board comes up at the size that was chosen last time
// rather than at the default and then jumping.
function setupPageSizes() {
  state.limit = storedPage("cpcpub-page-board", state.limit);
  state.mineLimit = storedPage("cpcpub-page-mine", state.mineLimit);
  fillPageSizes($("#filters").elements.limit, state.limit);
  const mine = $("#mine-limit");
  fillPageSizes(mine, state.mineLimit);
  mine.addEventListener("change", () => {
    state.mineLimit = pageSize(mine.value, state.mineLimit);
    rememberPage("cpcpub-page-mine", state.mineLimit);
    state.mineOffset = 0;
    state.localOffset = 0;
    renderMine();
  });
}

// The drop target is also a file picker and also keyboard-reachable: the same
// step done by dragging, by clicking, or by tabbing to it and pressing enter.
function setupDropzone() {
  const zone = $("#drop");
  const input = $("#file");
  const take = (files) => {
    if (files && files.length) takeFiles(files).catch((e) => alert(e.message));
  };
  zone.addEventListener("click", () => input.click());
  zone.addEventListener("keydown", (e) => {
    if (e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      input.click();
    }
  });
  input.addEventListener("change", () => take(input.files));
  for (const type of ["dragenter", "dragover"]) {
    zone.addEventListener(type, (e) => {
      e.preventDefault();
      zone.classList.add("over");
    });
  }
  for (const type of ["dragleave", "dragend"]) {
    zone.addEventListener(type, () => zone.classList.remove("over"));
  }
  zone.addEventListener("drop", (e) => {
    e.preventDefault();
    zone.classList.remove("over");
    take(e.dataTransfer && e.dataTransfer.files);
  });
  // Dropping anywhere else must not make the browser navigate to the file,
  // which looks exactly like the upload silently failing.
  for (const type of ["dragover", "drop"]) {
    window.addEventListener(type, (e) => {
      if (!zone.contains(e.target)) e.preventDefault();
    });
  }
  // The label is part of what each pending upload will be called, so the
  // preview follows it as it is typed.
  $("#label").addEventListener("input", () => {
    if (state.files.length) renderPicked();
  });
}

async function boot() {
  setupTabs();
  setupDropzone();
  setupPageSizes();
  for (const b of document.querySelectorAll("button.copy")) {
    b.addEventListener("click", () => copyButton(b));
  }
  renderCommands();
  // Once before /api/stats answers, so the board is never explained by a blank
  // gap, and again from the hub's numbers when they arrive.
  renderBoardLegend();
  renderVerifiedNote();

  const meta = await api("/api/metrics");
  state.metrics = meta.metrics;

  // Which release this hub verifies against and how many runs carry a
  // signature, both of which the legend is written from. Not fatal if it fails:
  // the board is readable without it, and renderBoardLegend() has already run
  // once with what little that leaves it.
  api("/api/stats").then((s) => {
    state.release = s.release || null;
    state.signed = s.attested || null;
    renderBoardLegend();
    renderVerifiedNote();
  }).catch(() => {});

  $("#filters").addEventListener("submit", (e) => {
    e.preventDefault();
    // Submitting the form is the search: it is the one filter that waits to be
    // asked for, since a board that reshuffles on every keystroke is unusable.
    state.search = $("#filters").elements.q.value.trim();
    loadBoard().catch((err) => alert(err.message));
  });
  // Picking from a dropdown is the whole gesture -- there is nothing to confirm
  // afterwards, so each one reloads the board itself.
  // `norm` is in here rather than with the redraw-only control below because
  // it is what the board is ranked by as well as what it prints: switching to
  // per GHz reorders the rows, so it has to ask the server again.
  for (const name of ["scope", "target", "vectorize", "fma", "verified",
                      "limit", "norm"]) {
    $("#filters").elements[name].addEventListener("change", () => {
      loadBoard().catch((err) => alert(err.message));
    });
  }
  // This one changes how the rows are drawn, not which rows they are or in
  // what order, so it does not refetch.
  $("#filters").elements.detailed.addEventListener("change", (e) => {
    state.detailed = e.target.checked;
    renderBoard();
    renderSelection();
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

  // Who is signed in decides what the Account tab and the shell commands say,
  // and whether My uploads has an account half. It never blocks the board.
  await loadUser();
  renderSelection();

  // #user=name is a link to one submitter's uploads, so it has to be read
  // before the board is first loaded rather than reloading it afterwards.
  const who = location.hash.match(/user=([A-Za-z0-9._-]{3,32})/);
  if (who) state.submitter = who[1];
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
