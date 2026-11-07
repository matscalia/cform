'use strict';

// Node bindings for the cform WebAssembly module.
//
//   const { loadCform } = require('cform/wasm');
//   const cform = await loadCform();
//   const value = cform.run(schema, document?);   // blocking, uses the TTY
//
// The C++ prompt UI renders through the callbacks below. By default the UI is
// written to stderr and keys are read from stdin; stdin must be a TTY so it can
// be switched to raw mode.

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { promisify } = require('node:util');
const { spawnSync } = require('node:child_process');

const SLEEP = new Int32Array(new SharedArrayBuffer(4));

function sleep(milliseconds) {
  if (milliseconds > 0) Atomics.wait(SLEEP, 0, 0, milliseconds);
}

function fileDescriptor(value, fallback) {
  if (typeof value === 'number') return value;
  if (value && typeof value.fd === 'number') return value.fd;
  return fallback;
}

function packedColor(value, fallback) {
  if (value == null) return fallback;
  if (typeof value === 'number') return value & 0xFFFFFF;
  if (typeof value !== 'string') return fallback;
  const hex = value.startsWith('#') ? value.slice(1) : value;
  if (!/^[0-9a-fA-F]{6}$/.test(hex)) {
    throw new Error(`cform: invalid color \`${value}\``);
  }
  return Number.parseInt(hex, 16);
}

function resolveCaptionPosition(value, hasCaption) {
  if (value == null) return hasCaption ? 1 : -1;
  if (typeof value === 'number') {
    if (!Number.isFinite(value) || value < 0) return -1;
    return Math.min(1, Math.max(0, value));
  }
  const text = String(value).trim().toLowerCase();
  if (text === 'none') return -1;
  if (text === 'top') return 0;
  if (text === 'center' || text === 'middle') return 0.5;
  if (text === 'bottom') return 1;
  const parsed = Number(text);
  if (Number.isFinite(parsed)) return Math.min(1, Math.max(0, parsed));
  throw new Error(`cform: invalid imageCaptionPosition \`${value}\``);
}

function terminalSize(options) {
  const output =
    options.output && typeof options.output === 'object' ? options.output : null;
  const columns =
    options.columns ||
    (output && output.columns) ||
    process.stderr.columns ||
    process.stdout.columns ||
    80;
  const rows =
    options.rows ||
    (output && output.rows) ||
    process.stderr.rows ||
    process.stdout.rows ||
    24;
  return { columns, rows };
}

let chafaModule = null;

async function loadChafa() {
  let imported;
  try {
    imported = require('chafa-wasm');
  } catch (error) {
    const hint = new Error(
      'cform: image support requires the `chafa-wasm` package; run `npm install chafa-wasm`');
    hint.cause = error;
    throw hint;
  }
  if (!chafaModule) {
    const factory = imported.default || imported;
    chafaModule = await factory();
  }
  return chafaModule;
}

const FONT_RATIO = 0.5; // terminal cell width / height

function calcCanvasGeometry(srcWidth, srcHeight, maxWidth, maxHeight,
                            fontRatio = FONT_RATIO) {
  if (srcWidth <= 0 || srcHeight <= 0) {
    return { width: Math.max(1, maxWidth), height: Math.max(1, maxHeight) };
  }
  let width = Math.max(1, maxWidth);
  let height = Math.ceil((srcHeight / srcWidth) * width * fontRatio);
  if (height > maxHeight) {
    height = Math.max(1, maxHeight);
    width = Math.ceil((srcWidth / srcHeight) * (height / fontRatio));
  }
  return {
    width: Math.max(1, Math.min(width, Math.max(1, maxWidth))),
    height: Math.max(1, Math.min(height, Math.max(1, maxHeight))),
  };
}

function chooseImageSize(image, options, maxWidth, maxHeight) {
  if (options.imageWidth != null && options.imageHeight != null) {
    return {
      width: Math.max(1, Math.round(options.imageWidth)),
      height: Math.max(1, Math.round(options.imageHeight)),
    };
  }
  if (options.imageWidth != null) {
    const width = Math.max(1, Math.ceil(options.imageWidth));
    return {
      width,
      height: Math.max(
        1,
        Math.ceil(width * (image.height / image.width) * FONT_RATIO),
      ),
    };
  }
  if (options.imageHeight != null) {
    const height = Math.max(1, Math.ceil(options.imageHeight));
    return {
      height,
      width: Math.max(
        1,
        Math.ceil(height * (image.width / image.height) / FONT_RATIO),
      ),
    };
  }
  return calcCanvasGeometry(image.width, image.height, maxWidth, maxHeight);
}

