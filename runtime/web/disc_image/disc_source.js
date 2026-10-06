// Emscripten JS library for mkw_disc (src/lib.rs): the disc image is the File the page picked,
// read synchronously with FileReaderSync, which only exists in workers (disc_worker.js).
addToLibrary({
  mkw_disc_source_read: (offset, out, len) => {
    try {
      const file = Module['mkwDiscFile'];
      const end = Math.min(offset + len, file.size);
      if (offset >= end) return 0;
      const bytes = new Uint8Array(Module['mkwReader'].readAsArrayBuffer(file.slice(offset, end)));
      HEAPU8.set(bytes, out);
      Module['mkwSourceBytes'] += bytes.length;
      return bytes.length;
    } catch (e) {
      err(`reading the disc image at ${offset} failed: ${e}`);
      return -1;
    }
  },
  mkw_disc_source_size: () => Module['mkwDiscFile'].size,
});
