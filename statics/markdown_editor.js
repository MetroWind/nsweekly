"use strict";

// Keeps editor instances available to integrations such as attachment insertion.
const MARKDOWN_EDITORS = new WeakMap();

// Returns the enhanced editor, or null when the CDN script is unavailable.
function markdownEditor(textarea)
{
    return MARKDOWN_EDITORS.get(textarea) || null;
}

// Enhances source fields while retaining their native form and error messages.
function initializeMarkdownEditor(textarea)
{
    if(typeof EasyMDE !== "function" || markdownEditor(textarea))
    {
        return;
    }
    const toolbar = ["bold", "italic", "heading", "|", "quote", "code",
        "unordered-list", "ordered-list", "|", "link", "image", "table"];
    if(!textarea.hasAttribute("data-external-preview"))
    {
        toolbar.push("|", "preview", "side-by-side");
    }
    toolbar.push("|", "undo", "redo");
    const editor = new EasyMDE(
    {
        element: textarea,
        autoDownloadFontAwesome: false,
        forceSync: true,
        spellChecker: false,
        autosave: {enabled: false},
        minHeight: "320px",
        sideBySideFullscreen: false,
        shortcuts: {toggleFullScreen: null},
        toolbar
    });
    MARKDOWN_EDITORS.set(textarea, editor);
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
