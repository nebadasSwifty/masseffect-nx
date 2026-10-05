// The page: picks the source, shows the detected edition, runs the pipeline with progress.
import { CONFIG } from '../config.js';
import { sourceFromIso, sourceFromFileList, sourceFromDirectoryHandle, sourceFromDataTransfer, inspectDisc } from './source.js';
import { planGameFiles, formatBytes, formatDuration } from './plan.js';
import { openSink, describeSinkSupport, cleanStaleTemporaryFiles } from './sink.js';
import { run, Cancelled, UserError, stageIds } from './pipeline.js';
import { initLanguage, getLanguage, setLanguage, t } from './i18n.js';

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
};

const state = { format: 'iso', disc: null, running: false, abort: null };

// ---- i18n & language toggle ------------------------------------------------------------------------------------
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

  setFormat(state.format);
  if (state.disc) {
    showEdition();
  }
}

initLanguage();

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
  $('format-hint').textContent = f === 'iso' ? t('format_hint_iso') : t('format_hint_folder');
  $('pick').textContent = f === 'iso' ? t('pick_iso') : t('pick_folder');
  const orSpan = document.querySelector('.or');
  if (orSpan) orSpan.textContent = t('drop_or');
  resetDisc();
}

function resetDisc() {
  if (state.running) return;
  state.disc = null;
  $('step-edition').hidden = true;
  $('step-create').hidden = true;
  $('source-status').textContent = '';
  $('source-status').className = 'status';
}

for (const r of document.querySelectorAll('input[name=format]')) r.addEventListener('change', () => r.checked && setFormat(r.value));

function status(text, isError = false) {
  const s = $('source-status');
  s.textContent = text;
  s.className = isError ? 'status err' : 'status';
}

$('pick').addEventListener('click', async () => {
  if (state.format === 'folder' && typeof showDirectoryPicker === 'function') {
    try {
      const handle = await showDirectoryPicker({ mode: 'read' });
      await load(async () => sourceFromDirectoryHandle(handle, (n) => status(t('status_folder_listing', { count: n }))));
    } catch (e) {
      if (e?.name !== 'AbortError') status(`Could not open the folder: ${e.message}`, true);
    }
    return;
  }
  (state.format === 'iso' ? $('file-iso') : $('file-folder')).click();
});

$('file-iso').addEventListener('change', (e) => {
  const files = [...e.target.files];
  e.target.value = '';
  if (files.length) load(() => sourceFromIso(files));
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
      load(() => sourceFromIso(dropped.files));
    } else if (dropped.file) {
      setRadio(dropped.file.name.toLowerCase().endsWith('.iso') || dropped.file.size > 100e6 ? 'iso' : state.format);
      load(() => sourceFromIso(dropped.file));
    } else {
      setRadio('folder');
      load(async () => dropped.source);
    }
  } catch (err) { status(`Could not read what was dropped: ${err.message}`, true); }
});

function setRadio(v) {
  document.querySelector(`input[name=format][value=${v}]`).checked = true;
  if (state.format !== v) {
    setFormat(v);
  }
}

