"use strict";

function assertEditor(condition, message)
{
    if(!condition)
    {
        throw new Error(message);
    }
}

function preventEditorSubmit(event)
{
    event.preventDefault();
}

function checkEditor(textarea)
{
    const editor = gameMarkdownEditor(textarea);
    const original = textarea.value;
    if(!editor)
    {
        assertEditor(typeof EasyMDE === "undefined", "CDN fallback");
        assertEditor(getComputedStyle(textarea).display !== "none",
            "native textarea stays visible");
        textarea.value = "Native source 日本語";
    }
    else
    {
        assertEditor(editor.value() === original, "original source loads");
        initializeMarkdownEditor(textarea);
        assertEditor(gameMarkdownEditor(textarea) === editor,
            "initialization keeps one editor per field");
        const cm = editor.codemirror;
        assertEditor(cm.getInputField().getAttribute("aria-describedby") ===
            textarea.getAttribute("aria-describedby"), "error association");
        const label = document.getElementById(
            cm.getInputField().getAttribute("aria-labelledby"));
        assertEditor(label.htmlFor === cm.getInputField().id, "visible label");
        editor.value("selected source");
        cm.setSelection({line: 0, ch: 0}, {line: 0, ch: 8});
        editor.toolbarElements.bold.click();
        assertEditor(textarea.value === "**selected** source", "formatting sync");
        cm.undo();
        assertEditor(textarea.value === "selected source", "undo sync");
        editor.value("");
        assertEditor(textarea.value === "", "optional source may be empty");
        editor.value("Saved source 日本語\n%code{hello}");
        textarea.value = "stale hidden field";
    }
    textarea.form.addEventListener("submit", preventEditorSubmit);
    textarea.form.dispatchEvent(new Event("submit", {cancelable: true}));
    const expected = editor ? editor.value() : textarea.value;
    const fields = new FormData(textarea.form);
    assertEditor(fields.get(textarea.name) === expected, "native submit sync");
    assertEditor(fields.getAll(textarea.name).length === 1,
        "only one source field is submitted");
}

function checkMarkdownEditors()
{
    const notes = document.getElementById("GameNotes");
    const note_editor = gameMarkdownEditor(notes);
    if(note_editor)
    {
        assertEditor(note_editor.codemirror.hasFocus(),
            "validation focuses visible notes editor");
        assertEditor(note_editor.codemirror.getInputField()
            .getAttribute("aria-invalid") === "true", "visible error state");
    }
    checkEditor(notes);
    if(note_editor)
    {
        note_editor.toolbarElements.fullscreen.click();
        assertEditor(document.querySelector("dialog").getBoundingClientRect()
            .width >= innerWidth - 1, "dialog expands for fullscreen");
        note_editor.toolbarElements.fullscreen.click();
    }
    document.querySelector("dialog").close();
    checkEditor(document.getElementById("ReviewText"));
    const gameplay = document.getElementById("Gameplay");
    gameplay.value = "6";
    gameplay.dispatchEvent(new Event("input", {bubbles: true}));
    assertEditor(document.getElementById("ReviewScore").textContent ===
        "5.3 / 10", "review scoring remains functional");
    assertEditor(document.documentElement.scrollWidth <= innerWidth,
        "editors stay inside narrow layout");
    document.getElementById("BrowserResult").textContent = "PASS";
}

try
{
    checkMarkdownEditors();
}
catch(error)
{
    document.getElementById("BrowserResult").textContent = "FAIL: " + error.message;
}
