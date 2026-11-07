/// <reference types="node" />

/**
 * The low-level embind bindings (`MainModule`, `RunOptions`, `RunResult`) are
 * generated at build time into `build/cform.d.ts` by Emscripten's
 * `--emit-tsd` (see `CMakeLists.txt`). The Node-facing API below is layered on
 * top of them; `index.js` passes a complete `RunOptions` object to the module.
 */
export type {
  MainModule,
  RunOptions as ModuleRunOptions,
  RunResult as ModuleRunResult,
} from "./build/cform.js";

export interface CformOptions {
  /** TTY stream used for keys. Defaults to `process.stdin`; must have `setRawMode`. */
  input?: NodeJS.ReadStream;
  /** File descriptor used for reads. Defaults to `input.fd` (normally 0). */
  inputFd?: number;
  /** Stream or file descriptor used for the prompt UI. Defaults to `process.stderr`. */
  output?: NodeJS.WriteStream | number;
  /** File descriptor used for the prompt UI. Defaults to 2. */
  outputFd?: number;
  columns?: number;
  rows?: number;
  /** Root used to complete `{lib:...}` references. */
  libDir?: string;
  /** Directory used for relative path completion. Defaults to `process.cwd()`. */
  baseDir?: string;
  /** Start by prompting the root; ctrl+f returns to the tree. */
  promptRoot?: boolean;
  /** Image shown on the right when it fits. A filesystem path or the already
   * decoded image bytes (PNG/JPEG/WebP). Requires `chafa-wasm`. */
  image?: string | Uint8Array | ArrayBuffer;
  /** Render image colors with the terminal's 16-color palette instead of truecolor. */
  useTerminalTheme?: boolean;
  /** Rendered image size in terminal cells (defaults to the full TUI height,
   * preserving the source aspect ratio). */
  imageWidth?: number;
  imageHeight?: number;
  /** Optional caps for the rendered image (0/undefined = no cap). */
  imageMaxWidth?: number;
  imageMaxHeight?: number;
  /** Hide the image below this rendered size (default 8x4). */
  imageMinWidth?: number;
  imageMinHeight?: number;
  /** Cells reserved for the prompt UI; longer lines are truncated (default 40). */
  imageMinContentWidth?: number;
  /** Blank cells between the prompt and the image (default 2). */
  imageGap?: number;
  /** Blank cells to the right of the image (default 0). */
  imageMargin?: number;
  /** `right` side panel (default) or right-aligned block `above` the UI. */
  imagePlacement?: 'right' | 'above';
  /** Plain-text caption drawn over the image (use `\n` for multiple lines). */
  imageCaption?: string;
  imageCaptionPosition?: 'top' | 'center' | 'middle' | 'bottom' | 'none' | number;
  imageCaptionAlign?: 'left' | 'center' | 'right';
  imageCaptionPadding?: number;
  /** `#rrggbb`/`rrggbb`, packed number, default white; `none`/`transparent` keeps the image fg. */
  imageCaptionColor?: string | number;
  /** `#rrggbb`/`rrggbb`, packed number, `none`/`transparent` (keeps the image bg), or `terminal` (uses the terminal background). */
  imageCaptionBackground?: string | number;
  /** Bold caption text. */
  imageCaptionBold?: boolean;
  /** Italic caption text. */
  imageCaptionItalic?: boolean;
  /** Editor command used by multiline prompts (defaults to `$VISUAL`, `$EDITOR`, `vi`). */
  editor?: string;
}

export interface RunOptions {
  libDir?: string;
  baseDir?: string;
  columns?: number;
  promptRoot?: boolean;
  imageMinWidth?: number;
  imageMinHeight?: number;
  imageMinContentWidth?: number;
  imageGap?: number;
  imageMargin?: number;
  imageMaxWidth?: number;
  imageMaxHeight?: number;
  imagePlacement?: 'right' | 'above';
  imageCaption?: string;
  imageCaptionPosition?: 'top' | 'center' | 'middle' | 'bottom' | 'none' | number;
  imageCaptionAlign?: 'left' | 'center' | 'right';
  imageCaptionPadding?: number;
  imageCaptionColor?: string | number;
  imageCaptionBackground?: string | number;
  /** Bold caption text. */
  imageCaptionBold?: boolean;
  /** Italic caption text. */
  imageCaptionItalic?: boolean;
}

export interface RunResult
  extends Omit<ModuleRunResult, "value" | "error"> {
  /** Pretty-printed JSON document, empty unless `ok`. */
  value: string;
  error: string;
}

export interface Cform {
  /** Runs the prompt UI and returns the parsed JSON value. Throws on cancel/error. */
  run(
    schema: string | object,
    document?: string | object | null,
    options?: RunOptions,
  ): unknown;

  /** Like `run`, but returns the pretty-printed JSON text. */
  runToString(
    schema: string | object,
    document?: string | object | null,
    options?: RunOptions,
  ): string;

  /** Runs the prompt UI and returns the raw result without throwing. */
  runResult(
    schema: string | object,
    document?: string | object | null,
    options?: RunOptions,
  ): RunResult;

  /** Renders an image with chafa-wasm for subsequent `run` calls. Accepts a
   * filesystem path or the already decoded image bytes (PNG/JPEG/WebP). */
  prepareImage(
    image: string | Uint8Array | ArrayBuffer | null,
    options?: CformOptions,
  ): Promise<{ width: number; height: number } | null>;

  /** Removes the currently prepared image. */
  clearImage(): void;
}

/** Loads the wasm module. The interactive call is blocking and needs a TTY. */
export function loadCform(options?: CformOptions): Promise<Cform>;
