// Pseudo HDR user interface. All processing happens in worker.js / engine.wasm.
// The preview is the HDR JPEG itself (screen size) shown in a plain <img>:
// browsers with HDR image support (Safari 26+, Chrome) light it up exactly
// like the saved file. The <img> deliberately has no position/transform and
// the page has no CSS transitions, which make Safari fall back to SDR.
const TEXT = {
  de: {
    open: 'Öffnen', drop: 'PNG- oder JPEG-Bild öffnen oder hier ablegen',
    hint: 'Das Bild wird zum HDR-Foto mit Gain Map: Auf HDR-Displays leuchten helle Bereiche heller als Weiß. Alles wird lokal auf diesem Gerät berechnet.',
    strength: 'Stärke', threshold: 'Schwelle', stops: n => `${n} Blende${n === '1' ? '' : 'n'}`,
    view: 'Ansicht', views: ['HDR', 'Original'], curve: 'Aufhellung über der Helligkeit des Originals',
    save: 'HDR-JPG speichern', opening: 'Öffne …', saving: 'Erzeuge HDR-JPG …', saved: 'Gespeichert: ',
    info: 'Erklärung',
    explain: 'Die Helligkeiten des Bildes werden wie eine Ziehharmonika in den HDR-Bereich gedehnt. ' +
      'Stärke: so viel heller als normales Weiß werden die hellsten Stellen (jede Blende verdoppelt). ' +
      'Schwelle: ab welcher Helligkeit gedehnt wird. Bei 0 % wird das ganze Bild gleichmäßig gedehnt, ' +
      'bei hohen Werten leuchten nur die Lichter. Displays zeigen so viel davon, wie sie können; ' +
      'auf normalen Displays bleibt das Bild unverändert.',
    hdrDisplay: 'HDR-Display', sdrDisplay: 'kein HDR-Display',
    noHdr: 'Dieses Display oder dieser Browser zeigt kein HDR. Die Vorschau bleibt normal, die gespeicherte Datei enthält trotzdem HDR.',
    keepJpeg: 'Das Originalbild bleibt bitgenau erhalten; nur die Gain Map kommt hinzu.',
    pngJpeg: 'PNG wird als JPEG gespeichert (Qualität 95).', grayJpeg: 'Graustufen-JPEG wird als Farb-JPEG gespeichert (Apple zeigt sonst kein HDR).',
    alpha: 'Transparenz wird auf Weiß gelegt.', gainmap: 'Die vorhandene HDR-Gain-Map wird ersetzt.',
    errors: {1: 'Nicht genug Speicher für dieses Bild.', 2: 'Kein PNG- oder JPEG-Bild.', 4: 'Die Datei ist beschädigt oder unvollständig.',
      5: 'Kein Bild geöffnet.', 6: 'Das Bild ist zu groß (höchstens 65535 Pixel pro Seite und 200 Megapixel).', '-1': 'Interner Fehler: '},
    unsupported: {0: 'Diese Bildvariante wird nicht unterstützt.', 1: 'CMYK-JPEGs werden nicht unterstützt.',
      2: 'Arithmetisch kodierte JPEGs werden nicht unterstützt.', 3: '12-Bit- und verlustfreie JPEGs werden nicht unterstützt.',
      4: 'Dieses PNG ist bereits ein HDR-Bild (PQ/HLG).', 5: 'Dieser Bildaufbau wird nicht unterstützt.'}
  },
  en: {
    open: 'Open', drop: 'Open or drop a PNG or JPEG image',
    hint: 'The image becomes an HDR photo with a gain map: on HDR displays bright areas shine brighter than white. Everything is computed locally on this device.',
    strength: 'Strength', threshold: 'Threshold', stops: n => `${n} stop${n === '1' ? '' : 's'}`,
    view: 'View', views: ['HDR', 'Original'], curve: 'Brightening over the brightness of the original',
    save: 'Save HDR JPG', opening: 'Opening …', saving: 'Creating HDR JPG …', saved: 'Saved: ',
    info: 'Explanation',
    explain: 'The brightness range of the image is stretched into the HDR range like an accordion. ' +
      'Strength: how much brighter than normal white the brightest parts become (each stop doubles). ' +
      'Threshold: from which brightness on the image is stretched. At 0 % the whole image is stretched evenly, ' +
      'at high values only the highlights glow. Displays show as much of it as they can; ' +
      'on normal displays the image stays unchanged.',
    hdrDisplay: 'HDR display', sdrDisplay: 'no HDR display',
    noHdr: 'This display or browser shows no HDR. The preview stays normal; the saved file still contains HDR.',
    keepJpeg: 'The original image is kept bit for bit; only the gain map is added.',
    pngJpeg: 'The PNG is saved as JPEG (quality 95).', grayJpeg: 'A greyscale JPEG is saved as colour JPEG (Apple shows no HDR otherwise).',
    alpha: 'Transparency is placed on white.', gainmap: 'The existing HDR gain map is replaced.',
    errors: {1: 'Not enough memory for this image.', 2: 'Not a PNG or JPEG image.', 4: 'The file is damaged or incomplete.',
      5: 'No image open.', 6: 'The image is too large (at most 65535 pixels per side and 200 megapixels).', '-1': 'Internal error: '},
    unsupported: {0: 'This image variant is not supported.', 1: 'CMYK JPEGs are not supported.',
      2: 'Arithmetic-coded JPEGs are not supported.', 3: '12-bit and lossless JPEGs are not supported.',
      4: 'This PNG already is an HDR image (PQ/HLG).', 5: 'This image layout is not supported.'}
  }
};
const FLAG_ALPHA = 1, FLAG_GAINMAP = 2;
const KIND_JPEG = 1;
const DEFAULTS = {stops: 2, threshold: 0.6};
const STORE = 'pseudo-hdr-settings';
const segment = (ref, label, names) =>
  `<div class="seg" data-ref="${ref}" role="radiogroup" aria-label="${label}">` +
  names.map((n, i) => `<button type="button" role="radio" aria-checked="${i === 0}" class="${i === 0 ? 'on' : ''}" data-value="${i}" disabled>${n}</button>`).join('') + '</div>';

