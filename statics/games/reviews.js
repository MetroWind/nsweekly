"use strict";

// Returns the spreadsheet's weighted mean, or null for incomplete inputs.
function reviewScore(values)
{
    if(values.length !== 5 || values.some(isMissingReviewScore))
    {
        return null;
    }
    const scores = values.map(Number);
    if(scores.some(isInvalidReviewScore))
    {
        return null;
    }
    return (scores[0] + scores[1] * 2 + scores[2] + scores[3] + scores[4]) / 6;
}

function isMissingReviewScore(value)
{
    return value === null || value === undefined || String(value).trim() === "";
}

function isInvalidReviewScore(value)
{
    return !Number.isFinite(value) || value < 1 || value > 10;
}

function updateReviewScore()
{
    const inputs = document.querySelectorAll("[data-review-score]");
    const score = reviewScore(Array.from(inputs, reviewInputValue));
    document.getElementById("ReviewScore").textContent =
        score === null ? "Incomplete" : score.toFixed(1) + " / 10";
}

function reviewInputValue(input)
{
    return input.value;
}

function initializeReviews()
{
    for(const row of document.querySelectorAll("#Games tbody tr"))
    {
        const values = Array.from(row.querySelectorAll(".ReviewDimension"),
            reviewCellValue);
        const score = reviewScore(values);
        const cell = row.querySelector(".ReviewOverall");
        cell.textContent = score === null ? "Incomplete" : score.toFixed(1);
        cell.dataset.key = score === null ? "" : String(score);
        cell.dataset.missing = String(score === null);
    }
    if(document.getElementById("ReviewScore"))
    {
        for(const input of document.querySelectorAll("[data-review-score]"))
        {
            input.addEventListener("input", updateReviewScore);
        }
        updateReviewScore();
    }
}

function reviewCellValue(cell)
{
    return cell.dataset.key;
}

// Formats original Markdown and scores for sharing outside the tracker.
function reviewClipboardText(text, name, values, url)
{
    const labels = ["Story", "Gameplay", "Graphics", "Audio", "Special"];
    const lines = labels.map((label, index) =>
        "- " + label + ": " + (isMissingReviewScore(values[index]) ?
            "—" : values[index]) + "/10");
    const overall = reviewScore(values);
    lines.push("- Overall: " +
        (overall === null ? "—" : overall.toFixed(1)) + "/10");
    return text + "\n\n" + name + "\n\n" + lines.join("\n") +
        "\n\nSee " + url + " for details.";
}

// Enables review copying with visible and screen-reader feedback.
function initializeReviewCopy()
{
    const feedback_timers = new WeakMap();
    function clearFeedback(button)
    {
        button.removeAttribute("data-feedback");
        button.title = "Copy review";
        feedback_timers.delete(button);
    }
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
    async function copyReview(event)
    {
        const button = event.currentTarget;
        const row = button.closest("tr");
        const name = row.querySelector(".GameName").dataset.key;
        const text = row.querySelector(".ReviewText").dataset.key;
        const scores = Array.from(row.querySelectorAll(".ReviewDimension"),
            reviewCellValue);
        const url = document.querySelector(".GameViews a").href;
        const output = reviewClipboardText(text, name, scores, url);
        const status = document.getElementById("ReviewCopyStatus");
        status.textContent = "";
        let copied = false;
        try
        {
            if(navigator.clipboard?.writeText)
            {
                await navigator.clipboard.writeText(output);
                copied = true;
            }
        }
        catch(error)
        {
            // Fall back for unavailable or denied Clipboard API access.
        }
        if(!copied)
        {
            try
            {
                copied = copyWithSelection(output);
            }
            catch(error)
            {
                copied = false;
            }
        }
        const message = copied ? "Copied!" : "Copy failed";
        button.dataset.feedback = message;
        button.title = message;
        status.textContent = (copied ? "Review copied: " :
            "Could not copy review: ") + name;
        clearTimeout(feedback_timers.get(button));
        feedback_timers.set(button, setTimeout(clearFeedback, 2000, button));
    }
    for(const button of document.querySelectorAll(".ReviewCopy"))
    {
        button.hidden = false;
        button.addEventListener("click", copyReview);
    }
}

initializeReviews();
initializeReviewCopy();