async function load(makeSource) {
  if (state.running) return;
  resetDisc();
  status(t('status_reading'));
  try {
    const source = await makeSource();
    status(t('status_checking'));
    const info = await inspectDisc(source, CONFIG);
    state.disc = { source, ...info };
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
  if (d.edition) {
    const isUnverified = d.matchType === 'unverified_header';
    const badge = isUnverified
      ? el('span', { className: 'badge warn', textContent: t('badge_unverified') })
      : el('span', { className: 'badge ok', textContent: t('badge_supported') });
    const elements = [
      el('dl', { className: 'ed' },
        el('dt', { textContent: t('edition_label') }), el('dd', {}, el('strong', { textContent: d.edition.name }), ' ', badge),
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
    body.replaceChildren(...elements);
    showCreate();
  } else {
    $('step-create').hidden = true;
    const msg = d.matchType === 'invalid'
      ? t('invalid_xex_msg', { error: d.error || 'corrupted or truncated executable' })
      : t('not_supported_msg');
    body.replaceChildren(
      el('p', {}, el('span', { className: 'badge bad', textContent: t('badge_not_supported') }), ' ', msg),
      el('dl', { className: 'ed' }, el('dt', { textContent: 'SHA-256' }), el('dd', {}, el('code', { textContent: d.sha256 }))),
      el('p', { textContent: t('supported_editions_intro') }),
      el('ul', {}, ...CONFIG.editions.map((e) => el('li', { textContent: e.name }))),
      el('p', { textContent: t('check_original_hint') }),
      el('p', {}, el('a', { href: CONFIG.project.issuesUrl, textContent: t('open_issue'), target: '_blank', rel: 'noopener' })),
    );
  }
}

function showCreate() {
  const d = state.disc;
  const fullBytes = d.plan.copyBytes + CONFIG.limits.expectedShaderBytes + CONFIG.limits.expectedNroBytes;
  const updBytes = CONFIG.limits.expectedShaderBytes + CONFIG.limits.expectedNroBytes;
  const info = $('create-info');
  const lines = [
    el('p', { innerHTML: t('create_estimate', { full: formatBytes(fullBytes), upd: formatBytes(updBytes) }) }),
  ];
  const sink = describeSinkSupport();
  if (sink === 'memory' && fullBytes > CONFIG.limits.blobWarnBytes) {
    lines.push(el('div', { className: 'notice err', textContent: t('memory_warning', { size: formatBytes(fullBytes) }) }));
  }
  info.replaceChildren(...lines);
  $('step-create').hidden = false;
  setButtons(true);
}

function setButtons(enabled) {
  const ok = enabled && !envProblems().problems.length;
  $('create-full').disabled = !ok;
  $('create-update').disabled = !ok;
}

// ---- running ---------------------------------------------------------------------------------------------------
const rows = {};
function buildStages() {
  const list = $('stages');
  list.replaceChildren();
  for (const id of stageIds) {
    const bar = el('i');
    const label = el('span', { className: 'label' });
    const stageName = t(STAGE_KEYS[id] || id);
    const li = el('li', { className: 'stage pending' },
      el('div', { className: 'top' }, el('span', { className: 'name', textContent: stageName }), label),
      el('div', { className: 'bar' }, bar));
    rows[id] = { li, bar, label };
    list.append(li);
  }
}

function onProgress(id, p) {
  const idx = stageIds.indexOf(id);
  stageIds.forEach((sid, i) => {
    const r = rows[sid];
    if (i < idx) { r.li.className = 'stage done'; r.bar.style.width = '100%'; }
  });
  const r = rows[id];
  const finished = p.total > 0 && p.done >= p.total;
  r.li.className = `stage ${finished ? 'done' : 'active'}`;
  r.bar.style.width = `${p.total > 0 ? Math.min(100, (p.done / p.total) * 100) : 0}%`;
  r.label.textContent = p.label ?? '';
}

const logLines = [];
function log(text, level = 'info') {
  logLines.push(`${level === 'info' ? '' : `[${level}] `}${text}`);
  $('log').textContent = logLines.join('\n');
  $('log-count').textContent = `(${logLines.length} lines)`;
  const wrap = $('log');
  wrap.scrollTop = wrap.scrollHeight;
}

function createWorker(kind) {
  const file = { scan: 'scan', shader: 'shader', pack: 'pack' }[kind];
  return new Worker(new URL(`./workers/${file}.worker.js`, import.meta.url), { type: 'module' });
}

async function start(mode) {
  if (state.running || !state.disc?.edition) return;
  const name = mode === 'full' ? CONFIG.zip.fullName : CONFIG.zip.updateName;
  const d = state.disc;
  const expected = (mode === 'full' ? d.plan.copyBytes : 0) + CONFIG.limits.expectedShaderBytes + CONFIG.limits.expectedNroBytes;
  const sink = await openSink(name, expected);
  if (!sink) return;

  state.running = true;
  state.abort = new AbortController();
  setButtons(false);
  for (const id of ['pick', 'create-full', 'create-update']) $(id).disabled = true;
  document.querySelectorAll('input[name=format]').forEach((r) => (r.disabled = true));
  $('result').hidden = true;
  $('progress').hidden = false;
  $('cancel').disabled = false;
  logLines.length = 0;
  $('log').textContent = '';
  buildStages();
  const t0 = Date.now();
  const timer = setInterval(() => { $('elapsed').textContent = `Elapsed: ${formatDuration((Date.now() - t0) / 1000)}`; }, 1000);
  const block = (e) => { e.preventDefault(); e.returnValue = ''; };
  window.addEventListener('beforeunload', block);
  let wake = null;
  try { wake = await navigator.wakeLock?.request('screen'); } catch { /* not available or denied */ }
  log(`Mode: ${mode === 'full' ? 'full install' : 'update'}; edition ${d.edition.name}; saving as ${name} (${sink.kind === 'file' ? 'straight to the chosen file' : sink.kind === 'opfs' ? 'temporary storage, then download' : 'in memory'}).`);

  try {
    const result = await run({
      config: CONFIG, edition: d.edition, files: d.files, mode, sink, createWorker,
      baseUrl: new URL('.', location.href).href, signal: state.abort.signal, onProgress, onLog: log,
    });
    stageIds.forEach((id) => { rows[id].li.className = 'stage done'; rows[id].bar.style.width = '100%'; });
    showResult(true, [
      el('p', {}, el('strong', { textContent: `${name} is ready` }), ` (${formatBytes(result.zipBytes)}).`),
      el('p', { textContent: result.sinkResult === 'saved' ? 'It was saved to the file you chose.' : 'Your browser is saving it to your Downloads folder.' }),
      el('p', {}, 'Extract it into ', el('code', { textContent: 'sdmc:/switch/' }), ' on the SD card.'),
      result.shaders.failures.length
        ? el('p', { className: 'muted', textContent: `${result.shaders.ok.toLocaleString('en-US')} shaders were made; ${result.shaders.failures.length} containers were skipped, which is expected (see Details).` })
        : el('p', { className: 'muted', textContent: `${result.shaders.ok.toLocaleString('en-US')} shaders were made.` }),
    ]);
  } catch (e) {
    if (e instanceof Cancelled) {
      showResult(false, [el('p', { textContent: 'Cancelled. Nothing was saved.' })], 'warn');
    } else {
      console.error(e);
      const msg = e instanceof UserError ? e.message : `Something went wrong: ${e?.message ?? e}`;
      log(`ERROR: ${msg}`, 'error');
      showResult(false, [el('p', { textContent: msg }), el('p', { className: 'muted', textContent: 'Nothing was saved. The Details section has the full log.' })], 'err');
    }
  } finally {
    clearInterval(timer);
    window.removeEventListener('beforeunload', block);
    try { await wake?.release(); } catch { /* ignore */ }
    state.running = false;
    state.abort = null;
    $('pick').disabled = false;
    document.querySelectorAll('input[name=format]').forEach((r) => (r.disabled = false));
    setButtons(true);
    $('cancel').disabled = true;
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