function renderAnsiSync(image, options, maxWidth, maxHeight) {
  const { width, height } = chooseImageSize(image, options, maxWidth, maxHeight);
  let result = null;
  try {
    chafaModule.imageToAnsi(
      image,
      {
        width,
        height,
        fontRatio: FONT_RATIO,
        format: chafaModule.ChafaPixelMode.CHAFA_PIXEL_MODE_SYMBOLS.value,
        colors: options.useTerminalTheme
          ? chafaModule.ChafaCanvasMode.CHAFA_CANVAS_MODE_INDEXED_16.value
          : chafaModule.ChafaCanvasMode.CHAFA_CANVAS_MODE_TRUECOLOR.value,
        fill: 'none',
        symbols: 'block+border+space-wide-inverted',
        preprocess: true,
      },
      (error, value) => {
        if (!error) result = value;
      },
    );
  } catch {
    return null;
  }
  if (!result) return null;
  return {
    ansi: result.ansi,
    width: (result.config && result.config.width) || width,
    height: (result.config && result.config.height) || height,
  };
}

// Reads the image source: a filesystem path or already-decoded image bytes
// (Buffer/Uint8Array/ArrayBuffer).
async function readImageSource(source) {
  if (typeof source === 'string') return fs.promises.readFile(source);
  if (Buffer.isBuffer(source)) return source;
  if (source instanceof ArrayBuffer) return Buffer.from(new Uint8Array(source));
  if (ArrayBuffer.isView(source)) {
    return Buffer.from(source.buffer, source.byteOffset, source.byteLength);
  }
  throw new Error(
    'cform: image must be a filesystem path, Buffer, Uint8Array or ArrayBuffer');
}

async function renderImage(source, options) {
  const { columns, rows } = terminalSize(options);
  const placement = options.imagePlacement ?? 'right';
  const availableWidth = Math.max(
    1,
    columns -
      (placement === 'above'
        ? (options.imageMargin ?? 0)
        : (options.imageMinContentWidth ?? 40) +
          (options.imageGap ?? 2) +
          (options.imageMargin ?? 0)),
  );
  const defaultHeight =
    placement === 'above'
      ? Math.max(1, Math.floor(rows / 2))
      : Math.max(1, rows - 1);
  const maxWidth = Math.max(
    1,
    Math.min(availableWidth, options.imageMaxWidth || availableWidth),
  );
  const maxHeight = Math.max(
    1,
    Math.min(defaultHeight, options.imageMaxHeight || defaultHeight),
  );

  const chafa = await loadChafa();
  const buffer = await readImageSource(source);
  const data = buffer.buffer.slice(
    buffer.byteOffset,
    buffer.byteOffset + buffer.byteLength,
  );
  const image = await promisify(chafa.decodeImage)(data);
  const rendered = renderAnsiSync(image, options, maxWidth, maxHeight);
  if (!rendered) throw new Error('cform: image rendering failed');
  return { ...rendered, image };
}

