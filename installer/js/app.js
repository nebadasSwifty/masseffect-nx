// The page: picks the source, shows the detected edition, runs the pipeline with progress.
import { CONFIG } from '../config.js?v=0.3.3';
import { sourceFromIso, sourceFromFileList, sourceFromDirectoryHandle, sourceFromDataTransfer, inspectDisc, auditPackages, classifyAudit, knownBadFor } from './source.js?v=0.3.3';
import { planGameFiles, formatBytes } from './plan.js';
import { openSink, describeSinkSupport, cleanStaleTemporaryFiles } from './sink.js';
import { run, Cancelled, UserError, stageIds, stagesFor } from './pipeline.js';
import { initLanguage, getLanguage, setLanguage, t } from './i18n.js?v=0.3.3';
import {
  parseProdKeys, forgetKeys, estimateNspBytes, estimateProgramUpdateBytes, parseBaseMetadata, nextUpdateVersion,
  withLastUpdateVersion, pythonJson, inspectBaseNsp, PartsReader,
} from './nsp.js';
import { openNspSink, nspSinkSupport, saveTextFile } from './nsp_sink.js';
import { findPermittedSwitch, requestSwitch, WebUsbTransport, USB_VENDOR_ID, USB_PRODUCT_ID } from './usb_install.js';
import { openStfs, verifyStfs } from './stfs.js';

const $ = (id) => document.getElementById(id);
const el = (tag, props = {}, ...kids) => {
  const e = Object.assign(document.createElement(tag), props);
  for (const k of kids) e.append(k);
  return e;
};

const STAGE_KEYS = {
  download: 'stage_download',
  scan: 'stage_scan',
  translate: 'stage_translate',
  pack: 'stage_pack',
  zip: 'stage_zip',
  nsp_base: 'stage_nsp_base',
  nsp_hash: 'stage_nsp_hash',
  nsp_write: 'stage_nsp_write',
  nsp_usb: 'stage_nsp_usb',
};
// A USB install has three passes; its first two stages say so.
const STAGE_KEYS_USB = { nsp_hash: 'stage_nsp_hash_usb', nsp_write: 'stage_nsp_write_usb' };
const stageName = (id) => t((state.usbRun && STAGE_KEYS_USB[id]) || STAGE_KEYS[id] || id);

// outcome: null | 'ok' | 'err' | 'cancelled' (last run of step 4, drives the step indicator).
const state = {
  format: 'iso', disc: null, running: false, abort: null, dlc: [], dlcBusy: false, outcome: null, stages: stageIds,
  // Installable NSP: keys (only the two the package needs, in memory, dropped on pagehide), the kind, the output
  // form, and the base of an update ({ meta, name, metaName } or { parts, name, metaName, info }).
  nsp: { keys: null, kind: 'full', out: 'file', base: null },
};

/** Localized duration for the page (same rounding as formatDuration in plan.js). */
function duration(seconds) {
  if (!isFinite(seconds) || seconds < 0) return '';
  const s = Math.round(seconds);
  if (s < 60) return t('dur_s', { s });
  const m = Math.floor(s / 60);
  if (m < 60) return t('dur_min', { m, s: String(s % 60).padStart(2, '0') });
  return t('dur_h', { h: Math.floor(m / 60), m: String(m % 60).padStart(2, '0') });
}

// ---- i18n & language toggle ------------------------------------------------------------------------------------
function updateFormatTexts() {
  const f = state.format;
  $('format-hint').textContent = f === 'iso' ? t('format_hint_iso') : t('format_hint_folder');
  $('pick').textContent = f === 'iso' ? t('pick_iso') : t('pick_folder');
  const orSpan = document.querySelector('.or');
  if (orSpan) orSpan.textContent = t('drop_or');
}

function applyLanguage() {
  const lang = getLanguage();
  document.documentElement.lang = lang;
  const btnEn = $('lang-en');
  const btnRu = $('lang-ru');
  if (btnEn) btnEn.classList.toggle('active', lang === 'en');
  if (btnRu) btnRu.classList.toggle('active', lang === 'ru');

  for (const elem of document.querySelectorAll('[data-i18n]')) {
    const key = elem.getAttribute('data-i18n');
    const isHtml = elem.getAttribute('data-i18n-html') === 'true';
    if (isHtml) elem.innerHTML = t(key);
    else elem.textContent = t(key);
  }
  for (const elem of document.querySelectorAll('[data-i18n-alt]')) elem.alt = t(elem.getAttribute('data-i18n-alt'));
  for (const elem of document.querySelectorAll('[data-i18n-aria]')) elem.setAttribute('aria-label', t(elem.getAttribute('data-i18n-aria')));

  updateFormatTexts();
  if (state.disc) {
    showEdition();
  }
  renderDlc();
  renderNsp();
  updateWizard();
  if (state.running) for (const id of state.stages) if (rows[id]) rows[id].name.textContent = stageName(id);
}

// ---- step indicator --------------------------------------------------------------------------------------------
/** Sets pending / active / done / error / optional on the four step columns from the page state. */
function updateWizard() {
  const items = document.querySelectorAll('#wizard .wstep');
  if (!items.length) return;
  const sourceError = $('source-status').classList.contains('err');
  const dlcError = $('dlc-status').classList.contains('err');
  const d = state.disc;
  let s1, s2, s3, s4;
  if (!d) {
    s1 = sourceError ? 'error' : 'active';
    s2 = s3 = s4 = 'pending';
  } else {
    s1 = 'done';
    s2 = 'done';
    s3 = state.dlcBusy ? 'active' : dlcError ? 'error' : state.dlc.length ? 'done' : 'optional';
    s4 = state.running ? 'active' : state.outcome === 'ok' ? 'done' : state.outcome === 'err' ? 'error' : 'active';
    if (state.dlcBusy && s4 === 'active') s4 = 'pending';
  }
  [s1, s2, s3, s4].forEach((st, i) => {
    const li = items[i];
    li.className = `wstep ${st}`;
    if (st === 'active') li.setAttribute('aria-current', 'step');
    else li.removeAttribute('aria-current');
    const sr = li.querySelector('.wstate');
    if (sr) sr.textContent = ` (${t(`step_state_${st}`)})`;
  });
}

initLanguage();
applyLanguage();

const btnEn = $('lang-en');
const btnRu = $('lang-ru');
if (btnEn) btnEn.addEventListener('click', () => { setLanguage('en'); applyLanguage(); });
if (btnRu) btnRu.addEventListener('click', () => { setLanguage('ru'); applyLanguage(); });

