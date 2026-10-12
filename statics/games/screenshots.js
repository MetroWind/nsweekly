"use strict";

// Enhances preview links without ever assigning an original URL to an image.
function initializeScreenshots()
{
    const dialog = document.getElementById("ScreenshotDialog");
    if(!dialog)
    {
        return;
    }
    const image = document.getElementById("ScreenshotImage");
    const title = document.getElementById("ScreenshotTitle");
    const status = document.getElementById("ScreenshotStatus");
    const original = document.getElementById("ScreenshotOriginal");
    const compressed = document.getElementById("ScreenshotCompressed");
    let trigger;
    function clearPreview()
    {
        image.onload = null;
        image.onerror = null;
        image.removeAttribute("src");
        image.hidden = true;
        image.alt = "";
        original.removeAttribute("href");
        compressed.removeAttribute("href");
        title.textContent = "";
        status.textContent = "";
    }
    function showPreview(event)
    {
        if(event.button !== 0 || event.ctrlKey || event.metaKey ||
           event.shiftKey || event.altKey || !dialog.showModal)
        {
            return;
        }
        event.preventDefault();
        clearPreview();
        trigger = event.currentTarget;
        title.textContent = trigger.dataset.filename;
        image.alt = trigger.dataset.filename;
        original.href = trigger.dataset.original;
        compressed.href = trigger.href;
        status.textContent = "Loading preview…";
        image.onload = function()
        {
            image.hidden = false;
            status.textContent = "";
        };
        image.onerror = function()
        {
            image.hidden = true;
            status.textContent = "Your browser could not display this " +
                "compressed image. Use the compressed file or original link.";
        };
        dialog.showModal();
        image.src = trigger.href;
    }
    function closePreview()
    {
        dialog.close();
    }
    function restoreFocus()
    {
        clearPreview();
        trigger?.focus();
    }
    function thumbnailError(event)
    {
        const notice = event.currentTarget.closest("figure")
            .querySelector(".ScreenshotImageError");
        notice.hidden = false;
    }
    for(const link of document.querySelectorAll(".ScreenshotPreview"))
    {
        link.addEventListener("click", showPreview);
        const thumbnail = link.querySelector("img");
        thumbnail.addEventListener("error", thumbnailError);
        if(thumbnail.complete && thumbnail.naturalWidth === 0)
        {
            thumbnailError({currentTarget: thumbnail});
        }
    }
    document.getElementById("ScreenshotClose")
        .addEventListener("click", closePreview);
    dialog.addEventListener("close", restoreFocus);
}

// Sends bounded per-file requests sequentially and keeps failed files for retry.
function initializeScreenshotUpload()
{
    const form = document.getElementById("ScreenshotUpload");
    if(!form || !window.fetch || !window.FormData)
    {
        return;
    }
    const input = form.querySelector("input[type=file]");
    const submit = form.querySelector("button[type=submit]");
    const retry = document.getElementById("ScreenshotRetry");
    const results = document.getElementById("ScreenshotResults");
    const status = document.getElementById("ScreenshotUploadStatus");
    const progress = document.getElementById("ScreenshotProgress");
    let active = false;
    let failed = [];
    async function uploadFiles(files)
    {
        if(active || files.length === 0)
        {
            return;
        }
        active = true;
        failed = [];
        input.disabled = true;
        submit.disabled = true;
        retry.hidden = true;
        progress.hidden = false;
        progress.max = files.length;
        progress.value = 0;
        for(const file of files)
        {
            const row = document.createElement("li");
            row.textContent = file.name + ": uploading…";
            results.append(row);
            const body = new FormData();
            body.append("file", file, file.name);
            try
            {
                const response = await fetch(form.action,
                    {method: "POST", body,
                     headers: {Accept: "application/json"},
                     credentials: "same-origin"});
                const result = await response.json();
                if(!response.ok)
                {
                    throw new Error(result.message || "Upload failed");
                }
                row.textContent = file.name + (result.already_uploaded ?
                    ": already uploaded" : ": uploaded");
            }
            catch(error)
            {
                failed.push(file);
                row.textContent = file.name + ": " + error.message;
            }
            progress.value += 1;
            status.textContent = progress.value + " of " + files.length +
                " files completed. Refresh the gallery to view uploads.";
        }
        active = false;
        input.disabled = false;
        submit.disabled = false;
        retry.hidden = failed.length === 0;
    }
    function submitFiles(event)
    {
        event.preventDefault();
        uploadFiles(Array.from(input.files));
    }
    function retryFiles()
    {
        uploadFiles(failed.slice());
    }
    form.addEventListener("submit", submitFiles);
    retry.addEventListener("click", retryFiles);
}

initializeScreenshots();
initializeScreenshotUpload();
