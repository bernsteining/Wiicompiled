// Page-side setup for the web build (linked with --pre-js). Before main() runs it:
//   * mounts the game's files at /dvd (disc_fs.js) from, in order of preference: the copy
//     installed in this browser (Origin Private File System); with ?data=<url>, a server that
//     answers HTTP Range requests and serves <url>/manifest.json; or what the player picks: their
//     disc image (.wbfs, .iso, .rvz, ... read by nod in disc_image/disc_worker.js) or an extracted
//     DATA folder. Picked data can be installed (?install installs from a server too);
//     ?uninstall removes the installed copy;
//   * mounts /userdata on IndexedDB (IDBFS) for Config.toml, the NAND and saves, and writes it back
//     periodically and when the page is hidden, after running Module.mkwUserDataLoaded hooks
//     (unlock.js uses one).
// Game data never leaves the machine: picked files are read in place, never uploaded.
//
// The shell page provides #mkw-status and the #mkw-setup panel (see shell.html); without them
// (tests) the loader still works in ?data= mode.

(() => {
  const status = (text) => {
    const element = typeof document !== 'undefined' && document.getElementById('mkw-status');
    if (element) element.textContent = text;
    out(`[web] ${text}`);
  };

  // Files the runtime reads synchronously (sys/fst.bin through std::ifstream) are fetched whole.
  const fetchUpFront = (path) => path.startsWith('sys/');

  // A picked folder may be the DATA directory itself or a parent that contains it.
  async function findDataDirectory(handle) {
    const has = async (dir, name, kind) => {
      try {
        return kind === 'directory' ? await dir.getDirectoryHandle(name) : await dir.getFileHandle(name);
      } catch (e) {
        return null;
      }
    };
    const isData = async (dir) => (await has(dir, 'files', 'directory')) && (await has(dir, 'sys', 'directory'));
    if (await isData(handle)) return handle;
    for (const name of ['DATA', 'data']) {
      const child = await has(handle, name, 'directory');
      if (child && (await isData(child))) return child;
    }
    return null;
  }

  async function entriesFromDirectory(root) {
    const entries = [];
    const walk = async (dir, prefix) => {
      for await (const [name, handle] of dir.entries()) {
        const path = prefix ? `${prefix}/${name}` : name;
        if (handle.kind === 'directory') {
          await walk(handle, path);
        } else {
          const file = await handle.getFile();
          entries.push({
            path,
            size: file.size,
            read: async (offset, length) =>
              new Uint8Array(await file.slice(offset, offset + length).arrayBuffer()),
            data: fetchUpFront(path) ? new Uint8Array(await file.arrayBuffer()) : undefined,
          });
        }
      }
    };
    await walk(root, '');
    return entries;
  }

  // Bytes start..end (inclusive) of a served file. A dropped connection (a Wi-Fi blip) is retried;
  // an HTTP error answer is not.
  async function fetchRange(url, start, end) {
    for (let attempt = 1; ; ++attempt) {
      let response;
      let bytes;
      try {
        response = await fetch(url, { headers: { Range: `bytes=${start}-${end}` } });
        if (response.status === 206) bytes = new Uint8Array(await response.arrayBuffer());
      } catch (e) {
        if (attempt === 4) throw new Error(`${url}: ${e.message || e}`);
        err(`[web] ${url}: ${e.message || e}; retrying`);
        await new Promise((resolve) => setTimeout(resolve, 250 * 2 ** attempt));
        continue;
      }
      if (!bytes) throw new Error(`${url}: expected 206, got ${response.status}`);
      if (bytes.length !== end - start + 1) throw new Error(`${url}: got ${bytes.length} of ${end - start + 1} bytes`);
      return bytes;
    }
  }

  // Disc reads over HTTP go through a cache of aligned blocks: the game makes many small reads and
  // rereads the same ranges (one music file five times a second). Consecutive missing blocks are
  // fetched in one request, and a block already on its way is shared.
  const kBlockSize = 256 << 10;
  const kCacheBytes = 64 << 20;
  const blocks = new Map(); // `${url}#${index}` -> { promise, size }, least recently used first
  let cachedBytes = 0;

  function evictBlocks() {
    for (const [key, block] of blocks) {
      if (cachedBytes <= kCacheBytes) return;
      if (block.size === 0) continue; // still loading
      blocks.delete(key);
      cachedBytes -= block.size;
    }
  }

  function fetchBlocks(url, fileSize, first, last) {
    const run = fetchRange(url, first * kBlockSize, Math.min((last + 1) * kBlockSize, fileSize) - 1);
    for (let index = first; index <= last; ++index) {
      const key = `${url}#${index}`;
      const block = { size: 0 };
      block.promise = run.then((bytes) => {
        const part = bytes.slice((index - first) * kBlockSize, (index - first + 1) * kBlockSize);
        if (blocks.get(key) === block) {
          block.size = part.length;
          cachedBytes += part.length;
          evictBlocks();
        }
        return part;
      }, (error) => {
        if (blocks.get(key) === block) blocks.delete(key);
        throw error;
      });
      block.promise.catch(() => {}); // the reader that asked for it reports the error
      blocks.set(key, block);
    }
  }

  function cachedReader(url, fileSize) {
    return async (offset, length) => {
      const first = Math.floor(offset / kBlockSize);
      const last = Math.floor((offset + length - 1) / kBlockSize);
      for (let index = first, runStart = -1; index <= last + 1; ++index) {
        const missing = index <= last && !blocks.has(`${url}#${index}`);
        if (missing && runStart < 0) runStart = index;
        if (!missing && runStart >= 0) {
          fetchBlocks(url, fileSize, runStart, index - 1);
          runStart = -1;
        }
      }
      const pending = [];
      for (let index = first; index <= last; ++index) {
        const key = `${url}#${index}`;
        const block = blocks.get(key);
        blocks.delete(key); // move to the most recently used end
        blocks.set(key, block);
        pending.push(block.promise);
      }
      const result = new Uint8Array(length);
      (await Promise.all(pending)).forEach((part, i) => {
        const blockStart = (first + i) * kBlockSize;
        const from = Math.max(offset, blockStart);
        const to = Math.min(offset + length, blockStart + part.length);
        result.set(part.subarray(from - blockStart, to - blockStart), from - offset);
      });
      return result;
    };
  }

  async function entriesFromServer(base) {
    base = base.replace(/\/+$/, '');
    const manifest = await (await fetch(`${base}/manifest.json`)).json();
    return Promise.all(manifest.map(async ({ path, size }) => {
      const url = `${base}/${path.split('/').map(encodeURIComponent).join('/')}`;
      const data = fetchUpFront(path) && size > 0 ? await fetchRange(url, 0, size - 1) : undefined;
      return { path, size, read: cachedReader(url, size), data };
    }));
  }

  // The player's disc image (.wbfs, .iso, .rvz, ...), read in place by nod in a worker
  // (disc_image/disc_worker.js), which decrypts the game partition. Works with a plain file input,
  // so in every browser.
  function openDiscImage(file) {
    const worker = new Worker('disc_worker.js');
    const pending = new Map();
    let nextId = 0;
    return new Promise((resolve, reject) => {
      worker.onerror = (e) => reject(new Error(`the disc image reader failed to start: ${e.message || e}`));
      worker.onmessage = ({ data }) => {
        if (data.type === 'log') {
          out(`[disc] ${data.text}`);
        } else if (data.type === 'read') {
          const request = pending.get(data.id);
          pending.delete(data.id);
          request.resolve(data.bytes);
        } else if (data.type === 'error' && pending.has(data.id)) {
          const request = pending.get(data.id);
          pending.delete(data.id);
          request.reject(new Error(data.message));
        } else if (data.type === 'error') {
          reject(new Error(data.message));
        } else if (data.type === 'opened') {
          const { listing, sys } = data;
          const entries = listing.files.map(([path, size], index) => ({
            path,
            size,
            read: (offset, length) => new Promise((resolve, reject) => {
              const id = nextId++;
              pending.set(id, { resolve, reject });
              worker.postMessage({ type: 'read', id, file: index, offset, length });
            }),
          }));
          for (const [path, bytes] of Object.entries(sys)) {
            entries.push({ path, size: bytes.length, read: async (offset, length) => bytes.slice(offset, offset + length), data: bytes });
          }
          out(`[web] disc image: ${listing.gameId} "${listing.title}", ${listing.format}, ${listing.files.length} files`);
          resolve({ entries, gameId: listing.gameId, label: file.name });
        }
      };
      worker.postMessage({ type: 'open', file });
    });
  }

  // An extracted folder from <input webkitdirectory>, for browsers without showDirectoryPicker.
  // Each file's webkitRelativePath starts with the picked folder; DATA is where sys/fst.bin is.
  function entriesFromFileList(fileList) {
    const files = [...fileList];
    const fst = files.find((file) => /(^|\/)sys\/fst\.bin$/.test(file.webkitRelativePath));
    if (!fst) return null;
    const root = fst.webkitRelativePath.slice(0, -'sys/fst.bin'.length);
    const inData = files.filter((file) => file.webkitRelativePath.startsWith(root));
    if (!inData.some((file) => file.webkitRelativePath.startsWith(`${root}files/`))) return null;
    return Promise.all(inData.map(async (file) => {
      const path = file.webkitRelativePath.slice(root.length);
      return {
        path,
        size: file.size,
        read: async (offset, length) => new Uint8Array(await file.slice(offset, offset + length).arrayBuffer()),
        data: fetchUpFront(path) ? new Uint8Array(await file.arrayBuffer()) : undefined,
      };
    }));
  }

  function gameIdOf(entries) {
    const boot = entries.find((entry) => entry.path === 'sys/boot.bin');
    return boot && boot.data ? String.fromCharCode(...boot.data.subarray(0, 6)) : '';
  }

  // Only the clean PAL disc matches the recompiled code.
  function checkGame(source) {
    const gameId = source.gameId || gameIdOf(source.entries);
    if (gameId !== 'RMCP01') {
      throw new Error(`${source.label} is ${gameId || 'not a Wii disc'}; WiiCompiled needs the PAL disc, RMCP01.`);
    }
    return { ...source, gameId };
  }

  // --- The copy installed in this browser -------------------------------------------------------
  // Installing copies the game files into the Origin Private File System (private to this site,
  // never uploaded): later visits start from it without picking anything or a server. Files are
  // stored flat (f0, f1, ...) with manifest.json, written last, mapping them to their paths: a copy
  // without a manifest is an interrupted install and is not used.
  const kInstallDir = 'wiicompiled-data';
  const kInstallChunk = 4 << 20;
  const canInstall = () => typeof navigator !== 'undefined' && !!navigator.storage?.getDirectory;

  async function readInstalled() {
    if (!canInstall()) return null;
    try {
      const dir = await (await navigator.storage.getDirectory()).getDirectoryHandle(kInstallDir);
      const file = await (await dir.getFileHandle('manifest.json')).getFile();
      return { dir, manifest: JSON.parse(await file.text()) };
    } catch (e) {
      return null;
    }
  }

  async function removeInstalled() {
    if (!canInstall()) return;
    await (await navigator.storage.getDirectory()).removeEntry(kInstallDir, { recursive: true }).catch(() => {});
  }

  function entriesFromInstalled({ dir, manifest }) {
    return Promise.all(manifest.files.map(async ([path, size, name]) => {
      let file = null;
      const read = async (offset, length) => {
        file ??= (await dir.getFileHandle(name)).getFile();
        return new Uint8Array(await (await file).slice(offset, offset + length).arrayBuffer());
      };
      return { path, size, read, data: fetchUpFront(path) ? await read(0, size) : undefined };
    }));
  }

  // Writes happen in a worker: synchronous access handles (the OPFS write API every browser with
  // OPFS has, Safari included) only exist there.
  const kWriterSource = `
    let dir = null, handle = null;
    onmessage = async ({ data }) => {
      try {
        if (data.type === 'begin') {
          dir = await (await navigator.storage.getDirectory()).getDirectoryHandle(data.dir, { create: true });
        } else if (data.type === 'open') {
          handle = await (await dir.getFileHandle(data.name, { create: true })).createSyncAccessHandle();
          handle.truncate(0);
        } else if (data.type === 'write') {
          if (handle.write(data.bytes, { at: data.at }) !== data.bytes.length) throw new Error('short write');
        } else if (data.type === 'close') {
          handle.flush();
          handle.close();
          handle = null;
        }
        postMessage({ id: data.id });
      } catch (e) {
        postMessage({ id: data.id, error: (e && e.message) || String(e) });
      }
    };`;

  async function install(entries, gameId) {
    if (!canInstall()) throw new Error('this browser cannot store files for a site (no Origin Private File System)');
    const total = entries.reduce((sum, entry) => sum + entry.size, 0);
    const gigabytes = (bytes) => (bytes / 1e9).toFixed(1);
    const estimate = await navigator.storage.estimate?.().catch(() => null);
    if (estimate && estimate.quota - estimate.usage < total) {
      throw new Error(`not enough storage to install: the game needs ${gigabytes(total)} GB and this browser ` +
                      `allows this site ${gigabytes(estimate.quota - estimate.usage)} GB more`);
    }
    const persisted = await navigator.storage.persist?.().catch(() => false);
    out(`[web] installing ${entries.length} files (${gigabytes(total)} GB); storage ${persisted ? 'persistent' : 'not marked persistent (the browser may clear it)'}`);
    await removeInstalled();
    const writer = new Worker(URL.createObjectURL(new Blob([kWriterSource], { type: 'text/javascript' })));
    const waiting = new Map();
    let nextId = 0;
    writer.onmessage = ({ data }) => {
      const request = waiting.get(data.id);
      waiting.delete(data.id);
      if (data.error) request.reject(new Error(`storing the game failed: ${data.error}`));
      else request.resolve();
    };
    const call = (message) => new Promise((resolve, reject) => {
      const id = nextId++;
      waiting.set(id, { resolve, reject });
      writer.postMessage({ ...message, id });
    });
    const started = performance.now();
    let done = 0;
    let shownPercent = -1;
    const files = [];
    try {
      await call({ type: 'begin', dir: kInstallDir });
      for (const [index, entry] of entries.entries()) {
        const name = `f${index}`;
        await call({ type: 'open', name });
        for (let offset = 0; offset < entry.size; offset += kInstallChunk) {
          const bytes = await entry.read(offset, Math.min(kInstallChunk, entry.size - offset));
          await call({ type: 'write', bytes, at: offset });
          done += bytes.length;
          const percent = Math.floor((100 * done) / total);
          if (percent !== shownPercent) {
            shownPercent = percent;
            const seconds = (performance.now() - started) / 1000;
            Module['mkwProgress']?.(`Installing in this browser: ${gigabytes(done)} / ${gigabytes(total)} GB (${percent}%)`);
            if (percent % 10 === 0) out(`[web] install ${percent}% after ${seconds.toFixed(0)} s (${(done / 1e6 / seconds).toFixed(0)} MB/s)`);
          }
        }
        await call({ type: 'close' });
        files.push([entry.path, entry.size, name]);
      }
      const manifest = new TextEncoder().encode(JSON.stringify({ version: 1, gameId, files }));
      await call({ type: 'open', name: 'manifest.json' });
      await call({ type: 'write', bytes: manifest, at: 0 });
      await call({ type: 'close' });
    } finally {
      writer.terminate();
    }
    Module['mkwProgress']?.('');
    out(`[web] installed in ${((performance.now() - started) / 1000).toFixed(0)} s`);
  }

  // --- Choosing the game data -------------------------------------------------------------------
  // The setup panel (shell.html): a disc image through a plain file input, an extracted folder
  // where the browser can pick one, and whether to install.
  function pickSource() {
    const element = (id) => document.getElementById(id);
    const panel = element('mkw-setup');
    const imageInput = element('mkw-image-input');
    const folderInput = element('mkw-folder-input');
    const installBox = element('mkw-install');
    element('mkw-install-row').hidden = !canInstall();
    // iPadOS reports itself as a Mac; its file picker cannot choose folders.
    const touchApple = /Macintosh|iPhone|iPad/.test(navigator.userAgent) && navigator.maxTouchPoints > 1;
    const canPickFolder = typeof showDirectoryPicker === 'function' || ('webkitdirectory' in folderInput && !touchApple);
    element('mkw-pick-folder').hidden = !canPickFolder;
    panel.hidden = false;
    status('Choose your own copy of Mario Kart Wii (PAL, RMCP01).');
    return new Promise((resolve) => {
      const done = (promise) => {
        panel.hidden = true;
        promise.then(checkGame).then((source) => resolve({ ...source, install: canInstall() && installBox.checked }), (error) => {
          status(`${error.message || error} Choose again.`);
          panel.hidden = false;
        });
      };
      const openImage = (file) => {
        status(`Opening ${file.name}...`);
        done(openDiscImage(file).catch((e) => { throw new Error(`${file.name}: ${e.message}.`); }));
      };
      element('mkw-pick-image').onclick = () => imageInput.click();
      imageInput.onchange = () => {
        const file = imageInput.files[0];
        imageInput.value = '';
        if (file) openImage(file);
      };
      // A disc image dropped anywhere on the page while the panel is up.
      const onDragOver = (event) => {
        if (panel.hidden) return;
        event.preventDefault();
        event.dataTransfer.dropEffect = 'copy';
      };
      const onDrop = (event) => {
        if (panel.hidden) return;
        event.preventDefault();
        const file = event.dataTransfer.files[0];
        if (file) openImage(file);
      };
      addEventListener('dragover', onDragOver);
      addEventListener('drop', onDrop);
      element('mkw-pick-folder').onclick = async () => {
        if (typeof showDirectoryPicker !== 'function') {
          folderInput.click();
          return;
        }
        let picked;
        try {
          picked = await showDirectoryPicker({ id: 'mkw-data', mode: 'read' });
        } catch (e) {
          return; // cancelled
        }
        status('Indexing game files...');
        done(findDataDirectory(picked).then((data) => {
          if (!data) throw new Error(`"${picked.name}" has no files/ and sys/ folders.`);
          return entriesFromDirectory(data).then((entries) => ({ entries, label: picked.name }));
        }));
      };
      folderInput.onchange = () => {
        const files = folderInput.files;
        status('Indexing game files...');
        const entries = entriesFromFileList(files);
        folderInput.value = '';
        done(entries ? entries.then((list) => ({ entries: list, label: 'folder' }))
                     : Promise.reject(new Error('That folder has no extracted game (files/ and sys/fst.bin).')));
      };
    });
  }

  async function chooseDataEntries() {
    const query = typeof location !== 'undefined' ? new URLSearchParams(location.search) : new URLSearchParams();
    if (query.has('uninstall')) {
      await removeInstalled();
      out('[web] ?uninstall: removed the copy installed in this browser');
    }
    const installed = query.has('uninstall') ? null : await readInstalled();
    if (installed && !query.has('install')) {
      status(`Using the copy installed in this browser (${installed.manifest.gameId}, ${installed.manifest.files.length} files).`);
      return entriesFromInstalled(installed);
    }
    let source;
    const server = query.get('data');
    if (server) {
      status(`Reading game data from ${server}`);
      source = { entries: await entriesFromServer(server), label: server, install: query.has('install') };
    } else if (typeof document !== 'undefined' && document.getElementById('mkw-setup')) {
      source = await pickSource();
    } else {
      throw new Error('no game data: use ?data=<url>');
    }
    source = checkGame(source);
    if (!source.install) return source.entries;
    try {
      await install(source.entries, source.gameId);
    } catch (e) {
      err(`[web] install failed: ${e.message || e}`);
      Module['mkwNotice']?.(`Installing failed (${e.message || e}); playing from ${source.label} instead.`);
      await removeInstalled();
      return source.entries;
    }
    return entriesFromInstalled(await readInstalled());
  }

  // One IDBFS sync at a time: the timer, page-hide events and startup hooks can overlap.
  let syncing = false;
  let syncAgain = false;
  // Set when the stored user data did not load in time and the game started without it.
  let userDataOff = false;
  function persistUserData() {
    if (userDataOff) return;
    if (syncing) {
      syncAgain = true;
      return;
    }
    syncing = true;
    FS.syncfs(false, (error) => {
      syncing = false;
      if (error) err(`[web] saving /userdata failed: ${error}`);
      if (syncAgain) {
        syncAgain = false;
        persistUserData();
      }
    });
  }

  // Diagnostics for when IndexedDB does not answer: what opening the /userdata database (IDBFS
  // names it after the mount point) and a scratch database actually do.
  function probeIndexedDB() {
    if (typeof indexedDB === 'undefined') {
      err('[web] IndexedDB probe: this browser has no indexedDB');
      return;
    }
    for (const [name, version] of [['/userdata', undefined], ['mkw-probe', 1]]) {
      const request = version ? indexedDB.open(name, version) : indexedDB.open(name);
      for (const event of ['success', 'error', 'blocked', 'upgradeneeded']) {
        request.addEventListener(event, () => {
          const db = event === 'success' ? request.result : null;
          err(`[web] IndexedDB probe ${name}: ${event}` + (request.error ? ` ${request.error}` : '') +
              (db ? ` version ${db.version}, stores [${[...db.objectStoreNames]}]` : ''));
          if (db) db.close();
        });
      }
    }
    navigator.storage?.estimate?.().then((estimate) => err(`[web] storage estimate ${JSON.stringify(estimate)}`));
    // Which read of the file store answers: a plain count, the timestamp index's key cursor (what
    // IDBFS lists stored files with) and getAllKeys, each in its own read-only transaction.
    const reopen = indexedDB.open('/userdata');
    reopen.onsuccess = () => {
      const db = reopen.result;
      const reads = {
        count: (store) => store.count(),
        keyCursor: (store) => store.index('timestamp').openKeyCursor(),
        getAllKeys: (store) => store.getAllKeys(),
      };
      for (const [name, start] of Object.entries(reads)) {
        let request;
        try {
          request = start(db.transaction(['FILE_DATA'], 'readonly').objectStore('FILE_DATA'));
        } catch (e) {
          err(`[web] IndexedDB probe ${name}: threw ${e}`);
          continue;
        }
        const silent = setTimeout(() => err(`[web] IndexedDB probe ${name}: no answer after 5 s`), 5000);
        let answered = false;
        request.onsuccess = () => {
          clearTimeout(silent);
          if (answered) return;
          answered = true;
          const result = request.result;
          err(`[web] IndexedDB probe ${name}: ` + (name === 'keyCursor'
            ? (result ? `first key ${result.primaryKey}` : 'empty')
            : (Array.isArray(result) ? `${result.length} keys` : String(result))));
        };
        request.onerror = () => {
          clearTimeout(silent);
          err(`[web] IndexedDB probe ${name}: error ${request.error}`);
        };
      }
    };
  }

  // With ?log, trace each step of IDBFS's load (diagnostics for browsers where it stalls).
  function traceIdbfs() {
    for (const name of ['getLocalSet', 'getRemoteSet', 'reconcile', 'loadRemoteEntry', 'storeLocalEntry', 'getDB']) {
      const original = IDBFS[name];
      if (typeof original !== 'function') continue;
      let calls = 0;
      IDBFS[name] = (...args) => {
        const call = ++calls;
        const callback = args.pop();
        if (call <= 3) out(`[web] IDBFS.${name} #${call} started`);
        return original(...args, (error, result) => {
          if (call <= 3 || error) {
            const entries = result && result.entries ? ` (${Object.keys(result.entries).length} entries)` : '';
            out(`[web] IDBFS.${name} #${call} finished${error ? `: ${error}` : ''}${entries}`);
          }
          callback(error, result);
        });
      };
    }
  }

  // ?lowres: render at the Wii's own resolution ([video] resolution_multiplier = 1 in each
  // application's Config.toml; a missing file gets the runtime's default, which is 1 too).
  function setLowResolution() {
    for (const name of FS.readdir('/userdata').filter((entry) => entry[0] !== '.')) {
      const path = `/userdata/${name}/Config.toml`;
      if (!FS.analyzePath(path).exists) continue;
      const text = FS.readFile(path, { encoding: 'utf8' });
      const line = /^([ \t]*resolution_multiplier[ \t]*=)[^\n]*$/m;
      const updated = line.test(text) ? text.replace(line, '$1 1.0')
        : /^\[video\][ \t]*$/m.test(text) ? text.replace(/^\[video\][ \t]*$/m, '[video]\nresolution_multiplier = 1.0')
        : `${text}\n[video]\nresolution_multiplier = 1.0\n`;
      if (updated !== text) {
        FS.writeFile(path, updated);
        out(`[web] ?lowres: resolution_multiplier = 1.0 in ${path}`);
      }
    }
  }

  // Which pipeline seed aurora compiles at startup. A compile Safari has not cached costs it
  // ~100-300 ms with the page stalled (measured on an iPad, iPadOS 27.2), and its cache does not
  // survive quitting Safari: the full web seed would add minutes to such a start, so WebKit gets
  // the native seed and learns the rest as it plays. Chrome compiles in parallel off the page (the
  // full seed: ~14 s on a desktop) and gets the web seed, so nothing compiles mid-race.
  // ?seed=native or ?seed=web overrides.
  function choosePipelineSeed() {
    const asked = typeof location !== 'undefined' ? new URLSearchParams(location.search).get('seed') : null;
    const agent = typeof navigator !== 'undefined' ? navigator.userAgent : '';
    const webkitOnly = /AppleWebKit/.test(agent) && !/Chrome|Chromium|Edg\//.test(agent);
    const seed = asked === 'native' || asked === 'web' ? asked : webkitOnly ? 'native' : 'web';
    FS.writeFile('/initial_pipeline_cache.db', FS.readFile(`/app/pipeline/${seed}.db`));
    const params = new URLSearchParams(location.search);
    // aurora's ubershader (lib/gx/ubershader.hpp). ?uber (or ?uber=hybrid): a draw whose specialised
    // pipeline has not compiled yet uses it instead of being skipped or stalling the page.
    // ?uber=all: every draw it covers uses it (fewest compiles; ~2.5x the GPU time on an iPad).
    const uber = params.has('uber') ? params.get('uber') || 'hybrid' : 'off';
    if (uber === 'hybrid' || uber === 'all') Module['mkwUbershader'] = uber;
    // Hybrid only: Safari stalls the page for every compile, so its missing specialised pipelines
    // wait for a loading screen (frames with few draws); ?defer=0 or ?defer=1 overrides.
    const defer = params.has('defer') ? params.get('defer') !== '0' : webkitOnly;
    if (defer) Module['mkwDeferPipelines'] = true;
    // How many asynchronous compiles aurora keeps going: Safari stalls the page for each, so a
    // few at a time keep the screen moving between them; ?inflight=N overrides.
    const inFlight = Number(params.get('inflight')) || (webkitOnly ? 4 : 0);
    if (inFlight) Module['mkwPipelinesInFlight'] = inFlight;
    out(`[web] pipeline seed: ${seed}${asked ? ' (?seed)' : webkitOnly ? ' (WebKit)' : ''}` +
        (inFlight ? `, ${inFlight} compiles in flight` : '') +
        (Module['mkwUbershader'] ? `, ubershader ${Module['mkwUbershader']}` +
         (Module['mkwUbershader'] === 'hybrid' && defer ? ' (specialised compiles deferred)' : '') : ''));
  }

  // ?resetpipelines: forget the pipelines this browser learned (aurora's Cache/pipeline_cache.db
  // in each application's /userdata folder); the seed is merged back in at startup.
  function resetPipelineCache() {
    for (const name of FS.readdir('/userdata').filter((entry) => entry[0] !== '.')) {
      for (const suffix of ['', '-wal', '-shm', '-journal']) {
        const path = `/userdata/${name}/Cache/pipeline_cache.db${suffix}`;
        if (FS.analyzePath(path).exists) {
          FS.unlink(path);
          out(`[web] ?resetpipelines: removed ${path}`);
        }
      }
    }
  }

  // Just before main(), once every preloaded file is in place.
  const runtimeInitialized = Module['onRuntimeInitialized'];
  Module['onRuntimeInitialized'] = () => {
    choosePipelineSeed();
    if (typeof location !== 'undefined' && new URLSearchParams(location.search).has('resetpipelines')) {
      resetPipelineCache();
    }
    runtimeInitialized?.();
  };

  Module['preRun'] = [...(Module['preRun'] || []), () => {
    if (typeof location !== 'undefined' && new URLSearchParams(location.search).has('log')) traceIdbfs();
    if (typeof location !== 'undefined' && new URLSearchParams(location.search).has('lowres')) {
      Module['mkwUserDataLoaded'] = [setLowResolution, ...(Module['mkwUserDataLoaded'] || [])];
    }
    addRunDependency('mkw-userdata');
    FS.mkdir('/userdata');
    FS.mount(IDBFS, {}, '/userdata');
    const loadStarted = performance.now();
    // A browser can leave IndexedDB waiting forever: Safari keeps frozen copies of the page whose
    // unfinished write locks the store. Start the game without saves rather than hang, and make
    // sure a load that answers later cannot replace files under the running game.
    const reconcile = IDBFS.reconcile;
    IDBFS.reconcile = (src, dst, callback) => (userDataOff ? callback(null) : reconcile(src, dst, callback));
    const slowLoad = setTimeout(() => {
      userDataOff = true;
      err('[web] /userdata (IndexedDB) has not loaded after 8 s; starting without saved data');
      Module['mkwNotice']?.('Saves are off for this session: the browser\'s storage did not answer. ' +
        'Close other tabs running the game (or restart the browser), then reload.');
      if (new URLSearchParams(location.search).has('log')) probeIndexedDB();
      removeRunDependency('mkw-userdata');
    }, 8000);
    syncing = true; // nothing is written back before the stored data is loaded
    FS.syncfs(true, (error) => {
      clearTimeout(slowLoad);
      if (userDataOff) {
        out('[web] /userdata answered after the game started without it; ignored');
        return;
      }
      syncing = false;
      syncAgain = false;
      if (error) {
        err(`[web] loading /userdata failed: ${error}`);
      } else {
        out(`[web] /userdata loaded in ${Math.round(performance.now() - loadStarted)} ms`);
        // Other --pre-js files (unlock.js) can adjust user data here, before the game reads it.
        const hooks = Module['mkwUserDataLoaded'] || [];
        for (const hook of hooks) {
          try {
            hook();
          } catch (e) {
            err(`[web] user data hook failed: ${e.stack || e}`);
          }
        }
        if (hooks.length) persistUserData();
      }
      removeRunDependency('mkw-userdata');
    });
    if (typeof document !== 'undefined') {
      setInterval(persistUserData, 5000);
      document.addEventListener('visibilitychange', () => {
        if (document.visibilityState === 'hidden') persistUserData();
      });
      addEventListener('pagehide', persistUserData);
      // An unload handler keeps this page out of the back/forward cache. Safari otherwise keeps a
      // left or reloaded copy frozen with its IndexedDB transaction open, and that lock stalls the
      // next copy's /userdata load (see the 8 s fallback above).
      addEventListener('unload', () => {});
    }

    addRunDependency('mkw-disc');
    chooseDataEntries().then((entries) => {
      FS.mkdir('/dvd');
      FS.mount(Module['MKWDISCFS'], { entries }, '/dvd');
      status(`Game data ready (${entries.length} files).`);
      removeRunDependency('mkw-disc');
    }, (error) => {
      status(`Cannot read game data: ${error.message || error}`);
      abort(error);
    });
  }];
})();
