#include <charconv>
#include <cmath>
#include <format>
#include "game_review.hpp"

E<GameReviewInput> validateReview(const GameFields &fields)
{
    GameReviewInput input;
    GameValidationError errors;
    for(size_t i = 0; i < REVIEW_KEYS.size(); ++i)
    {
        const std::string key(REVIEW_KEYS[i]);
        auto value = fields.scalars.find(key);
        if(value != fields.scalars.end() && !value->second.empty())
        {
            double score = 0;
            const auto &text = value->second;
            auto [end, ec] = std::from_chars(
                text.data(), text.data() + text.size(), score);
            if(ec != std::errc{} || end != text.data() + text.size() ||
               !std::isfinite(score) || score < 1 || score > 10)
            {
                errors.fields[key] = "Enter a score from 1 to 10, or leave blank.";
            }
            else
            {
                input.scores[i] = score;
            }
        }
    }
    if(auto text = fields.scalars.find("text"); text != fields.scalars.end())
    {
        input.text = text->second;
        if(!validGameText(input.text))
        {
            errors.fields["text"] = "Enter valid UTF-8 text.";
        }
    }
    if(!errors.fields.empty())
    {
        return std::unexpected(Error(std::move(errors)));
    }
    return input;
}

GameFields reviewFields(const GameReviewInput &input)
{
    GameFields fields;
    for(size_t i = 0; i < REVIEW_KEYS.size(); ++i)
    {
        const std::string key(REVIEW_KEYS[i]);
        fields.scalars[key] = input.scores[i] ?
            std::format("{}", *input.scores[i]) : "";
    }
    fields.scalars["text"] = input.text;
    return fields;
}
