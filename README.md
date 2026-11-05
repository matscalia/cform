# cform

C++20 port of the schema-driven prompt utility behind `orchestra prompt`: read
a JSON Schema from **stdin**, collect a value through interactive prompts on the
controlling terminal, print the JSON on **stdout**.

```bash
cform < schema.json > value.json                 # start from an empty value
cform existing.json < schema.json > value.json   # start from a partially filled document
```

cform always opens on the tree screen; the optional `document` only seeds it.

Prompts are drawn on `/dev/tty`, so stdout stays clean and redirectable. Editing
a node uses the same wizard as `orchestra prompt`: same traversal, messages,
validation and back-navigation.

## Build

Requires a C++20 compiler, CMake ≥ 3.20 and simdjson (used for parsing; the
values are copied into an ordered model so `$ref`s can be resolved and the
answer serialized with stable key order).

```bash
cmake -S cform -B cform/build -G Ninja
cmake --build cform/build
./cform/build/cform_test      # behaviour tests (scripted prompter, no TTY)
```

simdjson is located through `pkg-config` (`simdjson.pc`). On Debian/Ubuntu:
`apt install libsimdjson-dev`; on macOS: `brew install simdjson`.

If `libchafa` is found through `pkg-config` (`chafa.pc`), the native CLI also
supports `--image` (rendered with chafa; stb_image, fetched through CMake
`FetchContent`, decodes the file). On Debian/Ubuntu: `apt install libchafa-dev`; on macOS:
`brew install chafa`. Without it the build still works, just without images.
`FetchContent` pins `nothings/stb`; for an offline build, pre-populate it and
pass `-DFETCHCONTENT_SOURCE_DIR_STB=/path/to/stb`.

## Node / WebAssembly

The prompt UI can also be built as a single-file WebAssembly module with Node
bindings in `wasm/`. This build does not need simdjson: `src/json_parse.cpp`
is the parser used by wasm.

Requires Emscripten (with `emcmake` on `PATH`) and Node ≥ 18.

```bash
emcmake cmake -S . -B build-wasm -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm
```

This writes `wasm/build/cform.js` (wasm embedded, ~800 KB). Then:

```js
const { loadCform } = require('./wasm');

const cform = await loadCform();   // no TTY needed yet
const value = cform.run(schema);   // blocking; draws the UI, returns JSON
const edited = cform.run(schema, document);  // with an existing document
```

`run` returns the parsed value and throws on cancel/error. Use
`runToString(schema, document?)` for the JSON text, or
`runResult(schema, document?)` for `{ ok, cancelled, value, error }`.

Images use the same libchafa as the CLI, via the `chafa-wasm` package:

```js
const cform = await loadCform({ image: 'logo.png' });
// or, rendered before a later run:
await cform.prepareImage('logo.png');
cform.clearImage();
```

The image is right-aligned inside the frame only when the terminal has room
for it **and** it is at least `imageMinWidth` x `imageMinHeight` cells. It is
truncated from the UI area, which keeps at least `imageMinContentWidth` cells
(default 40). `imageWidth`/`imageHeight` set the rendered size (or one of
them, in which case the other keeps the source aspect ratio);
`imageMaxWidth`/`imageMaxHeight` are optional caps (0 = no cap), and `imageGap`
and `imageMargin` control the spacing. The renderer keeps the image's aspect ratio
(assuming the usual 0.5 terminal cell width/height ratio). In `right` placement
the image is sized to the full TUI height (terminal rows minus one), bounded by
the side-panel width, so the rendered block and the image share the same height.
In `above` placement it uses up to half the terminal height so the prompt still
fits. With a right-side image, the tree's bottom help/status bar is pinned to
the bottom of the image block, so both share the same lower edge. During a
wizard, the prompt history flows in the left column next to the image; once the
available rows are exhausted the oldest lines scroll off while the image stays
fixed. Descriptions and prompt hints are word-wrapped to the available width
rather than truncated. If any UI line would still be shortened by the image, the
image is re-rendered at a narrower width to make room; it is omitted only when it
cannot fit even at the minimum size (the tree's compact help/status bar is
exempt). Set
`imagePlacement: 'above'` to show the image as a right-aligned block above the
prompt instead of a side panel (this keeps the footer readable).

