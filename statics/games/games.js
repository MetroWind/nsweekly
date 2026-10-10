"use strict";

const GAME_COLLATOR = new Intl.Collator();
const GAME_REVIEWS = document.getElementById("Games").dataset.view === "reviews";
const GAME_SORT_COOKIE = GAME_REVIEWS ?
    "nsweekly-reviews-sort" : "nsweekly-games-sort";
let game_sort_column = -1;
let game_sort_direction = 1;

function sortGames(event)
{
    const column = Number(event.currentTarget.dataset.sort);
    game_sort_direction = column === game_sort_column ?
        -game_sort_direction : 1;
    game_sort_column = column;
    applyGameSort();
    document.cookie = GAME_SORT_COOKIE + "=" + column + ":" +
        (game_sort_direction === 1 ? "asc" : "desc") +
        "; Path=/; Max-Age=31536000; SameSite=Lax" +
        (location.protocol === "https:" ? "; Secure" : "");
}

function applyGameSort()
{
    const tbody = document.querySelector("#Games tbody");
    const rows = Array.from(tbody.rows);
    rows.sort(compareGames);
    for(const row of rows)
    {
        tbody.append(row);
    }
    for(const header of document.querySelectorAll("#Games th[aria-sort]"))
    {
        header.setAttribute("aria-sort", "none");
    }
    document.querySelector('#Games [data-sort="' + game_sort_column + '"]')
        .closest("th").setAttribute("aria-sort",
        game_sort_direction === 1 ? "ascending" : "descending");
}

function restoreGameSort()
{
    game_sort_column = GAME_REVIEWS ? 6 : 2;
    game_sort_direction = GAME_REVIEWS ? -1 : 1;
    for(const cookie of document.cookie.split(";"))
    {
        const value = cookie.trim();
        if(!value.startsWith(GAME_SORT_COOKIE + "="))
        {
            continue;
        }
        const match = /^(\d{1,2}):(asc|desc)$/.exec(
            value.slice(GAME_SORT_COOKIE.length + 1));
        if(!match || !document.querySelector(
            '#Games [data-sort="' + match[1] + '"]'))
        {
            break;
        }
        game_sort_column = Number(match[1]);
        game_sort_direction = match[2] === "asc" ? 1 : -1;
        break;
    }
    applyGameSort();
}

function compareGames(a, b)
{
    const left = a.cells[game_sort_column];
    const right = b.cells[game_sort_column];
    const left_missing = left.dataset.missing === "true";
    const right_missing = right.dataset.missing === "true";
    if(left_missing !== right_missing)
    {
        return left_missing ? 1 : -1;
    }
    let order = 0;
    if(!left_missing)
    {
        if(GAME_REVIEWS ? game_sort_column >= 1 && game_sort_column <= 7 :
            [2, 3, 4].includes(game_sort_column))
        {
            order = Number(left.dataset.key) - Number(right.dataset.key);
        }
        else if((GAME_REVIEWS ? [8, 9] : [5, 6]).includes(game_sort_column))
        {
            order = left.dataset.key < right.dataset.key ? -1 :
                left.dataset.key > right.dataset.key ? 1 : 0;
        }
        else
        {
            order = GAME_COLLATOR.compare(left.dataset.key, right.dataset.key);
        }
    }
    return order * game_sort_direction ||
        Number(a.dataset.originalIndex) - Number(b.dataset.originalIndex);
}

function updateEndMinimum()
{
    const start = document.getElementById("GameStart");
    const end = document.getElementById("GameEnd");
    if(start.value)
    {
        end.min = start.value;
    }
    else
    {
        end.removeAttribute("min");
    }
}

function cancelGameDialog(event)
{
    event.preventDefault();
    window.location.assign(event.currentTarget.dataset.cancelUrl);
}

function initializeGames()
{
    const tbody = document.querySelector("#Games tbody");
    if(tbody)
    {
        for(const [index, row] of Array.from(tbody.rows).entries())
        {
            row.dataset.originalIndex = String(index);
        }
        for(const button of document.querySelectorAll("#Games [data-sort]"))
        {
            button.addEventListener("click", sortGames);
        }
        restoreGameSort();
    }
    const dialog = document.querySelector("#Games dialog");
    const editor = dialog || document.getElementById("ReviewForm");
    if(!editor)
    {
        return;
    }
    if(dialog && typeof dialog.showModal === "function")
    {
        dialog.removeAttribute("open");
        dialog.showModal();
        dialog.addEventListener("cancel", cancelGameDialog);
    }
    const errors = Array.from(editor.querySelectorAll(".FieldError"));
    const error = errors.find(hasFieldError);
    let focus = error && error.id ?
        editor.querySelector('[aria-describedby="' + error.id + '"]') :
        editor.querySelector("input, select, textarea, button");
    if(focus && focus.tagName === "FIELDSET")
    {
        focus = focus.querySelector("input");
    }
    if(focus)
    {
        if(error)
        {
            focus.setAttribute("aria-invalid", "true");
        }
        focus.focus();
    }
    const start = document.getElementById("GameStart");
    if(start)
    {
        start.addEventListener("change", updateEndMinimum);
        updateEndMinimum();
    }
}

function hasFieldError(element)
{
    return element.textContent.trim().length > 0;
}

// Keeps whole graphemes within the UTF-8 budget, including an oversized first.
function textExcerpt(text)
{
    const encoder = new TextEncoder();
    const segmenter = new Intl.Segmenter(undefined,
        {granularity: "grapheme"});
    let preview = "";
    let bytes = 0;
    for(const {segment} of segmenter.segment(text))
    {
        const size = encoder.encode(segment).length;
        if(bytes + size > 40 && preview)
        {
            break;
        }
        preview += segment;
        bytes += size;
    }
    return preview;
}

// Opens the original rendered content without reparsing HTML or Markdown.
function showFullText(event)
{
    const button = event.currentTarget;
    const dialog = document.createElement("dialog");
    const heading = document.createElement("h2");
    heading.id = "FullTextTitle";
    heading.textContent = button.dataset.title;
    dialog.setAttribute("aria-labelledby", heading.id);
    const close = document.createElement("button");
    close.type = "button";
    close.textContent = "Close";
    close.autofocus = true;
    close.addEventListener("click", closeFullText);
    dialog.addEventListener("close", removeFullText);
    dialog.append(heading, button.full_text, close);
    document.getElementById("Games").append(dialog);
    dialog.showModal();
}

function closeFullText(event)
{
    event.currentTarget.closest("dialog").close();
}

function removeFullText(event)
{
    event.currentTarget.remove();
}

// Leaves short entries and unsupported browsers with full rendered content.
function initializeTextExcerpts()
{
    if(typeof Intl.Segmenter !== "function" ||
       typeof HTMLDialogElement.prototype.showModal !== "function")
    {
        return;
    }
    for(const element of document.querySelectorAll("#Games .TextExcerpt"))
    {
        const text = element.innerText.replace(/\s+/gu, " ").trim();
        const preview = textExcerpt(text);
        if(preview === text)
        {
            continue;
        }
        const full_text = document.createElement("div");
        full_text.className = "FullText";
        while(element.firstChild)
        {
            full_text.append(element.firstChild);
        }
        const button = document.createElement("button");
        button.type = "button";
        button.textContent = "...";
        button.dataset.title = element.dataset.textTitle;
        button.setAttribute("aria-label", "Show full " +
            element.dataset.textTitle.toLowerCase());
        button.full_text = full_text;
        button.addEventListener("click", showFullText);
        element.append(document.createTextNode(preview + " "), button);
    }
}

initializeGames();
initializeTextExcerpts();
