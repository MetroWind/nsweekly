"use strict";
const game_information = document.getElementById("GameInformation");
if(game_information)
{
    const desktop = window.matchMedia("(min-width: 56rem)");
    function updateDisclosure()
    {
        game_information.open = desktop.matches;
    }
    updateDisclosure();
    desktop.addEventListener("change", updateDisclosure);
}