A plain-text caption can be drawn inside the image rectangle:

```js
const cform = await loadCform({
  image: 'logo.png',
  imagePlacement: 'above',
  imageCaption: 'cform\nprompt schemas',
  imageCaptionPosition: 'bottom', // or 'center', 'top', 'none', or 0..1
  imageCaptionAlign: 'center',
  imageCaptionColor: '#ffffff',
  imageCaptionBackground: '#000000', // or 'none' to keep the image background
  imageCaptionBold: true,
  imageCaptionItalic: true,
});
```

`imageCaption` is plain text (`\n` for multiple lines). It replaces the
caption rows of the image with a solid band, so it stays readable over any
artwork; longer lines are clipped to the image width. `imageCaptionPosition`
accepts `top`, `center`, `bottom`, `none`, or a number in `0..1` (0 = first row,
1 = last row). The same options exist as `--image-caption*` CLI flags.

With `imageCaptionBackground: 'none'` (or `--image-caption-bg none`) the caption
is composited cell-by-cell: the glyphs are replaced but each cell keeps its
original background color. `imageCaptionColor: 'none'` likewise keeps the
original foreground; by default the text is white. `imageCaptionBold` and
`imageCaptionItalic` add font styling (set both for bold italic).

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
  --image <file>    right-aligned image, when the terminal has room (chafa)
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
  --image-caption-position <top|center|bottom|none|0..1>
                    caption row (default bottom)
  --image-caption-align <left|center|right>
                    caption alignment (default center)
  --image-caption-padding <n>
                    cells between caption and image edge (default 1)
  --image-caption-color <rrggbb|none>, --image-caption-bg <rrggbb|none>
                    caption colors (default ffffff on 000000;
                    none keeps the image's original color)
  --image-caption-bold, --image-caption-italic
                    caption font style (combine for both)
```

`orchestra prompt` passes its shipped library path as `--lib-dir`; `--base-dir`
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
 ↑↓/jk:nav  ←/h:fold  →/l:open  enter:edit  ctrl+f:tree  q:done          7/7
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
- `enter` (or space) fills/edits the selected node with the usual prompt series:
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
- **`ctrl+f`** returns to the tree from any prompt. Fields already answered
  during the current edit are written to the document immediately (the walker
  reports each finalized value), so the tree shows them right away and
  re-entering the node uses them as the prefilled/default values; only the
  prompt that was still open is dropped.
- `q` (or `esc`) finishes and prints the document; `ctrl-c` aborts. Fields you
  do not edit are preserved byte-for-byte, including keys the schema does not
  know and hand-written pipe-combined `patternProperties` keys.
- The tree is the starting point even without a document: an untouched document
  prints as the empty value, so select the nodes you want (or press `enter` on
  the root to answer the whole schema).

The document is validated as a *source*, not as the final answer: partial or
invalid values are still accepted (their rows are simply diagnosed).

## Behaviour

Same as the TypeScript walker/UI:

- required sections/fields are always asked; optional ones are gated by a
  `set <path>?` confirm (initial `yes` when the schema has a `default`);
- `enum` → select, `const` → fixed, `default` → placeholder accepted with
  Enter; numeric/string constraints (`minimum`, `maximum`, `integer`,
  `minLength`, `maxLength`, `pattern`), required non-empty strings,
  `minItems`/`maxItems`, records (`additionalProperties` + `propertyNames`),
  unions (`anyOf`/`oneOf`, nullable → confirm), `$ref`/`$defs` and `allOf`
  folding;
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

Differences from the TypeScript UI are cosmetic: the banner/error prefix says
`cform`, and line breaks are written as explicit `\r\n`.

## Layout

| File | Role |
| --- | --- |
| `src/json.cpp` | simdjson parsing, ordered JSON value, serialization |
| `src/schema.cpp` | `$ref`/`allOf`, type inspection, validators, `patternProperties` parser |
| `src/tui.cpp` | raw-mode terminal, renderer, key parser, prompts, completion |
| `src/walker.cpp` | schema walker (prompt order, back navigation, history) |
| `src/main.cpp` | stdin schema → value → stdout |
| `test/walker_test.cpp` | scripted-prompter behaviour tests |
