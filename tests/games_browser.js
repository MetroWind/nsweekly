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
    const original = Array.from(tbody.rows);
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
        "ties, integer hours, server notes, dialog, and date minimum. " +
        "Use Tab and Enter on sort buttons; reload to restore default order.";
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
