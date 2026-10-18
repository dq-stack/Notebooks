/*
 * Notebooks: screens and talking to the helper. ES5 only (Kindle mesquite).
 *
 * Data flow: the helper keeps /tmp/notebooks/data.js up to date. Every
 * change is sent to the helper over LIPC with a request id; we then reload
 * data.js until it acknowledges that id, and only report "Saved" when the
 * note text it read back from disk matches what was sent.
 */
var NBApp = (function () {
    "use strict";

    var APP_ID = "com.dqstack.notebooks";
    var HELPER = "com.dqstack.notebooks.helper";
    var DATA_URL = window.NB_DATA_URL || "file:///tmp/notebooks/data.js";
    var PAGES_URL = window.NB_PAGES_URL || "file:///tmp/notebooks/pages.js";
    var AUTOSAVE_MS = 2500;
    var ACK_TIMEOUT_MS = 8000;
    var PING_MS = 60000;
    var FONT_SIZES = [20, 23, 26, 30, 34];

    var NB = window.NBLib;
    var data = null;
    var model = null;
    var current = null;     /* open notebook */
    var tab = "all";
    var editing = null;     /* { path, isNew, savedText, created } */
    var saveTimer = null;
    var saving = false;
    var requestSeq = 0;
    var fontIndex = 2;
    var modalAction = null;


    function $(id) {
        return document.getElementById(id);
    }

    function show(id) {
        var screens = ["home", "notebook", "editor"];

        for (var i = 0; i < screens.length; i++) {
            var cls = $(screens[i]).className;

            if (screens[i] === id) {
                $(screens[i]).className = cls.replace(/\s*hidden/g, "");
            } else if (cls.indexOf("hidden") < 0) {
                $(screens[i]).className = cls + " hidden";
            }
        }
    }

    function el(tag, cls, text) {
        var e = document.createElement(tag);

        if (cls) {
            e.className = cls;
        }

        if (text !== undefined && text !== null) {
            e.appendChild(document.createTextNode(text));
        }

        return e;
    }

    function clear(node) {
        while (node.firstChild) {
            node.removeChild(node.firstChild);
        }
    }

    function toast(text, ms) {
        var t = $("toast");
        t.textContent = text;
        t.className = "toast";
        setTimeout(function () {
            t.className = "toast hidden";
        }, ms || 2500);
    }


    /* ----- dates ----- */

    var MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
    var DAYS = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];

    function sameDay(a, b) {
        return a.getFullYear() === b.getFullYear() && a.getMonth() === b.getMonth() && a.getDate() === b.getDate();
    }

    function shortDate(t) {
        if (!t) {
            return "";
        }

        var d = new Date(t);
        return sameDay(d, new Date()) ? "Today" : d.getDate() + " " + MONTHS[d.getMonth()];
    }

    function longDate(t) {
        var d = new Date(t);
        return DAYS[d.getDay()] + " " + d.getDate() + " " + MONTHS[d.getMonth()];
    }

    function plural(n, word) {
        return n + " " + word + (n === 1 ? "" : "s");
    }


    /* ----- helper + data.js ----- */

    function send(prop, payload) {
        var id = "r" + new Date().getTime() + "-" + (++requestSeq);

        try {
            window.kindle.messaging.sendStringMessage(HELPER, prop, id + (payload === undefined ? "" : "\n" + payload));
        } catch (e) {
            return null;
        }

        return id;
    }

    function loadPages(callback) {
        var s = document.createElement("script");
        s.charset = "utf-8";
        s.src = PAGES_URL + "?t=" + new Date().getTime();
        s.onload = s.onerror = function () {
            callback();
        };
        document.getElementsByTagName("head")[0].appendChild(s);
    }

    function loadData(callback) {
        var old = $("nb-data");

        if (old) {
            old.parentNode.removeChild(old);
        }

        var s = document.createElement("script");
        s.id = "nb-data";
        s.charset = "utf-8";
        s.src = DATA_URL + "?t=" + new Date().getTime();
        s.onload = function () {
            callback(window.NB_DATA || null);
        };
        s.onerror = function () {
            callback(null);
        };
        document.getElementsByTagName("head")[0].appendChild(s);
    }

    /* Reload data.js until the helper has answered request id (or give up). */
    function waitForAck(id, callback) {
        var started = new Date().getTime();

        function poll() {
            loadData(function (d) {
                if (d && d.acks && d.acks[id] !== undefined) {
                    setData(d);
                    callback(d.acks[id]);
                } else if (new Date().getTime() - started > ACK_TIMEOUT_MS) {
                    callback(null);
                } else {
                    setTimeout(poll, 400);
                }
            });
        }

        setTimeout(poll, 250);
    }

    function setData(d) {
        data = d;
        model = NB.buildNotebooks(d);

        if (current) {
            current = findNotebook(current.name) || current;
        }
    }

    function findNotebook(name) {
        for (var i = 0; i < model.notebooks.length; i++) {
            if (model.notebooks[i].name === name) {
                return model.notebooks[i];
            }
        }

        return null;
    }

    function noteOnDisk(path) {
        var notes = (data && data.notes) || [];

        for (var i = 0; i < notes.length; i++) {
            if (notes[i].path === path) {
                return notes[i];
            }
        }

        return null;
    }


    /* ----- paging (lists scroll a whole screen at a time; e-ink friendly) ----- */

    function updatePager(listId) {
        var list = $(listId);
        var h = list.clientHeight || 1;
        var pages = Math.max(1, Math.ceil((list.scrollHeight - 2) / h));
        var page = Math.min(pages, Math.round(list.scrollTop / h) + 1);
        $(listId + "-page").textContent = pages > 1 ? "Page " + page + " of " + pages : "";

        var buttons = document.querySelectorAll('[data-pager="' + listId + '"]');

        for (var i = 0; i < buttons.length; i++) {
            buttons[i].style.visibility = pages > 1 ? "visible" : "hidden";
        }
    }

    function turnPage(listId, dir) {
        var list = $(listId);
        list.scrollTop = Math.max(0, list.scrollTop + dir * list.clientHeight);
        updatePager(listId);
    }


    /* ----- covers ----- */

    function coverInto(box, nb, size) {
        clear(box);
        box.className = "cover " + size + (nb && nb.name === NB.LOOSE ? " loose" : "");

        if (nb && nb.name === NB.LOOSE) {
            box.innerHTML = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M4 4h12l4 4v12H4z"/><path d="M8 12h8M8 16h5"/></svg>';
            return;
        }

        /* Without a thumbnail, small covers show the first letter; titles don't fit. */
        var title = nb ? nb.title : "";
        var text = el("div", "cover-text serif", size === "big" ? title : title.replace(/^(the|a|an)\s+/i, "").charAt(0).toUpperCase());
        box.appendChild(text);

        var src = nb && nb.book ? NB.thumbnailPath(nb.book) : "";

        if (src) {
            var img = document.createElement("img");
            img.onerror = function () {
                if (img.parentNode) {
                    img.parentNode.removeChild(img);
                }
            };
            img.src = src;
            box.appendChild(img);
        }
    }

    function counts(nb) {
        var highlights = nb.kindle.length;
        var parts = [plural(nb.notes.length, "note")];

        if (highlights) {
            parts.push(plural(highlights, "highlight"));
        }

        return parts;
    }

    function progressText(nb) {
        if (!nb.book || nb.book.percent === null) {
            return "";
        }

        return nb.book.percent >= 100 ? "Finished" : nb.book.percent + "%";
    }


    /* ----- home ----- */

    function renderHome() {
        var rn = model.readingNow;

        $("reading").className = rn ? "reading" : "reading hidden";

        if (rn) {
            coverInto($("reading-cover"), rn, "big");
            $("reading-title").textContent = rn.title;
            $("reading-author").textContent = rn.author || "";
            var pct = rn.book && rn.book.percent !== null ? Math.min(100, rn.book.percent) : 0;
            $("reading-bar").style.width = pct + "%";
            $("reading-percent").textContent = pct + "%";
            $("reading-counts").textContent = counts(rn).join(" · ");
        }

        var list = $("home-list");
        clear(list);

        var shown = 0;

        for (var i = 0; i < model.notebooks.length; i++) {
            var nb = model.notebooks[i];

            if (nb === rn) {
                continue;
            }

            list.appendChild(homeRow(nb));
            shown++;
        }

        if (!shown) {
            list.appendChild(el("div", "empty", "Notebooks for your other books appear here once they have notes or highlights."));
        }

        list.scrollTop = 0;
        show("home");
        updatePager("home-list");
    }

    function homeRow(nb) {
        var row = el("div", "row");
        var coverCell = el("div", "cover-cell");
        var cover = el("div");
        coverInto(cover, nb, "small");
        coverCell.appendChild(cover);

        var text = el("div", "row-text");
        text.appendChild(el("div", "row-title", nb.title));

        var meta = nb.name === NB.LOOSE ? ["Not tied to a book", plural(nb.notes.length, "note")] : counts(nb);
        var prog = progressText(nb);

        if (prog) {
            meta.push(prog);
        }

        text.appendChild(el("div", "muted", meta.join(" · ")));

        row.appendChild(coverCell);
        row.appendChild(text);
        row.appendChild(el("div", "row-date", shortDate(nb.last)));
        row.onclick = function () {
            openNotebook(nb);
        };
        return row;
    }


    /* ----- notebook ----- */

    function openNotebook(nb) {
        current = nb;
        tab = "all";
        renderNotebook(true);
    }

    function renderNotebook(resetScroll) {
        var nb = current;
        coverInto($("nb-cover"), nb, "mid");
        $("nb-title").textContent = nb.title;

        var meta = [];

        if (nb.name === NB.LOOSE) {
            meta.push("Notes not tied to a book");
        } else {
            if (nb.author) {
                meta.push(nb.author);
            }

            var prog = progressText(nb);

            if (prog) {
                meta.push(prog === "Finished" ? prog : prog + " read");
            }
        }

        $("nb-meta").textContent = meta.join(" · ");
        $("nb-read").className = nb.book ? "secondary" : "secondary hidden";

        var tabs = document.querySelectorAll(".tab");
        var labels = { all: "All", notes: "Notes " + nb.notes.length, highlights: "Highlights " + nb.kindle.length };

        for (var i = 0; i < tabs.length; i++) {
            var key = tabs[i].getAttribute("data-tab");
            tabs[i].textContent = labels[key];
            tabs[i].className = "tab" + (key === tab ? " active" : "");
        }

        var list = $("nb-list");
        var keep = resetScroll ? 0 : list.scrollTop;
        clear(list);

        var items = NB.entries(nb, tab);

        for (i = 0; i < items.length; i++) {
            list.appendChild(entryNode(items[i]));
        }

        if (!items.length) {
            list.appendChild(el("div", "empty", tab === "highlights" ?
                "Highlights and notes you make while reading show up here." :
                "No notes yet. Tap “+ New note” to write one."));
        }

        show("notebook");
        list.scrollTop = keep;
        updatePager("nb-list");
    }

    function entryHead(left, right) {
        var head = el("div", "entry-head");
        head.appendChild(el("span", "", left));
        head.appendChild(el("span", "", right));
        return head;
    }

    /* Page as the Kindle recorded it, else worked out from the book's APNX, else the location. */
    function where(item) {
        if (item.page !== null) {
            return "Page " + item.page;
        }

        var table = current && current.book && window.NB_PAGES ? window.NB_PAGES[current.book.path] : null;
        var page = NB.pageFor(table, item.locStart);

        if (page) {
            return "Page " + page;
        }

        return item.locStart !== null ? "Location " + item.locStart : "";
    }

    function entryNode(item) {
        var box = el("div", "entry");

        if (item.type === "note") {
            box.appendChild(entryHead("Your note", longDate(item.created)));
            box.appendChild(noteBody(item));
            box.onclick = function () {
                openEditor(item);
            };
            return box;
        }

        var label = item.type === "kindle-note" ? "Kindle note" : "Highlight";
        var loc = where(item);
        box.appendChild(entryHead(loc ? label + " · " + loc : label, item.added ? longDate(item.added) : ""));

        if (item.quote) {
            box.appendChild(el("div", "entry-text quote", "“" + item.quote + "”"));
        }

        if (item.note) {
            var noteRow = el("div", "entry-note");
            noteRow.innerHTML = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M4 20h4L19 9l-4-4L4 16z"/></svg>';
            noteRow.appendChild(el("div", "entry-text", item.note));
            box.appendChild(noteRow);
        }

        return box;
    }

    /* A note's text, with checklist lines that can be ticked in place. */
    function noteBody(item) {
        var body = el("div", "entry-text");
        var lines = item.text.split("\n");

        for (var i = 0; i < lines.length; i++) {
            var m = /^\[([ xX])\] (.*)$/.exec(lines[i]);

            if (!m) {
                body.appendChild(el("span", "", lines[i] + (i < lines.length - 1 ? "\n" : "")));
                continue;
            }

            var line = el("span", "check-line");
            line.appendChild(el("span", "check-box", m[1] === " " ? "☐" : "☑"));
            line.appendChild(document.createTextNode(m[2]));
            line.onclick = tickHandler(item, i);
            body.appendChild(line);
        }

        return body;
    }

    function tickHandler(item, index) {
        return function (e) {
            e.stopPropagation();

            var text = NB.toggleCheck(item.text, index);
            item.text = text;
            renderNotebook(false);
            saveText(item.path, text, function (ok) {
                if (!ok) {
                    toast("Couldn't save that tick. Try again.");
                }
            });
        };
    }


    /* ----- saving ----- */

    function encode(text) {
        return window.btoa(unescape(encodeURIComponent(text)));
    }

    /* Save text to path; callback(true) only once data.js shows the same text on disk. */
    function saveText(path, text, callback) {
        var id = send("save", path + "\n" + encode(text));

        if (!id) {
            callback(false);
            return;
        }

        waitForAck(id, function (result) {
            var onDisk = noteOnDisk(path);
            callback(result === "ok" && !!onDisk && onDisk.text === text);
        });
    }

    function removeNote(path, callback) {
        var id = send("remove", path);

        if (!id) {
            callback(false);
            return;
        }

        waitForAck(id, function (result) {
            callback(result === "ok" && !noteOnDisk(path));
        });
    }


    /* ----- editor ----- */

    function setStatus(text, isError) {
        var s = $("ed-status");
        s.textContent = text;
        s.className = "status" + (isError ? " error" : "");
    }

    function openEditor(note) {
        var now = new Date().getTime();

        editing = note ? {
            path: note.path,
            isNew: false,
            savedText: note.text,
            created: note.created
        } : {
            path: current.name + "/" + NB.noteFileName(now),
            isNew: true,
            savedText: "",
            created: now
        };

        $("ed-title").textContent = current.title;
        $("ed-meta").textContent = "Your note · " + longDate(editing.created);
        $("ed-text").value = editing.savedText;
        $("tb-delete").style.visibility = editing.isNew ? "hidden" : "visible";
        setStatus("");
        applyFont();
        show("editor");
        $("ed-text").focus();
    }

    function scheduleSave() {
        if (saveTimer) {
            clearTimeout(saveTimer);
        }

        saveTimer = setTimeout(function () {
            saveTimer = null;
            saveNow(null);
        }, AUTOSAVE_MS);
    }

    /* Save the editor if it changed; callback(ok) when finished. */
    function saveNow(callback) {
        var text = $("ed-text").value;

        if (!editing || text === editing.savedText || saving) {
            if (callback) {
                callback(!saving);
            }
            return;
        }

        if (editing.isNew && /^\s*$/.test(text)) {
            if (callback) {
                callback(true);
            }
            return;
        }

        saving = true;
        setStatus("Saving…");

        saveText(editing.path, text, function (ok) {
            saving = false;

            if (!editing) {
                return;
            }

            if (ok) {
                editing.savedText = text;
                editing.isNew = false;
                $("tb-delete").style.visibility = "visible";
                setStatus($("ed-text").value === text ? "Saved" : "");
            } else {
                setStatus("Not saved", true);
            }

            if (callback) {
                callback(ok);
            }
        });
    }

    function done() {
        if (saveTimer) {
            clearTimeout(saveTimer);
            saveTimer = null;
        }

        var text = $("ed-text").value;

        /* An emptied note is deleted rather than kept as a blank file. */
        if (/^\s*$/.test(text) && editing && !editing.isNew) {
            setStatus("Deleting…");
            removeNote(editing.path, function (ok) {
                if (ok) {
                    leaveEditor();
                } else {
                    setStatus("Not deleted", true);
                }
            });
            return;
        }

        saveNow(function (ok) {
            if (ok) {
                leaveEditor();
            } else {
                toast("Your note isn't saved yet. Tap Done again to retry.", 3500);
            }
        });
    }

    function leaveEditor() {
        editing = null;
        $("ed-text").blur();
        renderNotebook(false);
    }

    function applyList(kind) {
        var ta = $("ed-text");
        var r = NB.toggleList(ta.value, ta.selectionStart, ta.selectionEnd, kind);
        ta.value = r.text;
        ta.focus();
        ta.setSelectionRange(r.start, r.end);
        scheduleSave();
    }

    function onInput() {
        var ta = $("ed-text");
        var pos = ta.selectionStart;
        var r = pos === ta.selectionEnd ? NB.continueList(ta.value, pos) : null;

        if (r) {
            ta.value = r.text;
            ta.setSelectionRange(r.start, r.end);
        }

        if (editing && ta.value !== editing.savedText) {
            setStatus("");
        }

        scheduleSave();
    }

    function applyFont() {
        $("ed-text").style.fontSize = FONT_SIZES[fontIndex] + "px";
    }

    function changeFont(step) {
        fontIndex = Math.max(0, Math.min(FONT_SIZES.length - 1, fontIndex + step));
        applyFont();

        try {
            window.localStorage.setItem("nb-font", String(fontIndex));
        } catch (e) {
            /* per-device preference only; fine to lose */
        }
    }

    function confirmDelete() {
        modal("Delete this note?", function () {
            if (saveTimer) {
                clearTimeout(saveTimer);
                saveTimer = null;
            }

            setStatus("Deleting…");
            removeNote(editing.path, function (ok) {
                if (ok) {
                    leaveEditor();
                } else {
                    setStatus("Not deleted", true);
                }
            });
        });
    }

    function modal(text, onOk) {
        $("modal-text").textContent = text;
        modalAction = onOk;
        $("modal").className = "modal";
    }

    function closeModal() {
        $("modal").className = "modal hidden";
        modalAction = null;
    }


    /* ----- reading ----- */

    function readBook() {
        if (!current || !current.book) {
            return;
        }

        var id = send("open", current.book.path);

        if (!id) {
            toast("Couldn't open the book.");
            return;
        }

        waitForAck(id, function (result) {
            if (result !== "ok") {
                toast("Couldn't open the book.");
            }
        });
    }


    /* ----- start-up ----- */

    function wire() {
        $("reading-open").onclick = function () {
            if (model.readingNow) {
                openNotebook(model.readingNow);
            }
        };
        $("nb-back").onclick = function () {
            current = null;
            renderHome();
        };
        $("nb-read").onclick = readBook;
        $("nb-new").onclick = function () {
            openEditor(null);
        };

        var tabs = document.querySelectorAll(".tab");

        for (var i = 0; i < tabs.length; i++) {
            tabs[i].onclick = (function (key) {
                return function () {
                    tab = key;
                    renderNotebook(true);
                };
            }(tabs[i].getAttribute("data-tab")));
        }

        var pagers = document.querySelectorAll("[data-pager]");

        for (i = 0; i < pagers.length; i++) {
            pagers[i].onclick = (function (b) {
                return function () {
                    turnPage(b.getAttribute("data-pager"), b.className.indexOf("next") >= 0 ? 1 : -1);
                };
            }(pagers[i]));
        }

        $("ed-done").onclick = done;
        $("ed-text").oninput = onInput;

        /*
         * The Kindle keyboard covers the bottom of the screen without
         * resizing the app, so lift the toolbar above it while typing.
         * Toolbar taps must not blur the text (that would drop it again
         * mid-tap), hence preventDefault on mousedown.
         */
        $("ed-text").onfocus = function () {
            $("editor").className = "screen keyboard";
        };
        $("ed-text").onblur = function () {
            $("editor").className = "screen";
        };

        var tools = document.querySelectorAll(".toolbar button");

        for (i = 0; i < tools.length; i++) {
            tools[i].onmousedown = function (e) {
                e.preventDefault();
            };
        }
        $("tb-bullet").onclick = function () { applyList("bullet"); };
        $("tb-number").onclick = function () { applyList("number"); };
        $("tb-check").onclick = function () { applyList("check"); };
        $("tb-smaller").onclick = function () { changeFont(-1); };
        $("tb-larger").onclick = function () { changeFont(1); };
        $("tb-delete").onclick = confirmDelete;
        $("modal-cancel").onclick = closeModal;
        $("modal-ok").onclick = function () {
            var action = modalAction;
            closeModal();

            if (action) {
                action();
            }
        };
    }

    function configureChrome() {
        try {
            window.kindle.messaging.sendMessage("com.lab126.chromebar", "configureChrome", {
                appId: APP_ID,
                topNavBar: {
                    template: "title",
                    title: "Notebooks",
                    buttons: [
                        { id: "KPP_MORE", state: "enabled", handling: "system" },
                        { id: "KPP_CLOSE", state: "enabled", handling: "system" }
                    ]
                }
            });
        } catch (e) {
            /* the stock bar stays; nothing else depends on it */
        }
    }

    function start() {
        try {
            var saved = parseInt(window.localStorage.getItem("nb-font"), 10);

            if (saved >= 0 && saved < FONT_SIZES.length) {
                fontIndex = saved;
            }
        } catch (e) {
            /* no stored preference */
        }

        wire();

        if (window.kindle && window.kindle.appmgr) {
            window.kindle.appmgr.ongo = configureChrome;
        }

        /* The helper writes data.js as it starts; ask for a fresh copy too. */
        var id = send("refresh");
        var started = new Date().getTime();

        function first() {
            loadData(function (d) {
                if (d && (!id || (d.acks && d.acks[id] !== undefined) || new Date().getTime() - started > 4000)) {
                    setData(d);
                    loadPages(renderHome);
                } else if (new Date().getTime() - started > 15000) {
                    $("home-list").innerHTML = "";
                    $("home-list").appendChild(el("div", "empty",
                        "Notebooks couldn't load your books. Close it and open it again from the Library."));
                    $("reading").className = "reading hidden";
                } else {
                    setTimeout(first, 400);
                }
            });
        }

        first();

        setInterval(function () {
            send("ping");
        }, PING_MS);
    }

    return { start: start };
}());
