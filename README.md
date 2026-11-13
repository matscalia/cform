# cform

A schema-driven prompt utility: read a JSON Schema from **stdin**, collect a
value through interactive prompts on the controlling terminal, print the JSON
on **stdout**.

```bash
cform < schema.json > value.json                 # start from an empty value
cform existing.json < schema.json > value.json   # start from a partially filled document
```

## Build

Requires a C++20 compiler, CMake ≥ 3.20 and simdjson.

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

## Node / WebAssembly

The prompt UI can also be built as a single-file WebAssembly module with Node
bindings in `wasm/`.

Requires Emscripten (with `emcmake` on `PATH`) and Node ≥ 18.

```bash
emcmake cmake -S . -B build-wasm -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm
```

This writes `wasm/build/cform.js` and `wasm/build/cform.d.ts`. Then:

```js
const { loadCform } = require('./wasm');

const cform = await loadCform();   // no TTY needed yet
const value = cform.run(schema);   // blocking; draws the UI, returns JSON
const edited = cform.run(schema, document);  // with an existing document
const direct = cform.run(schema, null, { promptRoot: true }); // start at the root
// ctrl+f returns to the tree; q in the tree prints the result
```

`run` returns the parsed value and throws on cancel/error. Use
`runToString(schema, document?)` for the JSON text, or
`runResult(schema, document?)` for `{ ok, cancelled, value, error }`. Numbers
that do not fit in 64 bits are rejected while parsing, as in the CLI.

Images use the same libchafa as the CLI, via the `chafa-wasm` package:

```js
const cform = await loadCform({ image: 'logo.png' });
// render using the terminal's 16-color palette instead of truecolor:
const themed = await loadCform({ image: 'logo.png', useTerminalTheme: true });
// or, rendered before a later run:
await cform.prepareImage('logo.png');
cform.clearImage();
```

By default chafa emits truecolor RGB values. With `useTerminalTheme: true`
(or `--use-terminal-theme`), it maps the image to the terminal's 16 ANSI
colors, so the actual red/green/blue values follow the user's terminal theme
instead of the source image's exact colors.

The image is right-aligned inside the frame only when the terminal has room
for it **and** it is at least `imageMinWidth` x `imageMinHeight` cells.

A plain-text caption can be drawn inside the image rectangle:

```js
const cform = await loadCform({
  image: 'logo.png',
  imagePlacement: 'above',
  imageCaption: 'cform\nprompt schemas',
  imageCaptionPosition: 'bottom', // or 'center'/'middle', 'top', 'none', or 0..1
  imageCaptionAlign: 'center',
  imageCaptionColor: '#ffffff',
  imageCaptionBackground: '#000000', // 'none'/'transparent' keep the image background,
                                      // 'terminal' uses the terminal background
  imageCaptionBold: true,
  imageCaptionItalic: true,
});
```

`imageCaption` is plain text (`\n` for multiple lines). It replaces the
caption rows of the image with a solid band, so it stays readable over any
artwork; longer lines are clipped to the image width. `imageCaptionPosition`
accepts `top`, `center`/`middle`, `bottom`, `none`, or a number in `0..1`
(0 = first row, 1 = last row). The same options exist as `--image-caption*`
CLI flags.

With `imageCaptionBackground: 'none'` or `'transparent'` (or
`--image-caption-bg none`) the caption is composited cell-by-cell: the glyphs
are replaced but each cell keeps its original background color. Use
`imageCaptionBackground: 'terminal'` (or `--image-caption-bg terminal`) to
paint the caption row with the terminal's default background instead, which
adapts to the user's terminal theme. `imageCaptionColor: 'none'` likewise
keeps the original foreground; by default the text is white.
`imageCaptionBold` and `imageCaptionItalic` add font styling (set both for
bold italic).

- The UI is written to `stderr` by default, so `stdout` stays free; pass
  `{ output: process.stdout }`, `{ outputFd: 1 }` or `{ columns }` to
  `loadCform` to change that.
