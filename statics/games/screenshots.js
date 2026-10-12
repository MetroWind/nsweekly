"use strict";

// Justifies complete rows using recorded dimensions, before images load.
function initializeScreenshotGallery()
{
    const gallery = document.querySelector(".ScreenshotGallery");
    if(!gallery)
    {
        return;
    }
    const items = Array.from(gallery.querySelectorAll("figure")).map(figure =>
    {
        const image = figure.querySelector("img");
        const width = Number(image.getAttribute("width"));
        const height = Number(image.getAttribute("height"));
        return {figure, ratio: width > 0 && height > 0 ? width / height : 1};
    });
    let previous_width = 0;
    function layoutRows()
    {
        // clientWidth rounds up some fractional widths and can force a wrap.
        const width = gallery.getBoundingClientRect().width;
        if(width <= 0 || width === previous_width)
        {
            return;
        }
        previous_width = width;
        // This is the rough height if a thumbnail row.
        const target_height = 130;
        const gap = 1;
        const fragment = document.createDocumentFragment();
        const focused = gallery.contains(document.activeElement) ?
            document.activeElement : null;
        let row = [];
        let ratio_sum = 0;
        function finishRow(complete)
        {
            const fitted_height = (width - gap * (row.length - 1)) / ratio_sum;
            const height = complete ? fitted_height :
                Math.min(target_height, fitted_height);
            const container = document.createElement("div");
            container.className = "ScreenshotRow";
            container.style.height = height + "px";
            container.style.width = complete ? "100%" :
                height * ratio_sum + gap * (row.length - 1) + "px";
            for(const item of row)
            {
                // Let flex distribute subpixels within an explicit row.
                // The browser must not choose different row breaks.
                item.figure.style.removeProperty("width");
                item.figure.style.removeProperty("height");
                item.figure.style.flexGrow = item.ratio;
                container.append(item.figure);
            }
            fragment.append(container);
            row = [];
            ratio_sum = 0;
        }
        for(const item of items)
        {
            row.push(item);
            ratio_sum += item.ratio;
            if(ratio_sum * target_height + gap * (row.length - 1) >= width)
            {
                finishRow(true);
            }
        }
        if(row.length)
        {
            finishRow(false);
        }
        gallery.replaceChildren(fragment);
        gallery.setAttribute("data-justified", "");
        focused?.focus({preventScroll: true});
    }
    layoutRows();
    if(window.ResizeObserver)
    {
        new ResizeObserver(layoutRows).observe(gallery);
    }
    else
    {
        window.addEventListener("resize", layoutRows);
    }
}

