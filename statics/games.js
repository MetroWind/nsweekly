"use strict";

const GAME_COLLATOR = new Intl.Collator();
let game_sort_column = -1;
let game_sort_direction = 1;

function sortGames(event)
{
    const column = Number(event.currentTarget.dataset.sort);
    game_sort_direction = column === game_sort_column ?
        -game_sort_direction : 1;
    game_sort_column = column;
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
    event.currentTarget.closest("th").setAttribute("aria-sort",
        game_sort_direction === 1 ? "ascending" : "descending");
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
        if([2, 3, 4].includes(game_sort_column))
        {
            order = Number(left.dataset.key) - Number(right.dataset.key);
        }
        else if([5, 6].includes(game_sort_column))
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
    for(const [index, row] of Array.from(tbody.rows).entries())
    {
        row.dataset.originalIndex = String(index);
    }
    for(const button of document.querySelectorAll("#Games [data-sort]"))
    {
        button.addEventListener("click", sortGames);
    }
    const dialog = document.querySelector("#Games dialog");
    if(!dialog)
    {
        return;
    }
    if(typeof dialog.showModal === "function")
    {
        dialog.removeAttribute("open");
        dialog.showModal();
        dialog.addEventListener("cancel", cancelGameDialog);
    }
    const errors = Array.from(dialog.querySelectorAll(".FieldError"));
    const error = errors.find(hasFieldError);
    let focus = error && error.id ?
        dialog.querySelector('[aria-describedby="' + error.id + '"]') :
        dialog.querySelector("input, select, textarea, button");
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

initializeGames();
