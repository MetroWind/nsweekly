"use strict";

// Keep this allowlist identical to AttachmentModule::download().
function attachmentMarkdown(record)
{
    const label = record.filename.replace(/[\x00-\x1f\x7f-\x9f\u2028\u2029]/g, " ")
        .replace(/\\/g, "\\\\").replace(/\[/g, "\\[").replace(/\]/g, "\\]");
    if(/[<>\r\n]/.test(record.public_url))
    {
        throw new Error("Invalid attachment URL");
    }
    const image = ["image/jpeg", "image/png", "image/webp"]
        .includes(record.mime_type);
    return `${image ? "!" : ""}[${label}](<${record.public_url}>)`;
}

function initializeJournal()
{
    const form = document.getElementById("JournalForm");
    if(!form)
    {
        return;
    }
    const textarea = document.getElementById("JournalText");
    const upload = document.getElementById("JournalUpload");
    const file = document.getElementById("JournalFile");
    const save = document.getElementById("JournalSave");
    const status = document.getElementById("UploadStatus");
    const results = document.getElementById("UploadResults");
    const upload_button = upload.querySelector("button");
    const editor = markdownEditor(textarea);
    let uploading = false;
    let saving = false;

    function insertAttachment(record)
    {
        const cm = editor ? editor.codemirror : null;
        const source = cm ? cm.getValue() : textarea.value;
        const start = cm ? cm.indexFromPos(cm.getCursor("from")) :
            textarea.selectionStart;
        const end = cm ? cm.indexFromPos(cm.getCursor("to")) :
            textarea.selectionEnd;
        let snippet = attachmentMarkdown(record);
        if(start > 0 && source[start - 1] !== "\n")
        {
            snippet = "\n" + snippet;
        }
        if(source[end] !== "\n")
        {
            snippet += "\n";
        }
        if(cm)
        {
            if(editor.isPreviewActive())
            {
                EasyMDE.togglePreview(editor);
            }
            cm.replaceSelection(snippet, "end", "+input");
            cm.focus();
        }
        else
        {
            textarea.setRangeText(snippet, start, end, "end");
            textarea.focus();
            textarea.dispatchEvent(new Event("input", {bubbles: true}));
        }
    }

    async function uploadAttachment(event)
    {
        event.preventDefault();
        if(uploading || saving)
        {
            return;
        }
        if(!file.files.length)
        {
            status.textContent = "Choose a file first.";
            return;
        }
        uploading = true;
        upload_button.disabled = true;
        save.disabled = true;
        status.textContent = "Uploading…";
        const data = new FormData();
        data.append("file", file.files[0]);
        try
        {
            const response = await fetch(upload.action, {
                method: "POST", headers: {Accept: "application/json"},
                credentials: "same-origin", body: data
            });
            if(!response.headers.get("Content-Type")?.includes("application/json"))
            {
                throw new Error("Upload failed. Try again.");
            }
            const record = await response.json();
            if(!response.ok)
            {
                throw new Error(record.error?.message || "Upload failed. Try again.");
            }
            if(!["string", "number"].includes(typeof record.id) ||
               typeof record.filename !== "string" ||
               typeof record.public_url !== "string" ||
               typeof record.mime_type !== "string" ||
               typeof record.byte_size !== "number" ||
               typeof record.already_owned !== "boolean")
            {
                throw new Error("Upload failed. Try again.");
            }
            attachmentMarkdown(record);
            const url = new URL(record.public_url, window.location.href);
            if(url.origin !== window.location.origin)
            {
                throw new Error("Invalid attachment URL");
            }
            const row = document.createElement("p");
            const link = document.createElement("a");
            link.textContent = record.filename;
            link.href = record.public_url;
            const insert = document.createElement("button");
            insert.type = "button";
            insert.textContent = "Insert into entry";
            insert.addEventListener("click", function()
            {
                insertAttachment(record);
            });
            row.append(link, " ", insert);
            results.append(row);
            status.textContent = record.already_owned ?
                "Already uploaded; ready to reuse." : "Uploaded.";
            file.value = "";
        }
        catch(error)
        {
            status.textContent = error.message || "Upload failed. Try again.";
        }
        finally
        {
            uploading = false;
            upload_button.disabled = false;
            save.disabled = false;
        }
    }

    function saveEntry(event)
    {
        if(uploading || saving)
        {
            event.preventDefault();
            return;
        }
        if(editor)
        {
            editor.codemirror.save();
        }
        saving = true;
        save.disabled = true;
        upload_button.disabled = true;
    }

    function restoreControls()
    {
        saving = false;
        save.disabled = uploading;
        upload_button.disabled = uploading;
    }
    upload.addEventListener("submit", uploadAttachment);
    form.addEventListener("submit", saveEntry);
    window.addEventListener("pageshow", restoreControls);
    upload.hidden = false;
}

if(typeof document !== "undefined")
{
    initializeJournal();
}
if(typeof module !== "undefined")
{
    module.exports = {attachmentMarkdown};
}