export async function start(root, {release} = {}) {
  const lang = (navigator.languages?.[0] || navigator.language || 'en').toLowerCase().startsWith('de') ? 'de' : 'en';
  const t = TEXT[lang];
  document.documentElement.lang = lang;
  const css = document.createElement('link');
  css.rel = 'stylesheet'; css.href = new URL('app.css', import.meta.url).href;
  await new Promise(resolve => { css.onload = css.onerror = resolve; document.head.append(css); });

  let settings = {...DEFAULTS};
  try { Object.assign(settings, JSON.parse(localStorage.getItem(STORE) || '{}')); } catch { /* private mode */ }
  if (!(settings.stops >= 0.25 && settings.stops <= 4)) settings.stops = DEFAULTS.stops;
  if (!(settings.threshold >= 0 && settings.threshold <= 0.95)) settings.threshold = DEFAULTS.threshold;

  const ui = document.createElement('div');
  ui.className = 'ph';
  ui.dataset.release = release || '';
  ui.innerHTML = `
    <header class="bar">
      <span class="brand">Pseudo HDR</span>
      <span class="meta" data-ref="meta"></span>
      <label class="btn primary"><span>${t.open}</span><input data-ref="file" type="file" accept="image/png,image/jpeg,.png,.jpg,.jpeg"></label>
    </header>
    <section class="stage" data-ref="stage">
      <img data-ref="imgHdr" alt="" hidden><img data-ref="imgSdr" alt="" hidden>
      <div class="empty" data-ref="empty"><p class="drop">${t.drop}</p><p class="hint">${t.hint}</p></div>
      <div class="busy" data-ref="busy" hidden><span class="spinner"></span><span data-ref="busyText"></span></div>
    </section>
    <footer class="panel">
      <p class="tip" id="ph-tip" data-ref="tip" role="note" hidden>${t.explain}</p>
      <div class="row">
        <label class="label" for="ph-stops">${t.strength}</label>
        <input id="ph-stops" data-ref="stops" class="slider" type="range" min="0.25" max="4" step="0.05">
        <output class="value" data-ref="stopsValue" for="ph-stops"></output>
      </div>
      <div class="row">
        <label class="label" for="ph-threshold">${t.threshold}</label>
        <input id="ph-threshold" data-ref="threshold" class="slider" type="range" min="0" max="0.95" step="0.01">
        <output class="value" data-ref="thresholdValue" for="ph-threshold"></output>
      </div>
      <div class="row actions">
        <canvas class="curve" data-ref="curve" width="120" height="44" role="img" aria-label="${t.curve}" title="${t.curve}"></canvas>
        <button type="button" class="info" data-ref="info" aria-label="${t.info}" aria-expanded="false" aria-controls="ph-tip">i</button>
        ${segment('view', t.view, t.views)}
        <span class="wait" data-ref="wait" aria-hidden="true"><span class="spinner"></span></span>
        <button data-ref="save" class="btn primary" disabled>${t.save}</button>
        <span class="msg" data-ref="msg" role="status"></span>
      </div>
    </footer>`;
  const $ = {};
  ui.querySelectorAll('[data-ref]').forEach(el => { $[el.dataset.ref] = el; });
  root.querySelector('#boot')?.remove();
  root.append(ui);
  $.stops.value = settings.stops; $.threshold.value = settings.threshold;

  const hdrQuery = matchMedia('(dynamic-range: high)');
  const worker = new Worker(new URL('worker.js', import.meta.url));
  let opened = false, busyJob = null, pendingFile = null, name = 'image', metaText = '', notes = '';
  let showOriginal = false, frame = null, urls = {hdr: null, sdr: null}, curveCodes = null, curveStops = settings.stops;

  const num = (v, digits) => v.toLocaleString(lang, {minimumFractionDigits: digits, maximumFractionDigits: digits});
  function showValues() {
    const s = +$.stops.value;
    const stopsText = num(s, s * 4 % 1 ? 2 : s % 1 ? 1 : 0);
    $.stopsValue.textContent = `${num(2 ** s, 1)}× · ${t.stops(stopsText)}`;
    $.thresholdValue.textContent = `${Math.round(+$.threshold.value * 100)} %`;
  }
  function updateMeta() {
    const display = hdrQuery.matches ? t.hdrDisplay : t.sdrDisplay;
    $.meta.textContent = metaText ? `${metaText} · ${display}` : display;
  }
  function message(text, kind = '') { $.msg.textContent = text; $.msg.dataset.kind = kind; }
  function setBusy(text) {
    busyJob = text;
    $.busy.hidden = !text; $.busyText.textContent = text || '';
    enable();
  }
  function enable() {
    const off = !!busyJob || !opened;
    $.save.disabled = off;
    for (const b of $.view.querySelectorAll('button')) b.disabled = !opened;
    $.file.disabled = !!busyJob;
  }
  function errorText(data) {
    if (data.code === 3) return t.unsupported[data.detail] || t.unsupported[0];
    if (data.code === -1) return t.errors['-1'] + (data.message || '');
    return t.errors[data.code] || t.errors[2];
  }
  // Curve: brightening (log scale, x1 ... x2^stops) over the original's brightness.
  function drawCurve() {
    const c = $.curve, ctx = c.getContext('2d'), dpr = Math.min(window.devicePixelRatio || 1, 3);
    const w = c.clientWidth || 120, h = c.clientHeight || 44;
    if (c.width !== Math.round(w * dpr)) { c.width = Math.round(w * dpr); c.height = Math.round(h * dpr); }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    const style = getComputedStyle(ui);
    ctx.strokeStyle = style.getPropertyValue('--line'); ctx.lineWidth = 1;
    ctx.strokeRect(0.5, 0.5, w - 1, h - 1);
    if (!curveCodes) return;
    const grad = ctx.createLinearGradient(0, 0, w, 0);
    grad.addColorStop(0, '#6c6f78'); grad.addColorStop(1, '#ffe7b0');
    ctx.beginPath();
    for (let i = 0; i < 256; i++) {
      const x = 2 + (w - 4) * i / 255, y = h - 3 - (h - 6) * curveCodes[i] / 255;
      if (i) ctx.lineTo(x, y); else ctx.moveTo(x, y);
    }
    ctx.lineTo(w - 2, h - 2); ctx.lineTo(2, h - 2); ctx.closePath();
    ctx.fillStyle = grad; ctx.globalAlpha = 0.25; ctx.fill(); ctx.globalAlpha = 1;
    ctx.beginPath();
    for (let i = 0; i < 256; i++) {
      const x = 2 + (w - 4) * i / 255, y = h - 3 - (h - 6) * curveCodes[i] / 255;
      if (i) ctx.lineTo(x, y); else ctx.moveTo(x, y);
    }
    ctx.strokeStyle = grad; ctx.lineWidth = 2; ctx.stroke();
    ctx.fillStyle = style.getPropertyValue('--muted'); ctx.font = '10px -apple-system, sans-serif';
    ctx.textAlign = 'right'; ctx.fillText(`${num(2 ** curveStops, 1)}×`, w - 5, 12);
  }
  // Small spinner for waits the user did not ask for (decoding a preview).
  // It appears only after WAIT_DELAY, so quick updates do not flash it, and
  // once visible it stays for at least WAIT_MIN.
  const WAIT_DELAY = 300, WAIT_MIN = 400;
  let waitTimer = 0, waitSince = 0;
  function setWaiting(on) {
    clearTimeout(waitTimer);
    if (on) {
      if (!waitSince) waitTimer = setTimeout(() => { waitSince = performance.now(); $.wait.classList.add('on'); }, WAIT_DELAY);
    } else if (waitSince) {
      waitTimer = setTimeout(() => { waitSince = 0; $.wait.classList.remove('on'); },
        Math.max(0, WAIT_MIN - (performance.now() - waitSince)));
    }
  }
  // Both previews stay decoded and painted in the same grid cell; the one
  // in front covers the other, so switching the view is instant. New frames
  // are decoded before they replace the old ones, so sliders do not flicker.
  // Only one decode runs at a time; frames arriving meanwhile are coalesced.
  const imgs = {hdr: $.imgHdr, sdr: $.imgSdr};
  let decoding = false, frameDirty = false, inFlight = [];
  // A blob URL is revoked once it is neither current, shown nor being decoded.
  function release(url) {
    if (!url || url === urls.hdr || url === urls.sdr || inFlight.includes(url)) return;
    if (Object.values(imgs).some(img => img.getAttribute('src') === url)) return;
    URL.revokeObjectURL(url);
  }
  function front() {
    $.imgHdr.classList.toggle('front', !showOriginal);
    $.imgSdr.classList.toggle('front', showOriginal);
  }
  async function show() {
    frameDirty = true;
    if (decoding) return;
    decoding = true;
    setWaiting(true);
    while (frameDirty && frame) {
      frameDirty = false;
      const want = {...urls}, size = {...frame};
      const kinds = Object.keys(imgs).filter(k => want[k] && imgs[k].getAttribute('src') !== want[k]);
      inFlight = kinds.map(k => want[k]);
      await Promise.all(kinds.map(k => { const p = new Image(); p.src = want[k]; return p.decode().catch(() => {}); }));
      if (opened) {
        const dpr = Math.min(window.devicePixelRatio || 1, 4);
        for (const k of kinds) {
          const img = imgs[k], old = img.getAttribute('src');
          img.src = want[k];
          img.style.width = `${size.width / dpr}px`; img.style.height = `${size.height / dpr}px`;
          img.hidden = false;
          release(old);
        }
      }
      const done = inFlight; inFlight = [];
      done.forEach(release);
    }
    decoding = false;
    setWaiting(false);
  }
  function clearPreview() {
    for (const img of Object.values(imgs)) {
      const old = img.getAttribute('src');
      img.hidden = true; img.removeAttribute('src'); release(old);
    }
  }
  function setUrl(kind, buffer) {
    const old = urls[kind];
    urls[kind] = URL.createObjectURL(new Blob([buffer], {type: 'image/jpeg'}));
    release(old);
  }
  function requestView() {
    const dpr = Math.min(window.devicePixelRatio || 1, 4);
    worker.postMessage({type: 'view', width: Math.round($.stage.clientWidth * dpr), height: Math.round($.stage.clientHeight * dpr)});
  }
  function sendParams() {
    showValues();
    settings = {stops: +$.stops.value, threshold: +$.threshold.value};
    worker.postMessage({type: 'params', ...settings});
  }
  function storeParams() { try { localStorage.setItem(STORE, JSON.stringify(settings)); } catch { /* private mode */ } }

  async function save(buffer) {
    const file = new File([buffer], `${name}-HDR.jpg`, {type: 'image/jpeg'});
    const touch = matchMedia('(pointer: coarse)').matches;
    if (touch && navigator.canShare?.({files: [file]})) {
      try { await navigator.share({files: [file]}); message(t.saved + file.name, 'ok'); return; }
      catch (error) { if (error.name === 'AbortError') { message(notes); return; } }
    }
    const url = URL.createObjectURL(file);
    const a = document.createElement('a');
    a.href = url; a.download = file.name; a.rel = 'noopener';
    document.body.append(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 60000);
    message(t.saved + file.name, 'ok');
  }
  async function openFile(file) {
    if (!file) return;
    if (busyJob) { pendingFile = file; return; }
    setBusy(t.opening); message('');
    name = file.name.replace(/\.[^.]+$/, '') || 'image';
    try {
      const buffer = await file.arrayBuffer();
      worker.postMessage({type: 'open', buffer}, [buffer]);
    } catch (error) { setBusy(null); message(String(error.message || error), 'error'); }
  }

  worker.onmessage = ({data}) => {
    switch (data.type) {
    case 'ready': case 'curve':
      curveCodes = data.curve; if (data.stops) curveStops = data.stops; drawCurve();
      break;
    case 'opened': {
      opened = true; frame = null;
      const i = data.info;
      metaText = `${i.kind === KIND_JPEG ? 'JPEG' : 'PNG'} · ${i.width} × ${i.height}`;
      updateMeta();
      const n = [];
      if (!hdrQuery.matches) n.push(t.noHdr);
      if (i.flags & FLAG_GAINMAP) n.push(t.gainmap);
      if (i.flags & FLAG_ALPHA) n.push(t.alpha);
      n.push(!i.reencodes ? t.keepJpeg : i.kind === KIND_JPEG ? t.grayJpeg : t.pngJpeg);
      notes = n.join(' ');
      message(notes, hdrQuery.matches ? '' : 'warn');
      $.empty.hidden = true;
      setBusy(null);
      requestView();
      break;
    }
    case 'frame': {
      frame = {width: data.width, height: data.height};
      setUrl('hdr', data.hdr);
      if (data.sdr) setUrl('sdr', data.sdr);
      curveCodes = data.curve; curveStops = data.stops; drawCurve();
      show();
      break;
    }
    case 'exported':
      setBusy(null);
      save(data.buffer);
      break;
    case 'error':
      if (data.during !== 'export' && busyJob === t.opening) {
        opened = false; frame = null; clearPreview(); $.empty.hidden = false; metaText = ''; updateMeta();
      }
      setBusy(null);
      message(errorText(data), 'error');
      break;
    }
    if (!busyJob && pendingFile) { const f = pendingFile; pendingFile = null; openFile(f); }
  };
  worker.postMessage({type: 'init', wasm: new URL('engine.wasm', import.meta.url).href});
  sendParams();

  $.file.addEventListener('change', () => { openFile($.file.files[0]); $.file.value = ''; });
  for (const el of [$.stops, $.threshold]) {
    el.addEventListener('input', sendParams);
    el.addEventListener('change', () => { sendParams(); storeParams(); });
  }
  $.view.addEventListener('click', e => {
    const b = e.target.closest('button');
    if (!b || b.disabled) return;
    showOriginal = b.dataset.value === '1';
    for (const x of $.view.querySelectorAll('button')) { const on = x === b; x.classList.toggle('on', on); x.setAttribute('aria-checked', on); }
    front();
  });
  $.view.addEventListener('keydown', e => {
    const step = {ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1}[e.key];
    if (!step) return;
    const buttons = [...$.view.querySelectorAll('button')], i = buttons.indexOf(document.activeElement);
    if (i < 0) return;
    e.preventDefault();
    const next = buttons[(i + step + buttons.length) % buttons.length];
    next.focus(); next.click();
  });
  $.save.addEventListener('click', () => { setBusy(t.saving); worker.postMessage({type: 'export'}); });
  function showTip(on) { $.tip.hidden = !on; $.info.setAttribute('aria-expanded', on); $.info.classList.toggle('on', on); }
  $.info.addEventListener('click', () => showTip($.tip.hidden));
  document.addEventListener('pointerdown', e => { if (!$.tip.hidden && e.target !== $.info && !$.tip.contains(e.target)) showTip(false); });
  document.addEventListener('keydown', e => { if (e.key === 'Escape' && !$.tip.hidden) showTip(false); });
  hdrQuery.addEventListener?.('change', updateMeta);

  const stop = e => { e.preventDefault(); e.stopPropagation(); };
  ui.addEventListener('dragover', e => { stop(e); ui.classList.add('dragging'); });
  ui.addEventListener('dragleave', e => { if (e.target === ui || !ui.contains(e.relatedTarget)) ui.classList.remove('dragging'); });
  ui.addEventListener('drop', e => { stop(e); ui.classList.remove('dragging'); openFile(e.dataTransfer?.files?.[0]); });
  let resizeTimer;
  const observer = new ResizeObserver(() => {
    drawCurve();
    clearTimeout(resizeTimer); resizeTimer = setTimeout(() => opened && requestView(), 150);
  });
  observer.observe($.stage);
  updateMeta(); showValues(); setBusy(null); front();

  return {
    release,
    busy: () => opened || !!busyJob,
    dispose() {
      observer.disconnect(); worker.terminate(); ui.remove(); css.remove();
      for (const u of Object.values(urls)) if (u) URL.revokeObjectURL(u);
    }
  };
}
