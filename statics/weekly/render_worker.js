import createMacroDown from "../macrodown/macrodown.mjs";

let renderer;
try
{
    renderer = await createMacroDown();
    self.postMessage({type: "ready"});
}
catch(error)
{
    self.postMessage({type: "error", message: String(error)});
}

self.onmessage = function(event)
{
    const {id, source} = event.data;
    try
    {
        const html = renderer.renderMarkdown(source);
        self.postMessage({type: "result", id, html});
    }
    catch(error)
    {
        self.postMessage({type: "error", id, message: String(error)});
    }
};
