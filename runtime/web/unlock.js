// Linked with --pre-js next to loader.js. With ?unlock in the page address it unlocks every cup,
// character, vehicle and Mirror in each license of the Mario Kart Wii save, once the loader has
// loaded /userdata and before the game reads the save.
//
// rksys.dat layout (https://mkwiiki.org/wiki/Rksys.dat): four 0x8CC0-byte "RKPD" licenses from
// 0x08, unlock flags in bytes 0x30-0x37 of each (the bits not named there are unused by the
// game's unlock checks), and a CRC-32 of bytes 0..0x27FFB stored big-endian at 0x27FFC (the wiki
// says little-endian; real saves are big-endian).

(() => {
  if (typeof location === 'undefined' || !new URLSearchParams(location.search).has('unlock')) return;

  function crc32(bytes) {
    let crc = ~0;
    for (const byte of bytes) {
      crc ^= byte;
      for (let bit = 0; bit < 8; ++bit) crc = (crc >>> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc >>> 0;
  }

  // Returns whether anything changed.
  function unlockSave(save) {
    const magic = (offset) => String.fromCharCode(...save.subarray(offset, offset + 4));
    if (save.length !== 0x2BC000 || magic(0) !== 'RKSD') return false;
    let changed = false;
    for (let base = 0x08; base < 0x08 + 4 * 0x8CC0; base += 0x8CC0) {
      if (magic(base) !== 'RKPD') continue;
      for (let offset = base + 0x30; offset < base + 0x38; ++offset) {
        changed ||= save[offset] !== 0xFF;
        save[offset] = 0xFF;
      }
    }
    if (changed) new DataView(save.buffer, save.byteOffset).setUint32(0x27FFC, crc32(save.subarray(0, 0x27FFC)));
    return changed;
  }

  // The save of each application under /userdata, in the runtime's managed NAND
  // (RuntimeNandPath::ManagedNandRootPath, nand_path.h; PAL title 00010004/524d4350). A nand_root
  // set in Config.toml points elsewhere and is left alone.
  function unlockSaves() {
    const saves = FS.readdir('/userdata').filter((name) => name[0] !== '.')
      .map((name) => `/userdata/${name}/NAND/title/00010004/524d4350/data/rksys.dat`)
      .filter((path) => FS.analyzePath(path).exists);
    if (!saves.length) out('[web] ?unlock: no save yet. Create a license, then reload the page.');
    for (const path of saves) {
      const save = FS.readFile(path);
      // Keep the save from before the first unlock, outside the NAND, so it can be restored.
      const backup = path.replace(/\/NAND\/.*$/, '/rksys-before-unlock.dat');
      if (!FS.analyzePath(backup).exists) FS.writeFile(backup, save);
      if (unlockSave(save)) {
        FS.writeFile(path, save);
        out(`[web] ?unlock: unlocked everything in ${path}`);
      }
    }
  }

  Module['mkwUserDataLoaded'] = [...(Module['mkwUserDataLoaded'] || []), unlockSaves];
})();
