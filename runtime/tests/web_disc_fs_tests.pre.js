// Mounts a synthetic DATA directory on MKWDISCFS for web_disc_fs_tests.cpp. File k's byte i is
// ((i * 2654435761) >>> 24) ^ (k * 31), the same formula the C++ side checks against. Large files
// are only readable asynchronously, through a delayed promise like a real disk or network read.
(() => {
  const bytes = (file, offset, length) => {
    const out = new Uint8Array(length);
    for (let i = 0; i < length; ++i) {
      out[i] = ((Math.imul(offset + i, 2654435761 | 0) >>> 24) ^ (file * 31)) & 0xff;
    }
    return out;
  };
  const asyncFile = (file, path, size) => ({
    path,
    size,
    read: (offset, length) =>
      new Promise((resolve) => setTimeout(() => resolve(bytes(file, offset, length)), 1)),
  });
  const entries = [
    { path: 'sys/fst.bin', size: 64, data: bytes(4, 0, 64) },
    asyncFile(1, 'files/a.bin', 300000),
    asyncFile(2, 'files/dir/b.bin', 1234),
    asyncFile(3, 'files/empty.bin', 0),
  ];
  Module['preRun'] = [() => {
    FS.mkdir('/dvd');
    FS.mount(Module['MKWDISCFS'], { entries }, '/dvd');
  }];
})();
