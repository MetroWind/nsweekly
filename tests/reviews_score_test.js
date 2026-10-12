"use strict";

const ASSERT = require("node:assert/strict");
const FS = require("node:fs");
const PATH = require("node:path");
const VM = require("node:vm");
const SOURCE = FS.readFileSync(
    PATH.join(__dirname, "../statics/games/reviews.js"), "utf8");
const CONTEXT = VM.createContext({
    document: {querySelectorAll: emptyReviewRows, getElementById: noReviewForm}
});
VM.runInContext(SOURCE, CONTEXT);

function emptyReviewRows()
{
    return [];
}

function noReviewForm()
{
    return null;
}

function calculate(values)
{
    CONTEXT.values = values;
    return VM.runInContext("reviewScore(values)", CONTEXT);
}

// Spreadsheet sample, neutral score, endpoints, and fractional inputs.
ASSERT.equal(calculate([10, 10, 8, 10, 10]), 58 / 6);
ASSERT.equal(calculate([10, 10, 8, 10, 10]).toFixed(1), "9.7");
ASSERT.equal(calculate([5, 5, 5, 5, 5]), 5);
ASSERT.equal(calculate([1, 1, 1, 1, 1]), 1);
ASSERT.equal(calculate([10, 10, 10, 10, 10]), 10);
ASSERT.equal(calculate([5, 8.5, 5, 5, 5]), 37 / 6);
ASSERT.equal(calculate(["5", "5", "5", "5", "5"]), 5);

// Drafts and invalid controls never contribute implicit zeroes.
for(const value of ["", " ", null, undefined, 0, 11, NaN, Infinity, "oops"])
{
    ASSERT.equal(calculate([value, 5, 5, 5, 5]), null);
}
ASSERT.equal(calculate([5, 5]), null);

// Rounding must not erase distinctions used by the table's numeric sort.
const LOWER = calculate([5, 5, 5, 5, 5]);
const HIGHER = calculate([5.01, 5.01, 5.01, 5.01, 5.01]);
ASSERT.equal(LOWER.toFixed(1), HIGHER.toFixed(1));
ASSERT.ok(HIGHER > LOWER);
console.log("Review score checks passed.");

function clipboardText(text, name, values, url)
{
    CONTEXT.clipboard_input = {text, name, values, url};
    return VM.runInContext(
        'reviewClipboardText(clipboard_input.text, clipboard_input.name, ' +
        'clipboard_input.values, clipboard_input.url)', CONTEXT);
}

// Sharing preserves the Markdown source and uses the displayed weighted score.
ASSERT.equal(clipboardText('**Full review**\n\n- A Markdown list',
    'Game & Title', [10, 10, 8, 10, 10], 'https://example.test/games/alice'),
    '**Full review**\n\n- A Markdown list\n\nGame & Title\n\n' +
    '- Story: 10/10\n- Gameplay: 10/10\n- Graphics: 8/10\n' +
    '- Audio: 10/10\n- Special: 10/10\n- Overall: 9.7/10\n\n' +
    'See https://example.test/games/alice for details.');
const DRAFT = clipboardText('', 'Draft', [5, '', 5, 5, 5],
    'https://example.test/games/alice');
ASSERT.ok(DRAFT.includes('- Gameplay: —/10'));
ASSERT.ok(DRAFT.includes('- Overall: —/10'));
console.log('Review clipboard formatting checks passed.');
