#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "http_adaptation.hpp"

using ::testing::Return;
using ::testing::HasSubstr;

TEST(HttpAdaptation, CopyReqToHttplibReq)
{
    {
        auto req = HTTPRequest("http://test/").setPayload("aaa")
            .setContentType("text/plain").addHeader("X-Something", "something");
        httplib::Request http_req;
        copyToHttplibReq(req, http_req);
        EXPECT_EQ(http_req.body, "aaa");
        EXPECT_EQ(http_req.get_header_value("Content-Type"), "text/plain");
        EXPECT_EQ(http_req.get_header_value("X-Something"), "something");
    }
    {
        auto req = HTTPRequest("http://test/").setContentType("image/png");
        httplib::Request http_req;
        copyToHttplibReq(req, http_req);
        EXPECT_EQ(http_req.get_header_value("Content-Type"), "image/png");
    }
}