// ---- header / footer -------------------------------------------------------------------------------------------
$('repo-link').href = CONFIG.project.repoUrl;
$('author').textContent = CONFIG.project.author;

// ---- environment checks ----------------------------------------------------------------------------------------
function supportsModuleWorkers() {
  try {
    let ok = false;
    const blob = new Blob([''], { type: 'text/javascript' });
    const url = URL.createObjectURL(blob);
    const w = new Worker(url, { get type() { ok = true; return 'module'; } });
    w.terminate();
    URL.revokeObjectURL(url);
    return ok;
  } catch { return false; }
}

function envProblems() {
  const problems = [];
  const warnings = [];
  if (!window.isSecureContext || !crypto?.subtle) problems.push('This page must be opened over https:// (or http://localhost): the browser disables the hashing it needs otherwise.');
  if (typeof Worker === 'undefined' || !supportsModuleWorkers()) problems.push('This browser does not support module Web Workers. Use a current Chrome, Edge, Firefox or Safari.');
  if (typeof WebAssembly === 'undefined') problems.push('This browser has no WebAssembly.');
  const gb = navigator.deviceMemory;
  if (gb && gb < 8) warnings.push(`Your device reports ${gb} GB of memory (browsers cap this value at 8). Packing the shaders needs several GB; use a desktop computer.`);
  const sink = describeSinkSupport();
  if (sink === 'opfs') warnings.push('This browser cannot save straight to a file you choose. The zip is written to temporary storage first and then downloaded, so you need free disk space of about the zip size (several GB).');
  if (sink === 'memory') warnings.push('This browser can neither save to a chosen file nor use temporary file storage: the whole zip (several GB) would be built in memory and will most likely fail. Use a current Chrome or Edge.');
  return { problems, warnings };
}

(function showEnv() {
  const { problems, warnings } = envProblems();
  const box = $('env-warning');
  if (!problems.length && !warnings.length) return;
  box.hidden = false;
  box.className = `notice ${problems.length ? 'err' : 'warn'}`;
  box.replaceChildren(...[...problems, ...warnings].map((text) => el('p', { textContent: text })));
  if (problems.length) {
    for (const id of ['pick', 'file-iso', 'file-folder']) $(id).disabled = true;
  }
})();
cleanStaleTemporaryFiles();

// ---- step 1 ---------------------------------------------------------------------------------------------------
function setFormat(f) {
  state.format = f;
  updateFormatTexts();
  resetDisc();
}

function resetDisc() {
  if (state.running) return;
  state.disc = null;
  $('step-edition').hidden = true;
  $('step-dlc').hidden = true;
  $('step-create').hidden = true;
  $('source-status').textContent = '';
  $('source-status').className = 'status';
  state.outcome = null;
  updateWizard();
}

for (const r of document.querySelectorAll('input[name=format]')) r.addEventListener('change', () => r.checked && setFormat(r.value));

function status(text, isError = false) {
  const s = $('source-status');
  s.textContent = text;
  s.className = isError ? 'status err' : 'status';
  updateWizard();
}

$('pick').addEventListener('click', async () => {
  if (state.format === 'folder' && typeof showDirectoryPicker === 'function') {
    try {
      const handle = await showDirectoryPicker({ mode: 'read' });
      await load(async () => sourceFromDirectoryHandle(handle, (n) => status(t('status_folder_listing', { count: n }))));
    } catch (e) {
      if (e?.name !== 'AbortError') status(t('err_open_folder', { error: e.message }), true);
    }
    return;
  }
  (state.format === 'iso' ? $('file-iso') : $('file-folder')).click();
});

$('file-iso').addEventListener('change', (e) => {
  const files = [...e.target.files];
  e.target.value = '';
  if (files.length) load(() => sourceFromIso(files, isoOptions()));
});

$('file-folder').addEventListener('change', (e) => {
  const list = [...e.target.files];
  e.target.value = '';
  if (list.length) load(async () => sourceFromFileList(list));
});

const drop = $('drop');
for (const ev of ['dragenter', 'dragover']) drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.add('over'); });
for (const ev of ['dragleave', 'drop']) drop.addEventListener(ev, () => drop.classList.remove('over'));
drop.addEventListener('drop', async (e) => {
  e.preventDefault();
  if (state.running) return;
  try {
    const dropped = await sourceFromDataTransfer(e.dataTransfer, (n) => status(t('status_folder_listing', { count: n })));
    if (dropped.files?.length) {
      setRadio('iso');
      load(() => sourceFromIso(dropped.files, isoOptions()));
    } else if (dropped.file) {
      setRadio(dropped.file.name.toLowerCase().endsWith('.iso') || dropped.file.size > 100e6 ? 'iso' : state.format);
      load(() => sourceFromIso(dropped.file, isoOptions()));
    } else {
      setRadio('folder');
      load(async () => dropped.source);
    }
  } catch (err) { status(t('err_drop', { error: err.message }), true); }
});

function setRadio(v) {
  document.querySelector(`input[name=format][value=${v}]`).checked = true;
  if (state.format !== v) {
    setFormat(v);
  }
}

function isoOptions() {
  return { onProgress: (done, total) => status(t('status_merging', { done, total })) };
}

async function load(makeSource) {
  if (state.running) return;
  resetDisc();
  status(t('status_reading'));
  try {
    const source = await makeSource();
    status(t('status_checking'));
    const info = await inspectDisc(source, CONFIG);
    const check = CONFIG.disc.packageCheck ?? { scope: 'all', block: false };
    const packages = await auditPackages(info.files, {
      scope: check.scope, cache: source.checks, known: knownBadFor(check, info.edition?.id),
      onProgress: (done, total) => status(t('status_packages', { done, total })),
    });
    state.disc = { source, ...info, packages };
    status(`${source.label}: ${info.files.length.toLocaleString('en-US')} files (${source.detail}).`);
    showEdition();
  } catch (e) {
    status(e.message || String(e), true);
  }
}

