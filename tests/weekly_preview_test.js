const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");

function createPreview(fail_worker = false)
{
    const editor = {value: "Initial", addEventListener(type, callback)
    {
        this.input = callback;
    }};
    const preview = {innerHTML: ""};
    const status = {textContent: "Loading"};
    const callbacks = {};
    const requests = [];
    let timer;
    class Worker
    {
        constructor()
        {
            if(fail_worker)
            {
                throw new Error("Worker unavailable");
            }
        }
        addEventListener(type, callback)
        {
            callbacks[type] = callback;
        }
        postMessage(request)
        {
            requests.push(request);
        }
        terminate() {}
    }
    const context = {
        document: {querySelector(selector)
        {
            return {"#EditView textarea": editor,
                "#Preview .Weekly": preview, "#PreviewStatus": status}[selector];
        }},
        Worker, URL, setTimeout(callback) { timer = callback; },
        clearTimeout() { timer = undefined; }
    };
    const source = fs.readFileSync("statics/weekly/preview.js", "utf8")
        .replace("import.meta.url", '"https://example.org/statics/weekly/preview.js"');
    vm.runInNewContext(source, context);
    return {editor, preview, status, requests,
        receive(result) { callbacks.message({data: result}); },
        flush() { if(timer) { timer(); timer = undefined; } },
        fail() { callbacks.error({message: "Worker crashed"}); }};
}

const app = createPreview();
app.editor.value = "Latest before ready";
app.editor.input();
app.flush();
assert.equal(app.requests.length, 0);
app.receive({type: "ready"});
assert.equal(app.requests[0].source, "Latest before ready");
app.editor.value = "%code{new}";
app.editor.input();
app.flush();
assert.equal(app.requests.length, 1);
app.receive({type: "result", id: 1, html: "stale"});
assert.equal(app.preview.innerHTML, "");
assert.equal(app.requests[1].source, "%code{new}");
app.receive({type: "result", id: 2, html: "<code>new</code>"});
assert.equal(app.preview.innerHTML, "<code>new</code>");
app.editor.input();
app.flush();
app.receive({type: "error", id: 3, message: "Bad input"});
assert.equal(app.preview.innerHTML, "<code>new</code>");
assert.match(app.status.textContent, /Bad input/);
app.fail();
assert.match(app.status.textContent, /Worker crashed/);
assert.match(createPreview(true).status.textContent, /Worker unavailable/);
console.log("Weekly preview tests passed.");
