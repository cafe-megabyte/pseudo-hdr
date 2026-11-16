// Build-time smoke test of engine.wasm in JavaScriptCore (no sample files needed).
(async () => {
  const {instance} = await WebAssembly.instantiate(moduleBytes, {});
  const e = instance.exports;
  if (e.abi_version() !== 1) throw new Error('ABI version');
  const junk = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]);
  const p = e.mem_alloc(junk.length);
  new Uint8Array(e.memory.buffer).set(junk, p);
  if (e.image_open(p, junk.length) !== 2) throw new Error('Garbage must be rejected as a format error');
  if (e.preview_render(100, 100) !== 5 || e.export_jpeg() !== 5) throw new Error('State errors expected without an image');
  if (e.params_set(2, 0.6) || Math.abs(e.params_stops() - 2) > 1e-12) throw new Error('Parameters');
  report('WASM smoke test passed');
  globalThis.testDone = true;
})().catch(error => { report('WASM smoke test failed: ' + error); globalThis.testFailed = true; globalThis.testDone = true; });