// ---- step 2 / 3 -----------------------------------------------------------------------------------------------
function showEdition() {
  const d = state.disc;
  const body = $('edition-body');
  $('step-edition').hidden = false;
  const plan = planGameFiles(d.files, CONFIG.disc);
  d.plan = plan;
  if (!d.edition) {
    d.edition = CONFIG.editions[0];
    d.matchType = 'unverified_manual';
  }
  // The known-broken list depends on the edition (it can be changed by hand below).
  if (d.packages) {
    const check = CONFIG.disc.packageCheck ?? {};
    d.packages = classifyAudit(d.packages, { scope: d.packages.scope, known: knownBadFor(check, d.edition.id) });
  }
  const isUnverified = d.matchType !== 'exact';
  const badge = isUnverified
    ? el('span', { className: 'badge warn', textContent: t('badge_unverified') })
    : el('span', { className: 'badge ok', textContent: t('badge_supported') });

  const editionSelect = el('select', { className: 'edition-picker', id: 'edition-picker' });
  for (const ed of CONFIG.editions) {
    const opt = el('option', { value: ed.id, textContent: ed.name });
    if (ed.id === d.edition.id) opt.selected = true;
    editionSelect.append(opt);
  }
  editionSelect.addEventListener('change', (e) => {
    const found = CONFIG.editions.find((ed) => ed.id === e.target.value);
    if (found) {
      d.edition = found;
      const exactMatch = Array.isArray(found.xexSha256)
        ? found.xexSha256.some((x) => x.toLowerCase() === d.sha256.toLowerCase())
        : (found.xexSha256 && found.xexSha256.toLowerCase() === d.sha256.toLowerCase());
      d.matchType = exactMatch ? 'exact' : 'unverified_manual';
      showEdition();
    }
  });

  const elements = [
    el('dl', { className: 'ed' },
      el('dt', { textContent: t('edition_label') }),
      el('dd', {},
        el('strong', { textContent: d.edition.name }), ' ', badge,
        el('div', { className: 'edition-picker-wrap' },
          el('label', { className: 'edition-picker-label', htmlFor: 'edition-picker', textContent: t('override_edition_label') }),
          editionSelect,
        ),
      ),
      el('dt', { textContent: 'default.xex' }), el('dd', {}, el('code', { textContent: d.sha256 })),
      el('dt', { textContent: t('game_files_label') }), el('dd', { textContent: t('files_summary', { count: plan.copy.length.toLocaleString('en-US'), bytes: formatBytes(plan.copyBytes) }) }),
      el('dt', { textContent: t('left_out_label') }), el('dd', { textContent: t('left_out_summary', { count: plan.skipped.length.toLocaleString('en-US'), bytes: formatBytes(plan.skippedBytes), prefixes: CONFIG.disc.skip.map((s) => s.prefix.replace(/\/$/, '')).join(', ') }) }),
    ),
  ];
  if (isUnverified) {
    elements.push(
      el('div', { className: 'notice warn' },
        el('p', {}, el('strong', { textContent: t('unverified_notice_title', { name: d.edition.name }) })),
        el('p', { textContent: t('unverified_notice_body', { hash: d.sha256 }) }),
      ),
    );
  }
  const blocked = packageWarning(d, elements);
  body.replaceChildren(...elements);
  if (blocked) {
    $('step-dlc').hidden = true;
    $('step-create').hidden = true;
    updateWizard();
    return;
  }
  $('step-dlc').hidden = false;
  renderDlc();
  showCreate();
}

/** Adds the warning about bad or swapped packages (if any) to `elements`. Returns true when creating is blocked. */
function packageWarning(d, elements) {
  const p = d.packages;
  if (p?.known?.length) {
    elements.push(el('div', { className: 'notice' },
      el('p', { textContent: t('pkg_known_body', { count: p.known.length }) }),
      el('ul', {}, ...p.known.map((k) => el('li', {}, el('code', { textContent: k.path }))))));
  }
  if (!p || (!p.bad.length && !p.duplicates.length)) return false;
  const items = [];
  for (const b of p.bad) items.push(`${b.path}: ${b.problems[0]}`);
  for (const g of p.duplicates) {
    for (const path of g) {
      const name = (x) => x.slice(x.lastIndexOf('/') + 1);
      const others = g.filter((x) => name(x).toLowerCase() !== name(path).toLowerCase()).map(name).join(', ');
      items.push(`${path}: ${t('pkg_warn_dup', { others })}`);
    }
  }
  const shown = items.slice(0, 12);
  const advice = d.source.kind === 'folder' ? 'pkg_warn_advice_folder'
    : (d.source.discCount ?? 1) > 1 ? 'pkg_warn_advice_merged' : 'pkg_warn_advice_iso';
  const blocked = Boolean(CONFIG.disc.packageCheck?.block);
  elements.push(
    el('div', { className: 'notice warn' },
      el('p', {}, el('strong', { textContent: t('pkg_warn_title', { count: items.length }) })),
      el('p', { textContent: t('pkg_warn_body') }),
      el('ul', {}, ...shown.map((text) => el('li', {}, el('code', { textContent: text })))),
      ...(items.length > shown.length ? [el('p', { textContent: t('pkg_warn_more', { count: items.length - shown.length }) })] : []),
      el('p', { textContent: t(advice) }),
      el('p', { textContent: t(blocked ? 'pkg_warn_blocked' : 'pkg_warn_continue') }),
    ),
  );
  return blocked;
}

// ---- step 3: optional DLC packages ------------------------------------------------------------------------------
function dlcBytes() {
  return state.dlc.reduce((a, p) => a + p.payloadBytes + p.header.length, 0);
}

function dlcExtraBytes() {
  return state.dlc.length ? dlcBytes() + CONFIG.limits.expectedDlcShaderBytes : 0;
}

function dlcStatus(text, isError = false) {
  const s = $('dlc-status');
  s.textContent = text;
  s.className = isError ? 'status err' : 'status';
  updateWizard();
}

function renderDlc() {
  const list = $('dlc-list');
  if (!list) return;
  list.replaceChildren(...state.dlc.map((p, i) => {
    const remove = el('button', { className: 'btn small', type: 'button', textContent: t('dlc_remove') });
    remove.disabled = state.running || state.dlcBusy;
    remove.addEventListener('click', () => {
      if (state.running || state.dlcBusy) return;
      state.dlc.splice(i, 1);
      renderDlc();
      dlcStatus(state.dlc.length ? t('dlc_added', { count: state.dlc.length, bytes: formatBytes(dlcBytes()) }) : t('dlc_none'));
      if (state.disc) showCreate();
    });
    return el('li', {},
      el('div', { className: 'meta' },
        el('strong', { textContent: p.meta.displayName || p.folderName }),
        el('span', { className: 'muted', textContent: t('dlc_item_detail', { file: p.fileName, files: p.files.length.toLocaleString('en-US'), bytes: formatBytes(p.meta.fileSize) }) })),
      remove);
  }));
  $('dlc-pick').disabled = state.running || state.dlcBusy || envProblems().problems.length > 0;
}

