/*
 * Notebooks: data and text logic with no DOM access, shared by the app and
 * the Node tests. Written as ES5 for the Kindle's old WebKit.
 */
(function (root) {
    "use strict";

    var LOOSE = "Loose notes";
    var BOOK_EXT = /\.(azw3|azw|mobi|kfx|pdf|txt|prc|azw4)$/i;
    var MONTHS = ["january", "february", "march", "april", "may", "june", "july",
        "august", "september", "october", "november", "december"];


    function trim(s) {
        return String(s).replace(/^[\s﻿]+|[\s﻿]+$/g, "");
    }

    function pad2(n) {
        return (n < 10 ? "0" : "") + n;
    }


    /* ----- library (cc.db rows from the helper, fields split by \x1f) ----- */

    function authorDisplay(collation) {
        var a = trim(collation || "");
        var m = /^([^,]+),\s*([^,]+)$/.exec(a);
        return m ? m[2] + " " + m[1] : a;
    }

    function parseLibrary(tsv) {
        var books = [];
        var lines = String(tsv || "").split("\n");

        for (var i = 0; i < lines.length; i++) {
            var f = lines[i].split("\x1f");

            if (f.length < 7 || !BOOK_EXT.test(f[0])) {
                continue;
            }

            books.push({
                path: f[0],
                title: trim(f[1]) || f[0].replace(/^.*\//, ""),
                author: authorDisplay(f[2]),
                percent: f[3] === "" ? null : Math.round(parseFloat(f[3])),
                lastAccess: parseInt(f[4], 10) || 0,
                cdeKey: f[5],
                cdeType: f[6]
            });
        }

        return books;
    }

    function thumbnailPath(book) {
        if (!book || !book.cdeKey || !book.cdeType) {
            return "";
        }

        return "file:///mnt/us/system/thumbnails/thumbnail_" + book.cdeKey + "_" +
            book.cdeType + "_portrait.jpg";
    }


    /* ----- My Clippings.txt ----- */

    /* "Thursday, 1 October 2026 09:32:18" or "Thursday, October 1, 2026 9:32:18 AM" */
    function parseAdded(raw) {
        var s = trim(raw).replace(/^[A-Za-z]+,\s*/, "");
        var m = /^(\d{1,2}) ([A-Za-z]+) (\d{4}) (\d{1,2}):(\d{2}):(\d{2})/.exec(s);
        var day, month, year, h, min, sec;

        if (m) {
            day = +m[1]; month = m[2]; year = +m[3]; h = +m[4]; min = +m[5]; sec = +m[6];
        } else {
            m = /^([A-Za-z]+) (\d{1,2}), (\d{4}) (\d{1,2}):(\d{2}):(\d{2})(?: ?([AP]M))?/i.exec(s);

            if (!m) {
                return 0;
            }

            month = m[1]; day = +m[2]; year = +m[3]; h = +m[4]; min = +m[5]; sec = +m[6];

            if (m[7]) {
                h = h % 12 + (m[7].toUpperCase() === "PM" ? 12 : 0);
            }
        }

        var mi = MONTHS.indexOf(month.toLowerCase());
        return mi < 0 ? 0 : new Date(year, mi, day, h, min, sec).getTime();
    }

    function splitTitleAuthor(header) {
        var h = trim(header);
        var m = /^(.*) \(([^()]*)\)$/.exec(h);

        if (!m) {
            return { title: h, author: "" };
        }

        return { title: trim(m[1]), author: m[2].split(";").join(", ") };
    }

    function parseClippings(text) {
        var out = [];
        var blocks = String(text || "").replace(/\r\n?/g, "\n").split(/\n?==========\n?/);

        for (var i = 0; i < blocks.length; i++) {
            var lines = blocks[i].replace(/^\n+/, "").split("\n");

            if (lines.length < 2) {
                continue;
            }

            var meta = lines[1];
            var kind = /^- Your (Highlight|Note|Bookmark)\b/i.exec(meta);

            if (!kind || kind[1].toLowerCase() === "bookmark") {
                continue;
            }

            var loc = /location (\d+)(?:-(\d+))?/i.exec(meta);
            var page = /page (\d+)/i.exec(meta);
            var added = /Added on (.*)$/i.exec(meta);
            var ta = splitTitleAuthor(lines[0]);
            var body = trim(lines.slice(2).join("\n"));

            if (!body) {
                continue;
            }

            out.push({
                kind: kind[1].toLowerCase(),
                title: ta.title,
                author: ta.author,
                page: page ? +page[1] : null,
                locStart: loc ? +loc[1] : null,
                locEnd: loc ? +(loc[2] || loc[1]) : null,
                added: added ? parseAdded(added[1]) : 0,
                text: body
            });
        }

        return out;
    }

    /*
     * Turn raw clippings into notebook items. The Kindle appends a new entry
     * every time a highlight or note is edited, so only the newest entry per
     * book + start location is kept. A note is attached to the highlight it
     * was written on (its location is the highlight's end, give or take one).
     */
    function kindleItems(clips) {
        var latest = {};
        var keys = [];

        for (var i = 0; i < clips.length; i++) {
            var c = clips[i];
            var key = c.kind + "\x1f" + normTitle(c.title) + "\x1f" + (c.locStart === null ? "p" + c.page : c.locStart);

            if (!latest[key]) {
                keys.push(key);
                latest[key] = c;
            } else if (c.added >= latest[key].added) {
                latest[key] = c;
            }
        }

        var highlights = [];
        var notes = [];

        for (i = 0; i < keys.length; i++) {
            var e = latest[keys[i]];
            var item = {
                type: e.kind === "note" ? "kindle-note" : "highlight",
                title: e.title,
                author: e.author,
                page: e.page,
                locStart: e.locStart,
                locEnd: e.locEnd,
                added: e.added,
                quote: e.kind === "highlight" ? e.text : "",
                note: e.kind === "note" ? e.text : ""
            };

            (e.kind === "note" ? notes : highlights).push(item);
        }

        var items = highlights.slice();

        for (i = 0; i < notes.length; i++) {
            var n = notes[i];
            var best = null;
            var bestScore = Infinity;

            for (var j = 0; j < highlights.length; j++) {
                var hl = highlights[j];

                if (hl.note || normTitle(hl.title) !== normTitle(n.title) || n.locStart === null || hl.locEnd === null) {
                    continue;
                }

                var off = n.locStart >= hl.locStart && n.locStart <= hl.locEnd + 1 ? 0 :
                    Math.abs(n.locStart - hl.locEnd);

                if (off > 1) {
                    continue;
                }

                var score = off * 1e12 + Math.abs(n.added - hl.added);

                if (score < bestScore) {
                    best = hl;
                    bestScore = score;
                }
            }

            if (best) {
                best.type = "kindle-note";
                best.note = n.note;
                best.added = Math.max(best.added, n.added);
            } else {
                items.push(n);
            }
        }

        return items;
    }


    /*
     * Page number for a Kindle location from a book's APNX table, as the
     * helper sends it: {step, count} for evenly spaced pages, else the list
     * of page start positions. Location L starts at position (L - 1) * 150.
     */
    function pageFor(table, loc) {
        if (!table || loc === null || loc === undefined) {
            return null;
        }

        var pos = (loc - 1) * 150;

        if (table.step) {
            return Math.max(1, Math.min(table.count, Math.floor(pos / table.step) + 1));
        }

        var lo = 0;
        var hi = table.length;

        while (lo < hi) {
            var mid = (lo + hi) >> 1;

            if (table[mid] <= pos) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }

        return table.length ? Math.max(1, lo) : null;
    }


    /* ----- notebooks ----- */

    function normTitle(t) {
        return String(t || "").toLowerCase().replace(/[^a-z0-9]+/g, "");
    }

    /* Folder name for a book's notebook; must pass the helper's valid_note_path(). */
    function notebookName(title) {
        var s = String(title || "").replace(/[\x00-\x1f\x7f\/\\]+/g, " ");
        var colon = s.indexOf(":");

        if (colon >= 3) {
            s = s.slice(0, colon);
        }

        s = trim(s.replace(/\s+/g, " ")).replace(/^[.\s]+/, "");

        if (s.length > 60) {
            s = trim(s.slice(0, 60));
        }

        return s || "Untitled";
    }

    function noteFileName(time) {
        var d = new Date(time);
        return d.getFullYear() + "-" + pad2(d.getMonth() + 1) + "-" + pad2(d.getDate()) + " " +
            pad2(d.getHours()) + pad2(d.getMinutes()) + pad2(d.getSeconds()) + ".txt";
    }

    /* Creation time from "2026-10-18 201503.txt"; 0 if the name was changed by hand. */
    function noteCreated(fileName) {
        var m = /^(\d{4})-(\d{2})-(\d{2}) (\d{2})(\d{2})(\d{2})/.exec(fileName);
        return m ? new Date(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6]).getTime() : 0;
    }

    /*
     * Combine library, clippings and note files into notebooks.
     * Returns { readingNow, notebooks } where each notebook is
     *   { name, title, author, book, notes: [...], kindle: [...], last }
     */
    function buildNotebooks(data) {
        var books = parseLibrary(data.library);
        var items = kindleItems(parseClippings(data.clippings));
        var byName = {};
        var list = [];
        var i;

        function notebookFor(name, title, author) {
            if (!byName[name]) {
                byName[name] = { name: name, title: title, author: author, book: null, notes: [], kindle: [], last: 0 };
                list.push(byName[name]);
            }

            return byName[name];
        }

        var readingNow = null;

        for (i = 0; i < books.length; i++) {
            if (!readingNow || books[i].lastAccess > readingNow.lastAccess) {
                readingNow = books[i];
            }
        }

        for (i = 0; i < books.length; i++) {
            var nbName = notebookName(books[i].title);

            if (!byName[nbName] || books[i].lastAccess > (byName[nbName].book || {}).lastAccess) {
                var nb = notebookFor(nbName, books[i].title, books[i].author);
                nb.book = books[i];
                nb.title = books[i].title;
                nb.author = books[i].author || nb.author;
            }
        }

        for (i = 0; i < items.length; i++) {
            var kb = notebookFor(notebookName(items[i].title), items[i].title, items[i].author);

            if (items[i].author) {
                kb.author = items[i].author;
            }

            kb.kindle.push(items[i]);
            kb.last = Math.max(kb.last, items[i].added);
        }

        var notes = data.notes || [];

        for (i = 0; i < notes.length; i++) {
            var slash = notes[i].path.indexOf("/");
            var folder = notes[i].path.slice(0, slash);
            var file = notes[i].path.slice(slash + 1);
            var owner = notebookFor(folder, folder, "");
            var created = noteCreated(file) || notes[i].mtime * 1000;

            owner.notes.push({
                type: "note",
                path: notes[i].path,
                text: notes[i].text,
                created: created,
                modified: notes[i].mtime * 1000
            });
            owner.last = Math.max(owner.last, notes[i].mtime * 1000);
        }

        notebookFor(LOOSE, LOOSE, "");

        var visible = [];

        for (i = 0; i < list.length; i++) {
            var n = list[i];

            if (n.notes.length || n.kindle.length || n.name === LOOSE ||
                (readingNow && n.book === readingNow)) {
                if (n.book) {
                    n.last = Math.max(n.last, n.book.lastAccess * 1000);
                }

                visible.push(n);
            }
        }

        visible.sort(function (a, b) {
            return b.last - a.last;
        });

        return {
            readingNow: readingNow ? byName[notebookName(readingNow.title)] : null,
            notebooks: visible
        };
    }

    /* Everything in a notebook, newest first (the "All" tab). */
    function entries(nb, filter) {
        var out = [];
        var i;

        if (filter !== "highlights") {
            for (i = 0; i < nb.notes.length; i++) {
                out.push({ when: nb.notes[i].created, item: nb.notes[i] });
            }
        }

        if (filter !== "notes") {
            for (i = 0; i < nb.kindle.length; i++) {
                out.push({ when: nb.kindle[i].added, item: nb.kindle[i] });
            }
        }

        out.sort(function (a, b) {
            return b.when - a.when;
        });

        for (i = 0; i < out.length; i++) {
            out[i] = out[i].item;
        }

        return out;
    }


    /* ----- editor text operations ----- */

    var LIST_RE = /^(• |\d+\. |\[[ xX]\] )/;

    function lineBounds(text, pos) {
        var start = text.lastIndexOf("\n", pos - 1) + 1;
        var end = text.indexOf("\n", pos);
        return { start: start, end: end < 0 ? text.length : end };
    }

    /*
     * Toggle a list style on every line touched by the selection. kind is
     * "bullet", "number" or "check". Returns { text, start, end }.
     */
    function toggleList(text, selStart, selEnd, kind) {
        var first = lineBounds(text, selStart).start;
        var last = lineBounds(text, Math.max(selStart, selEnd)).end;
        var lines = text.slice(first, last).split("\n");
        var want = kind === "bullet" ? /^• / : kind === "number" ? /^\d+\. / : /^\[[ xX]\] /;
        var allHave = true;
        var i;

        for (i = 0; i < lines.length; i++) {
            if (!want.test(lines[i])) {
                allHave = false;
            }
        }

        for (i = 0; i < lines.length; i++) {
            var bare = lines[i].replace(LIST_RE, "");

            if (allHave) {
                lines[i] = bare;
            } else if (kind === "bullet") {
                lines[i] = "• " + bare;
            } else if (kind === "number") {
                lines[i] = (i + 1) + ". " + bare;
            } else {
                lines[i] = "[ ] " + bare;
            }
        }

        var block = lines.join("\n");
        var newText = text.slice(0, first) + block + text.slice(last);
        return { text: newText, start: first + block.length, end: first + block.length };
    }

    /*
     * Called after a newline was typed at pos (just after the "\n"). If the
     * previous line was a list item, carry the list on; an empty item ends
     * the list instead. Returns null when nothing should change.
     */
    function continueList(text, pos) {
        if (pos < 1 || text.charAt(pos - 1) !== "\n") {
            return null;
        }

        var prev = lineBounds(text, pos - 1);
        var line = text.slice(prev.start, prev.end);
        var m = LIST_RE.exec(line);

        if (!m) {
            return null;
        }

        if (trim(line.slice(m[0].length)) === "") {
            var cleared = text.slice(0, prev.start) + text.slice(pos);
            return { text: cleared, start: prev.start, end: prev.start };
        }

        var marker = /^\d+\. /.test(m[0]) ? (parseInt(m[0], 10) + 1) + ". " :
            /^\[/.test(m[0]) ? "[ ] " : m[0];
        var t = text.slice(0, pos) + marker + text.slice(pos);
        return { text: t, start: pos + marker.length, end: pos + marker.length };
    }

    /* Flip "[ ]" / "[x]" at the start of line number index (0-based). */
    function toggleCheck(text, index) {
        var lines = text.split("\n");
        var line = lines[index];

        if (line === undefined || !/^\[[ xX]\] /.test(line)) {
            return text;
        }

        lines[index] = (line.charAt(1) === " " ? "[x]" : "[ ]") + line.slice(3);
        return lines.join("\n");
    }


    var api = {
        LOOSE: LOOSE,
        parseLibrary: parseLibrary,
        thumbnailPath: thumbnailPath,
        pageFor: pageFor,
        parseAdded: parseAdded,
        parseClippings: parseClippings,
        kindleItems: kindleItems,
        normTitle: normTitle,
        notebookName: notebookName,
        noteFileName: noteFileName,
        noteCreated: noteCreated,
        buildNotebooks: buildNotebooks,
        entries: entries,
        toggleList: toggleList,
        continueList: continueList,
        toggleCheck: toggleCheck
    };

    if (typeof module !== "undefined" && module.exports) {
        module.exports = api;
    } else {
        root.NBLib = api;
    }
}(this));
