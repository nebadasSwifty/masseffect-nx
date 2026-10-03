// The page: picks the source, shows the detected edition, runs the pipeline with progress.
import { CONFIG } from '../config.js';
import { sourceFromIso, sourceFromFileList, sourceFromDirectoryHandle, sourceFromDataTransfer, inspectDisc } from './source.js';
import { planGameFiles, formatBytes, formatDuration } from './plan.js';
import { openSink, describeSinkSupport, cleanStaleTemporaryFiles } from './sink.js';
import { run, Cancelled, UserError, stageIds } from './pipeline.js';

const $ = (id) => document.getElementById(id);
const el = (tag, props = {}, ...kids) => {
  const e = Object.assign(document.createElement(tag), props);
  for (const k of kids) e.append(k);
  return e;
};

const STAGE_NAMES = {
  download: 'Download the build',
  scan: 'Read the disc',
  translate: 'Translate the shaders',
  pack: 'Pack the shader library',
  zip: 'Write the zip',
};

const state = { format: 'iso', disc: null, running: false, abort: null };

// ---- header / footer -------------------------------------------------------------------------------------------
$('repo-link').href = CONFIG.project.repoUrl;
$('author').textContent = CONFIG.project.author;
for (const [id, name] of [['create-full', CONFIG.zip.fullName], ['create-update', CONFIG.zip.updateName]]) {
  $(id).textContent = `Create ${name}`;
}

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
  box.replaceChildren(...[...problems, ...warnings].map((t) => el('p', { textContent: t })));
  if (problems.length) {
    for (const id of ['pick', 'file-iso', 'file-folder']) $(id).disabled = true;
  }
})();
cleanStaleTemporaryFiles();

// ---- step 1 ---------------------------------------------------------------------------------------------------
const hints = {
  iso: 'The disc image of the Xbox 360 game (single or dual layer). It is read in place and never loaded as a whole.',
  folder: `The extracted disc: the folder that contains ${CONFIG.disc.xex} next to the game's folders (Layer0, Layer1...). It is read in place.`,
};

function setFormat(f) {
  state.format = f;
  $('format-hint').textContent = hints[f];
  $('pick').textContent = f === 'iso' ? 'Choose .iso file' : 'Choose folder';
  document.querySelector('.or').textContent = f === 'iso' ? 'or drop it here' : 'or drop the folder here';
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
      await load(async () => sourceFromDirectoryHandle(handle, (n) => status(`Listing the folder: ${n} files...`)));
    } catch (e) {
      if (e?.name !== 'AbortError') status(`Could not open the folder: ${e.message}`, true);
    }
    return;
  }
  (state.format === 'iso' ? $('file-iso') : $('file-folder')).click();
});
$('file-iso').addEventListener('change', (e) => {
  const f = e.target.files[0];
  e.target.value = '';
  if (f) load(() => sourceFromIso(f));
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
    const dropped = await sourceFromDataTransfer(e.dataTransfer, (n) => status(`Listing the folder: ${n} files...`));
    if (dropped.file) {
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
  if (state.format !== v) { state.format = v; $('format-hint').textContent = hints[v]; $('pick').textContent = v === 'iso' ? 'Choose .iso file' : 'Choose folder'; }
}

async function load(makeSource) {
  if (state.running) return;
  resetDisc();
  status('Reading the disc...');
  try {
    const source = await makeSource();
    status(`Checking ${CONFIG.disc.xex}...`);
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
    body.replaceChildren(
      el('dl', { className: 'ed' },
        el('dt', { textContent: 'Edition' }), el('dd', {}, el('strong', { textContent: d.edition.name }), ' ', el('span', { className: 'badge ok', textContent: 'supported' })),
        el('dt', { textContent: 'default.xex' }), el('dd', {}, el('code', { textContent: d.sha256 })),
        el('dt', { textContent: 'Game files' }), el('dd', { textContent: `${plan.copy.length.toLocaleString('en-US')} files, ${formatBytes(plan.copyBytes)} go into game_root` }),
        el('dt', { textContent: 'Left out' }), el('dd', { textContent: `${plan.skipped.length.toLocaleString('en-US')} files, ${formatBytes(plan.skippedBytes)} that the game never reads (${CONFIG.disc.skip.map((s) => s.prefix.replace(/\/$/, '')).join(', ')})` }),
      ));
    showCreate();
  } else {
    $('step-create').hidden = true;
    body.replaceChildren(
      el('p', {}, el('span', { className: 'badge bad', textContent: 'not supported' }), ` This ${CONFIG.disc.xex} is not one of the editions the port is built for.`),
      el('dl', { className: 'ed' }, el('dt', { textContent: 'SHA-256' }), el('dd', {}, el('code', { textContent: d.sha256 }))),
      el('p', { textContent: 'The port is the recompiled program of one exact executable, so it cannot run with another. Supported editions:' }),
      el('ul', {}, ...CONFIG.editions.map((e) => el('li', { textContent: e.name }))),
      el('p', { textContent: 'Check that you picked an original, unmodified dump of the game (not a title-update patched, trimmed or re-authored one, and not another region). If you believe your edition should work, open an issue and include the SHA-256 above.' }),
      el('p', {}, el('a', { href: CONFIG.project.issuesUrl, textContent: 'Open an issue', target: '_blank', rel: 'noopener' })),
    );
  }
}

function showCreate() {
  const d = state.disc;
  const fullBytes = d.plan.copyBytes + CONFIG.limits.expectedShaderBytes + CONFIG.limits.expectedNroBytes;
  const updBytes = CONFIG.limits.expectedShaderBytes + CONFIG.limits.expectedNroBytes;
  const info = $('create-info');
  const lines = [
    el('p', {}, 'The full zip will be about ', el('strong', { textContent: formatBytes(fullBytes) }), '; the update zip about ', el('strong', { textContent: formatBytes(updBytes) }),
      '. Making the shaders is the long part (30,000 shaders are translated on your computer): expect a long wait, and keep this tab open and in the foreground.'),
  ];
  const sink = describeSinkSupport();
  if (sink === 'memory' && fullBytes > CONFIG.limits.blobWarnBytes) {
    lines.push(el('div', { className: 'notice err', textContent: `Warning: this browser can only build the zip in memory and ${formatBytes(fullBytes)} will very likely not fit. Use Chrome or Edge, or make only the update zip.` }));
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
    const li = el('li', { className: 'stage pending' },
      el('div', { className: 'top' }, el('span', { className: 'name', textContent: STAGE_NAMES[id] }), label),
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
  // The file picker needs the click's user activation: ask for the target before anything else.
  const expected = (mode === 'full' ? d.plan.copyBytes : 0) + CONFIG.limits.expectedShaderBytes + CONFIG.limits.expectedNroBytes;
  const sink = await openSink(name, expected);
  if (!sink) return; // picker cancelled

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

// For debugging in the console.
window.__installer = { state, CONFIG };