/** Opens, validates and SHA-1 checks each picked file; adds the good ones to state.dlc, reports the others. */
async function addDlc(fileList) {
  if (state.running || state.dlcBusy) return;
  state.dlcBusy = true;
  setButtons(false);
  renderDlc();
  const errors = [];
  try {
    for (const file of fileList) {
      dlcStatus(t('dlc_reading', { name: file.name }));
      try {
        const pkg = await openStfs(file, {
          fileName: file.name,
          expect: { titleId: CONFIG.dlc.titleId, contentType: CONFIG.dlc.contentType },
          licenseMask: CONFIG.dlc.licenseMask,
        });
        if (state.dlc.some((p) => p.meta.contentId === pkg.meta.contentId || p.folderName.toLowerCase() === pkg.folderName.toLowerCase())) {
          errors.push(t('dlc_rejected', { name: file.name, reason: t('dlc_duplicate') }));
          continue;
        }
        await verifyStfs(pkg, {
          onProgress: (done, total) => dlcStatus(t('dlc_verifying', { name: pkg.meta.displayName || file.name, percent: total ? Math.floor((done / total) * 100) : 100 })),
        });
        pkg.fileName = file.name;
        state.dlc.push(pkg);
        renderDlc();
      } catch (e) {
        errors.push(t('dlc_rejected', { name: file.name, reason: e?.message ?? String(e) }));
      }
    }
  } finally {
    state.dlcBusy = false;
    renderDlc();
    const summary = state.dlc.length ? t('dlc_added', { count: state.dlc.length, bytes: formatBytes(dlcBytes()) }) : t('dlc_none');
    dlcStatus(errors.length ? `${errors.join('\n')}\n${summary}` : summary, errors.length > 0);
    if (state.disc) showCreate();
  }
}

$('dlc-pick').addEventListener('click', () => $('file-dlc').click());
$('file-dlc').addEventListener('change', (e) => {
  const files = [...e.target.files];
  e.target.value = '';
  if (files.length) addDlc(files);
});
const dlcDrop = $('dlc-drop');
for (const ev of ['dragenter', 'dragover']) dlcDrop.addEventListener(ev, (e) => { e.preventDefault(); dlcDrop.classList.add('over'); });
for (const ev of ['dragleave', 'drop']) dlcDrop.addEventListener(ev, () => dlcDrop.classList.remove('over'));
dlcDrop.addEventListener('drop', (e) => {
  e.preventDefault();
  const files = [...(e.dataTransfer?.files ?? [])].filter((f) => f.size > 0);
  if (files.length) addDlc(files);
});

function showCreate() {
  const d = state.disc;
  const extra = dlcExtraBytes();
  const fullBytes = d.plan.copyBytes + CONFIG.limits.expectedShaderBytes + CONFIG.limits.expectedNroBytes + extra;
  const updBytes = CONFIG.limits.expectedShaderBytes + CONFIG.limits.expectedNroBytes + extra;
  const info = $('create-info');
  const card = (label, value) => el('div', { className: 'info-card' }, el('span', { textContent: label }), el('b', { textContent: value }));
  const lines = [
    el('div', { className: 'info-cards' },
      card(t('card_game_files'), `${d.plan.copy.length.toLocaleString('en-US')} · ${formatBytes(d.plan.copyBytes)}`),
      card(t('card_dlc'), state.dlc.length ? t('card_dlc_value', { count: state.dlc.length, bytes: formatBytes(dlcBytes()) }) : t('card_dlc_none')),
      card(t('card_full_zip'), `~${formatBytes(fullBytes)}`),
      card(t('card_update_zip'), `~${formatBytes(updBytes)}`)),
    el('p', { className: 'muted', textContent: t('create_note') }),
  ];
  const sink = describeSinkSupport();
  if (sink === 'memory' && fullBytes > CONFIG.limits.blobWarnBytes) {
    lines.push(el('div', { className: 'notice err', textContent: t('memory_warning', { size: formatBytes(fullBytes) }) }));
  }
  info.replaceChildren(...lines);
  $('step-create').hidden = false;
  setButtons(true);
  updateWizard();
}

function setButtons(enabled) {
  const ok = enabled && !state.running && !state.dlcBusy && !envProblems().problems.length;
  $('create-full').disabled = !ok;
  $('create-update').disabled = !ok;
  const n = state.nsp;
  const support = nspSinkSupport();
  $('create-nsp').disabled = !(ok && n.keys && canWriteFull(n.out, support));
  $('create-nsp-update').disabled = !(ok && n.keys && n.base && updateVersion());
  for (const id of ['nsp-keys-pick', 'nsp-base-pick', 'nsp-update-data', 'nsp-update-version', 'nsp-update-usb']) $(id).disabled = state.running;
  for (const r of document.querySelectorAll('input[name=nsp-kind], input[name=nsp-out]')) r.disabled = state.running;
}

// ---- installable NSP (experimental; docs/full-nsp.md) -------------------------------------------------------------
// The keys are read with the browser's file access, never uploaded, stored or logged; only header_key and
// key_area_key_application_00 are kept, in memory, and overwritten on pagehide.
const LAST_UPDATE_KEY = 'masseffect_nsp_last_update';

function setText(id, text, isError = false) {
  const s = $(id);
  s.textContent = text;
  s.classList.toggle('err', isError);
}

/** The last update number made in this browser for a base (title + Program NCA); not sensitive. */
function rememberedUpdate(title, programNca) {
  try { return Number(JSON.parse(localStorage.getItem(LAST_UPDATE_KEY) || '{}')[`${title}:${programNca}`]) || 0; } catch { return 0; }
}
function rememberUpdate(title, programNca, n) {
  try {
    const all = JSON.parse(localStorage.getItem(LAST_UPDATE_KEY) || '{}');
    all[`${title}:${programNca}`] = Math.max(n, Number(all[`${title}:${programNca}`]) || 0);
    localStorage.setItem(LAST_UPDATE_KEY, JSON.stringify(all));
  } catch { /* storage unavailable: the number input still works */ }
}

