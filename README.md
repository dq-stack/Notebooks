# Notebooks

**A notebook for every book on your Kindle.** For jailbroken Kindles, including older models like the Paperwhite 2.

Each book gets its own notebook. Your Kindle highlights and the notes you make on them in the reader appear there automatically, next to longer notes you write in Notebooks itself.

<!-- Screenshots: mockups (docs/) + a real device screenshot of the keyboard to come -->

---

## Features

- **Reading now** at the top: the book you last opened, with its cover and progress, one tap from its notebook
- **A notebook per book**, newest first, with three kinds of entry:
  - *Your notes*, written in Notebooks
  - *Highlights* you made in the Kindle reader
  - *Kindle notes*, shown with the highlight they belong to
- **Read book** jumps straight back into the book
- A full-height **writing screen** in a book font, with autosave
- **Bullet lists, numbered lists and checklists** (tick items off right in the notebook)
- **Loose notes** for anything not tied to a book
- Notes are **plain text files**, one folder per book, so they're readable on your computer:
  `notebooks/Walden/2026-10-18 201503.txt`
- **Never silently loses a note:** "Saved" only appears once the text has been read back from disk

Highlights and Kindle notes are read from `My Clippings.txt` and are never changed. Edit or delete them in the book itself.

---

## Installation

You need a **jailbroken Kindle** with Scriptlet support (the Library runs `.sh` files from `documents/`).

1. Download `notebooks-<version>.zip` from [Releases](../../releases/latest).
2. Unzip it and copy `documents` and `extensions` into the root of the Kindle. If those folders already exist, copy the contents into them.
3. Eject and open **Notebooks** from the Library.

The first launch registers Notebooks with the Kindle's app manager.

### Updating

Copy the new `extensions/notebooks` over the old one. Your notes in `/notebooks/` are never touched.

### Uninstalling

Delete `documents/Notebooks.sh` and `extensions/notebooks/`. Keep `/notebooks/` if you want your notes.

---

## How it works

- The app is a web page run by the Kindle's built-in `mesquite` browser engine.
- A small native helper (`notebooks_helper`, C) does the file work. The page sends it requests over LIPC; it can only save or delete `.txt` files inside `/mnt/us/notebooks/`, refresh the data, and open a book from `documents/`. It never runs a shell with anything the page sends.
- After every change the helper rewrites `/tmp/notebooks/data.js` (library from the Kindle's database, `My Clippings.txt`, all notes, and an acknowledgement for the request). The page reloads it and compares the saved text with what you typed.
- The helper runs from a copy in `/tmp`, so plugging in over USB can't leave it holding files open, and it exits after 15 minutes without the app.

## Building

The helper is cross-compiled by GitHub Actions with [koxtoolchain](https://github.com/KindleModding/koxtoolchain) and [kindle-sdk](https://github.com/KindleModding/kindle-sdk) for `kindlepw2` (soft-float, older firmware) and `kindlehf`.

Tests run on any machine:

```sh
./tests/run_core_tests.sh      # helper: paths, base64, JSON, atomic writes, URL encoding
node tests/test_lib.js         # app logic: clippings, library, notebooks, lists
```

## Tested on

Kindle Paperwhite 2 (6th gen), firmware 5.12.2.2. Other models and firmware are untested.

## Credits

- Inspired by [KNotes](https://github.com/crizmo/KNotes) by kurizu; Notebooks is a separate app written from scratch.
- LIPC header from [openlipc](https://github.com/arkq/openlipc) (MIT).
- The Kindle modding community's write-ups on Illusion-style mesquite apps.

## License

MIT
