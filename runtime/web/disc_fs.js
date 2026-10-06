// Read-only file system for the game's extracted DATA directory on the web build.
//
// The page mounts it before main() with a list of entries:
//   { path: 'files/Race/Course/x.szs', size: <bytes>,
//     read: async (offset, length) => Uint8Array,   // the bytes [offset, offset + length)
//     data: Uint8Array | undefined }                 // whole file, for small files fetched up front
//
//   FS.mkdir('/dvd');
//   FS.mount(Module.MKWDISCFS, { entries }, '/dvd');
//
// Directory listings and sizes are answered synchronously from that list, so the runtime's
// std::filesystem discovery of the DATA tree works unchanged. Game data is never read through this
// file system: DvdReadContract::ReadExact calls mkw_web_disc_read, which suspends through JSPI while
// the page reads the range (from a directory the user picked, or over HTTP). Synchronous reads
// (std::ifstream, e.g. sys/fst.bin) are only served for entries whose data was fetched up front.
// Link with --js-library <this file> and -sJSPI.

addToLibrary({
  $MKWDISCFS__deps: ['$FS'],
  $MKWDISCFS__postset: "Module['MKWDISCFS'] = MKWDISCFS;",
  $MKWDISCFS: {
    DIR_MODE: {{{ cDefs.S_IFDIR | 0o555 }}},
    FILE_MODE: {{{ cDefs.S_IFREG | 0o444 }}},

    mount(mount) {
      const root = MKWDISCFS.createNode(null, '/', MKWDISCFS.DIR_MODE, null);
      const directories = new Map([['', root]]);
      const ensureDirectory = (path) => {
        let node = directories.get(path);
        if (node) return node;
        const slash = path.lastIndexOf('/');
        const parent = ensureDirectory(slash < 0 ? '' : path.slice(0, slash));
        node = MKWDISCFS.createNode(parent, path.slice(slash + 1), MKWDISCFS.DIR_MODE, null);
        directories.set(path, node);
        return node;
      };
      for (const entry of mount.opts['entries'] || []) {
        const path = entry['path'].replace(/^\/+/, '');
        const slash = path.lastIndexOf('/');
        const parent = ensureDirectory(slash < 0 ? '' : path.slice(0, slash));
        MKWDISCFS.createNode(parent, path.slice(slash + 1), MKWDISCFS.FILE_MODE, entry);
      }
      return root;
    },

    createNode(parent, name, mode, entry) {
      const node = FS.createNode(parent, name, mode);
      node.mode = mode;
      node.node_ops = MKWDISCFS.node_ops;
      node.stream_ops = MKWDISCFS.stream_ops;
      node.atime = node.mtime = node.ctime = Date.now();
      if (entry) {
        node.size = entry['size'];
        node.entry = entry;
      } else {
        node.size = 4096;
        node.contents = {};
      }
      if (parent) {
        parent.contents[name] = node;
      }
      return node;
    },

    node_ops: {
      getattr(node) {
        return {
          dev: 1,
          ino: node.id,
          mode: node.mode,
          nlink: 1,
          uid: 0,
          gid: 0,
          rdev: 0,
          size: node.size,
          atime: new Date(node.atime),
          mtime: new Date(node.mtime),
          ctime: new Date(node.ctime),
          blksize: 4096,
          blocks: Math.ceil(node.size / 4096),
        };
      },
      setattr(node, attr) {
        throw new FS.ErrnoError({{{ cDefs.EROFS }}});
      },
      lookup(parent, name) {
        throw new FS.ErrnoError({{{ cDefs.ENOENT }}});
      },
      mknod(parent, name, mode, dev) {
        throw new FS.ErrnoError({{{ cDefs.EROFS }}});
      },
      rename(oldNode, newDir, newName) {
        throw new FS.ErrnoError({{{ cDefs.EROFS }}});
      },
      unlink(parent, name) {
        throw new FS.ErrnoError({{{ cDefs.EROFS }}});
      },
      rmdir(parent, name) {
        throw new FS.ErrnoError({{{ cDefs.EROFS }}});
      },
      readdir(node) {
        return ['.', '..', ...Object.keys(node.contents)];
      },
      symlink(parent, newName, oldPath) {
        throw new FS.ErrnoError({{{ cDefs.EROFS }}});
      },
    },

    stream_ops: {
      read(stream, buffer, offset, length, position) {
        const data = stream.node.entry && stream.node.entry['data'];
        if (!data) {
          // Large files are only read asynchronously, through mkw_web_disc_read.
          throw new FS.ErrnoError({{{ cDefs.EIO }}});
        }
        if (position >= data.length) return 0;
        const chunk = data.subarray(position, Math.min(data.length, position + length));
        buffer.set(chunk, offset);
        return chunk.length;
      },
      write(stream, buffer, offset, length, position) {
        throw new FS.ErrnoError({{{ cDefs.EROFS }}});
      },
      llseek(stream, offset, whence) {
        let position = offset;
        if (whence === {{{ cDefs.SEEK_CUR }}}) {
          position += stream.position;
        } else if (whence === {{{ cDefs.SEEK_END }}} && FS.isFile(stream.node.mode)) {
          position += stream.node.size;
        }
        if (position < 0) {
          throw new FS.ErrnoError({{{ cDefs.EINVAL }}});
        }
        return position;
      },
    },
  },

  // Reads [offset, offset + length) of a file on the MKWDISCFS mount into wasm memory at dst.
  // Returns 0 on success, 1 missing file, 2 offset at or past the end, 3 range past the end,
  // 4 read failure (reported as a missing file). See DvdReadContract::ReadExact in
  // runtime/include/hle/dvd_contract.h.
  mkw_web_disc_read__deps: ['$MKWDISCFS', '$FS', '$UTF8ToString'],
  mkw_web_disc_read__async: true,
  mkw_web_disc_read: async (pathPtr, offset, length, dst) => {
    offset >>>= 0;
    length >>>= 0;
    let node;
    try {
      node = FS.lookupPath(UTF8ToString(pathPtr), { follow: true }).node;
    } catch (e) {
      return 1;
    }
    const entry = node.entry;
    if (!entry) return 1;
    if (offset >= entry['size']) return 2;
    if (length > entry['size'] - offset) return 3;
    if (length === 0) return 0;
    try {
      const started = performance.now();
      const data = entry['data']
        ? entry['data'].subarray(offset, offset + length)
        : await entry['read'](offset, length);
      const perf = Module['mkwPerf'];
      if (perf) {
        const ms = performance.now() - started;
        perf.disc += ms;
        perf.discReads++;
        perf.discBytes += length;
        perf.discWorst = Math.max(perf.discWorst, ms);
      }
      if (data.length !== length) return 3;
      HEAPU8.set(data, dst);
      return 0;
    } catch (e) {
      err(`disc read failed: ${UTF8ToString(pathPtr)} @${offset}+${length}: ${e}`);
      return 4;
    }
  },
});
