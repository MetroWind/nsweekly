"use strict";

function assertBrowser(condition, message)
{
    if(!condition)
    {
        throw new Error(message);
    }
}

function checkBrowserGames()
{
    const tbody = document.querySelector("#Games tbody");
    const buttons = document.querySelectorAll("#Games [data-sort]");
    const original = Array.from(tbody.rows).sort(originalGameOrder);
    game_sort_column = -1;
    game_sort_direction = 1;
    for(const row of original)
    {
        tbody.append(row);
    }
    for(const button of buttons)
    {
        const column = Number(button.dataset.sort);
        button.click();
        let sorted = Array.from(tbody.rows);
        assertBrowser(button.closest("th").getAttribute("aria-sort") ===
            "ascending", "ascending aria-sort");
        assertBrowser(sorted.at(-1).cells[column].dataset.missing === "true",
            "ascending missing-last, column " + column);
        button.click();
        sorted = Array.from(tbody.rows);
        assertBrowser(button.closest("th").getAttribute("aria-sort") ===
            "descending", "descending aria-sort");
        assertBrowser(sorted.at(-1).cells[column].dataset.missing === "true",
            "descending missing-last, column " + column);
        assertBrowser(sorted.indexOf(original[1]) < sorted.indexOf(original[2]),
            "equal keys retain original order, column " + column);
    }
    buttons[4].click();
    assertBrowser(tbody.rows[0].cells[4].dataset.key === "0",
        "zero hours sorts before maximum hours");
    assertBrowser(tbody.rows[3].cells[4].dataset.key === "2147483647",
        "maximum integer hours sorts numerically");
    game_sort_column = -1;
    restoreGameSort();
    assertBrowser(game_sort_column === 4 && game_sort_direction === 1,
        "ascending sort restores from cookie");
    buttons[4].click();
    game_sort_column = -1;
    game_sort_direction = 1;
    restoreGameSort();
    assertBrowser(game_sort_column === 4 && game_sort_direction === -1,
        "descending sort restores from cookie");
    document.cookie = GAME_SORT_COOKIE + "=invalid; Path=/; SameSite=Lax";
    game_sort_column = -1;
    restoreGameSort();
    assertBrowser(game_sort_column === -1, "malformed cookie is ignored");
    assertBrowser(document.querySelector("#Games td code").textContent ===
        "MacroDown", "notes exist before sorting");
    const start = document.getElementById("GameStart");
    const end = document.getElementById("GameEnd");
    start.value = "2024-02-29";
    start.dispatchEvent(new Event("change"));
    assertBrowser(end.min === "2024-02-29", "end-date minimum follows start");
    start.value = "";
    start.dispatchEvent(new Event("change"));
    assertBrowser(!end.hasAttribute("min"), "empty start removes minimum");
    const dialog = document.querySelector("dialog");
    assertBrowser(dialog.open, "dialog is visible");
    dialog.close();
    document.getElementById("BrowserResult").textContent =
        "Passed: all eight columns, both directions, missing-last, stable " +
        "ties, integer hours, cookie restoration, server notes, dialog, " +
        "and date minimum. Use Tab and Enter on sort buttons.";
}

function originalGameOrder(a, b)
{
    return Number(a.dataset.originalIndex) - Number(b.dataset.originalIndex);
}

let previous_game_sort_cookie = "";
for(const cookie of document.cookie.split(";"))
{
    if(cookie.trim().startsWith(GAME_SORT_COOKIE + "="))
    {
        previous_game_sort_cookie = cookie.trim();
    }
}

try
{
    checkBrowserGames();
}
catch(error)
{
    document.getElementById("BrowserResult").textContent = "FAILED: " + error;
    throw error;
}
finally
{
    document.cookie = (previous_game_sort_cookie || GAME_SORT_COOKIE + "=") +
        "; Path=/; SameSite=Lax; Max-Age=" +
        (previous_game_sort_cookie ? "31536000" : "0");
}