function createTerminal(getModule, options) {
  const input = options.input || process.stdin;
  if (typeof input.setRawMode !== 'function') {
    throw new Error(
      'cform: no interactive terminal (stdin is not a TTY; run Node attached to a terminal)');
  }

  const inputFd = fileDescriptor(options.inputFd ?? options.input, input.fd ?? 0);
  const outputFd = fileDescriptor(options.outputFd ?? options.output, 2);
  const output = options.output && typeof options.output === 'object' ? options.output : null;

  let pending = Buffer.alloc(0);
  let eof = false;

  function windowSize() {
    if (options.columns && options.rows) {
      return { columns: options.columns, rows: options.rows };
    }
    const handle =
      (output && output._handle) ||
      process.stderr._handle ||
      process.stdout._handle;
    if (handle && typeof handle.getWindowSize === 'function') {
      const result = [];
      try {
        handle.getWindowSize(result);
        if (result[0] > 0 && result[1] > 0) {
          return {
            columns: options.columns || result[0],
            rows: options.rows || result[1],
          };
        }
      } catch {
        // fall back to the stream properties below
      }
    }
    return {
      columns:
        options.columns ||
        (output && output.columns) ||
        process.stderr.columns ||
        process.stdout.columns ||
        80,
      rows:
        options.rows ||
        (output && output.rows) ||
        process.stderr.rows ||
        process.stdout.rows ||
        24,
    };
  }

  function pump() {
    if (pending.length > 0 || eof) return;
    const chunk = Buffer.allocUnsafe(4096);
    let count;
    try {
      count = fs.readSync(inputFd, chunk, 0, chunk.length, null);
    } catch (error) {
      if (error.code === 'EAGAIN' || error.code === 'EWOULDBLOCK') return;
      throw error;
    }
    if (count === 0) {
      eof = true;
      return;
    }
    const bytes = chunk.subarray(0, count);
    pending = pending.length > 0 ? Buffer.concat([pending, bytes]) : Buffer.from(bytes);
  }

  function columns() { return windowSize().columns; }
  function rows() { return windowSize().rows; }

  let lastColumns = columns();
  let lastRows = rows();
  let lastResizeCheck = 0;

  // Returns 1 for input, 0 for timeout, -1 for a terminal resize.
  function waitForData(timeoutMs, detectResize = true) {
    const deadline = timeoutMs < 0 ? Infinity : Date.now() + timeoutMs;
    for (;;) {
      if (detectResize && Date.now() - lastResizeCheck >= 100) {
        lastResizeCheck = Date.now();
        const size = windowSize();
        if (size.columns !== lastColumns || size.rows !== lastRows) {
          lastColumns = size.columns;
          lastRows = size.rows;
          return -1;
        }
      }
      pump();
      if (pending.length > 0 || eof) return 1;
      const remaining = deadline - Date.now();
      if (timeoutMs >= 0 && remaining <= 0) return 0;
      sleep(timeoutMs < 0 ? 5 : Math.max(1, Math.min(5, remaining)));
    }
  }

  function editorName() {
    return options.editor || process.env.VISUAL || process.env.EDITOR || 'vi';
  }

  return {
    write(pointer, size) {
      const bytes = Buffer.from(getModule().HEAPU8.subarray(pointer, pointer + size));
      fs.writeSync(outputFd, bytes);
    },

    read(pointer, size) {
      if (waitForData(-1, false) <= 0) return 0;
      if (pending.length === 0) return 0;
      const count = Math.min(size, pending.length);
      getModule().HEAPU8.set(pending.subarray(0, count), pointer);
      pending = pending.subarray(count);
      return count;
    },

    wait(timeoutMs) {
      return waitForData(timeoutMs, true);
    },

    setRaw(enabled) {
      input.setRawMode(!!enabled);
    },

    columns,
    rows,

    cwd() {
      return process.cwd();
    },

    listDir(directory) {
      try {
        return fs.readdirSync(directory, { withFileTypes: true })
          .filter(entry => entry.name && entry.name[0] !== '.')
          .map(entry => ({ name: entry.name, dir: entry.isDirectory() }));
      } catch {
        return [];
      }
    },

    edit(initial) {
      const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'cform-edit-'));
      const file = path.join(directory, 'value.txt');
      try {
        fs.writeFileSync(file, initial);
        const status = spawnSync(editorName(), [file], {
          stdio: 'inherit',
          shell: true,
        });
        if (status.error) return null;
        return fs.readFileSync(file, 'utf8');
      } finally {
        fs.rmSync(directory, { recursive: true, force: true });
      }
    },

    editorName,
  };
}

