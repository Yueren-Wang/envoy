#include "source/extensions/filters/http/oauth2/filter.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "source/common/common/assert.h"
#include "source/common/common/base64.h"
#include "source/common/common/empty_string.h"
#include "source/common/common/enum_to_int.h"
#include "source/common/common/fmt.h"
#include "source/common/common/hex.h"
#include "source/common/crypto/utility.h"
#include "source/common/http/header_map_impl.h"
#include "source/common/http/header_utility.h"
#include "source/common/http/headers.h"
#include "source/common/http/utility.h"
#include "source/common/protobuf/utility.h"
#include "source/common/runtime/runtime_features.h"

#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "jwt_verify_lib/jwt.h"
#include "jwt_verify_lib/status.h"

using namespace std::chrono_literals;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Oauth2 {

namespace {
Http::RegisterCustomInlineHeader<Http::CustomInlineHeaderRegistry::Type::RequestHeaders>
    authorization_handle(Http::CustomHeaders::get().Authorization);

constexpr const char* CookieDeleteFormatString =
    "{}=deleted; path=/; expires=Thu, 01 Jan 1970 00:00:00 GMT";
constexpr const char* CookieTailHttpOnlyFormatString = ";path=/;Max-Age={};secure;HttpOnly{}";
constexpr const char* CookieDomainFormatString = ";domain={}";

constexpr absl::string_view UnauthorizedBodyMessage = "OAuth flow failed.";

constexpr absl::string_view queryParamsError = "error";
constexpr absl::string_view queryParamsCode = "code";
constexpr absl::string_view queryParamsState = "state";
constexpr absl::string_view queryParamsRedirectUri = "redirect_uri";

constexpr absl::string_view stateParamsUrl = "url";
constexpr absl::string_view stateParamsCsrfToken = "csrf_token";

constexpr absl::string_view REDIRECT_RACE = "oauth.race_redirect";
constexpr absl::string_view REDIRECT_LOGGED_IN = "oauth.logged_in";
constexpr absl::string_view REDIRECT_FOR_CREDENTIALS = "oauth.missing_credentials";
constexpr absl::string_view SIGN_OUT = "oauth.sign_out";
constexpr absl::string_view DEFAULT_AUTH_SCOPE = "user";

constexpr absl::string_view SameSiteLax = ";SameSite=Lax";
constexpr absl::string_view SameSiteStrict = ";SameSite=Strict";
constexpr absl::string_view SameSiteNone = ";SameSite=None";
constexpr absl::string_view HmacPayloadSeparator = "\n";

template <class T>
std::vector<Http::HeaderUtility::HeaderDataPtr>
headerMatchers(const T& matcher_protos, Server::Configuration::CommonFactoryContext& context) {
  std::vector<Http::HeaderUtility::HeaderDataPtr> matchers;
  matchers.reserve(matcher_protos.size());

  for (const auto& proto : matcher_protos) {
    matchers.emplace_back(Http::HeaderUtility::createHeaderData(proto, context));
  }

  return matchers;
}

// Transforms the proto list of 'auth_scopes' into a vector of std::string, also
// handling the default value logic.
std::vector<std::string>
authScopesList(const Protobuf::RepeatedPtrField<std::string>& auth_scopes_protos) {
  std::vector<std::string> scopes;

  // If 'auth_scopes' is empty it must return a list with the default value.
  if (auth_scopes_protos.empty()) {
    scopes.emplace_back(DEFAULT_AUTH_SCOPE);
  } else {
    scopes.reserve(auth_scopes_protos.size());

    for (const auto& scope : auth_scopes_protos) {
      scopes.emplace_back(scope);
    }
  }
  return scopes;
}

// Transforms the proto list into encoded resource params
// Takes care of percentage encoding http and https is needed
std::string encodeResourceList(const Protobuf::RepeatedPtrField<std::string>& resources_protos) {
  std::string result = "";
  for (const auto& resource : resources_protos) {
    result += "&resource=" + Http::Utility::PercentEncoding::urlEncodeQueryParameter(resource);
  }
  return result;
}

// Sets the auth token as the Bearer token in the authorization header.
void setBearerToken(Http::RequestHeaderMap& headers, const std::string& token) {
  headers.setInline(authorization_handle.handle(), absl::StrCat("Bearer ", token));
}

std::string findValue(const absl::flat_hash_map<std::string, std::string>& map,
                      const std::string& key) {
  const auto value_it = map.find(key);
  return value_it != map.end() ? value_it->second : EMPTY_STRING;
}

AuthType
getAuthType(envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType auth_type) {
  switch (auth_type) {
    PANIC_ON_PROTO_ENUM_SENTINEL_VALUES;
  case envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType::
      OAuth2Config_AuthType_BASIC_AUTH:
    return AuthType::BasicAuth;
  case envoy::extensions::filters::http::oauth2::v3::OAuth2Config_AuthType::
      OAuth2Config_AuthType_URL_ENCODED_BODY:
  default:
    return AuthType::UrlEncodedBody;
  }
}

// Helper function to get SameSite attribute string from proto enum.
std::string
getSameSiteString(envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite same_site) {
  switch (same_site) {
    PANIC_ON_PROTO_ENUM_SENTINEL_VALUES;
  case envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite::
      CookieConfig_SameSite_STRICT:
    return std::string(SameSiteStrict);
  case envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite::
      CookieConfig_SameSite_LAX:
    return std::string(SameSiteLax);
  case envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite::
      CookieConfig_SameSite_NONE:
    return std::string(SameSiteNone);
  case envoy::extensions::filters::http::oauth2::v3::CookieConfig_SameSite::
      CookieConfig_SameSite_DISABLED:
    return EMPTY_STRING;
  }
}

Http::Utility::QueryParamsMulti buildAutorizationQueryParams(
    const envoy::extensions::filters::http::oauth2::v3::OAuth2Config& proto_config) {
  auto query_params =
      Http::Utility::QueryParamsMulti::parseQueryString(proto_config.authorization_endpoint());
  query_params.overwrite("client_id", proto_config.credentials().client_id());
  query_params.overwrite("response_type", "code");
  std::string scopes_list = absl::StrJoin(authScopesList(proto_config.auth_scopes()), " ");
  query_params.overwrite("scope",
                         Http::Utility::PercentEncoding::urlEncodeQueryParameter(scopes_list));
  return query_params;
}

std::string encodeHmacHexBase64(const std::vector<uint8_t>& secret, absl::string_view domain,
                                absl::string_view expires, absl::string_view token = "",
                                absl::string_view id_token = "",
                                absl::string_view refresh_token = "") {
  auto& crypto_util = Envoy::Common::Crypto::UtilitySingleton::get();
  const auto hmac_payload =
      absl::StrJoin({domain, expires, token, id_token, refresh_token}, HmacPayloadSeparator);
  std::string encoded_hmac;
  absl::Base64Escape(Hex::encode(crypto_util.getSha256Hmac(secret, hmac_payload)), &encoded_hmac);
  return encoded_hmac;
}

// Generates a SHA256 HMAC from a secret and a message and returns the result as a base64 encoded
// string.
std::string generateHmacBase64(const std::vector<uint8_t>& secret, std::string& message) {
  auto& crypto_util = Envoy::Common::Crypto::UtilitySingleton::get();
  std::vector<uint8_t> hmac_result = crypto_util.getSha256Hmac(secret, message);
  std::string hmac_string(hmac_result.begin(), hmac_result.end());
  std::string base64_encoded_hmac;
  absl::Base64Escape(hmac_string, &base64_encoded_hmac);
  return base64_encoded_hmac;
}

std::string encodeHmacBase64(const std::vector<uint8_t>& secret, absl::string_view domain,
                             absl::string_view expires, absl::string_view token = "",
                             absl::string_view id_token = "",
                             absl::string_view refresh_token = "") {
  std::string hmac_payload =
      absl::StrJoin({domain, expires, token, id_token, refresh_token}, HmacPayloadSeparator);
  return generateHmacBase64(secret, hmac_payload);
}

std::string encodeHmac(const std::vector<uint8_t>& secret, absl::string_view domain,
                       absl::string_view expires, absl::string_view token = "",
                       absl::string_view id_token = "", absl::string_view refresh_token = "") {
  return encodeHmacBase64(secret, domain, expires, token, id_token, refresh_token);
}

// Generates a CSRF token that can be used to prevent CSRF attacks.
// The token is in the format of <nonce>.<hmac(nonce)> recommended by
// https://cheatsheetseries.owasp.org/cheatsheets/Cross-Site_Request_Forgery_Prevention_Cheat_Sheet.html#signed-double-submit-cookie-recommended
std::string generateCsrfToken(const std::string& hmac_secret, Random::RandomGenerator& random) {
  std::vector<uint8_t> hmac_secret_vec(hmac_secret.begin(), hmac_secret.end());
  std::string random_string = Hex::uint64ToHex(random.random());
  std::string hmac = generateHmacBase64(hmac_secret_vec, random_string);
  std::string csrf_token = fmt::format("{}.{}", random_string, hmac);
  return csrf_token;
}

// validate the csrf token hmac to prevent csrf token forgery
bool validateCsrfTokenHmac(const std::string& hmac_secret, const std::string& csrf_token) {
  size_t pos = csrf_token.find('.');
  if (pos == std::string::npos) {
    return false;
  }

  std::string token = std::string(csrf_token.substr(0, pos));
  std::string hmac = std::string(csrf_token.substr(pos + 1));
  std::vector<uint8_t> hmac_secret_vec(hmac_secret.begin(), hmac_secret.end());
  return generateHmacBase64(hmac_secret_vec, token) == hmac;
}

/**
 * Encodes the state parameter for the OAuth2 flow.
 * The state parameter is a base64Url encoded JSON object containing the original request URL and a
 * CSRF token for CSRF protection.
 */
std::string encodeState(const std::string& original_request_url, const std::string& csrf_token) {
  std::string buffer;
  absl::string_view sanitized_url = Json::sanitize(buffer, original_request_url);
  absl::string_view sanitized_csrf_token = Json::sanitize(buffer, csrf_token);
  std::string json =
      fmt::format(R"({{"url":"{}","csrf_token":"{}"}})", sanitized_url, sanitized_csrf_token);
  return Base64Url::encode(json.data(), json.size());
}

} // namespace

FilterConfig::FilterConfig(
    const envoy::extensions::filters::http::oauth2::v3::OAuth2Config& proto_config,
    Server::Configuration::CommonFactoryContext& context,
    std::shared_ptr<SecretReader> secret_reader, Stats::Scope& scope,
    const std::string& stats_prefix)
    : oauth_token_endpoint_(proto_config.token_endpoint()),
      authorization_endpoint_(proto_config.authorization_endpoint()),
      authorization_query_params_(buildAutorizationQueryParams(proto_config)),
      client_id_(proto_config.credentials().client_id()),
      redirect_uri_(proto_config.redirect_uri()),
      redirect_matcher_(proto_config.redirect_path_matcher(), context),
      signout_path_(proto_config.signout_path(), context), secret_reader_(secret_reader),
      stats_(FilterConfig::generateStats(stats_prefix, scope)),
      encoded_resource_query_params_(encodeResourceList(proto_config.resources())),
      pass_through_header_matchers_(headerMatchers(proto_config.pass_through_matcher(), context)),
      deny_redirect_header_matchers_(headerMatchers(proto_config.deny_redirect_matcher(), context)),
      cookie_names_(proto_config.credentials().cookie_names()),
      cookie_domain_(proto_config.credentials().cookie_domain()),
      auth_type_(getAuthType(proto_config.auth_type())),
      default_expires_in_(PROTOBUF_GET_SECONDS_OR_DEFAULT(proto_config, default_expires_in, 0)),
      default_refresh_token_expires_in_(
          PROTOBUF_GET_SECONDS_OR_DEFAULT(proto_config, default_refresh_token_expires_in, 604800)),
      forward_bearer_token_(proto_config.forward_bearer_token()),
      preserve_authorization_header_(proto_config.preserve_authorization_header()),
      use_refresh_token_(FilterConfig::shouldUseRefreshToken(proto_config)),
      disable_id_token_set_cookie_(proto_config.disable_id_token_set_cookie()),
      disable_access_token_set_cookie_(proto_config.disable_access_token_set_cookie()),
      disable_refresh_token_set_cookie_(proto_config.disable_refresh_token_set_cookie()),
      bearer_token_cookie_settings_(
          (proto_config.has_cookie_configs() &&
           proto_config.cookie_configs().has_bearer_token_cookie_config())
              ? CookieSettings(proto_config.cookie_configs().bearer_token_cookie_config())
              : CookieSettings()),
      hmac_cookie_settings_(
          (proto_config.has_cookie_configs() &&
           proto_config.cookie_configs().has_oauth_hmac_cookie_config())
              ? CookieSettings(proto_config.cookie_configs().oauth_hmac_cookie_config())
              : CookieSettings()),
      expires_cookie_settings_(
          (proto_config.has_cookie_configs() &&
           proto_config.cookie_configs().has_oauth_expires_cookie_config())
              ? CookieSettings(proto_config.cookie_configs().oauth_expires_cookie_config())
              : CookieSettings()),
      id_token_cookie_settings_(
          (proto_config.has_cookie_configs() &&
           proto_config.cookie_configs().has_id_token_cookie_config())
              ? CookieSettings(proto_config.cookie_configs().id_token_cookie_config())
              : CookieSettings()),
      refresh_token_cookie_settings_(
          (proto_config.has_cookie_configs() &&
           proto_config.cookie_configs().has_refresh_token_cookie_config())
              ? CookieSettings(proto_config.cookie_configs().refresh_token_cookie_config())
              : CookieSettings()),
      nonce_cookie_settings_(
          (proto_config.has_cookie_configs() &&
           proto_config.cookie_configs().has_oauth_nonce_cookie_config())
              ? CookieSettings(proto_config.cookie_configs().oauth_nonce_cookie_config())
              : CookieSettings()) {
  if (!context.clusterManager().clusters().hasCluster(oauth_token_endpoint_.cluster())) {
    throw EnvoyException(fmt::format("OAuth2 filter: unknown cluster '{}' in config. Please "
                                     "specify which cluster to direct OAuth requests to.",
                                     oauth_token_endpoint_.cluster()));
  }
  if (!authorization_endpoint_url_.initialize(authorization_endpoint_,
                                              /*is_connect_request=*/false)) {
    throw EnvoyException(
        fmt::format("OAuth2 filter: invalid authorization endpoint URL '{}' in config.",
                    authorization_endpoint_));
  }

  if (proto_config.has_retry_policy()) {
    retry_policy_ = Http::Utility::convertCoreToRouteRetryPolicy(
        proto_config.retry_policy(), "5xx,gateway-error,connect-failure,reset");
  }
}

FilterStats FilterConfig::generateStats(const std::string& prefix, Stats::Scope& scope) {
  return {ALL_OAUTH_FILTER_STATS(POOL_COUNTER_PREFIX(scope, prefix))};
}

bool FilterConfig::shouldUseRefreshToken(
    const envoy::extensions::filters::http::oauth2::v3::OAuth2Config& proto_config) const {
  if (Runtime::runtimeFeatureEnabled("envoy.reloadable_features.oauth2_use_refresh_token")) {
    return PROTOBUF_GET_WRAPPED_OR_DEFAULT(proto_config, use_refresh_token, true);
  }

  return proto_config.use_refresh_token().value();
}

void OAuth2CookieValidator::setParams(const Http::RequestHeaderMap& headers,
                                      const std::string& secret) {
  const auto& cookies = Http::Utility::parseCookies(headers, [this](absl::string_view key) -> bool {
    return key == cookie_names_.oauth_expires_ || key == cookie_names_.bearer_token_ ||
           key == cookie_names_.oauth_hmac_ || key == cookie_names_.id_token_ ||
           key == cookie_names_.refresh_token_;
  });

  expires_ = findValue(cookies, cookie_names_.oauth_expires_);
  token_ = findValue(cookies, cookie_names_.bearer_token_);
  id_token_ = findValue(cookies, cookie_names_.id_token_);
  refresh_token_ = findValue(cookies, cookie_names_.refresh_token_);
  hmac_ = findValue(cookies, cookie_names_.oauth_hmac_);
  host_ = headers.Host()->value().getStringView();

  secret_.assign(secret.begin(), secret.end());
}

bool OAuth2CookieValidator::canUpdateTokenByRefreshToken() const { return !refresh_token_.empty(); }

bool OAuth2CookieValidator::hmacIsValid() const {
  absl::string_view cookie_domain = host_;
  if (!cookie_domain_.empty()) {
    cookie_domain = cookie_domain_;
  }
  return ((encodeHmacBase64(secret_, cookie_domain, expires_, token_, id_token_, refresh_token_) ==
           hmac_) ||
          (encodeHmacHexBase64(secret_, cookie_domain, expires_, token_, id_token_,
                               refresh_token_) == hmac_));
}

bool OAuth2CookieValidator::timestampIsValid() const {
  uint64_t expires;
  if (!absl::SimpleAtoi(expires_, &expires)) {
    return false;
  }

  const auto current_epoch = time_source_.systemTime().time_since_epoch();
  return std::chrono::seconds(expires) > current_epoch;
}

bool OAuth2CookieValidator::isValid() const { return hmacIsValid() && timestampIsValid(); }

OAuth2Filter::OAuth2Filter(FilterConfigSharedPtr config,
                           std::unique_ptr<OAuth2Client>&& oauth_client, TimeSource& time_source,
                           Random::RandomGenerator& random)
    : validator_(std::make_shared<OAuth2CookieValidator>(time_source, config->cookieNames(),
                                                         config->cookieDomain())),
      oauth_client_(std::move(oauth_client)), config_(std::move(config)), time_source_(time_source),
      random_(random) {

  oauth_client_->setCallbacks(*this);
}

/**
 * primary cases:
 * 1) pass through header is matching
 * 2) user is signing out
 * 3) /_oauth redirect
 * 4) user is authorized
 * 5) user is unauthorized
 */
Http::FilterHeadersStatus OAuth2Filter::decodeHeaders(Http::RequestHeaderMap& headers, bool) {
  // Skip Filter and continue chain if a Passthrough header is matching
  // Must be done before the sanitation of the authorization header,
  // otherwise the authorization header might be altered or removed
  for (const auto& matcher : config_->passThroughMatchers()) {
    if (matcher->matchesHeaders(headers)) {
      config_->stats().oauth_passthrough_.inc();
      return Http::FilterHeadersStatus::Continue;
    }
  }

  // Only sanitize the Authorization header if preserveAuthorizationHeader is false
  if (!config_->preserveAuthorizationHeader()) {
    // Sanitize the Authorization header, since we have no way to validate its content. Also,
    // if token forwarding is enabled, this header will be set based on what is on the HMAC cookie
    // before forwarding the request upstream.
    headers.removeInline(authorization_handle.handle());
  }

  // The following 2 headers are guaranteed for regular requests. The asserts are helpful when
  // writing test code to not forget these important variables in mock requests
  const Http::HeaderEntry* host_header = headers.Host();
  ASSERT(host_header != nullptr);
  host_ = host_header->value().getStringView();

  const Http::HeaderEntry* path_header = headers.Path();
  ASSERT(path_header != nullptr);
  const absl::string_view path_str = path_header->value().getStringView();

  // We should check if this is a sign out request.
  if (config_->signoutPath().match(path_header->value().getStringView())) {
    return signOutUser(headers);
  }

  if (canSkipOAuth(headers)) {
    // Update the path header with the query string parameters after a successful OAuth login.
    // This is necessary if a website requests multiple resources which get redirected to the
    // auth server. A cached login on the authorization server side will set cookies
    // correctly but cause a race condition on future requests that have their location set
    // to the callback path.
    if (config_->redirectPathMatcher().match(path_str)) {
      // Even though we're already logged in and don't technically need to validate the presence
      // of the auth code, we still perform the validation to ensure consistency and reuse the
      // validateOAuthCallback method. This is acceptable because the auth code is always present
      // in the query string of the callback path according to the OAuth2 spec.
      // More information can be found here:
      // https://datatracker.ietf.org/doc/html/rfc6749#section-4.1.2
      const CallbackValidationResult result = validateOAuthCallback(headers, path_str);
      if (!result.is_valid_) {
        sendUnauthorizedResponse();
        return Http::FilterHeadersStatus::StopIteration;
      }

      // Return 401 unauthorized if the original request URL in the state matches the redirect
      // config to avoid infinite redirect loops.
      Http::Utility::Url original_request_url;
      original_request_url.initialize(result.original_request_url_, false);
      if (config_->redirectPathMatcher().match(original_request_url.pathAndQueryParams())) {
        ENVOY_LOG(debug, "state url query params {} matches the redirect path matcher",
                  original_request_url.pathAndQueryParams());
        sendUnauthorizedResponse();
        return Http::FilterHeadersStatus::StopIteration;
      }

      // Since the user is already logged in, we don't need to exchange the auth code for tokens.
      // Instead, we redirect the user back to the original request URL.
      Http::ResponseHeaderMapPtr response_headers{
          Http::createHeaderMap<Http::ResponseHeaderMapImpl>(
              {{Http::Headers::get().Status, std::to_string(enumToInt(Http::Code::Found))},
               {Http::Headers::get().Location, result.original_request_url_}})};
      decoder_callbacks_->encodeHeaders(std::move(response_headers), true, REDIRECT_RACE);
      return Http::FilterHeadersStatus::StopIteration;
    }

    // Continue on with the filter stack.
    return Http::FilterHeadersStatus::Continue;
  }

  // Save the request headers for later modification if needed.
  request_headers_ = &headers;
  // If this isn't the callback URI, redirect to acquire credentials.
  //
  // The following conditional could be replaced with a regex pattern-match,
  // if we're concerned about strict matching against the callback path.
  if (!config_->redirectPathMatcher().match(path_str)) {

    // Check if we can update the access token via a refresh token.
    if (config_->useRefreshToken() && validator_->canUpdateTokenByRefreshToken()) {

      ENVOY_LOG(debug, "Trying to update the access token using the refresh token");

      // try to update access token by refresh token
      oauth_client_->asyncRefreshAccessToken(validator_->refreshToken(), config_->clientId(),
                                             config_->clientSecret(), config_->authType());
      // pause while we await the next step from the OAuth server
      return Http::FilterHeadersStatus::StopAllIterationAndWatermark;
    }

    if (canRedirectToOAuthServer(headers)) {
      ENVOY_LOG(debug, "redirecting to OAuth server", path_str);
      redirectToOAuthServer(headers);
      return Http::FilterHeadersStatus::StopIteration;
    } else {
      ENVOY_LOG(debug, "unauthorized, redirecting to OAuth server is not allowed", path_str);
      sendUnauthorizedResponse();
      return Http::FilterHeadersStatus::StopIteration;
    }
  }

  // At this point, we *are* on /_oauth. We believe this request comes from the authorization
  // server and we expect the query strings to contain the information required to get the access
  // token.
  const CallbackValidationResult result = validateOAuthCallback(headers, path_str);
  if (!result.is_valid_) {
    sendUnauthorizedResponse();
    return Http::FilterHeadersStatus::StopIteration;
  }

  original_request_url_ = result.original_request_url_;
  auth_code_ = result.auth_code_;
  Formatter::FormatterPtr formatter = THROW_OR_RETURN_VALUE(
      Formatter::FormatterImpl::create(config_->redirectUri()), Formatter::FormatterPtr);
  const auto redirect_uri =
      formatter->formatWithContext({&headers}, decoder_callbacks_->streamInfo());
  oauth_client_->asyncGetAccessToken(auth_code_, config_->clientId(), config_->clientSecret(),
                                     redirect_uri, config_->authType());

  // pause while we await the next step from the OAuth server
  return Http::FilterHeadersStatus::StopAllIterationAndBuffer;
}

Http::FilterHeadersStatus OAuth2Filter::encodeHeaders(Http::ResponseHeaderMap& headers, bool) {
  if (was_refresh_token_flow_) {
    addResponseCookies(headers, getEncodedToken());
    was_refresh_token_flow_ = false;
  }

  return Http::FilterHeadersStatus::Continue;
}

// Defines a sequence of checks determining whether we should initiate a new OAuth flow or skip to
// the next filter in the chain.
bool OAuth2Filter::canSkipOAuth(Http::RequestHeaderMap& headers) const {
  // We can skip OAuth if the supplied HMAC cookie is valid. Apply the OAuth details as headers
  // if we successfully validate the cookie.
  validator_->setParams(headers, config_->hmacSecret());
  if (validator_->isValid()) {
    config_->stats().oauth_success_.inc();
    if (config_->forwardBearerToken() && !validator_->token().empty()) {
      setBearerToken(headers, validator_->token());
    }
    ENVOY_LOG(debug, "skipping oauth flow due to valid hmac cookie");
    return true;
  }
  ENVOY_LOG(debug, "can not skip oauth flow");
  return false;
}

bool OAuth2Filter::canRedirectToOAuthServer(Http::RequestHeaderMap& headers) const {
  for (const auto& matcher : config_->denyRedirectMatchers()) {
    if (matcher->matchesHeaders(headers)) {
      ENVOY_LOG(debug, "redirect is denied for this request");
      return false;
    }
  }
  return true;
}

void OAuth2Filter::redirectToOAuthServer(Http::RequestHeaderMap& headers) {
  Http::ResponseHeaderMapPtr response_headers{Http::createHeaderMap<Http::ResponseHeaderMapImpl>(
      {{Http::Headers::get().Status, std::to_string(enumToInt(Http::Code::Found))}})};
  // Construct the correct scheme. We default to https since this is a requirement for OAuth to
  // succeed. However, if a downstream client explicitly declares the "http" scheme for whatever
  // reason, we also use "http" when constructing our redirect uri to the authorization server.
  auto scheme = Http::Headers::get().SchemeValues.Https;

  if (Http::Utility::schemeIsHttp(headers.getSchemeValue())) {
    scheme = Http::Headers::get().SchemeValues.Http;
  }
  const std::string base_path = absl::StrCat(scheme, "://", host_);
  const std::string original_url = absl::StrCat(base_path, headers.Path()->value().getStringView());

  // First, check if the CSRF token cookie exists.
  // The CSRF token cookie contains the CSRF token that is used to prevent CSRF attacks for the
  // OAuth flow. It was named "oauth_nonce" because the CSRF token contains a generated nonce.
  // "oauth_csrf_token" would be a more accurate name for the cookie.
  std::string csrf_token;
  bool csrf_token_cookie_exists = false;
  const auto csrf_token_cookie =
      Http::Utility::parseCookies(headers, [this](absl::string_view key) {
        return key == config_->cookieNames().oauth_nonce_;
      });
  if (csrf_token_cookie.find(config_->cookieNames().oauth_nonce_) != csrf_token_cookie.end()) {
    csrf_token = csrf_token_cookie.at(config_->cookieNames().oauth_nonce_);
    csrf_token_cookie_exists = true;
  } else {
    // Generate a CSRF token to prevent CSRF attacks.
    csrf_token = generateCsrfToken(config_->hmacSecret(), random_);
  }

  // Set the CSRF token cookie if it does not exist.
  if (!csrf_token_cookie_exists) {
    // Expire the CSRF token cookie in 10 minutes.
    // This should be enough time for the user to complete the OAuth flow.
    std::string csrf_expires = std::to_string(10 * 60);
    std::string same_site = getSameSiteString(config_->nonceCookieSettings().same_site_);
    std::string cookie_tail_http_only =
        fmt::format(CookieTailHttpOnlyFormatString, csrf_expires, same_site);
    if (!config_->cookieDomain().empty()) {
      cookie_tail_http_only = absl::StrCat(
          fmt::format(CookieDomainFormatString, config_->cookieDomain()), cookie_tail_http_only);
    }
    response_headers->addReferenceKey(
        Http::Headers::get().SetCookie,
        absl::StrCat(config_->cookieNames().oauth_nonce_, "=", csrf_token, cookie_tail_http_only));
  }

  // Validate the CSRF token HMAC if the CSRF token cookie exists.
  if (csrf_token_cookie_exists && !validateCsrfTokenHmac(config_->hmacSecret(), csrf_token)) {
    ENVOY_LOG(error, "csrf token validation failed");
    sendUnauthorizedResponse();
    return;
  }

  const std::string state = encodeState(original_url, csrf_token);
  auto query_params = config_->authorizationQueryParams();
  query_params.overwrite(queryParamsState, state);

  Formatter::FormatterPtr formatter = THROW_OR_RETURN_VALUE(
      Formatter::FormatterImpl::create(config_->redirectUri()), Formatter::FormatterPtr);
  const auto redirect_uri =
      formatter->formatWithContext({&headers}, decoder_callbacks_->streamInfo());
  const std::string escaped_redirect_uri =
      Http::Utility::PercentEncoding::urlEncodeQueryParameter(redirect_uri);
  query_params.overwrite(queryParamsRedirectUri, escaped_redirect_uri);

  // Copy the authorization endpoint URL to replace its query params.
  auto authorization_endpoint_url = config_->authorizationEndpointUrl();
  const std::string path_and_query_params = query_params.replaceQueryString(
      Http::HeaderString(authorization_endpoint_url.pathAndQueryParams()));
  authorization_endpoint_url.setPathAndQueryParams(path_and_query_params);
  const std::string new_url = authorization_endpoint_url.toString();
  response_headers->setLocation(new_url + config_->encodedResourceQueryParams());

  decoder_callbacks_->encodeHeaders(std::move(response_headers), true, REDIRECT_FOR_CREDENTIALS);

  config_->stats().oauth_unauthorized_rq_.inc();
}

/**
 * Modifies the state of the filter by adding response headers to the decoder_callbacks
 */
Http::FilterHeadersStatus OAuth2Filter::signOutUser(const Http::RequestHeaderMap& headers) {
  Http::ResponseHeaderMapPtr response_headers{Http::createHeaderMap<Http::ResponseHeaderMapImpl>(
      {{Http::Headers::get().Status, std::to_string(enumToInt(Http::Code::Found))}})};

  const std::string new_path = absl::StrCat(headers.getSchemeValue(), "://", host_, "/");

  std::string cookie_domain;
  if (!config_->cookieDomain().empty()) {
    cookie_domain = fmt::format(CookieDomainFormatString, config_->cookieDomain());
  }

  response_headers->addReferenceKey(
      Http::Headers::get().SetCookie,
      absl::StrCat(fmt::format(CookieDeleteFormatString, config_->cookieNames().oauth_hmac_),
                   cookie_domain));
  response_headers->addReferenceKey(
      Http::Headers::get().SetCookie,
      absl::StrCat(fmt::format(CookieDeleteFormatString, config_->cookieNames().bearer_token_),
                   cookie_domain));
  response_headers->addReferenceKey(
      Http::Headers::get().SetCookie,
      absl::StrCat(fmt::format(CookieDeleteFormatString, config_->cookieNames().id_token_),
                   cookie_domain));
  response_headers->addReferenceKey(
      Http::Headers::get().SetCookie,
      absl::StrCat(fmt::format(CookieDeleteFormatString, config_->cookieNames().refresh_token_),
                   cookie_domain));
  response_headers->addReferenceKey(
      Http::Headers::get().SetCookie,
      absl::StrCat(fmt::format(CookieDeleteFormatString, config_->cookieNames().oauth_nonce_),
                   cookie_domain));
  response_headers->setLocation(new_path);
  decoder_callbacks_->encodeHeaders(std::move(response_headers), true, SIGN_OUT);

  return Http::FilterHeadersStatus::StopIteration;
}

// Called after fetching access/refresh tokens.
void OAuth2Filter::updateTokens(const std::string& access_token, const std::string& id_token,
                                const std::string& refresh_token, std::chrono::seconds expires_in) {
  if (!config_->disableAccessTokenSetCookie()) {
    // Preventing this here excludes all other Access Token functionality
    // * setting the cookie
    // * omitting from HMAC computation (for setting, not for validating)
    access_token_ = access_token;
  }
  if (!config_->disableIdTokenSetCookie()) {
    // Preventing this here excludes all other ID Token functionality
    // * setting the cookie
    // * omitting from HMAC computation (for setting, not for validating)
    id_token_ = id_token;
  }
  if (!config_->disableRefreshTokenSetCookie()) {
    // Preventing this here excludes all other Refresh Token functionality
    // * setting the cookie
    // * omitting from HMAC computation (for setting, not for validating)
    refresh_token_ = refresh_token;
  }

  expires_in_ = std::to_string(expires_in.count());
  expires_refresh_token_in_ = getExpiresTimeForRefreshToken(refresh_token, expires_in);
  expires_id_token_in_ = getExpiresTimeForIdToken(id_token, expires_in);

  const auto new_epoch = time_source_.systemTime() + expires_in;
  new_expires_ = std::to_string(
      std::chrono::duration_cast<std::chrono::seconds>(new_epoch.time_since_epoch()).count());
}

std::string OAuth2Filter::getEncodedToken() const {
  auto token_secret = config_->hmacSecret();
  std::vector<uint8_t> token_secret_vec(token_secret.begin(), token_secret.end());
  std::string encoded_token;

  absl::string_view domain = host_;
  if (!config_->cookieDomain().empty()) {
    domain = config_->cookieDomain();
  }

  encoded_token =
      encodeHmac(token_secret_vec, domain, new_expires_, access_token_, id_token_, refresh_token_);

  return encoded_token;
}

std::string
OAuth2Filter::getExpiresTimeForRefreshToken(const std::string& refresh_token,
                                            const std::chrono::seconds& expires_in) const {
  if (config_->useRefreshToken()) {
    ::google::jwt_verify::Jwt jwt;
    if (jwt.parseFromString(refresh_token) == ::google::jwt_verify::Status::Ok && jwt.exp_ != 0) {
      const std::chrono::seconds expiration_from_jwt = std::chrono::seconds{jwt.exp_};
      const std::chrono::seconds now =
          std::chrono::time_point_cast<std::chrono::seconds>(time_source_.systemTime())
              .time_since_epoch();

      if (now < expiration_from_jwt) {
        const auto expiration_epoch = expiration_from_jwt - now;
        return std::to_string(expiration_epoch.count());
      } else {
        ENVOY_LOG(debug, "The expiration time in the refresh token is less than the current time");
        return "0";
      }
    }
    ENVOY_LOG(debug, "The refresh token is not a JWT or exp claim is omitted. The lifetime of the "
                     "refresh token will be taken from filter configuration");
    const std::chrono::seconds default_refresh_token_expires_in =
        config_->defaultRefreshTokenExpiresIn();
    return std::to_string(default_refresh_token_expires_in.count());
  }
  return std::to_string(expires_in.count());
}

std::string OAuth2Filter::getExpiresTimeForIdToken(const std::string& id_token,
                                                   const std::chrono::seconds& expires_in) const {
  if (!id_token.empty()) {
    ::google::jwt_verify::Jwt jwt;
    if (jwt.parseFromString(id_token) == ::google::jwt_verify::Status::Ok && jwt.exp_ != 0) {
      const std::chrono::seconds expiration_from_jwt = std::chrono::seconds{jwt.exp_};
      const std::chrono::seconds now =
          std::chrono::time_point_cast<std::chrono::seconds>(time_source_.systemTime())
              .time_since_epoch();

      if (now < expiration_from_jwt) {
        const auto expiration_epoch = expiration_from_jwt - now;
        return std::to_string(expiration_epoch.count());
      } else {
        ENVOY_LOG(debug, "The expiration time in the id token is less than the current time");
        return "0";
      }
    }
    ENVOY_LOG(debug, "The id token is not a JWT or exp claim is omitted, even though it is "
                     "required by the OpenID Connect 1.0 specification. "
                     "The lifetime of the id token will be aligned with the access token");
    return std::to_string(expires_in.count());
  }
  return std::to_string(expires_in.count());
}

// Helper function to build the cookie tail string.
std::string OAuth2Filter::BuildCookieTail(int cookie_type) const {
  std::string same_site;
  std::string expires_time = expires_in_;

  switch (cookie_type) {
    PANIC_ON_PROTO_ENUM_SENTINEL_VALUES;
  case 1: // BEARER_TOKEN TYPE
    same_site = getSameSiteString(config_->bearerTokenCookieSettings().same_site_);
    break;
  case 2: // OAUTH_HMAC TYPE
    same_site = getSameSiteString(config_->hmacCookieSettings().same_site_);
    break;
  case 3: // OAUTH_EXPIRES TYPE
    same_site = getSameSiteString(config_->expiresCookieSettings().same_site_);
    break;
  case 4: // ID_TOKEN TYPE
    same_site = getSameSiteString(config_->idTokenCookieSettings().same_site_);
    expires_time = expires_id_token_in_;
    break;
  case 5: // REFRESH_TOKEN TYPE
    same_site = getSameSiteString(config_->refreshTokenCookieSettings().same_site_);
    expires_time = expires_refresh_token_in_;
    break;
  case 6: // OAUTH_NONCE TYPE
    same_site = getSameSiteString(config_->refreshTokenCookieSettings().same_site_);
    break;
  }

  std::string cookie_tail = fmt::format(CookieTailHttpOnlyFormatString, expires_time, same_site);
  if (!config_->cookieDomain().empty()) {
    cookie_tail =
        absl::StrCat(fmt::format(CookieDomainFormatString, config_->cookieDomain()), cookie_tail);
  }
  return cookie_tail;
}

void OAuth2Filter::onGetAccessTokenSuccess(const std::string& access_code,
                                           const std::string& id_token,
                                           const std::string& refresh_token,
                                           std::chrono::seconds expires_in) {
  updateTokens(access_code, id_token, refresh_token, expires_in);
  finishGetAccessTokenFlow();
}

void OAuth2Filter::onRefreshAccessTokenSuccess(const std::string& access_code,
                                               const std::string& id_token,
                                               const std::string& refresh_token,
                                               std::chrono::seconds expires_in) {
  ASSERT(config_->useRefreshToken());
  updateTokens(access_code, id_token, refresh_token, expires_in);
  finishRefreshAccessTokenFlow();
}

void OAuth2Filter::finishGetAccessTokenFlow() {
  // At this point we have all of the pieces needed to authorize a user.
  // Now, we construct a redirect request to return the user to their
  // previous state and additionally set the OAuth cookies in browser.
  // The redirection should result in successfully passing this filter.
  Http::ResponseHeaderMapPtr response_headers{Http::createHeaderMap<Http::ResponseHeaderMapImpl>(
      {{Http::Headers::get().Status, std::to_string(enumToInt(Http::Code::Found))}})};

  addResponseCookies(*response_headers, getEncodedToken());
  response_headers->setLocation(original_request_url_);

  decoder_callbacks_->encodeHeaders(std::move(response_headers), true, REDIRECT_LOGGED_IN);
  config_->stats().oauth_success_.inc();
}

void OAuth2Filter::finishRefreshAccessTokenFlow() {
  ASSERT(config_->useRefreshToken());
  // At this point we have updated all of the pieces need to authorize a user
  // We need to actualize keys in the cookie header of the current request related
  // with authorization. So, the upstream can use updated cookies for itself purpose
  const CookieNames& cookie_names = config_->cookieNames();

  absl::flat_hash_map<std::string, std::string> cookies =
      Http::Utility::parseCookies(*request_headers_);

  cookies.insert_or_assign(cookie_names.oauth_hmac_, getEncodedToken());
  cookies.insert_or_assign(cookie_names.oauth_expires_, new_expires_);

  if (!access_token_.empty()) {
    cookies.insert_or_assign(cookie_names.bearer_token_, access_token_);
  }
  if (!id_token_.empty()) {
    cookies.insert_or_assign(cookie_names.id_token_, id_token_);
  }
  if (!refresh_token_.empty()) {
    cookies.insert_or_assign(cookie_names.refresh_token_, refresh_token_);
  }

  std::string new_cookies(absl::StrJoin(cookies, "; ", absl::PairFormatter("=")));
  request_headers_->setReferenceKey(Http::Headers::get().Cookie, new_cookies);
  if (config_->forwardBearerToken() && !access_token_.empty()) {
    setBearerToken(*request_headers_, access_token_);
  }

  was_refresh_token_flow_ = true;

  config_->stats().oauth_refreshtoken_success_.inc();
  config_->stats().oauth_success_.inc();
  decoder_callbacks_->continueDecoding();
}

void OAuth2Filter::onRefreshAccessTokenFailure() {
  config_->stats().oauth_refreshtoken_failure_.inc();
  // We failed to get an access token via the refresh token, so send the user to the oauth endpoint.
  if (canRedirectToOAuthServer(*request_headers_)) {
    redirectToOAuthServer(*request_headers_);
  } else {
    sendUnauthorizedResponse();
  }
}

void OAuth2Filter::addResponseCookies(Http::ResponseHeaderMap& headers,
                                      const std::string& encoded_token) const {
  // We use HTTP Only cookies.
  const CookieNames& cookie_names = config_->cookieNames();

  // Set the cookies in the response headers.
  headers.addReferenceKey(
      Http::Headers::get().SetCookie,
      absl::StrCat(cookie_names.oauth_hmac_, "=", encoded_token, BuildCookieTail(2))); // OAUTH_HMAC

  headers.addReferenceKey(Http::Headers::get().SetCookie,
                          absl::StrCat(cookie_names.oauth_expires_, "=", new_expires_,
                                       BuildCookieTail(3))); // OAUTH_EXPIRES

  if (!access_token_.empty()) {
    headers.addReferenceKey(Http::Headers::get().SetCookie,
                            absl::StrCat(cookie_names.bearer_token_, "=", access_token_,
                                         BuildCookieTail(1))); // BEARER_TOKEN
  }

  if (!id_token_.empty()) {
    headers.addReferenceKey(
        Http::Headers::get().SetCookie,
        absl::StrCat(cookie_names.id_token_, "=", id_token_, BuildCookieTail(4))); // ID_TOKEN
  }

  if (!refresh_token_.empty()) {
    headers.addReferenceKey(Http::Headers::get().SetCookie,
                            absl::StrCat(cookie_names.refresh_token_, "=", refresh_token_,
                                         BuildCookieTail(5))); // REFRESH_TOKEN
  }
}

void OAuth2Filter::sendUnauthorizedResponse() {
  config_->stats().oauth_failure_.inc();
  decoder_callbacks_->sendLocalReply(Http::Code::Unauthorized, UnauthorizedBodyMessage, nullptr,
                                     absl::nullopt, EMPTY_STRING);
}

// Validates the OAuth callback request.
// * Does the query parameters contain an error response?
// * Does the query parameters contain the code and state?
// * Does the state contain the original request URL and the CSRF token?
// * Does the CSRF token in the state match the one in the cookie?
CallbackValidationResult OAuth2Filter::validateOAuthCallback(const Http::RequestHeaderMap& headers,
                                                             const absl::string_view path_str) {
  // Return 401 unauthorized if the query parameters contain an error response.
  const auto query_parameters = Http::Utility::QueryParamsMulti::parseQueryString(path_str);
  if (query_parameters.getFirstValue(queryParamsError).has_value()) {
    ENVOY_LOG(debug, "OAuth server returned an error: \n{}", query_parameters.data());
    return {false, "", ""};
  }

  // Return 401 unauthorized if the query parameters do not contain the code and state.
  auto codeVal = query_parameters.getFirstValue(queryParamsCode);
  auto stateVal = query_parameters.getFirstValue(queryParamsState);
  if (!codeVal.has_value() || !stateVal.has_value()) {
    ENVOY_LOG(error, "code or state query param does not exist: \n{}", query_parameters.data());
    return {false, "", ""};
  }

  // Return 401 unauthorized if the state query parameter does not contain the original request URL
  // or the CSRF token.
  // Decode the state parameter to get the original request URL and the CSRF token.
  const std::string state = Base64Url::decode(stateVal.value());
  bool has_unknown_field;
  ProtobufWkt::Struct message;

  auto status = MessageUtil::loadFromJsonNoThrow(state, message, has_unknown_field);
  if (!status.ok()) {
    ENVOY_LOG(error, "state query param is not a valid JSON: \n{}", state);
    return {false, "", ""};
  }

  const auto& filed_value_pair = message.fields();
  if (!filed_value_pair.contains(stateParamsUrl) ||
      !filed_value_pair.contains(stateParamsCsrfToken)) {
    ENVOY_LOG(error, "state query param does not contain url or CSRF token: \n{}", state);
    return {false, "", ""};
  }

  // Return 401 unauthorized if the CSRF token cookie does not match the CSRF token in the state.
  //
  // This is to prevent attackers from injecting their own access token into a victim's
  // sessions via CSRF attack. The attack can result in victims saving their sensitive data
  // in the attacker's account.
  // More information can be found at https://datatracker.ietf.org/doc/html/rfc6819#section-5.3.5
  std::string csrf_token = filed_value_pair.at(stateParamsCsrfToken).string_value();
  if (!validateCsrfToken(headers, csrf_token)) {
    ENVOY_LOG(error, "csrf token validation failed");
    return {false, "", ""};
  }
  const std::string original_request_url = filed_value_pair.at(stateParamsUrl).string_value();

  // Return 401 unauthorized if the URL in the state is not valid.
  Http::Utility::Url url;
  if (!url.initialize(original_request_url, false)) {
    ENVOY_LOG(error, "state url {} can not be initialized", original_request_url);
    return {false, "", ""};
  }

  return {true, codeVal.value(), original_request_url};
}

// Validates the csrf_token in the state parameter against the one in the cookie.
bool OAuth2Filter::validateCsrfToken(const Http::RequestHeaderMap& headers,
                                     const std::string& csrf_token) const {
  const auto csrf_token_cookie =
      Http::Utility::parseCookies(headers, [this](absl::string_view key) {
        return key == config_->cookieNames().oauth_nonce_;
      });

  if (csrf_token_cookie.find(config_->cookieNames().oauth_nonce_) != csrf_token_cookie.end() &&
      csrf_token_cookie.at(config_->cookieNames().oauth_nonce_) == csrf_token &&
      validateCsrfTokenHmac(config_->hmacSecret(), csrf_token)) {
    return true;
  }
  return false;
}

} // namespace Oauth2
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
