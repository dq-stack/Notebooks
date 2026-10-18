/* Run: node tests/test_lib.js [path/to/My Clippings.txt] */
"use strict";

var assert = require("assert");
var fs = require("fs");
var path = require("path");
var NB = require(path.join(__dirname, "..", "app", "lib.js"));

var tests = [];

function test(name, fn) {
    tests.push({ name: name, fn: fn });
}

var CLIPS = [
    "﻿Walden (Henry David Thoreau)",
    "- Your Highlight at location 1095-1096 | Added on Friday, 17 October 2026 21:04:10",
    "",
    "The mass of men lead lives of quiet desperation.",
    "==========",
    "﻿Walden (Henry David Thoreau)",
    "- Your Highlight at location 1308-1310 | Added on Wednesday, 15 October 2026 20:00:00",
    "",
    "I went to the woods because I wished to live deliberately",
    "==========",
    "﻿Walden (Henry David Thoreau)",
    "- Your Note at location 1310 | Added on Wednesday, 15 October 2026 20:01:30",
    "",
    "Plan a phone-free weekend.",
    "==========",
    "﻿Walden (Henry David Thoreau)",
    "- Your Bookmark on page 4 | location 60 | Added on Wednesday, 15 October 2026 20:02:00",
    "",
    "",
    "==========",
    "﻿Walden (Henry David Thoreau)",
    "- Your Highlight at location 1095-1097 | Added on Saturday, 18 October 2026 08:00:00",
    "",
    "The mass of men lead lives of quiet desperation. What is called resignation is confirmed desperation.",
    "==========",
    "﻿Meditations (Marcus Aurelius;George Long)",
    "- Your Note on page 27 | location 402 | Added on Sunday, 12 October 2026 17:17:55",
    "",
    "A note with nothing highlighted nearby.",
    "==========",
    ""
].join("\r\n");

test("parseAdded handles day-first and US formats", function () {
    assert.strictEqual(NB.parseAdded("Thursday, 1 October 2026 09:32:18"), new Date(2026, 9, 1, 9, 32, 18).getTime());
    assert.strictEqual(NB.parseAdded("Thursday, October 1, 2026 9:32:18 PM"), new Date(2026, 9, 1, 21, 32, 18).getTime());
    assert.strictEqual(NB.parseAdded("Thursday, October 1, 2026 12:05:00 AM"), new Date(2026, 9, 1, 0, 5, 0).getTime());
    assert.strictEqual(NB.parseAdded("nonsense"), 0);
});

test("parseClippings reads highlights and notes, skips bookmarks", function () {
    var c = NB.parseClippings(CLIPS);
    assert.strictEqual(c.length, 5);
    assert.strictEqual(c[0].title, "Walden");
    assert.strictEqual(c[0].author, "Henry David Thoreau");
    assert.deepStrictEqual([c[0].locStart, c[0].locEnd], [1095, 1096]);
    assert.strictEqual(c[2].kind, "note");
    assert.strictEqual(c[4].author, "Marcus Aurelius, George Long");
    assert.strictEqual(c[4].page, 27);
    assert.strictEqual(c[4].locStart, 402);
});

test("kindleItems keeps the newest edit and attaches notes to highlights", function () {
    var items = NB.kindleItems(NB.parseClippings(CLIPS));
    var walden = items.filter(function (i) { return i.title === "Walden"; });
    assert.strictEqual(walden.length, 2);

    var edited = walden.filter(function (i) { return i.locStart === 1095; })[0];
    assert.ok(/resignation/.test(edited.quote));

    var withNote = walden.filter(function (i) { return i.locStart === 1308; })[0];
    assert.strictEqual(withNote.type, "kindle-note");
    assert.strictEqual(withNote.note, "Plan a phone-free weekend.");

    var lone = items.filter(function (i) { return i.title === "Meditations"; })[0];
    assert.strictEqual(lone.type, "kindle-note");
    assert.strictEqual(lone.quote, "");
});

test("notebookName makes folder names the helper accepts", function () {
    assert.strictEqual(NB.notebookName("Never Split the Difference: Negotiating as if"), "Never Split the Difference");
    assert.strictEqual(NB.notebookName("AC/DC: High Voltage"), "AC DC");
    assert.strictEqual(NB.notebookName("...Hidden"), "Hidden");
    assert.strictEqual(NB.notebookName(""), "Untitled");
    assert.ok(NB.notebookName(new Array(100).join("ab")).length <= 60);
});

test("note file names round-trip to creation time", function () {
    var t = new Date(2026, 9, 18, 20, 15, 3).getTime();
    assert.strictEqual(NB.noteFileName(t), "2026-10-18 201503.txt");
    assert.strictEqual(NB.noteCreated("2026-10-18 201503.txt"), t);
    assert.strictEqual(NB.noteCreated("my note.txt"), 0);
});

test("parseLibrary keeps books only and flips 'Last, First'", function () {
    var row = function (f) { return f.join("\x1f"); };
    var books = NB.parseLibrary([
        row(["/mnt/us/documents/Walden.azw3", "Walden", "Thoreau, Henry David", "30.4", "1792300000", "abc", "EBOK"]),
        row(["/mnt/us/documents/Notebooks.sh", "Notebooks", "", "0", "1792400000", "def", "PDOC"]),
        row(["/mnt/us/documents/x.pdf", "", "", "", "5", "", ""])
    ].join("\n"));
    assert.strictEqual(books.length, 2);
    assert.strictEqual(books[0].author, "Henry David Thoreau");
    assert.strictEqual(books[0].percent, 30);
    assert.strictEqual(books[1].title, "x.pdf");
    assert.strictEqual(NB.thumbnailPath(books[0]), "file:///mnt/us/system/thumbnails/thumbnail_abc_EBOK_portrait.jpg");
});