async function loadCform(options = {}) {
  // Loaded lazily so that a failed build produces a useful error message.
  let createCformModule;
  try {
    createCformModule = require('./build/cform.js');
  } catch (error) {
    const hint = new Error(
      'cform: wasm module not found; build it with `emcmake cmake -S . -B build-wasm && cmake --build build-wasm`');
    hint.cause = error;
    throw hint;
  }

  let module = null;
  let activeTerminal = null;

  const getModule = () => {
    if (!module) throw new Error('cform: wasm module is not initialized');
    return module;
  };
  // A JS exception thrown while entering wasm cannot be caught by the C++
  // `catch` blocks, so `runResult` creates the terminal (and validates the
  // TTY) before calling into the module.
  const getTerminal = () => {
    if (!activeTerminal) throw new Error('cform: run() is not active');
    return activeTerminal;
  };

  let artwork = null;
  let artworkImage = null;
  let artworkOptions = options;

  module = await createCformModule({
    cformWrite: (pointer, size) => getTerminal().write(pointer, size),
    cformRead: (pointer, size) => getTerminal().read(pointer, size),
    cformWait: timeoutMs => getTerminal().wait(timeoutMs),
    cformColumns: () => getTerminal().columns(),
    cformRows: () => getTerminal().rows(),
    cformSetRaw: enabled => getTerminal().setRaw(enabled),
    cformCwd: () => getTerminal().cwd(),
    cformListDir: directory => getTerminal().listDir(directory),
    cformEdit: initial => getTerminal().edit(initial),
    cformEditorName: () => getTerminal().editorName(),
    cformRenderArtwork: (maxWidth, maxHeight) => {
      if (!artworkImage) return '';
      const rendered = renderAnsiSync(
        artworkImage,
        artworkOptions,
        maxWidth,
        maxHeight,
      );
      return rendered ? rendered.ansi : '';
    },
  });

  if (options.image) {
    const rendered = await renderImage(options.image, options);
    artwork = rendered;
    artworkImage = rendered.image;
  }

  function runResult(schema, document, runOptions = {}) {
    const schemaText = typeof schema === 'string' ? schema : JSON.stringify(schema);
    const documentText =
      document == null
        ? ''
        : typeof document === 'string'
          ? document
          : JSON.stringify(document);
    const terminal = createTerminal(getModule, options);
    activeTerminal = terminal;
    const caption =
      runOptions.imageCaption ?? options.imageCaption ?? '';
    const captionPosition = resolveCaptionPosition(
      runOptions.imageCaptionPosition ?? options.imageCaptionPosition,
      caption.length > 0,
    );
    const captionBackground =
      runOptions.imageCaptionBackground ?? options.imageCaptionBackground;
    const captionForeground =
      runOptions.imageCaptionColor ?? options.imageCaptionColor;
    try {
      return module.run(schemaText, documentText, {
        libDir: runOptions.libDir ?? options.libDir ?? '',
        baseDir: runOptions.baseDir ?? options.baseDir ?? '',
        columns: runOptions.columns ?? options.columns ?? 0,
        promptRoot: !!(runOptions.promptRoot ?? options.promptRoot),
        artAnsi: artwork ? artwork.ansi : '',
        artWidth: artwork ? artwork.width : 0,
        artMinWidth: runOptions.imageMinWidth ?? options.imageMinWidth ?? 8,
        artMinHeight: runOptions.imageMinHeight ?? options.imageMinHeight ?? 4,
        artMinContentWidth: runOptions.imageMinContentWidth ?? options.imageMinContentWidth ?? 40,
        artGap: runOptions.imageGap ?? options.imageGap ?? 2,
        artMargin: runOptions.imageMargin ?? options.imageMargin ?? 0,
        artMaxWidth: runOptions.imageMaxWidth ?? options.imageMaxWidth ?? 0,
        artMaxHeight: runOptions.imageMaxHeight ?? options.imageMaxHeight ?? 0,
        artPlacement: runOptions.imagePlacement ?? options.imagePlacement ?? 'right',
        artCaption: caption,
        artCaptionPosition: captionPosition,
        artCaptionAlign: runOptions.imageCaptionAlign ?? options.imageCaptionAlign ?? 'center',
        artCaptionPadding: runOptions.imageCaptionPadding ?? options.imageCaptionPadding ?? 1,
        artCaptionFg:
          captionForeground === 'none' || captionForeground === 'transparent'
            ? -1
            : packedColor(captionForeground, 0xFFFFFF),
        artCaptionBg:
          captionBackground === 'none' || captionBackground === 'transparent'
            ? -1
            : captionBackground === 'terminal'
              ? -2
              : packedColor(captionBackground, 0x000000),
        artCaptionBold: !!(runOptions.imageCaptionBold ?? options.imageCaptionBold),
        artCaptionItalic: !!(runOptions.imageCaptionItalic ?? options.imageCaptionItalic),
      });
    } finally {
      activeTerminal = null;
      // A JS exception thrown from a callback unwinds wasm without running the
      // C++ destructors, so make sure the terminal is never left in raw mode.
      try {
        terminal.setRaw(false);
      } catch {
        // the TTY may already be gone
      }
    }
  }

  async function prepareImage(source, imageOptions = {}) {
    if (!source) {
      artwork = null;
      artworkImage = null;
      return null;
    }
    artworkOptions = { ...options, ...imageOptions };
    const rendered = await renderImage(source, artworkOptions);
    artwork = rendered;
    artworkImage = rendered.image;
    return { width: rendered.width, height: rendered.height };
  }

  function clearImage() {
    artwork = null;
    artworkImage = null;
  }

  function runToString(schema, document, runOptions) {
    const result = runResult(schema, document, runOptions);
    if (!result.ok) {
      const error = new Error(result.error || 'cform failed');
      error.cancelled = !!result.cancelled;
      throw error;
    }
    return result.value;
  }

  function run(schema, document, runOptions) {
    return JSON.parse(runToString(schema, document, runOptions));
  }

  return { run, runToString, runResult, prepareImage, clearImage };
}

module.exports = { loadCform };
