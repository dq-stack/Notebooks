/*
 * Click-through test of the app in headless Chromium with a fake helper.
 *
 *   NODE_PATH=<dir with playwright> node tests/browser_test.js [screenshot dir]
 *
 * The fake helper behaves like notebooks_helper: it stores notes, acks each
 * request id and serves a fresh data.js. Set FAIL_SAVES=1 inside the test to
 * make saves fail and check the app says so.
 */
"use strict";

var assert = require("assert");
var fs = require("fs");
var http = require("http");
var path = require("path");
var playwright = require("playwright");

var APP = path.join(__dirname, "..", "app");
var SHOTS = process.argv[2] || "";
var CHROME = process.env.NB_CHROME || undefined;

var row = function (f) { return f.join("\x1f"); };
var LIBRARY = [
    row(["/mnt/us/documents/Walden.azw3", "Walden", "Thoreau, Henry David", "30", "1792300000", "a", "EBOK"]),
    row(["/mnt/us/documents/Meditations.azw3", "Meditations", "Aurelius, Marcus", "100", "1792000000", "b", "EBOK"]),
    row(["/mnt/us/documents/Frankenstein.azw3", "Frankenstein", "Shelley, Mary", "62", "1791900000", "c", "EBOK"])
].join("\n");

var CLIPPINGS = [
    "Walden (Henry David Thoreau)",
    "- Your Highlight at location 1095-1096 | Added on Friday, 17 October 2026 21:04:10",
    "",
    "The mass of men lead lives of quiet desperation.",
    "==========",
    "Walden (Henry David Thoreau)",
    "- Your Highlight at location 1308-1310 | Added on Wednesday, 15 October 2026 20:00:00",
    "",
    "I went to the woods because I wished to live deliberately, to front only the essential facts of life.",
    "==========",
    "Walden (Henry David Thoreau)",
    "- Your Note at location 1310 | Added on Wednesday, 15 October 2026 20:01:30",
    "",
    "Plan a phone-free weekend and see what is actually essential.",
    "==========",
    "Meditations (Marcus Aurelius)",
    "- Your Highlight at location 200-201 | Added on Monday, 13 October 2026 07:00:00",
    "",
    "Begin the morning by saying to thyself, I shall meet with the busy-body, the ungrateful, arrogant, deceitful, envious, unsocial.",
    "=========="
].join("\r\n");

var store = {
    "Walden/2026-10-18 201503.txt": "Thoreau lists every cent the cabin cost him. Try that for one week: write down what each day actually costs.",
    "Loose notes/2026-10-18 090000.txt": "Books to look for:\n[ ] The Maine Woods\n[x] Cape Cod"
};
var acks = {};
var opened = [];
var failSaves = false;

function dataJs() {
    var notes = Object.keys(store).map(function (p) {
        return { path: p, mtime: 1792310000, text: store[p] };
    });
    return "window.NB_DATA=" + JSON.stringify({
        generated: Date.now(), acks: acks, library: LIBRARY, clippings: CLIPPINGS, notes: notes
    }) + ";";
}

function helper(prop, msg) {
    var nl = msg.indexOf("\n");
    var id = nl < 0 ? msg : msg.slice(0, nl);
    var args = nl < 0 ? "" : msg.slice(nl + 1);

    if (prop === "save") {
        var cut = args.indexOf("\n");
        var rel = args.slice(0, cut);

        if (failSaves) {
            acks[id] = "error: could not write note";
            return;
        }

        store[rel] = Buffer.from(args.slice(cut + 1), "base64").toString("utf8");
    } else if (prop === "remove") {
        delete store[args];
    } else if (prop === "open") {
        opened.push(args);
    }

    if (prop !== "ping") {
        acks[id] = "ok";
    }
}

function serve() {
    return new Promise(function (resolve) {
        var server = http.createServer(function (req, res) {
            var url = req.url.split("?")[0];

            if (url === "/pages.js") {
                res.writeHead(200, { "Content-Type": "application/javascript; charset=utf-8" });
                return res.end("window.NB_PAGES=" + JSON.stringify({ "/mnt/us/documents/Walden.azw3": { step: 1500, count: 500 } }) + ";");
            }

            if (url === "/data.js") {
                res.writeHead(200, { "Content-Type": "application/javascript; charset=utf-8" });
                return res.end(dataJs());
            }

            var file = path.join(APP, url === "/" ? "index.html" : url.slice(1));

            if (file.indexOf(APP) !== 0 || !fs.existsSync(file)) {
                res.writeHead(404);
                return res.end();
            }

            var type = { ".html": "text/html", ".js": "application/javascript", ".css": "text/css" }[path.extname(file)] || "text/plain";
            res.writeHead(200, { "Content-Type": type + "; charset=utf-8" });
            fs.createReadStream(file).pipe(res);
        });
        server.listen(0, "127.0.0.1", function () {
            resolve(server);
        });
    });
}

async function shot(page, name) {
    if (SHOTS) {
        await page.screenshot({ path: path.join(SHOTS, name + ".png") });
    }
}