function nspEstimate() {
  const d = state.disc;
  if (!d?.plan) return null;
  const payload = d.plan.copyBytes + CONFIG.limits.expectedShaderBytes + (state.dlc.length ? dlcBytes() : 0);
  const files = d.plan.copy.length + 4 + state.dlc.reduce((a, p) => a + p.files.length + 1, 0);
  return estimateNspBytes({ payloadBytes: payload, fileCount: files, nroBytes: CONFIG.limits.expectedNroBytes });
}

function updateVersion() {
  const v = Number($('nsp-update-version').value);
  return Number.isInteger(v) && v >= 1 && v <= 0xFFFF ? v : null;
}
function setUpdateVersion(v) { $('nsp-update-version').value = String(Math.min(Math.max(1, v), 0xFFFF)); }
function updateName(v) { return CONFIG.nsp.updateName.replace('{n}', String(v)); }

/** Whether this browser can produce a full NSP for the chosen output ('file', 'split' or 'usb'). */
function canWriteFull(out, support) {
  return out === 'usb' ? support.usb : out === 'split' ? support.split : support.file;
}

function renderNsp() {
  if (!$('nsp-full-panel')) return;
  const n = state.nsp;
  const support = nspSinkSupport();
  $('nsp-full-panel').hidden = n.kind !== 'full';
  $('nsp-update-panel').hidden = n.kind !== 'update';
  const canFull = canWriteFull(n.out, support);
  const est = nspEstimate();
  const usb = n.out === 'usb';
  $('nsp-usb-hint').hidden = !usb;
  const why = usb ? t(support.usbReason === 'insecure' ? 'nsp_usb_insecure' : 'nsp_browser_usb') : t('nsp_browser_full');
  setText('nsp-full-info', canFull ? (est ? t(usb ? 'nsp_usb_estimate' : 'nsp_full_estimate', { size: formatBytes(est) }) : '') : why, !canFull);
  $('nsp-update-usb-row').hidden = !support.usb;
  if (!support.usb) $('nsp-update-usb').checked = false;
  const v = updateVersion();
  $('create-nsp').textContent = t(usb ? 'create_nsp_usb_btn' : 'create_nsp_btn');
  $('create-nsp-update').textContent = t('create_nsp_update_btn', { name: v ? updateName(v) : updateName('N') });
  const lines = [$('nsp-update-data').checked
    ? t('nsp_update_estimate_data')
    : t('nsp_update_estimate', { size: formatBytes(estimateProgramUpdateBytes(CONFIG.limits.expectedNroBytes)) })];
  if (!support.file && !$('nsp-update-usb').checked) lines.push(t('nsp_update_memory'));
  setText('nsp-update-info', lines.join('\n'));
  setButtons(!$('step-create').hidden);
}

for (const r of document.querySelectorAll('input[name=nsp-kind]')) {
  r.addEventListener('change', () => { if (r.checked) { state.nsp.kind = r.value; setText('nsp-status', ''); renderNsp(); } });
}
for (const r of document.querySelectorAll('input[name=nsp-out]')) {
  r.addEventListener('change', () => { if (r.checked) { state.nsp.out = r.value; renderNsp(); } });
}
$('nsp-update-data').addEventListener('change', renderNsp);
$('nsp-update-usb').addEventListener('change', renderNsp);
$('nsp-update-version').addEventListener('input', renderNsp);

/** For a base NSP: its title and Program NCA (from the NCA headers, with the keys) give the next update number. */
async function refreshBaseVersion() {
  const base = state.nsp.base;
  if (!base?.parts || !state.nsp.keys) return;
  try {
    base.info = await inspectBaseNsp(new PartsReader(base.parts), state.nsp.keys);
    setUpdateVersion(nextUpdateVersion(null, rememberedUpdate(base.info.titleId, base.info.programNca)));
  } catch (err) {
    setText('nsp-base-status', t('nsp_base_bad', { file: base.name, reason: err?.message ?? String(err) }), true);
  }
  renderNsp();
}

$('nsp-keys-pick').addEventListener('click', () => $('file-keys').click());
$('file-keys').addEventListener('change', async (e) => {
  const file = e.target.files?.[0];
  e.target.value = '';
  if (!file) return;
  forgetKeys(state.nsp.keys);
  state.nsp.keys = null;
  try {
    if (file.size > 1 << 20) throw new Error('not a key file (too large)');
    state.nsp.keys = parseProdKeys(await file.text());
    setText('nsp-keys-status', t('nsp_keys_ok', { file: file.name }));
    await refreshBaseVersion();
  } catch (err) {
    setText('nsp-keys-status', t('nsp_keys_bad', { file: file.name, reason: err?.message ?? String(err) }), true);
  }
  renderNsp();
});

$('nsp-base-pick').addEventListener('click', () => $('file-base').click());
$('file-base').addEventListener('change', async (e) => {
  const files = [...e.target.files].sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
  e.target.value = '';
  if (!files.length) return;
  state.nsp.base = null;
  const label = files.map((f) => f.name).join(', ');
  try {
    const json = files.filter((f) => f.name.toLowerCase().endsWith('.json'));
    if (json.length) {
      if (files.length !== 1) throw new Error(t('nsp_base_mixed'));
      if (json[0].size > 256 << 20) throw new Error('too large for a base metadata file');
      const meta = parseBaseMetadata(await json[0].text());
      state.nsp.base = { meta, name: json[0].name, metaName: json[0].name };
      const last = Number(meta.last_update_version) || 0;
      setText('nsp-base-status', t('nsp_base_meta_ok', {
        file: json[0].name, title: meta.title_id, count: Object.keys(meta.files).length.toLocaleString('en-US'),
        last: last ? t('nsp_base_last', { n: last }) : '',
      }));
      setUpdateVersion(nextUpdateVersion(meta, rememberedUpdate(meta.title_id, meta.program_nca)));
    } else {
      if (files.length > 1 && !files.every((f) => /^\d{2}$/.test(f.name))) throw new Error(t('nsp_base_mixed'));
      const head = new Uint8Array(await files[0].slice(0, 4).arrayBuffer());
      if (new TextDecoder().decode(head) !== 'PFS0') throw new Error('not an NSP (no PFS0 header)');
      const name = files.length > 1 ? CONFIG.nsp.fullName : files[0].name;
      state.nsp.base = { parts: files, name, metaName: `${name}.basemeta.json` };
      const size = files.reduce((a, f) => a + f.size, 0);
      setText('nsp-base-status', t('nsp_base_nsp_ok', { file: label, size: formatBytes(size) }));
      setUpdateVersion(1);
      await refreshBaseVersion();
    }
  } catch (err) {
    state.nsp.base = null;
    setText('nsp-base-status', t('nsp_base_bad', { file: label, reason: err?.message ?? String(err) }), true);
  }
  renderNsp();
});