// Copies shareable fragment URLs without opening the image or dialog.
function initializeScreenshotCopy()
{
    const status = document.getElementById("ScreenshotCopyStatus");
    const feedback_timers = new WeakMap();
    function clearCopyFeedback(button)
    {
        button.removeAttribute("data-feedback");
        button.title = "Copy preview URL";
        feedback_timers.delete(button);
    }
    // Retain clipboard support on HTTP installations without Clipboard API.
    function copyWithSelection(text)
    {
        const focused = document.activeElement;
        const field = document.createElement("textarea");
        field.value = text;
        field.readOnly = true;
        field.style.position = "fixed";
        field.style.opacity = "0";
        document.body.append(field);
        try
        {
            field.select();
            return document.execCommand("copy");
        }
        finally
        {
            field.remove();
            focused?.focus({preventScroll: true});
        }
    }
    async function copyPreviewURL(event)
    {
        const button = event.currentTarget;
        const link = button.closest("figure")
            .querySelector(".ScreenshotPreview");
        status.textContent = "";
        let copied = false;
        try
        {
            if(navigator.clipboard?.writeText)
            {
                await navigator.clipboard.writeText(link.href);
                copied = true;
            }
        }
        catch(error)
        {
            // Try the selection fallback when clipboard access is unavailable.
        }
        if(!copied)
        {
            try
            {
                copied = copyWithSelection(link.href);
            }
            catch(error)
            {
                copied = false;
            }
        }
        const message = copied ? "Preview URL copied" :
            "Could not copy URL. Copy the thumbnail link instead";
        button.title = message;
        button.dataset.feedback = copied ? "Copied!" : "Copy failed";
        clearTimeout(feedback_timers.get(button));
        feedback_timers.set(button,
            setTimeout(clearCopyFeedback, 2000, button));
        status.textContent = message + ": " + link.dataset.filename;
    }
    for(const button of document.querySelectorAll(".ScreenshotCopy"))
    {
        button.hidden = false;
        button.addEventListener("click", copyPreviewURL);
    }
}

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
    const deletion = document.getElementById("ScreenshotDelete");
    const links = Array.from(document.querySelectorAll(".ScreenshotPreview"));
    const previous = document.getElementById("ScreenshotPrevious");
    const next = document.getElementById("ScreenshotNext");
    const close_buttons = [document.getElementById("ScreenshotClose"),
        document.getElementById("ScreenshotCloseLeft")];
    const controls = [...close_buttons, previous, next, deletion]
        .filter(Boolean);
    // Reveal each overlay within a 48px margin of its own hit target.
    function revealControls(event)
    {
        if(event.pointerType === "touch")
        {
            return;
        }
        for(const control of controls)
        {
            const bounds = control.getBoundingClientRect();
            const nearby = event.clientX >= bounds.left - 48 &&
                event.clientX <= bounds.right + 48 &&
                event.clientY >= bounds.top - 48 &&
                event.clientY <= bounds.bottom + 48;
            control.toggleAttribute("data-near", nearby);
        }
    }
    function hideControls()
    {
        for(const control of controls)
        {
            control.removeAttribute("data-near");
        }
    }
    let trigger;
    let current_index = -1;
    // Fit the image itself to the viewport without upscaling or extra space.
    function fitPreview()
    {
        if(!dialog.open || image.hidden || !image.naturalWidth)
        {
            return;
        }
        const max_width = document.documentElement.clientWidth * .96;
        const max_height = parseFloat(getComputedStyle(dialog).maxHeight);
        const scale = Math.min(1, max_width / image.naturalWidth,
            max_height / image.naturalHeight);
        const width = image.naturalWidth * scale;
        image.style.width = width + "px";
        image.style.height = image.naturalHeight * scale + "px";
        dialog.style.width = width + "px";
    }
    function clearPreview()
    {
        image.onload = null;
        image.onerror = null;
        image.removeAttribute("src");
        image.hidden = true;
        image.alt = "";
        image.style.removeProperty("width");
        image.style.removeProperty("height");
        dialog.style.removeProperty("width");
        original.removeAttribute("href");
        if(deletion)
        {
            deletion.removeAttribute("href");
            deletion.hidden = true;
        }
        title.textContent = "";
        status.textContent = "";
    }
    function previewFragment(index)
    {
        return "#screenshot-" + links[index].dataset.screenshotId;
    }
    function syncPreviewURL()
    {
        const index = links.findIndex(link =>
            "#screenshot-" + link.dataset.screenshotId === location.hash);
        if(index >= 0 && dialog.showModal)
        {
            if(!dialog.open || index !== current_index)
            {
                loadPreview(index, false);
            }
        }
        else if(dialog.open)
        {
            dialog.close();
        }
    }
    function loadPreview(index, update_url = true)
    {
        if(index < 0 || index >= links.length)
        {
            return;
        }
        if(update_url && location.hash !== previewFragment(index))
        {
            history.pushState(null, "", previewFragment(index));
        }
        clearPreview();
        current_index = index;
        trigger = links[index];
        previous.disabled = index === 0;
        next.disabled = index === links.length - 1;
        title.textContent = trigger.dataset.filename;
        image.alt = trigger.dataset.filename;
        original.href = trigger.dataset.original;
        if(deletion && trigger.dataset.delete)
        {
            deletion.href = trigger.dataset.delete;
            deletion.hidden = false;
        }
        status.textContent = "Loading preview…";
        image.onload = function()
        {
            image.hidden = false;
            status.textContent = "";
            fitPreview();
        };
        image.onerror = function()
        {
            image.hidden = true;
            dialog.style.removeProperty("width");
            status.textContent = "Your browser could not display this " +
                "preview. Open the original image.";
        };
        if(!dialog.open)
        {
            dialog.showModal();
            dialog.focus();
        }
        image.src = trigger.dataset.preview;
    }
    function showPreview(event)
    {
        if(event.button !== 0 || event.ctrlKey || event.metaKey ||
           event.shiftKey || event.altKey || !dialog.showModal)
        {
            return;
        }
        event.preventDefault();
        loadPreview(links.indexOf(event.currentTarget));
    }
    function previousPreview()
    {
        loadPreview(current_index - 1);
    }
    function nextPreview()
    {
        loadPreview(current_index + 1);
    }
    function navigatePreview(event)
    {
        if(event.altKey || event.ctrlKey || event.metaKey || event.shiftKey)
        {
            return;
        }
        if(event.key === "ArrowLeft")
        {
            event.preventDefault();
            previousPreview();
        }
        else if(event.key === "ArrowRight")
        {
            event.preventDefault();
            nextPreview();
        }
    }
    function closePreview()
    {
        dialog.close();
    }
    function closeOnBackdrop(event)
    {
        const bounds = dialog.getBoundingClientRect();
        if(event.target === dialog &&
           (event.clientX < bounds.left || event.clientX > bounds.right ||
            event.clientY < bounds.top || event.clientY > bounds.bottom))
        {
            closePreview();
        }
    }
    function restoreFocus()
    {
        if(dialog.open)
        {
            return;
        }
        if(current_index >= 0 && location.hash === previewFragment(current_index))
        {
            history.replaceState(null, "", location.pathname + location.search);
        }
        clearPreview();
        hideControls();
        trigger?.focus();
    }
    for(const link of links)
    {
        link.addEventListener("click", showPreview);
    }
    for(const button of close_buttons)
    {
        button.addEventListener("click", closePreview);
    }
    previous.addEventListener("click", previousPreview);
    next.addEventListener("click", nextPreview);
    dialog.addEventListener("click", closeOnBackdrop);
    dialog.addEventListener("pointermove", revealControls);
    dialog.addEventListener("pointerleave", hideControls);
    dialog.addEventListener("keydown", navigatePreview);
    dialog.addEventListener("close", restoreFocus);
    window.addEventListener("resize", fitPreview);
    window.addEventListener("popstate", syncPreviewURL);
    window.addEventListener("hashchange", syncPreviewURL);
    syncPreviewURL();
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

initializeScreenshotGallery();
initializeScreenshotCopy();
initializeScreenshots();
initializeScreenshotUpload();