- Keys are read from `stdin`, which must be a TTY (Node cannot set raw mode on
  an arbitrary fd), so run the process attached to a terminal.
- `{file:...}` / `{lib:...}` completion uses Node's `fs`; multiline prompts use
  `$VISUAL` / `$EDITOR` (or `options.editor`).

`run` is synchronous and blocks the event loop while the user is prompted —
call it from a CLI entry point, not from a request handler.

Resizing the terminal triggers an immediate redraw: the layout and text re-wrap
to the new size, and the artwork is re-rendered and rescaled for the new
terminal geometry without waiting for a keypress. Width-only and height-only
resizes are handled independently; if the new aspect would cut text, the image
is narrowed instead of disappearing.

There is a runnable example: `node wasm/example.js`.

## Usage

```
usage: cform [options] [document]

  document          existing JSON file to start from; top-level fields already
                    complete are marked with a tick and kept unless selected
  --lib-dir <dir>   root used to complete `{lib:...}` references
  --base-dir <dir>  directory used for relative path completion
  --prompt          start by prompting the root; ctrl+f returns to the tree
  --image <file>    right-aligned image, when the terminal has room (chafa)
  --use-terminal-theme
                    render image colors with the terminal's 16-color palette
                    instead of truecolor
  --image-min-width <n>, --image-min-height <n>
                    hide below this rendered cell size (default 8x4)
  --image-min-content-width <n>
                    cells kept for the UI (default 40)
  --image-gap <n>   blank cells between UI and image (default 2)
  --image-margin <n>
                    blank cells to the right of the image (default 0)
  --image-placement <right|above>
                    side panel or block above the UI (default right)
  --image-max-width <n>, --image-max-height <n>
                    optional caps; 0 = no cap (default)
  --image-caption <text>
                    plain-text caption drawn over the image
  --image-caption-position <top|center|middle|bottom|none|0..1>
                    caption row (default bottom)
  --image-caption-align <left|center|right>
                    caption alignment (default center)
  --image-caption-padding <n>
                    cells between caption and image edge (default 1)
  --image-caption-color <rrggbb|none|transparent>,
  --image-caption-bg <rrggbb|none|transparent|terminal>
                    caption colors (default ffffff on 000000;
                    none/transparent keep the image color; terminal uses
                    the terminal background)
  --image-caption-bold, --image-caption-italic
                    caption font style (combine for both)
  --version         print the version and exit

SPDX-License-Identifier: MIT
Copyright (c) 2026 Matteo Scalia
```

`--lib-dir` resolves `{lib:...}` references and has no default; `--base-dir`
defaults to the current directory.

## Tree screen

cform starts on a tree view of the document. Without a positional `document` the
tree starts from an empty value (`{}` for object schemas, `[]` for arrays,
`null` otherwise), so you pick what to fill; `enter` on the root row runs the
whole schema as one prompt series (the classic wizard). With a document, its
values pre-fill the tree:

```
fields /path/to/doc.json
▾ document {4} error: nested: is missing required property "inner" (+1)
    ├──   name "svc" ✓
    ├──   port 8080 ✓
    ├── ▾ nested {1} ✓
    │   └──    inner "x" ✓
    ├──   tags (unset)
    └──    legacy 1 error: unknown property
 ↑↓/jk:nav  ←/h:fold  →/l:open  enter:edit  sp:info  ctrl+o:prompt  ctrl+f:tree  q:done          7/7
```

- The tree mirrors the **document**: objects/arrays expand with `→` (`l`) and
  fold with `←` (`h`); `↑`/`↓` (`j`/`k`), `pgup`/`pgdn` move; the root is always
  expanded, everything else starts folded.
- Children are the schema's `properties`, its literal `patternProperties` keys
  (also listed when absent, so you can pick what to add) and any extra keys the
  document carries.
- Each row shows a **diagnostic** when the schema is not respected: the first
  validation error in red (`error: …`, with a `(+n)` count), `✓` in green when
  the node is valid, and `(unset)` for optional keys that are absent. Required
  missing fields read `error: missing required field`; unknown keys under
  `additionalProperties: false` read `error: unknown property`.