/** The NSP identity of the selected edition (config.js editions[].nsp): { titleId, dataDir }, or {} without one. */
function editionNsp() {
  const nsp = state.disc?.edition?.nsp;
  return nsp ? { titleId: nsp.titleId, dataDir: nsp.dataDir } : {};
}

$('create-nsp').addEventListener('click', async () => {
  const n = state.nsp;
  if (!n.keys) { setText('nsp-status', t('nsp_need_keys'), true); return; }
  let sink;
  try {
    sink = await openNspSink(n.out, CONFIG.nsp.fullName, usbSinkOptions());
  } catch (err) {
    setText('nsp-status', err?.message ?? String(err), true);
    return;
  }
  if (!sink) return;
  setText('nsp-status', '');
  const target = n.out === 'usb' ? { target: 'usb' } : {};
  startRun({ output: 'nsp', mode: 'full', sink, name: sink.name || CONFIG.nsp.fullName, nsp: { kind: 'full', keys: n.keys, ...target, ...editionNsp() } });
});

$('create-nsp-update').addEventListener('click', async () => {
  const n = state.nsp;
  if (!n.keys) { setText('nsp-status', t('nsp_need_keys'), true); return; }
  if (!n.base) { setText('nsp-status', t('nsp_need_base'), true); return; }
  const v = updateVersion();
  if (!v) { setText('nsp-status', t('nsp_bad_version'), true); return; }
  const name = updateName(v);
  const usb = $('nsp-update-usb').checked && nspSinkSupport().usb;
  let sink;
  try {
    sink = await openNspSink(usb ? 'usb' : nspSinkSupport().file ? 'file' : 'memory', name, usbSinkOptions());
  } catch (err) {
    setText('nsp-status', err?.message ?? String(err), true);
    return;
  }
  if (!sink) return;
  setText('nsp-status', '');
  const base = n.base;
  startRun({
    output: 'nsp', mode: 'full', sink, name: sink.name || name, base,
    // titleId: the selected edition's, checked against the base's own (which the update keeps); dataDir: used only
    // when the base recorded none.
    nsp: {
      kind: 'update', keys: n.keys, programOnly: !$('nsp-update-data').checked, version: v, base: base.meta ? { meta: base.meta } : { parts: base.parts },
      ...(usb ? { target: 'usb' } : {}), ...editionNsp(),
    },
  });
});

// ---- USB install (js/usb_install.js; docs/full-nsp.md section 6) ---------------------------------------------------
// The console is chosen when the package is ready (the passes before take long, and Sphaira's USB screen need not be
// open during them): a console this site may already use is taken without asking; otherwise the progress panel shows
// a button, because the browser's device chooser needs a click.

function showUsbPrompt(key, { button = false, params = {} } = {}) {
  $('usb-prompt-text').textContent = t(key, params);
  $('usb-connect').hidden = !button;
  $('usb-prompt').hidden = false;
}

function hideUsbPrompt() { $('usb-prompt').hidden = true; }

/** Waits for the user to pick the console (button + chooser), or for an allowed console to be plugged in. */
function askForSwitch(signal, key = 'usb_prompt_connect', params = {}) {
  return new Promise((resolve, reject) => {
    const usb = navigator.usb;
    const done = (fn, v) => {
      $('usb-connect').removeEventListener('click', onClick);
      usb.removeEventListener('connect', onConnect);
      signal?.removeEventListener('abort', onAbort);
      fn(v);
    };
    const onClick = async () => {
      try {
        const device = await requestSwitch(usb);
        if (device) done(resolve, device);
        else showUsbPrompt('usb_prompt_none', { button: true });
      } catch (err) {
        showUsbPrompt('usb_prompt_error', { button: true, params: { error: err?.message ?? String(err) } });
      }
    };
    const onConnect = (e) => {
      if (e.device?.vendorId === USB_VENDOR_ID && e.device?.productId === USB_PRODUCT_ID) done(resolve, e.device);
    };
    const onAbort = () => done(reject, signal.reason ?? new DOMException('Aborted', 'AbortError'));
    if (signal?.aborted) { onAbort(); return; }
    $('usb-connect').addEventListener('click', onClick);
    usb.addEventListener('connect', onConnect);
    signal?.addEventListener('abort', onAbort);
    showUsbPrompt(key, { button: true, params });
    $('usb-prompt').scrollIntoView({ block: 'nearest' });
  });
}

/** UsbNspSink's connect(): an opened WebUsbTransport to the console. Retries (via the button) until it works or the run is cancelled. */
async function connectSwitch({ signal, log }) {
  let device = null;
  try { device = await findPermittedSwitch(); } catch { /* ask instead */ }
  let prompt = ['usb_prompt_connect', {}];
  for (;;) {
    if (!device) device = await askForSwitch(signal, ...prompt);
    try {
      const transport = await WebUsbTransport.open(device, { signal });
      log(`USB: opened ${device.productName || 'the console'} (${device.vendorId.toString(16).padStart(4, '0')}:${device.productId.toString(16).padStart(4, '0')})`);
      return transport;
    } catch (err) {
      if (signal?.aborted) throw err;
      log(`USB: ${err?.message ?? err}`, 'warn');
      prompt = ['usb_prompt_error', { error: err?.message ?? String(err) }];
      device = null;
    }
  }
}

function usbSinkOptions() {
  return { connect: connectSwitch, onWaiting: () => showUsbPrompt('usb_prompt_waiting') };
}

window.addEventListener('pagehide', () => { forgetKeys(state.nsp.keys); state.nsp.keys = null; });