async function visible(page, sel) {
    return page.$eval(sel, function (e) { return e.offsetParent !== null; });
}

async function main() {
    var server = await serve();
    var base = "http://127.0.0.1:" + server.address().port + "/";
    var browser = await playwright.chromium.launch({ executablePath: CHROME });
    var page = await browser.newPage({ viewport: { width: 758, height: 908 } });
    var errors = [];

    page.on("pageerror", function (e) { errors.push(e.message); });
    await page.exposeFunction("__helper", helper);
    await page.addInitScript(function () {
        window.NB_DATA_URL = "/data.js";
        window.NB_PAGES_URL = "/pages.js";
        window.kindle = {
            messaging: {
                sendStringMessage: function (app, prop, msg) { window.__helper(prop, msg); },
                sendMessage: function () {}
            },
            appmgr: {}
        };
    });

    await page.goto(base);
    await page.waitForFunction(function () { return document.getElementById("reading-title").textContent === "Walden"; });

    assert.strictEqual(await page.textContent("#reading-percent"), "30%");
    assert.strictEqual(await page.textContent("#reading-counts"), "1 note · 2 highlights");
    var rows = await page.$$eval("#home-list .row-title", function (n) { return n.map(function (e) { return e.textContent; }); });
    assert.deepStrictEqual(rows.sort(), ["Loose notes", "Meditations"]);
    await shot(page, "1-home");

    /* Notebook: newest first, three kinds of entry */
    await page.click("#reading-open");
    assert.ok(await visible(page, "#notebook"));
    var heads = await page.$$eval("#nb-list .entry-head span:first-child", function (n) { return n.map(function (e) { return e.textContent; }); });
    assert.deepStrictEqual(heads, ["Your note", "Highlight · Page 110", "Kindle note · Page 131"]);
    await shot(page, "2-notebook");

    await page.click('[data-tab="highlights"]');
    assert.strictEqual((await page.$$("#nb-list .entry")).length, 2);
    await page.click('[data-tab="all"]');

    /* Read book */
    await page.click("#nb-read");
    await page.waitForTimeout(800);
    assert.deepStrictEqual(opened, ["/mnt/us/documents/Walden.azw3"]);

    /* New note with a checklist, saved by Done */
    await page.click("#nb-new");
    assert.ok(await visible(page, "#editor"));
    await page.click("#ed-text");
    assert.strictEqual(await page.$eval("#editor .toolbar", function (e) { return getComputedStyle(e).bottom; }), "351px");
    await page.keyboard.type("Experiments");
    await page.keyboard.press("Enter");
    await page.click("#tb-check");
    await page.keyboard.type("Walk without a phone");
    await page.keyboard.press("Enter");
    await page.keyboard.type("Cook from scratch");
    await shot(page, "3-editor");
    await page.click("#ed-done");
    await page.waitForFunction(function () { return !document.getElementById("notebook").classList.contains("hidden"); });

    var saved = Object.keys(store).filter(function (p) { return /^Walden\//.test(p) && store[p].indexOf("Experiments") === 0; });
    assert.strictEqual(saved.length, 1);
    assert.strictEqual(store[saved[0]], "Experiments\n[ ] Walk without a phone\n[ ] Cook from scratch");

    /* Tick a checklist item from the notebook view */
    await page.click("#nb-list .check-line >> nth=0");
    await page.waitForTimeout(1200);
    assert.strictEqual(store[saved[0]], "Experiments\n[x] Walk without a phone\n[ ] Cook from scratch");

    /* Autosave while typing */
    await page.click("#nb-list .entry-head >> nth=0");
    await page.click("#ed-text");
    await page.keyboard.press("End");
    await page.keyboard.type(" today");
    await page.waitForFunction(function () { return document.getElementById("ed-status").textContent === "Saved"; }, null, { timeout: 8000 });
    assert.ok(/Cook from scratch today$/.test(store[saved[0]]));

    /* Failing helper: the app must not claim it saved */
    failSaves = true;
    await page.keyboard.type("!");
    await page.click("#ed-done");
    await page.waitForFunction(function () { return document.getElementById("ed-status").textContent === "Not saved"; }, null, { timeout: 12000 });
    assert.ok(await visible(page, "#editor"));
    failSaves = false;

    /* Delete via the bin */
    await page.click("#tb-delete");
    await page.click("#modal-ok");
    await page.waitForFunction(function () { return !document.getElementById("notebook").classList.contains("hidden"); });
    assert.strictEqual(store[saved[0]], undefined);

    /* Loose notes notebook from home */
    await page.click("#nb-back");
    await page.click("#home-list .row >> text=Loose notes");
    assert.strictEqual(await page.textContent("#nb-title"), "Loose notes");
    assert.ok(!(await visible(page, "#nb-read")));
    await shot(page, "4-loose");

    assert.deepStrictEqual(errors, []);
    await browser.close();
    server.close();
    console.log("browser: all checks passed");
}

main().catch(function (e) {
    console.error("browser: FAILED\n" + (e && e.stack));
    process.exit(1);
});
