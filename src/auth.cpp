#include <memory>
#include <expected>
#include <string>
#include <string_view>
#include <format>

#include <nlohmann/json.hpp>

#include "auth.hpp"
#include "config.hpp"
#include "error.hpp"
#include "http_client.hpp"
#include "utils.hpp"


E<std::string> getStrProperty(
    const nlohmann::json& json_dict, std::string_view property)
{
    if(json_dict[property].is_string())
    {
        return json_dict[property].get<std::string>();
    }
    else
    {
        return std::unexpected(runtimeError(std::format("Invalid value of {}", property)));
    }
}

E<int> getIntProperty(
    const nlohmann::json& json_dict, std::string_view property)
{
    if(json_dict[property].is_number_integer())
    {
        return json_dict[property].get<int>();
    }
    else
    {
        return std::unexpected(runtimeError(std::format("Invalid value of {}", property)));
    }
}

E<std::unique_ptr<AuthOpenIDConnect>> AuthOpenIDConnect::create(
    const Configuration& config, std::string_view redirect_url,
    std::unique_ptr<HTTPSessionInterface> http)
{
    std::string prefix = config.openid_url_prefix;
    if(http == nullptr)
    {
        return std::unexpected(runtimeError("Null HTTP client"));
    }
    if(prefix.empty())
    {
        return std::unexpected(runtimeError("Empty auth prefix"));
    }

    // Remove trailing slashes from URL prefix.
    while(prefix.back() == '/')
    {
        prefix.pop_back();
        if(prefix.empty())
        {
            return std::unexpected(runtimeError("Invalid auth prefix"));
        }
    }

    std::string url = std::move(prefix) + "/.well-known/openid-configuration";
    E<const HTTPResponse*> result = http->get(url);
    if(!result.has_value())
    {
        return std::unexpected(result.error());
    }

    const HTTPResponse& res = **result;
    nlohmann::json data = parseJSON(res.payload);
    if(data.is_discarded())
    {
        return std::unexpected(
            runtimeError("Invalid OpenID configuration from server"));
    }
    auto auth = std::make_unique<AuthOpenIDConnect>(
        config, redirect_url, std::move(http));
    ASSIGN_OR_RETURN(auth->endpoint_auth,
                     getStrProperty(data, "authorization_endpoint"));
    ASSIGN_OR_RETURN(auth->endpoint_token,
                     getStrProperty(data, "token_endpoint"));
    ASSIGN_OR_RETURN(auth->endpoint_introspect,
                     getStrProperty(data, "introspection_endpoint"));
    ASSIGN_OR_RETURN(auth->endpoint_user_info,
                     getStrProperty(data, "userinfo_endpoint"));
    return auth;
}

E<HTTPResponse> AuthOpenIDConnect::initiate()
{
    std::string url = std::format(
        "{}?response_type=code&client_id={}&redirect_uri={}&scope=openid%20profile",
        endpoint_auth, urlEncode(config.client_id), urlEncode(redirection_url));
    E<const HTTPResponse*> res = http_client->get(url);
    return res.transform([](const auto* r) { return *r; });
}

E<void> AuthOpenIDConnect::authenticate(std::string_view code) const
{
    std::string payload = std::format(
        "grant_type=authorization_code&code={}&redirect_uri={}",
        urlEncode(code), urlEncode(redirection_url));
    E<const HTTPResponse*> result = http_client->post(
        endpoint_token, "application/x-www-form-urlencoded", payload);
    ASSIGN_OR_RETURN(const HTTPResponse* res, result);
    if(res->status != 200)
    {
        return std::unexpected(
            httpError(res->status, bytesToString(res->payload)));
    }
    nlohmann::json data = parseJSON(bytesToString(res->payload));
    if(data.is_discarded())
    {
        return std::unexpected(runtimeError("Invalid token response"));
    }

    Tokens tokens;
    ASSIGN_OR_RETURN(tokens.access_token, getStrProperty(data, "access_token"));
    ASSIGN_OR_RETURN(tokens.id_token, getStrProperty(data, "id_token"));
    ASSIGN_OR_RETURN(tokens.refresh_token, getStrProperty(data, "refresh_token"));
    ASSIGN_OR_RETURN(int seconds, getIntProperty(data, "expires_in"));


    return {};
}
