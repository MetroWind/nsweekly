#include <gtest/gtest.h>
#include "route_urls.hpp"

TEST(RouteUrls, PreservesEveryTemplateName)
{
    EXPECT_EQ(urlFor("index", "ignored"), "/");
    EXPECT_EQ(urlFor("openid-redirect", "ignored"), "/openid-redirect");
    EXPECT_EQ(urlFor("login", "ignored"), "/login");
    EXPECT_EQ(urlFor("weekly", "mw"), "/weekly/mw");
    EXPECT_EQ(urlFor("weekly", "mw/2000-01-03"),
              "/weekly/mw/2000-01-03");
    EXPECT_EQ(urlFor("edit", "mw/2000-01-03"), "/edit/mw/2000-01-03");
    EXPECT_EQ(urlFor("statics", "preview.js"), "/statics/preview.js");
    EXPECT_EQ(urlFor("unknown", "mw"), "");
}
