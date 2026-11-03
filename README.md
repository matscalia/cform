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

## Usage

```
usage: cform [options] [document]

  document          existing JSON file to start from; top-level fields already
                    complete are marked with a tick and kept unless selected
  --lib-dir <dir>   root used to complete `{lib:...}` references
  --base-dir <dir>  directory used for relative path completion
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
