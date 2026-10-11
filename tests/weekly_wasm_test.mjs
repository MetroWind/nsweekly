import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import createMacroDown from "../statics/macrodown/macrodown.mjs";

const renderer = await createMacroDown({
    wasmBinary: readFileSync(new URL(
        "../statics/macrodown/macrodown.wasm", import.meta.url))
});
assert.equal(renderer.renderMarkdown("# 中文 café"),
    "<h1>中文 café</h1>\n");
const source = "%def[highlight]{text}{<mark>%text</mark>}\n\n" +
    "%highlight{Hello}";
assert.match(renderer.renderMarkdown(source), /<mark>Hello<\/mark>/);
assert.doesNotMatch(renderer.renderMarkdown("%highlight{Other}"), /<mark>/);
assert.equal(renderer.renderMarkdown("%code{hello}"),
    "<p><code>hello</code></p>\n");
assert.equal(renderer.renderMarkdown(""), "");
console.log("Weekly Wasm rendering tests passed.");
