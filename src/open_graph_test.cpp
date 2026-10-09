#include <gtest/gtest.h>
#include "open_graph.hpp"

TEST(OpenGraph, UsesConfiguredAbsoluteUrlsAndRemovesTrackingParameters)
{
    const auto graph = openGraph("https://weekly.example/", "Game Reviews",
        "/games/alice/reviews?utm_source=share#game-1", "Review scores");
    EXPECT_EQ(graph["url"], "https://weekly.example/games/alice/reviews");
    EXPECT_EQ(graph["image"], "https://weekly.example/statics/icon-180.png");
    EXPECT_EQ(graph["title"], "Game Reviews");
}

TEST(OpenGraph, EscapesTextAndAttributeBoundaries)
{
    const auto graph = openGraph("https://weekly.example", "<Title> & \"test\"",
        "/games/a%22b", "A <description> & \"quote\"");
    EXPECT_EQ(graph["title"], "&lt;Title&gt; &amp; &quot;test&quot;");
    EXPECT_EQ(graph["description"],
        "A &lt;description&gt; &amp; &quot;quote&quot;");
    EXPECT_EQ(graph["url"], "https://weekly.example/games/a%22b");
}
