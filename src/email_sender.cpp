/* SMTP transport for gitm notifications, built on libcurl's easy interface.

   The mail server is discovered from the user's email domain (or configured
   in the gitm.manifest `[email]` section) and the account credentials are
   read from the platform credential manager (see login_mgr). */

#include "email_sender.hpp"

#include <curl/curl.h>

#include <cctype>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string joinNames(const std::vector<std::string>& names) {
    std::ostringstream out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i)
            out << ", ";
        out << names[i];
    }
    return out.str();
}

std::string lower(std::string s) {
    for (char& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// Builds the RFC5322 message payload curl will push to the server.
std::string buildMime(const mail_message& m) {
    std::ostringstream out;
    out << "From: <" << m.from << ">\r\n";
    out << "To: <" << joinNames(m.to) << ">\r\n";
    if (!m.cc.empty())
        out << "Cc: <" << joinNames(m.cc) << ">\r\n";
    out << "Subject: " << m.subject << "\r\n";
    out << "MIME-Version: 1.0\r\n";
    out << "Content-Type: text/plain; charset=UTF-8\r\n";
    out << "\r\n";
    out << m.body << "\r\n";
    return out.str();
}

// libcurl read callback fed by the message payload string.
size_t readCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    std::string* payload = static_cast<std::string*>(userdata);
    if (payload->empty())
        return 0;
    const size_t max = size * nitems;
    const size_t n = payload->size() > max ? max : payload->size();
    std::memcpy(buffer, payload->data(), n);
    payload->erase(0, n);
    return n;
}

} // namespace

bool send_email(const smtp_config& config, const mail_message& message, std::string& error) {
    if (config.host.empty() || config.username.empty()) {
        error = "SMTP server not configured";
        return false;
    }
    if (message.to.empty()) {
        error = "No recipient specified";
        return false;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL* curl = curl_easy_init();
    if (!curl) {
        curl_global_cleanup();
        error = "Failed to initialise libcurl";
        return false;
    }

    const std::string scheme = config.use_tls ? "smtps" : "smtp";
    const std::string url = scheme + "://" + config.host + ":" + std::to_string(config.port);
    std::string payload = buildMime(message);

    // SMTP RCPT list: everybody gets the message; bcc recipients are listed in
    // the envelope but not in the visible headers.
    curl_slist* env_recipients = nullptr;
    for (const auto& rcpt : message.to)
        env_recipients = curl_slist_append(env_recipients, rcpt.c_str());
    for (const auto& rcpt : message.cc)
        env_recipients = curl_slist_append(env_recipients, rcpt.c_str());
    for (const auto& rcpt : message.bcc)
        env_recipients = curl_slist_append(env_recipients, rcpt.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, message.from.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, env_recipients);
    curl_easy_setopt(curl, CURLOPT_MAIL_AUTH, message.from.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, config.username.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, config.password.c_str());
    curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_TRY);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, readCallback);
    curl_easy_setopt(curl, CURLOPT_READDATA, &payload);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(payload.size()));

    const CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(env_recipients);
    curl_easy_cleanup(curl);
    curl_global_cleanup();

    if (res != CURLE_OK) {
        error = std::string("SMTP send failed: ") + curl_easy_strerror(res);
        return false;
    }
    return true;
}

bool guess_smtp_config(const std::string& email, const std::string& password, smtp_config& out) {
    const std::size_t at = email.find('@');
    if (at == std::string::npos)
        return false;
    const std::string domain = lower(email.substr(at + 1));

    smtp_config cfg;
    cfg.username = email;
    cfg.password = password;

    if (domain == "gmail.com") {
        cfg.host = "smtp.gmail.com";
        cfg.port = 587;
        cfg.use_tls = false; // STARTTLS
    } else if (domain == "outlook.com" || domain == "hotmail.com" || domain == "live.com") {
        cfg.host = "smtp-mail.outlook.com";
        cfg.port = 587;
        cfg.use_tls = false;
    } else if (domain == "yahoo.com") {
        cfg.host = "smtp.mail.yahoo.com";
        cfg.port = 587;
        cfg.use_tls = false;
    } else if (domain == "icloud.com" || domain == "me.com") {
        cfg.host = "smtp.mail.me.com";
        cfg.port = 587;
        cfg.use_tls = false;
    } else {
        // generic: try the common smtp.<domain> on port 587 with STARTTLS
        cfg.host = "smtp." + domain;
        cfg.port = 587;
        cfg.use_tls = false;
    }

    out = std::move(cfg);
    return true;
}