/** The result notice of an NSP run: install notes and the base metadata to keep. */
async function nspResultKids(job, result) {
  const usb = job.sink.kind === 'usb';
  const kids = [
    el('p', {}, el('strong', { textContent: t('result_ready', { name: job.name }) }), ' ', t('result_size', { size: formatBytes(result.nspBytes) })),
    el('p', {
      textContent: usb ? usbResultText(job, result)
        : job.sink.kind === 'split' ? t('result_nsp_split', { name: job.name }) : result.sinkResult === 'saved' ? t('result_saved_file') : t('result_saved_download'),
    }),
  ];
  const offerSave = (metaName, text, note) => {
    const btn = el('button', { className: 'btn small', type: 'button', textContent: t('nsp_meta_save_btn', { name: metaName }) });
    const status = el('span', { className: 'muted' });
    btn.addEventListener('click', async () => {
      try {
        const r = await saveTextFile(metaName, text);
        if (r) status.textContent = ` ${t('nsp_meta_saved', { name: metaName })}`;
      } catch (err) {
        status.textContent = ` ${err?.message ?? String(err)}`;
      }
    });
    kids.push(el('p', { textContent: note }), el('p', { className: 'result-actions' }, btn, status));
  };
  if (job.nsp.kind === 'full') {
    if (!usb) kids.push(el('p', { textContent: t('result_nsp_install') }));
    const metaName = `${job.name}.basemeta.json`;
    const text = pythonJson(result.nsp.baseMeta);
    let saved = false;
    if (job.sink.kind === 'split' && job.sink.dir) {
      try { await saveTextFile(metaName, text, { dir: job.sink.dir }); saved = true; } catch { /* offer the button instead */ }
    }
    if (saved) kids.push(el('p', { textContent: `${t('nsp_meta_saved_split', { name: metaName })} ${t('nsp_meta_note')}` }));
    else offerSave(metaName, text, t('nsp_meta_note'));
  } else {
    const v = job.nsp.version;
    const meta = result.baseMeta;
    kids.push(el('p', { textContent: t('result_nsp_update', { n: v, version: `0x${(v * 0x10000).toString(16)}`, title: meta.title_id }) }));
    if (!usb) kids.push(el('p', { textContent: t('result_nsp_install') }));
    rememberUpdate(meta.title_id, meta.program_nca, v);
    const updated = withLastUpdateVersion(meta, v);
    if (state.nsp.base === job.base) {
      state.nsp.base = { meta: updated, name: job.base.name, metaName: job.base.metaName };
      setUpdateVersion(v + 1);
    }
    offerSave(job.base.metaName, pythonJson(updated), t('nsp_meta_update_note', { n: v, next: v + 1 }));
  }
  if (result.shaders) {
    kids.push(result.shaders.failures.length
      ? el('p', { className: 'muted', textContent: t('result_shaders_skipped', { ok: result.shaders.ok.toLocaleString('en-US'), skipped: result.shaders.failures.length }) })
      : el('p', { className: 'muted', textContent: t('result_shaders', { ok: result.shaders.ok.toLocaleString('en-US') }) }));
  }
  return kids;
}

function usbResultText(job, result) {
  const st = job.sink.stats ?? {};
  const secs = st.seconds ?? 0;
  return t('result_usb_installed', {
    size: formatBytes(result.nspBytes),
    time: duration(secs),
    rate: secs > 0 ? `${(result.nspBytes / 1e6 / secs).toFixed(1)} MB/s` : '–',
  });
}

// ---- running ---------------------------------------------------------------------------------------------------
const rows = {};
function buildStages() {
  const list = $('stages');
  list.replaceChildren();
  for (const k of Object.keys(rows)) delete rows[k];
  for (const id of state.stages) {
    const bar = el('i');
    const barBox = el('div', { className: 'bar' }, bar);
    barBox.setAttribute('role', 'progressbar');
    barBox.setAttribute('aria-valuemin', '0');
    barBox.setAttribute('aria-valuemax', '100');
    const name = el('span', { className: 'name', textContent: stageName(id) });
    barBox.setAttribute('aria-label', name.textContent);
    const pct = el('span', { className: 'pct' });
    const label = el('span', { className: 'label' });
    const eta = el('span', { className: 'eta' });
    const li = el('li', { className: 'stage pending' },
      el('div', { className: 'top' }, name, pct),
      barBox,
      el('div', { className: 'bottom' }, label, eta));
    rows[id] = { li, bar, barBox, name, pct, label, eta, t0: 0, f0: 0 };
    list.append(li);
  }
}

function markDone(r) {
  r.li.className = 'stage done';
  r.bar.style.width = '100%';
  r.barBox.classList.remove('indeterminate');
  r.barBox.setAttribute('aria-valuenow', '100');
  r.pct.textContent = '100 %';
  r.eta.textContent = '';
}

/**
 * Remaining time from the progress rate of this stage since its first report:
 * elapsed / (fraction gained) * (fraction left). Shown after 3 s and 1 % of progress.
 */
function etaText(r, f) {
  const now = performance.now();
  if (!r.t0) { r.t0 = now; r.f0 = f; return t('eta_estimating'); }
  const dt = (now - r.t0) / 1000;
  const df = f - r.f0;
  if (dt < 3 || df < 0.01) return t('eta_estimating');
  return t('eta_left', { time: duration((dt / df) * (1 - f)) });
}

function onProgress(id, p) {
  if (id === 'nsp_usb' && p.done > 0) hideUsbPrompt();
  const idx = state.stages.indexOf(id);
  if (idx < 0 || !rows[id]) return;
  state.stages.forEach((sid, i) => {
    if (i < idx && !rows[sid].li.classList.contains('done')) markDone(rows[sid]);
  });
  const r = rows[id];
  r.label.textContent = p.label ?? '';
  const finished = p.total > 0 && p.done >= p.total;
  if (finished) { markDone(r); return; }
  r.li.className = 'stage active';
  // A stage that only reports 0 of 1 (packing) has no measurable progress: animated bar, no percent.
  if (!(p.total > 1)) {
    r.barBox.classList.add('indeterminate');
    r.barBox.removeAttribute('aria-valuenow');
    r.pct.textContent = t('progress_working');
    r.eta.textContent = '';
    return;
  }
  const f = Math.min(1, Math.max(0, p.done / p.total));
  r.barBox.classList.remove('indeterminate');
  r.bar.style.width = `${f * 100}%`;
  r.barBox.setAttribute('aria-valuenow', String(Math.floor(f * 100)));
  r.pct.textContent = `${Math.floor(f * 100)} %`;
  r.eta.textContent = etaText(r, f);
}

const logLines = [];
function log(text, level = 'info') {
  logLines.push(`${level === 'info' ? '' : `[${level}] `}${text}`);
  $('log').textContent = logLines.join('\n');
  $('log-count').textContent = t('log_lines', { count: logLines.length });
  const wrap = $('log');
  wrap.scrollTop = wrap.scrollHeight;
}

