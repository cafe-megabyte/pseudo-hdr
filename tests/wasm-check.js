// Runs every case of build/internal/tests/cases.txt (written by core-check)
// with engine.wasm and requires byte-identical results: HDR export and preview.
(async () => {
  const {instance} = await WebAssembly.instantiate(moduleBytes, {});
  const e = instance.exports;
  const mem = () => new Uint8Array(e.memory.buffer);
  const dir = 'build/internal/tests/';
  function same(label, expectedPath, ptr, size) {
    const expected = readBytes(dir + expectedPath), actual = mem().subarray(ptr, ptr + size);
    if (!expected) throw new Error('Missing native reference ' + expectedPath);
    if (expected.length !== actual.length) throw new Error(`${label}: size ${actual.length} != ${expected.length}`);
    for (let i = 0; i < actual.length; i++)
      if (actual[i] !== expected[i]) throw new Error(`${label}: first difference at byte ${i}`);
  }
  const cases = readText(dir + 'cases.txt').trim().split('\n');
  for (const line of cases) {
    const [path, stops, threshold, base] = line.split('|');
    const data = readBytes(path);
    if (!data) throw new Error('Missing ' + path);
    const p = e.mem_alloc(data.length);
    mem().set(data, p);
    const r = e.image_open(p, data.length);
    if (r) throw new Error(`open ${path}: ${r}/${e.image_error_detail()}`);
    if (e.params_set(Number(stops), Number(threshold))) throw new Error('params');
    if (e.export_jpeg()) throw new Error(`${base}: export`);
    same(`${base} export`, `${base}-HDR.jpg`, e.export_address(), e.export_size());
    e.export_release();
    if (e.preview_render(640, 480)) throw new Error(`${base}: preview`);
    same(`${base} preview`, `${base}-preview.jpg`, e.preview_hdr_address(), e.preview_hdr_size());
  }
  report(`WASM check: ${cases.length} exports and previews identical to native`);
  e.image_close();
  globalThis.testDone = true;
})().catch(error => { report('WASM check failed: ' + error); globalThis.testFailed = true; globalThis.testDone = true; });
