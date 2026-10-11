#include <gtest/gtest.h>

#include "test_utils.hpp"
#include "weekly/weekly.hpp"

TEST(WeeklyRendering, MacroHtmlAndDocumentIsolation)
{
    WeeklyPost post;
    post.format = WeeklyPost::MARKDOWN;
    post.raw_content =
        "%def[highlight]{text}{<mark>%text</mark>}\n\n%highlight{Hello}";
    ASSIGN_OR_FAIL(auto html, post.render());
    EXPECT_NE(html.find("<mark>Hello</mark>"), std::string::npos);

    post.raw_content = "%highlight{Other}";
    ASSIGN_OR_FAIL(auto other, post.render());
    EXPECT_EQ(other.find("<mark>"), std::string::npos);
}

TEST(WeeklyRendering, MarkdownAndUnicode)
{
    WeeklyPost post;
    post.format = WeeklyPost::MARKDOWN;
    post.raw_content = "# 中文 café\n\n%code{hello}";
    ASSIGN_OR_FAIL(auto html, post.render());
    EXPECT_NE(html.find("<h1>中文 café</h1>"), std::string::npos);
    EXPECT_NE(html.find("<code>hello</code>"), std::string::npos);
}
