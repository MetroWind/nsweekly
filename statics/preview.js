function render(from, to, reader, writer)
{
    var tree = reader.parse(from.value);
    var result = writer.render(tree);
    to.innerHTML = result;
}

let editor = document.querySelector("#EditView textarea");
let preview = document.querySelector("#Preview .Weekly");

var reader = new commonmark.Parser();
var writer = new commonmark.HtmlRenderer();

render(editor, preview, reader, writer);

var old_text = editor.value;
window.setInterval(function() {
    if(editor.value != old_text)
    {
        old_text = editor.value;
        render(editor, preview, reader, writer);
    }
}, 1000);
