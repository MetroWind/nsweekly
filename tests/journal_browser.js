"use strict";

function assertJournal(condition, message)
{
    if(!condition)
    {
        throw new Error(message);
    }
}

async function checkJournal()
{
    const upload = document.getElementById("JournalUpload");
    const file = document.getElementById("JournalFile");
    const text = document.getElementById("JournalText");
    const save = document.getElementById("JournalSave");
    const status = document.getElementById("UploadStatus");
    const results = document.getElementById("UploadResults");
    const record = {id: 42, filename: '日本 [x](y) " \\.png',
        mime_type: "image/png", byte_size: 3,
        public_url: '/attachment/hash/%E6%97%A5%20%5Bx%5D(y)%22.png',
        already_owned: false};
    let reply;
    let request_count = 0;
    window.fetch = async function(url, options)
    {
        assertJournal(new URL(url).pathname === "/fixture-upload", "upload URL");
        assertJournal(options.headers.Accept === "application/json", "Accept");
        assertJournal(options.credentials === "same-origin", "credentials");
        assertJournal([...options.body.keys()].join() === "file", "one file");
        assertJournal(options.body.get("file").size === 3, "original bytes");
        ++request_count;
        return await new Promise(function(resolve, reject)
        {
            reply = {resolve, reject};
        });
    };
    function chooseFile()
    {
        const data = new DataTransfer();
        data.items.add(new File(["abc"], record.filename));
        file.files = data.files;
    }
    function submitUpload()
    {
        upload.dispatchEvent(new Event("submit", {cancelable: true}));
    }
    async function finishUpload(response)
    {
        reply.resolve(response);
        for(let i = 0; save.disabled && i < 100; ++i)
        {
            await new Promise(function(resolve) { setTimeout(resolve, 10); });
        }
    }
    function jsonResponse(value)
    {
        return new Response(JSON.stringify(value),
            {headers: {"Content-Type": "application/json"}});
    }
    submitUpload();
    assertJournal(request_count === 0, "missing file must not upload");
    chooseFile();
    submitUpload();
    assertJournal(save.disabled && !text.disabled, "pending controls");
    submitUpload();
    assertJournal(request_count === 1, "no concurrent upload");
    text.value = "edited during upload";
    await finishUpload(jsonResponse(record));
    assertJournal(!save.disabled && file.value === "", "restored controls: " + status.textContent);
    assertJournal(text.value === "edited during upload", "no automatic insert");
    text.setSelectionRange(7, 13);
    results.querySelector("button").click();
    assertJournal(text.value === "edited \n" + attachmentMarkdown(record) +
        "\n upload", "selection replacement and block boundaries");
    assertJournal(document.activeElement === text, "focus after insert");
    const original = text.value;
    chooseFile();
    submitUpload();
    await finishUpload(new Response("<h1>Proxy error</h1>",
        {status: 502, headers: {"Content-Type": "text/html"}}));
    assertJournal(text.value === original && !save.disabled, "HTML failure");
    assertJournal(results.children.length === 1, "failure keeps results");
    submitUpload();
    reply.reject(new TypeError("Connection lost"));
    await new Promise(function(resolve) { setTimeout(resolve, 0); });
    assertJournal(text.value === original && !save.disabled, "connection loss");
    submitUpload();
    await finishUpload(jsonResponse({...record, already_owned: true}));
    assertJournal(status.textContent.includes("reuse"), "duplicate reuse");
    assertJournal(results.children.length === 2, "retry retains results");
    assertJournal(!attachmentMarkdown({...record, mime_type: "image/svg+xml"})
        .startsWith("!"), "active formats are download links");
    assertJournal(attachmentMarkdown(record).includes("\\[x\\]"), "label escape");
    const details = document.getElementById("GameInformation");
    assertJournal(details.open === matchMedia("(min-width: 56rem)").matches,
        "responsive initial disclosure");
    details.querySelector("summary").click();
    const toggled = details.open;
    await new Promise(function(resolve) { setTimeout(resolve, 0); });
    assertJournal(details.open === toggled, "ordinary disclosure clicks work");
    details.querySelector("summary").click();
    const review = document.getElementById("LongReview");
    for(let i = 0; i < 30; ++i)
    {
        const p = document.createElement("p");
        p.textContent = "Long review paragraph " + i;
        review.append(p);
    }
    const wide = document.getElementById("WideContent");
    const pre = document.createElement("pre");
    pre.textContent = "wide code ".repeat(100);
    const table = document.createElement("table");
    const cell = table.insertRow().insertCell();
    cell.textContent = "wide_table_".repeat(100);
    const link = document.createElement("a");
    link.href = "#journal-entry-1";
    link.textContent = "long_link_".repeat(100);
    wide.append(pre, table, link);
    assertJournal(document.documentElement.scrollWidth <= innerWidth,
        "wide content stays inside page");
    document.getElementById("BrowserResult").textContent = "PASS";
}

checkJournal().catch(function(error)
{
    document.getElementById("BrowserResult").textContent = "FAIL: " + error.message;
});