test("buildNotebooks groups everything and sorts newest first", function () {
    var row = function (f) { return f.join("\x1f"); };
    var model = NB.buildNotebooks({
        library: [
            row(["/mnt/us/documents/Walden.azw3", "Walden", "Thoreau, Henry David", "30", "1792300000", "a", "EBOK"]),
            row(["/mnt/us/documents/Frankenstein.azw3", "Frankenstein", "Shelley, Mary", "62", "1792000000", "b", "EBOK"])
        ].join("\n"),
        clippings: CLIPS,
        notes: [
            { path: "Walden/2026-10-18 201503.txt", mtime: 1792310000, text: "Track spending" },
            { path: "Loose notes/2026-10-01 090000.txt", mtime: 1791000000, text: "Shopping" }
        ]
    });

    assert.strictEqual(model.readingNow.title, "Walden");
    assert.strictEqual(model.readingNow.notes.length, 1);
    assert.strictEqual(model.readingNow.kindle.length, 2);

    var names = model.notebooks.map(function (n) { return n.name; });
    assert.ok(names.indexOf("Meditations") >= 0, "clippings-only books get a notebook");
    assert.ok(names.indexOf("Loose notes") >= 0);
    assert.ok(names.indexOf("Frankenstein") < 0, "books with nothing in them are hidden");

    var all = NB.entries(model.readingNow, "all");
    assert.strictEqual(all[0].type, "note");
    assert.strictEqual(NB.entries(model.readingNow, "notes").length, 1);
    assert.strictEqual(NB.entries(model.readingNow, "highlights").length, 2);
});

test("pageFor matches the pages the Kindle itself records", function () {
    /* Plutarch's Lives (calibre APNX, 2300 per page): Kindle said page 27 at location 402 */
    var even = { step: 2300, count: 2815 };
    assert.strictEqual(NB.pageFor(even, 402), 27);
    assert.strictEqual(NB.pageFor(even, 352), 23);
    assert.strictEqual(NB.pageFor(even, 575), 38);
    assert.strictEqual(NB.pageFor(even, 1), 1);
    assert.strictEqual(NB.pageFor(even, 999999), 2815);

    var listed = [488, 47624, 49660, 51649];
    assert.strictEqual(NB.pageFor(listed, 1), 1);
    assert.strictEqual(NB.pageFor(listed, 330), 2);
    assert.strictEqual(NB.pageFor(listed, 345), 3);
    assert.strictEqual(NB.pageFor(listed, 99999), 4);
    assert.strictEqual(NB.pageFor(null, 5), null);
    assert.strictEqual(NB.pageFor(even, null), null);
});

test("toggleList adds and removes list markers", function () {
    var r = NB.toggleList("one\ntwo", 0, 7, "bullet");
    assert.strictEqual(r.text, "• one\n• two");
    assert.strictEqual(NB.toggleList(r.text, 0, r.text.length, "bullet").text, "one\ntwo");
    assert.strictEqual(NB.toggleList("a\nb", 0, 3, "number").text, "1. a\n2. b");
    assert.strictEqual(NB.toggleList("• a", 2, 2, "check").text, "[ ] a");
    assert.strictEqual(NB.toggleList("x\nkeep me\ny", 3, 3, "check").text, "x\n[ ] keep me\ny");
});

test("continueList carries lists on and ends them on an empty item", function () {
    assert.deepStrictEqual(NB.continueList("• a\n", 4), { text: "• a\n• ", start: 6, end: 6 });
    assert.strictEqual(NB.continueList("3. c\n", 5).text, "3. c\n4. ");
    assert.strictEqual(NB.continueList("[x] done\n", 9).text, "[x] done\n[ ] ");
    assert.deepStrictEqual(NB.continueList("• a\n• \n", 6 + 1), { text: "• a\n", start: 4, end: 4 });
    assert.strictEqual(NB.continueList("plain\n", 6), null);
});

test("toggleCheck flips only checklist lines", function () {
    assert.strictEqual(NB.toggleCheck("[ ] a\nb", 0), "[x] a\nb");
    assert.strictEqual(NB.toggleCheck("[x] a", 0), "[ ] a");
    assert.strictEqual(NB.toggleCheck("b", 0), "b");
});

var real = process.argv[2];

if (real) {
    test("real clippings file parses cleanly", function () {
        var raw = fs.readFileSync(real, "utf8");
        var clips = NB.parseClippings(raw);
        var items = NB.kindleItems(clips);
        var notes = items.filter(function (i) { return i.note; });
        var attached = notes.filter(function (i) { return i.quote; });
        var undated = clips.filter(function (c) { return !c.added; });

        console.log("  real file: " + clips.length + " clippings -> " + items.length + " items, " +
            notes.length + " with notes (" + attached.length + " attached to a highlight), " +
            undated.length + " without a parsed date");
        assert.strictEqual(undated.length, 0);
    });
}

var failed = 0;

tests.forEach(function (t) {
    try {
        t.fn();
        console.log("ok   " + t.name);
    } catch (e) {
        failed++;
        console.log("FAIL " + t.name + "\n     " + (e && e.message));
    }
});

console.log(failed ? failed + " failed" : "all " + tests.length + " passed");
process.exit(failed ? 1 : 0);
