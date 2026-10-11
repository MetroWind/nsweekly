const editor = document.querySelector("#EditView textarea");
const preview = document.querySelector("#Preview .Weekly");
const status = document.querySelector("#PreviewStatus");
let worker;
let ready = false;
let busy = false;
let revision = 0;
let timer;

function render()
{
    if(!ready || busy)
    {
        return;
    }
    busy = true;
    status.textContent = "Rendering…";
    worker.postMessage({id: revision, source: editor.value});
}

function scheduleRender()
{
    revision += 1;
    clearTimeout(timer);
    timer = setTimeout(render, 150);
}

function receiveMessage(event)
{
    const result = event.data;
    if(result.type === "ready")
    {
        ready = true;
        render();
        return;
    }
    busy = false;
    if(result.id !== undefined && result.id !== revision)
    {
        render();
        return;
    }
    if(result.type === "error")
    {
        status.textContent = `Preview failed: ${result.message}`;
        return;
    }
    preview.innerHTML = result.html;
    status.textContent = "";
}

function workerFailed(event)
{
    ready = false;
    busy = false;
    status.textContent = `Preview unavailable: ${event.message}`;
    if(worker)
    {
        worker.terminate();
    }
}

editor.addEventListener("input", scheduleRender);
try
{
    worker = new Worker(new URL("./render_worker.js", import.meta.url),
        {type: "module"});
    worker.addEventListener("message", receiveMessage);
    worker.addEventListener("error", workerFailed);
}
catch(error)
{
    workerFailed(error);
}
