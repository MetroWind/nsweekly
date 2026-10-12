"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const {attachmentMarkdown} = require("../statics/games/journal.js");
const record = {filename: 'a[b](c) 日本 " \\ \n.png',
    public_url: "/attachment/hash/a%5Bb%5D(c)%20.png", mime_type: "image/png"};
assert.equal(attachmentMarkdown(record),
    '![a\\[b\\](c) 日本 " \\\\  .png](</attachment/hash/a%5Bb%5D(c)%20.png>)');
for(const mime_type of ["image/svg+xml", "text/html", "application/pdf"])
{
    assert.ok(!attachmentMarkdown({...record, mime_type}).startsWith("!"));
}
const server = fs.readFileSync(path.join(__dirname,
    "../src/attachment/attachment_module.cpp"), "utf8");
const block = server.slice(server.indexOf("bool inline_image ="),
    server.indexOf("std::string ascii;"));
const inline_types = [...block.matchAll(/"(image\/[^"]+)"/g)]
    .map(function(match) { return match[1]; });
assert.deepEqual(inline_types, ["image/jpeg", "image/png", "image/webp",
    "image/avif", "image/jxl"]);
for(const mime_type of inline_types)
{
    assert.ok(attachmentMarkdown({...record, mime_type}).startsWith("!"));
}
for(const public_url of ["/bad<url", "/bad>url", "/bad\nurl"])
{
    assert.throws(function() { attachmentMarkdown({...record, public_url}); });
}
console.log("Journal Markdown tests passed");