function createWorker(kind) {
  const file = { scan: 'scan', shader: 'shader', pack: 'pack' }[kind];
  return new Worker(new URL(`./workers/${file}.worker.js`, import.meta.url), { type: 'module' });
}

async function start(mode) {
  if (state.running || state.dlcBusy || !state.disc?.edition) return;
  const name = mode === 'full' ? CONFIG.zip.fullName : CONFIG.zip.updateName;
  const d = state.disc;
  const expected = (mode === 'full' ? d.plan.copyBytes : 0) + CONFIG.limits.expectedShaderBytes + CONFIG.limits.expectedNroBytes + dlcExtraBytes();
  const sink = await openSink(name, expected);
  if (!sink) return;
  await startRun({ output: 'zip', mode, sink, name });
}

/** job: { output: 'zip' | 'nsp', mode, sink, name, nsp (NSP options for run()), base (the picked base, updates) }. */
async function startRun(job) {
  const { output, mode, sink, name } = job;
  if (state.running || state.dlcBusy || !state.disc?.edition) { await sink.abort?.(); return; }
  const d = state.disc;
  state.stages = stagesFor(output, job.nsp);
  state.usbRun = job.nsp?.target === 'usb';
  hideUsbPrompt();

  state.running = true;
  state.abort = new AbortController();
  setButtons(false);
  renderDlc();
  for (const id of ['pick', 'create-full', 'create-update', 'dlc-pick', 'create-nsp', 'create-nsp-update']) $(id).disabled = true;
  document.querySelectorAll('input[name=format]').forEach((r) => (r.disabled = true));
  $('result').hidden = true;
  state.outcome = null;
  $('progress').hidden = false;
  $('cancel').disabled = false;
  logLines.length = 0;
  $('log').textContent = '';
  buildStages();
  updateWizard();
  const t0 = Date.now();
  const tick = () => { $('elapsed').textContent = t('elapsed', { time: duration((Date.now() - t0) / 1000) }); };
  tick();
  const timer = setInterval(tick, 1000);
  const block = (e) => { e.preventDefault(); e.returnValue = ''; };
  window.addEventListener('beforeunload', block);
  let wake = null;
  try { wake = await navigator.wakeLock?.request('screen'); } catch { /* not available or denied */ }
  const dlc = [...state.dlc];
  const what = output === 'nsp'
    ? (job.nsp.kind === 'full' ? 'full NSP' : job.nsp.programOnly ? `NSP update ${job.nsp.version} (program only)` : `NSP update ${job.nsp.version} with game data`)
    : (mode === 'full' ? 'full install' : 'update');
  const where = { file: 'straight to the chosen file', split: 'a FAT32 split folder', opfs: 'temporary storage, then download', usb: 'nowhere: served to the Switch over USB' }[sink.kind] ?? 'in memory';
  log(`Mode: ${what}; edition ${d.edition.name}; ${dlc.length ? `${dlc.length} DLC package(s)` : 'no DLC'}; saving as ${name} (${where}).`);
  log(`Source: ${d.source.label} (${d.source.detail}).`);
  for (const line of d.source.log ?? []) log(line);
  for (const line of d.packages?.log ?? []) log(line, d.packages.bad.length || d.packages.duplicates.length ? 'warn' : 'info');

  try {
    const result = await run({
      config: CONFIG, edition: d.edition, files: d.files, dlc, mode, sink, createWorker, output, nsp: job.nsp,
      baseUrl: new URL('.', location.href).href, signal: state.abort.signal, onProgress, onLog: log,
    });
    state.stages.forEach((id) => markDone(rows[id]));
    state.outcome = 'ok';
    if (output === 'nsp') {
      showResult(true, await nspResultKids(job, result));
      return;
    }
    showResult(true, [
      el('p', {}, el('strong', { textContent: t('result_ready', { name }) }), ' ', t('result_size', { size: formatBytes(result.zipBytes) })),
      el('p', { textContent: result.sinkResult === 'saved' ? t('result_saved_file') : t('result_saved_download') }),
      el('p', { innerHTML: t('result_extract') }),
      ...(dlc.length ? [el('p', { textContent: t('result_dlc', { count: dlc.length }) })] : []),
      result.shaders.failures.length
        ? el('p', { className: 'muted', textContent: t('result_shaders_skipped', { ok: result.shaders.ok.toLocaleString('en-US'), skipped: result.shaders.failures.length }) })
        : el('p', { className: 'muted', textContent: t('result_shaders', { ok: result.shaders.ok.toLocaleString('en-US') }) }),
    ]);
  } catch (e) {
    if (e instanceof Cancelled) {
      state.outcome = 'cancelled';
      showResult(false, [el('p', { textContent: t('result_cancelled') })], 'warn');
    } else {
      console.error(e);
      state.outcome = 'err';
      for (const id of state.stages) if (rows[id].li.classList.contains('active')) { rows[id].li.className = 'stage error'; rows[id].eta.textContent = ''; }
      const msg = e instanceof UserError ? e.message : t('result_error', { error: e?.message ?? e });
      log(`ERROR: ${msg}`, 'error');
      showResult(false, [el('p', { textContent: msg }), el('p', { className: 'muted', textContent: t('result_nothing_saved') })], 'err');
    }
  } finally {
    hideUsbPrompt();
    clearInterval(timer);
    for (const id of state.stages) { rows[id].barBox.classList.remove('indeterminate'); rows[id].eta.textContent = ''; }
    window.removeEventListener('beforeunload', block);
    try { await wake?.release(); } catch { /* ignore */ }
    state.running = false;
    state.abort = null;
    $('pick').disabled = false;
    document.querySelectorAll('input[name=format]').forEach((r) => (r.disabled = false));
    setButtons(true);
    renderDlc();
    renderNsp();
    $('cancel').disabled = true;
    updateWizard();
  }
}

function showResult(ok, kids, cls = ok ? 'ok' : 'err') {
  const r = $('result');
  r.className = `notice ${cls}`;
  r.replaceChildren(...kids);
  r.hidden = false;
  r.scrollIntoView({ block: 'nearest' });
}

$('create-full').addEventListener('click', () => start('full'));
$('create-update').addEventListener('click', () => start('update'));
$('cancel').addEventListener('click', () => { $('cancel').disabled = true; state.abort?.abort(); });

// Apply initial language
applyLanguage();

// For debugging in the console.
window.__installer = { state, CONFIG, applyLanguage, setLanguage };
