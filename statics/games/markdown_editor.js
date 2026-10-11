"use strict";

// Keeps editor instances available to integrations such as attachment insertion.
const GAME_MARKDOWN_EDITORS = new WeakMap();

// Returns the enhanced editor, or null when the CDN script is unavailable.
function gameMarkdownEditor(textarea)
{
    return GAME_MARKDOWN_EDITORS.get(textarea) || null;
}

// Enhances source fields while retaining their native form and error messages.
function initializeMarkdownEditor(textarea)
{
    if(typeof EasyMDE !== "function" || gameMarkdownEditor(textarea))
    {
        return;
    }
    const editor = new EasyMDE(
    {
        element: textarea,
        autoDownloadFontAwesome: false,
        forceSync: true,
        spellChecker: false,
        autosave: {enabled: false},
        minHeight: "320px",
        sideBySideFullscreen: false,
        toolbar: ["bold", "italic", "heading", "|", "quote", "code",
            "unordered-list", "ordered-list", "|", "link", "image", "table",
            "|", "preview", "side-by-side", "fullscreen", "|", "undo", "redo"]
    });
    GAME_MARKDOWN_EDITORS.set(textarea, editor);
    const input = editor.codemirror.getInputField();
    input.id = textarea.id + "EditorInput";
    const label = textarea.labels[0];
    if(label)
    {
        label.id ||= textarea.id + "Label";
        input.setAttribute("aria-labelledby", label.id);
        label.htmlFor = input.id;
    }
    const description = textarea.getAttribute("aria-describedby");
    if(description)
    {
        input.setAttribute("aria-describedby", description);
    }

    function syncEditor()
    {
        editor.codemirror.save();
        textarea.dispatchEvent(new Event("input", {bubbles: true}));
    }
    editor.codemirror.on("change", syncEditor);
    textarea.form.addEventListener("submit", syncEditor);
}

for(const textarea of document.querySelectorAll("textarea[data-markdown-editor]"))
{
    initializeMarkdownEditor(textarea);
}