- `enter` fills/edits the selected node with the usual prompt series:
  selecting an **object** runs its fields as prompts, an array runs the
  item/add prompts, a leaf runs its single prompt. Values already present in the
  document are pre-filled and override the autocomplete/default, and objects are
  seeded from the document so unknown members survive an edit.
- Schema-less values are never edited as raw JSON: when the schema does not
  describe an object (opaque `type: object`, loose `additionalProperties`, or a
  node with no schema at all) its members are prompted one by one with the type
  inferred from the document (numbers as numbers, strings as strings, nested
  objects/arrays recursively). Adding a member or editing an absent schema-less
  value asks for a **type** first (`string`, `number`, `boolean`, `object`,
  `array`, `null`, or an explicit `JSON`).
- **`space`** toggles a popup with the selected field's `description` (or
  `(no description)` when it has none). The popup wraps its text in a
  `┌─┐`/`│`/`└─┘` box near the cursor; `space` again closes it.
- **`ctrl+f`** returns to the tree from any prompt. Fields already answered
  during the current edit are written to the document immediately, so the tree
  shows them right away and re-entering the node uses them as the
  prefilled/default values; only the
  prompt that was still open is dropped.
- **`ctrl+o`** in the tree reopens the last prompt at the exact point you left
  it: typed text, cursor position, selections, completion state and validation
  state are restored. It also works after arriving in the tree via `ctrl+f`,
  after the `--prompt` initial root wizard, and after a completed prompt series
  (the last prompt is reopened just before its accepting key).
- `q` (or `esc`) finishes and prints the document; `ctrl-c` aborts. Fields you
  do not edit are preserved byte-for-byte, including keys the schema does not
  know and hand-written pipe-combined `patternProperties` keys.
- The tree is the starting point even without a document: an untouched document
  prints as the empty value, so select the nodes you want (or press `enter` on
  the root to answer the whole schema).

The document is validated as a *source*, not as the final answer: partial or
invalid values are still accepted (their rows are simply diagnosed).

## Behaviour

- required sections/fields are always asked; optional ones are gated by a
  `set <path>?` confirm (initial `yes` when the schema has a `default`);
- `enum` → select, `const` → fixed, `default` → placeholder accepted with
  Enter; numeric/string constraints (`minimum`, `maximum`, `integer`,
  `minLength`, `maxLength`, `pattern`), required non-empty strings,
  `minItems`/`maxItems`, records (`additionalProperties` + `propertyNames`),
  unions (`anyOf`/`oneOf`, nullable → confirm), `$ref`/`$defs`, `allOf`
  folding, and `if`/`then`/`else`/`not` (conditional properties and required
  fields are re-evaluated as the object is filled);
- conditional property order is dependency-sorted: properties referenced by an
  `if` condition are always prompted before the fields controlled by that
  condition, regardless of where they appear in `properties`;
- `patternProperties` whose patterns are plain literal alternations
  (`^(a|b)$`, `^#x\.before$`) become a key picker with per-key typing and a
  `(done)` option;
- values answered for the same `$ref` are reused: they become the completion
  suggestions and the most frequent one becomes the default; several
  alternatives open a dropdown (`↑`/`↓` select, `tab` completes, `enter`
  accepts);
- `{file:...}`, `{lib:...}`, `{env:...}` completion plus general file/directory
  completion on tab; ghost text for path-like input or schema suggestions;
- `esc` goes back to the previous question (sibling, array item or the last
  question of a re-entered group), `ctrl-c` cancels, `ctrl+f` returns to the
  tree when editing a document;
- values the schema does not describe are prompted by inferring their type from
  the document, never as raw JSON (JSON stays available as an explicit choice);
- `format: "multiline"`/`"textarea"` opens `$VISUAL`/`$EDITOR`.

## License

cform is released under the MIT License; see `LICENSE`.
