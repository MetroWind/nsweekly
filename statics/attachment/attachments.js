function setupCopyButtons()
{
    if(!navigator.clipboard) { return; }
    for(const button of document.querySelectorAll('.CopyURL'))
    {
        button.hidden = false;
        button.addEventListener('click', async function copyURL()
        {
            const link = button.parentElement.querySelector('.PublicURL');
            try
            {
                await navigator.clipboard.writeText(link.href);
                button.textContent = 'Copied';
            }
            catch
            {
                button.textContent = 'Select and copy the link above';
            }
        });
    }
}
setupCopyButtons();
