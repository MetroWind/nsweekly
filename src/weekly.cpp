#include "weekly.hpp"

bool WeeklyPost::isValidFormatInt(int i)
{
    switch(i)
    {
    case MARKDOWN:
        return true;
    }
    return false;
}
