"use strict";

function assertWeekly(condition, message)
{
    if(!condition)
    {
        throw new Error(message);
    }
}

async function waitForPreview(html)
{
    for(let attempt = 0; attempt < 100; attempt += 1)
    {
        if(document.querySelector("#Preview .Weekly").innerHTML.includes(html))
        {
            return;
        }
        await new Promise(resolve => setTimeout(resolve, 50));
    }
    throw new Error("Preview did not render: " + html);
}

async function checkWeeklyEditor()
{
    const textarea = document.getElementById("WeeklyContent");
    const editor = markdownEditor(textarea);
    const fallback = new URLSearchParams(location.search).has("fallback");
    if(fallback)
    {
        assertWeekly(!editor, "native fallback");
        assertWeekly(getComputedStyle(textarea).display !== "none",
            "native textarea stays visible");
    }
    else
    {
        assertWeekly(editor, "EasyMDE loads");
        assertWeekly(!editor.toolbarElements.preview,
            "weekly uses the MacroDown preview");
        const input = editor.codemirror.getInputField();
        const label = document.getElementById(
            input.getAttribute("aria-labelledby"));
        assertWeekly(label.htmlFor === input.id, "editor label association");
        editor.value("selected source");
        editor.codemirror.setSelection({line: 0, ch: 0}, {line: 0, ch: 8});
        editor.toolbarElements.bold.click();
        assertWeekly(textarea.value === "**selected** source", "formatting sync");
        await waitForPreview("<strong>selected</strong>");
        editor.codemirror.undo();
        assertWeekly(textarea.value === "selected source", "undo sync");
        assertWeekly(!editor.toolbarElements.fullscreen,
            "fullscreen button is disabled");
        assertWeekly(!editor.codemirror.getOption("extraKeys").F11,
            "fullscreen shortcut is disabled");
    }
    const source = "%def[greet]{name}{<mark>Hello, %name</mark>}\n\n" +
        "%greet{中文 🌍}";
    if(editor)
    {
        editor.value(source);
    }
    else
    {
        textarea.value = source;
        textarea.dispatchEvent(new Event("input", {bubbles: true}));
    }
    await waitForPreview("<mark>Hello, 中文 🌍</mark>");
    if(editor)
    {
        textarea.value = "stale hidden field";
    }
    textarea.form.addEventListener("submit", event => event.preventDefault());
    textarea.form.dispatchEvent(new Event("submit", {cancelable: true}));
    assertWeekly(new FormData(textarea.form).get("content") === source,
        "form submits current source");
    assertWeekly(document.documentElement.scrollWidth <= innerWidth,
        "editor fits narrow layout");
}

checkWeeklyEditor().then(function()
{
    document.getElementById("BrowserResult").textContent = "PASS";
}).catch(function(error)
{
    document.getElementById("BrowserResult").textContent = "FAIL: " + error.message;
});
