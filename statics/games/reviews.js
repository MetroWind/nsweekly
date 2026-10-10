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

initializeReviews();
