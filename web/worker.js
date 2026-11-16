// Runs the WASM engine off the main thread. Preview requests are coalesced:
// while one is computed, only the newest slider position is kept.
'use strict';
let wasm, ready;
let viewW = 1024, viewH = 768, dirty = false, scheduled = false, open = false, sentSize = '';

function bytes() { return new Uint8Array(wasm.memory.buffer); }
function copy(ptr, size) { return bytes().slice(ptr, ptr + size).buffer; }
function curve() { return Array.from(bytes().subarray(wasm.params_curve(), wasm.params_curve() + 256)); }
function info() {
  return {
    width: wasm.image_width(), height: wasm.image_height(), kind: wasm.image_kind(), flags: wasm.image_flags(),
    reencodes: !!wasm.image_reencodes(), stops: wasm.params_stops(), threshold: wasm.params_threshold()
  };
}
function schedule() {
  dirty = true;
  if (scheduled) return;
  scheduled = true;
  setTimeout(render, 0);
}
function render() {
  scheduled = false;
  if (!dirty || !open) return;
  dirty = false;
  const r = wasm.preview_render(viewW, viewH);
  if (r) { postMessage({type: 'error', code: r, detail: 0}); return; }
  const width = wasm.preview_width(), height = wasm.preview_height();
  const hdr = copy(wasm.preview_hdr_address(), wasm.preview_hdr_size());
  // The SDR preview only changes with the preview size.
  const size = `${width}x${height}`;
  const sdr = size !== sentSize ? copy(wasm.preview_sdr_address(), wasm.preview_sdr_size()) : null;
  sentSize = size;
  const transfer = sdr ? [hdr, sdr] : [hdr];
  postMessage({type: 'frame', width, height, hdr, sdr, curve: curve(), stops: wasm.params_stops()}, transfer);
}

async function init(url) {
  const response = await fetch(url);
  const {instance} = await WebAssembly.instantiate(await response.arrayBuffer(), {});
  wasm = instance.exports;
  if (wasm.abi_version() !== 1) throw new Error('Engine version mismatch');
}

self.onmessage = async ({data}) => {
  try {
    if (data.type === 'init') { ready = init(data.wasm); await ready; postMessage({type: 'ready', curve: curve()}); return; }
    await ready;
    switch (data.type) {
    case 'open': {
      open = false; sentSize = '';
      wasm.image_close();
      const src = new Uint8Array(data.buffer);
      const ptr = wasm.mem_alloc(src.length);
      if (!ptr) { postMessage({type: 'error', code: 1, detail: 0}); return; }
      bytes().set(src, ptr);
      const r = wasm.image_open(ptr, src.length);
      if (r) { postMessage({type: 'error', code: r, detail: wasm.image_error_detail()}); return; }
      open = true;
      postMessage({type: 'opened', info: info()});
      schedule();
      break;
    }
    case 'view':
      viewW = Math.max(16, Math.min(4096, data.width | 0)); viewH = Math.max(16, Math.min(4096, data.height | 0));
      schedule();
      break;
    case 'params': {
      const r = wasm.params_set(data.stops, data.threshold);
      if (r) { postMessage({type: 'error', code: r, detail: 0}); return; }
      if (open) schedule(); else postMessage({type: 'curve', curve: curve(), stops: wasm.params_stops()});
      break;
    }
    case 'export': {
      const r = wasm.export_jpeg();
      if (r) { postMessage({type: 'error', code: r, detail: 0, during: 'export'}); return; }
      const out = copy(wasm.export_address(), wasm.export_size());
      wasm.export_release();
      postMessage({type: 'exported', buffer: out}, [out]);
      break;
    }
    }
  } catch (error) {
    postMessage({type: 'error', code: -1, detail: 0, message: String(error?.message || error)});
  }
};
