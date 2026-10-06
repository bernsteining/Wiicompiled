// Web Worker that reads the player's own disc image in place: nod (mkw_disc.js/.wasm, built from
// src/lib.rs) opens the File the page picked and decrypts the game partition. The page
// (runtime/web/loader.js) talks to it with messages:
//   {type: 'open', file}               -> {type: 'opened', listing, sys} or {type: 'error', message}
//      listing: {gameId, title, format, files: [[path, size], ...]}; a file's id is its index.
//      sys: {'sys/boot.bin': Uint8Array, ...}, the files an extracted DATA/sys folder holds.
//   {type: 'read', id, file, offset, length} -> {type: 'read', id, bytes} or {type: 'error', id, message}
importScripts('mkw_disc.js');

const sysFiles = ['sys/boot.bin', 'sys/bi2.bin', 'sys/apploader.img', 'sys/main.dol', 'sys/fst.bin'];
let module = null;
let scratch = 0;
let scratchSize = 0;

const ready = createMkwDisc({
  mkwReader: new FileReaderSync(),
  mkwSourceBytes: 0,
  print: (text) => postMessage({ type: 'log', text }),
  printErr: (text) => postMessage({ type: 'log', text }),
});

function bytesAt(pointer, length) {
  return module.HEAPU8.slice(pointer, pointer + length);
}

function lastError() {
  const lengthPointer = module._malloc(4);
  const pointer = module._mkw_disc_error(lengthPointer);
  const text = new TextDecoder().decode(bytesAt(pointer, module.HEAPU32[lengthPointer >> 2]));
  module._free(lengthPointer);
  return text;
}

function open(file) {
  module.mkwDiscFile = file;
  if (module._mkw_disc_open() !== 0) throw new Error(lastError());
  const lengthPointer = module._malloc(4);
  const take = (pointer) => bytesAt(pointer, module.HEAPU32[lengthPointer >> 2]);
  const listing = JSON.parse(new TextDecoder().decode(take(module._mkw_disc_listing(lengthPointer))));
  const sys = {};
  sysFiles.forEach((path, which) => { sys[path] = take(module._mkw_disc_sys(which, lengthPointer)); });
  module._free(lengthPointer);
  return { listing, sys };
}

function read(file, offset, length) {
  if (length > scratchSize) {
    if (scratch) module._free(scratch);
    scratchSize = Math.max(length, 1 << 20);
    scratch = module._malloc(scratchSize);
  }
  const started = performance.now();
  const before = module.mkwSourceBytes;
  const count = module._mkw_disc_read(file, offset, scratch, length);
  if (count < 0) throw new Error(lastError());
  return { bytes: bytesAt(scratch, count), ms: performance.now() - started, imageBytes: module.mkwSourceBytes - before };
}

onmessage = async ({ data }) => {
  try {
    module = await ready;
    if (data.type === 'open') {
      const { listing, sys } = open(data.file);
      postMessage({ type: 'opened', listing, sys });
    } else if (data.type === 'read') {
      const { bytes, ms, imageBytes } = read(data.file, data.offset, data.length);
      postMessage({ type: 'read', id: data.id, bytes, ms, imageBytes }, [bytes.buffer]);
    }
  } catch (e) {
    postMessage({ type: 'error', id: data.id, message: e.message || String(e) });
  }
};
